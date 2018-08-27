// ----------------------------------------------------------------------
// File: DrainJob.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/*----------------------------------------------------------------------------*/
#include "mgm/DrainJob.hh"
#include "mgm/FileSystem.hh"
#include "mgm/FsView.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Quota.hh"
#include "mgm/Master.hh"
#include "namespace/interface/IFsView.hh"
#include "namespace/Prefetcher.hh"
#include "common/FileId.hh"
#include "common/LayoutId.hh"
#include "common/Logging.hh"
#include "common/TransferQueue.hh"
#include "common/TransferJob.hh"

/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_BEGIN


DrainJob::~DrainJob()
/*----------------------------------------------------------------------------*/
/**
 * @brief Destructor
 *
 * Cancels and joins the drain thread.
 */
/*----------------------------------------------------------------------------*/
{
  eos_static_info("waiting for join ...");

  if (mThread) {
    XrdSysThread::Cancel(mThread);

    if (!gOFS->Shutdown) {
      XrdSysThread::Join(mThread, NULL);
    }

    mThread = 0;
  }

  ResetCounter();
  eos_static_notice("Stopping Drain Job for fs=%u", mFsId);
}

/*----------------------------------------------------------------------------*/
void
/*----------------------------------------------------------------------------*/
/**
 * @brief resets all drain relevant counters to 0
 *
 */
/*----------------------------------------------------------------------------*/
DrainJob::ResetCounter()
{
  FileSystem* fs = 0;

  if (FsView::gFsView.mIdView.count(mFsId)) {
    fs = FsView::gFsView.mIdView[mFsId];

    if (fs) {
      fs->OpenTransaction();
      fs->SetLongLong("stat.drainbytesleft", 0);
      fs->SetLongLong("stat.drainfiles", 0);
      fs->SetLongLong("stat.timeleft", 0);
      fs->SetLongLong("stat.drainprogress", 0);
      fs->SetLongLong("stat.drainretry", 0);
      fs->SetDrainStatus(eos::common::FileSystem::kNoDrain);
      SetDrainer();
      fs->CloseTransaction();
    }
  }
}

/*----------------------------------------------------------------------------*/
void*
DrainJob::StaticThreadProc(void* arg)
/*----------------------------------------------------------------------------*/
/**
 * @brief static thread start function
 *
 */
/*----------------------------------------------------------------------------*/
{
  return reinterpret_cast<DrainJob*>(arg)->Drain();
}

/*----------------------------------------------------------------------------*/
void
DrainJob::SetDrainer()
/*----------------------------------------------------------------------------*/
/**
 * @brief en-/disable the drain pull in all nodes participating in draining
 *
 */
/*----------------------------------------------------------------------------*/
{
  FileSystem* fs = 0;
  fs = 0;

  if (FsView::gFsView.mIdView.count(mFsId)) {
    fs = FsView::gFsView.mIdView[mFsId];
  }

  if (!fs) {
    return;
  }

  FsGroup::const_iterator git;
  bool setactive = false;

  if (FsView::gFsView.mGroupView.count(mGroup)) {
    for (git = FsView::gFsView.mGroupView[mGroup]->begin();
         git != FsView::gFsView.mGroupView[mGroup]->end(); git++) {
      if (FsView::gFsView.mIdView.count(*git)) {
        int drainstatus =
          (eos::common::FileSystem::GetDrainStatusFromString(
             FsView::gFsView.mIdView[*git]->GetString("drainstatus").c_str())
          );

        if ((drainstatus == eos::common::FileSystem::kDraining) ||
            (drainstatus == eos::common::FileSystem::kDrainStalling)) {
          // if any mGroup filesystem is draining, all the others have
          // to enable the pull for draining!
          setactive = true;
        }
      }
    }

    // if the mGroup get's disabled we stop the draining
    if (FsView::gFsView.mGroupView[mGroup]->GetConfigMember("status") != "on") {
      setactive = false;
    }

    for (git = FsView::gFsView.mGroupView[mGroup]->begin();
         git != FsView::gFsView.mGroupView[mGroup]->end(); git++) {
      fs = FsView::gFsView.mIdView[*git];

      if (fs) {
        if (setactive) {
          if (fs->GetString("stat.drainer") != "on") {
            fs->SetString("stat.drainer", "on");
          }
        } else {
          if (fs->GetString("stat.drainer") != "off") {
            fs->SetString("stat.drainer", "off");
          }
        }
      }
    }
  }
}

//------------------------------------------------------------------------------
// @brief Set number of transfers and rate in all participating nodes
//------------------------------------------------------------------------------
void
DrainJob::SetSpaceNode()

{
  std::string SpaceNodeTransfers = "";
  std::string SpaceNodeTransferRate = "";
  FileSystem* fs = 0;
  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

  if (FsView::gFsView.mSpaceView.count(mSpace)) {
    SpaceNodeTransfers =
      FsView::gFsView.mSpaceView[mSpace]->GetConfigMember("drainer.node.ntx");
    SpaceNodeTransferRate =
      FsView::gFsView.mSpaceView[mSpace]->GetConfigMember("drainer.node.rate");
  }

  FsGroup::const_iterator git;

  if (FsView::gFsView.mGroupView.count(mGroup)) {
    for (git = FsView::gFsView.mGroupView[mGroup]->begin();
         git != FsView::gFsView.mGroupView[mGroup]->end(); git++) {
      if (FsView::gFsView.mIdView.count(*git)) {
        fs = FsView::gFsView.mIdView[*git];

        if (fs) {
          FsNode* node = FsView::gFsView.mNodeView[fs->GetQueue()];

          if (node) {
            // broadcast the rate & stream configuration if changed
            if (node->GetConfigMember("stat.drain.ntx") != SpaceNodeTransfers) {
              node->SetConfigMember("stat.drain.ntx",
                                    SpaceNodeTransfers, false, "", true);
            }

            if (node->GetConfigMember("stat.drain.rate") != SpaceNodeTransferRate) {
              node->SetConfigMember("stat.drain.rate",
                                    SpaceNodeTransferRate, false, "", true);
            }
          }
        }
      }
    }
  }
}

/*----------------------------------------------------------------------------*/
void*
DrainJob::Drain(void)
/*----------------------------------------------------------------------------*/
/**
 * @brief thread function running the drain supervision
 *
 */
/*----------------------------------------------------------------------------*/
{
  XrdSysThread::SetCancelOn();
  XrdSysThread::SetCancelDeferred();
  // the retry is currently hardcoded to 1
  // e.g. the maximum time for a drain operation is 1 x <drainperiod>
  int maxtry = 1;
  int ntried = 0;
  long long filesleft = 0;
retry:
  ntried++;
  eos_static_notice(
    "Starting Drain Job for fs=%u onopserror=%d try=%d",
    mFsId,
    mOnOpsError,
    ntried);
  FileSystem* fs = 0;
  {
    eos::common::RWMutexReadLock fs_rd_lock(FsView::gFsView.ViewMutex);
    ResetCounter();
  }
  time_t drainstart = time(NULL);
  time_t drainperiod = 0;
  time_t drainendtime = 0;
  eos::common::FileSystem::fs_snapshot_t drain_snapshot;
  XrdSysThread::SetCancelOff();
  {
    // set status to 'prepare'
    eos::common::RWMutexReadLock fs_rd_lock(FsView::gFsView.ViewMutex);
    fs = 0;

    if (FsView::gFsView.mIdView.count(mFsId)) {
      fs = FsView::gFsView.mIdView[mFsId];
    }

    if (!fs) {
      eos_static_notice(
        "Filesystem fsid=%u has been removed during drain operation",
        mFsId);
      return 0;
    }

    fs->SetDrainStatus(eos::common::FileSystem::kDrainPrepare);
    fs->SetLongLong("stat.drainretry", ntried - 1);
    mGroup = fs->GetString("schedgroup");
    fs->SnapShotFileSystem(drain_snapshot, false);
    drainperiod = fs->GetLongLong("drainperiod");
    drainendtime = drainstart + drainperiod;
    mSpace = drain_snapshot.mSpace;
    mGroup = drain_snapshot.mGroup;
  }
  XrdSysThread::SetCancelOn();
  // now we wait 60 seconds or the service delay time indicated by Master
  size_t kLoop = gOFS->mMaster->GetServiceDelay();

  if (!kLoop) {
    kLoop = 60;
  }

  for (size_t k = 0; k < kLoop; k++) {
    XrdSysThread::SetCancelOff();
    fs->SetLongLong("stat.timeleft", kLoop - 1 - k);
    XrdSysThread::SetCancelOn();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    XrdSysThread::CancelPoint();
  }

  //----------------------------------------------------------------------------
  // wait that the namespace is initialized
  //----------------------------------------------------------------------------
  fs->SetDrainStatus(eos::common::FileSystem::kDrainWait);
  bool go = false;

  do {
    XrdSysThread::SetCancelOff();
    {
      XrdSysMutexHelper lock(gOFS->InitializationMutex);

      if (gOFS->mInitialized == gOFS->kBooted) {
        go = true;
      }
    }
    XrdSysThread::SetCancelOn();
    std::this_thread::sleep_for(std::chrono::seconds(1));
  } while (!go);

  // check if we should abort
  XrdSysThread::CancelPoint();
  // build the list of files to migrate
  long long totalfiles = 0;
  long long wopenfiles = 0;
  XrdSysThread::SetCancelOff();
  {
    //---------------------------------------------------------------------------
    eos::Prefetcher::prefetchFilesystemFileListAndWait(gOFS->eosView,
        gOFS->eosFsView, mFsId);
    eos::common::RWMutexReadLock fs_rd_lock(FsView::gFsView.ViewMutex);
    eos::common::RWMutexReadLock ns_rd_lock(gOFS->eosViewRWMutex);

    try {
      totalfiles = gOFS->eosFsView->getNumFilesOnFs(mFsId);

      if (fs->GetConfigStatus() == eos::common::FileSystem::kDrain) {
        //----------------------------------------------------------------------
        // if we are still an alive file system, we cannot finish a drain
        // as a long as we see some open files
        //----------------------------------------------------------------------
        wopenfiles = fs->GetLongLong("stat.wopen");
      }
    } catch (eos::MDException& e) {
      //------------------------------------------------------------------------
      // there are no files in that view
      //------------------------------------------------------------------------
    }
  }
  //----------------------------------------------------------------------------
  XrdSysThread::SetCancelOn();

  if ((!wopenfiles) && (!totalfiles)) {
    goto nofilestodrain;
  }

  XrdSysThread::SetCancelOff();
  // set the shared object counter
  {
    eos::common::RWMutexReadLock fs_rd_lock(FsView::gFsView.ViewMutex);
    fs = 0;

    if (FsView::gFsView.mIdView.count(mFsId)) {
      fs = FsView::gFsView.mIdView[mFsId];
    }

    if (!fs) {
      eos_static_notice("Filesystem fsid=%u has been removed during drain "
                        "operation", mFsId);
      XrdSysThread::SetCancelOn();
      return 0;
    }

    fs->SetLongLong("stat.drainbytesleft",
                    fs->GetLongLong("stat.statfs.usedbytes"));
    fs->SetLongLong("stat.drainfiles", totalfiles);
  }

  if (mOnOpsError) {
    time_t waitendtime;
    time_t waitreporttime;
    time_t now;
    XrdSysThread::SetCancelOff();
    {
      // Set status to 'waiting'
      eos::common::RWMutexReadLock fs_rd_lock(FsView::gFsView.ViewMutex);
      fs = 0;

      if (FsView::gFsView.mIdView.count(mFsId)) {
        fs = FsView::gFsView.mIdView[mFsId];
      }

      if (!fs) {
        eos_static_notice("Filesystem fsid=%u has been removed during drain "
                          "operation", mFsId);
        return 0;
      }

      fs->SetDrainStatus(eos::common::FileSystem::kDrainWait);
      waitendtime = time(NULL) + (time_t) fs->GetLongLong("graceperiod");
    }
    XrdSysThread::SetCancelOn();
    waitreporttime = time(NULL) + 10; // we report every 10 seconds

    while ((now = time(NULL)) < waitendtime) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      // Check if we should abort
      XrdSysThread::CancelPoint();

      if (now > waitreporttime) {
        XrdSysThread::SetCancelOff();
        // update stat.timeleft
        eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
        fs = 0;

        if (FsView::gFsView.mIdView.count(mFsId)) {
          fs = FsView::gFsView.mIdView[mFsId];
        }

        if (!fs) {
          eos_static_notice(
            "Filesystem fsid=%u has been removed during drain operation",
            mFsId);
          return 0;
        }

        fs->SetLongLong("stat.timeleft", waitendtime - now);
        waitreporttime = now + 10;
        XrdSysThread::SetCancelOn();
      }
    }

    //--------------------------------------------------------------------------
    // set the new drain times
    //--------------------------------------------------------------------------
    drainstart = now;
    drainendtime = drainstart + drainperiod;
  }

  //----------------------------------------------------------------------------
  // check if we should abort
  //----------------------------------------------------------------------------
  XrdSysThread::CancelPoint();
  //----------------------------------------------------------------------------
  // extract all fids to drain
  // make statistics of files to be lost if we are in draindead
  //----------------------------------------------------------------------------
  XrdSysThread::SetCancelOff();
  //----------------------------------------------------------------------------
  // set status to 'draining'
  //----------------------------------------------------------------------------
  {
    eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
    fs = 0;

    if (FsView::gFsView.mIdView.count(mFsId)) {
      fs = FsView::gFsView.mIdView[mFsId];
    }

    if (!fs) {
      eos_static_notice("Filesystem fsid=%u has been removed during drain "
                        "operation", mFsId);
      XrdSysThread::SetCancelOn();
      return 0;
    }

    fs->SetDrainStatus(eos::common::FileSystem::kDraining);
    // Enable the pull functionality on FST
    SetDrainer();
  }
  time_t last_filesleft_change;
  last_filesleft_change = time(NULL);
  long long last_filesleft;
  last_filesleft = 0;
  filesleft = 0;

  // Enable draining
  do {
    XrdSysThread::SetCancelOff();
    bool stalled = ((time(NULL) - last_filesleft_change) > 600);
    SetSpaceNode();
    {
      // TODO(gbitzes): It's a shame to prefetch the whole thing just to get its
      // size.. make getNumFilesOnFs not need to load the whole thing, or at
      // least introduce an async version.
      eos::Prefetcher::prefetchFilesystemFileListAndWait(gOFS->eosView,
          gOFS->eosFsView, mFsId);
      eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);
      last_filesleft = filesleft;
      filesleft = gOFS->eosFsView->getNumFilesOnFs(mFsId);
    }

    if (!last_filesleft) {
      last_filesleft = filesleft;
    }

    if (filesleft != last_filesleft) {
      last_filesleft_change = time(NULL);
    }

    eos_static_debug(
      "stalled=%d now=%llu last_filesleft_change=%llu filesleft=%llu last_filesleft=%llu",
      stalled,
      time(NULL),
      last_filesleft_change,
      filesleft,
      last_filesleft);

    // update drain display variables
    if ((filesleft != last_filesleft) || stalled) {
      // -----------------------------------------------------------------------
      // get a rough estimate about the drain progress
      // --------------------------------------------- -------------------------
      {
        eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
        fs = 0;

        if (FsView::gFsView.mIdView.count(mFsId)) {
          fs = FsView::gFsView.mIdView[mFsId];
        }

        if (!fs) {
          eos_static_notice(
            "Filesystem fsid=%u has been removed during drain operation", mFsId);
          return 0;
        }

        fs->SetLongLong("stat.drainbytesleft",
                        fs->GetLongLong("stat.statfs.usedbytes"));
        fs->SetLongLong("stat.drainfiles",
                        filesleft);

        if (stalled) {
          fs->SetDrainStatus(eos::common::FileSystem::kDrainStalling);
        } else {
          fs->SetDrainStatus(eos::common::FileSystem::kDraining);
        }
      }
      int progress = (int)(totalfiles) ? (100.0 * (totalfiles - filesleft) /
                                          totalfiles) : 100;
      {
        eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
        fs->SetLongLong("stat.drainprogress", progress, false);

        if ((drainendtime - time(NULL)) > 0) {
          fs->SetLongLong("stat.timeleft", drainendtime - time(NULL), false);
        } else {
          fs->SetLongLong("stat.timeleft", 99999999999LL, false);
        }
      }

      if (!filesleft) {
        break;
      }
    }

    if (!filesleft) {
      break;
    }

    if (!filesleft) {
      break;
    }

    //--------------------------------------------------------------------------
    // check how long we do already draining
    //--------------------------------------------------------------------------
    drainperiod = fs->GetLongLong("drainperiod");
    drainendtime = drainstart + drainperiod;
    //--------------------------------------------------------------------------
    // set timeleft
    //--------------------------------------------------------------------------
    {
      eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
      int progress = (int)(totalfiles) ? (100.0 * (totalfiles - filesleft) /
                                          totalfiles) : 100;
      fs = 0;

      if (FsView::gFsView.mIdView.count(mFsId)) {
        fs = FsView::gFsView.mIdView[mFsId];
      }

      if (!fs) {
        eos_static_notice(
          "Filesystem fsid=%u has been removed during drain operation",
          mFsId);
        XrdSysThread::SetCancelOn();
        return 0;
      }

      fs->SetLongLong("stat.drainprogress", progress, false);

      if ((drainendtime - time(NULL)) > 0) {
        fs->SetLongLong("stat.timeleft", drainendtime - time(NULL), false);
      } else {
        fs->SetLongLong("stat.timeleft", 99999999999LL, false);
      }
    }

    if ((drainperiod) && (drainendtime < time(NULL))) {
      eos_static_notice(
        "Terminating drain operation after drainperiod of %lld seconds has been exhausted",
        drainperiod);
      //------------------------------------------------------------------------
      // set status to 'drainexpired'
      //------------------------------------------------------------------------
      {
        eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
        fs = 0;

        if (FsView::gFsView.mIdView.count(mFsId)) {
          fs = FsView::gFsView.mIdView[mFsId];
        }

        if (!fs) {
          eos_static_notice(
            "Filesystem fsid=%u has been removed during drain operation",
            mFsId);
          XrdSysThread::SetCancelOn();
          return 0;
        }

        fs->SetLongLong("stat.drainfiles", filesleft);
        fs->SetDrainStatus(eos::common::FileSystem::kDrainExpired);
        SetDrainer();

        //----------------------------------------------------------------------
        // retry logic
        //----------------------------------------------------------------------
        if (ntried < maxtry) {
          //--------------------------------------------------------------------
          // trigger retry
          //--------------------------------------------------------------------
        } else {
          XrdSysThread::SetCancelOn();
          return 0;
        }
      }
      goto retry;
    }

    XrdSysThread::SetCancelOn();

    for (int k = 0; k < 10; k++) {
      //------------------------------------------------------------------------
      // check if we should abort
      //------------------------------------------------------------------------
      XrdSysThread::CancelPoint();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    XrdSysThread::SetCancelOff();
  } while (1);

nofilestodrain:
  //----------------------------------------------------------------------------
  // set status to 'drained'
  //----------------------------------------------------------------------------
  {
    XrdSysThread::SetCancelOff();
    eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
    fs = 0;

    if (FsView::gFsView.mIdView.count(mFsId)) {
      fs = FsView::gFsView.mIdView[mFsId];
    }

    if (!fs) {
      eos_static_notice(
        "Filesystem fsid=%u has been removed during drain operation",
        mFsId);
      XrdSysThread::SetCancelOn();
      return 0;
    }

    fs->SetLongLong("stat.drainfiles", filesleft);
    fs->SetDrainStatus(eos::common::FileSystem::kDrained);
    fs->SetLongLong("stat.drainbytesleft", 0);
    fs->SetLongLong("stat.timeleft", 0);
    SetDrainer();

    if (!gOFS->Shutdown) {
      //--------------------------------------------------------------------------
      // we automatically switch this filesystem to the 'empty' state -
      // if the system is not shutting down
      //--------------------------------------------------------------------------
      fs->SetString("configstatus", "empty");
      FsView::gFsView.StoreFsConfig(fs);
      fs->SetLongLong("stat.drainprogress", 100);
    }
  }
  XrdSysThread::SetCancelOn();
  return 0;
}

EOSMGMNAMESPACE_END
