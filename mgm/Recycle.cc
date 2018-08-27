// ----------------------------------------------------------------------
// File: Recycle.cc
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

#include "common/Logging.hh"
#include "common/LayoutId.hh"
#include "common/Mapping.hh"
#include "common/RWMutex.hh"
#include "common/Path.hh"
#include "mgm/Recycle.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Quota.hh"
#include "mgm/XrdMgmOfsDirectory.hh"
#include "namespace/interface/IView.hh"
#include "namespace/Prefetcher.hh"
#include "XrdOuc/XrdOucErrInfo.hh"

// MgmOfsConfigure prepends the proc directory path e.g. the bin is
// /eos/<instance/proc/recycle/
std::string Recycle::gRecyclingPrefix = "/recycle/";
std::string Recycle::gRecyclingAttribute = "sys.recycle";
std::string Recycle::gRecyclingTimeAttribute = "sys.recycle.keeptime";
std::string Recycle::gRecyclingKeepRatio = "sys.recycle.keepratio";
std::string Recycle::gRecyclingVersionKey = "sys.recycle.version.key";
std::string Recycle::gRecyclingPostFix = ".d";
int Recycle::gRecyclingPollTime = 30;

EOSMGMNAMESPACE_BEGIN

/*----------------------------------------------------------------------------*/
bool
Recycle::Start()
{
  // run an asynchronous recyling thread
  eos_static_info("constructor");
  mThread = 0;
  XrdSysThread::Run(&mThread, Recycle::StartRecycleThread,
                    static_cast<void*>(this), XRDSYSTHREAD_HOLD,
                    "Recycle garbage collection Thread");
  return (mThread ? true : false);
}

/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
void
Recycle::Stop()
{
  // cancel the asynchronous recycle thread
  if (mThread) {
    XrdSysThread::Cancel(mThread);
    XrdSysThread::Join(mThread, 0);
  }

  mThread = 0;
}

void*
Recycle::StartRecycleThread(void* arg)
{
  return reinterpret_cast<Recycle*>(arg)->Recycler();
}

/*----------------------------------------------------------------------------*/
void*
Recycle::Recycler()
{
  //.............................................................................
  // Eternal thread doing garbage clean-up in the garbege bin
  // - default garbage directory is '<instance-proc>/recycle/'
  // - one should define an attribute like 'sys.recycle.keeptime' on this dir
  //   to define the time in seconds how long files stay in the recycle bin
  //.............................................................................
  eos::common::Mapping::VirtualIdentity rootvid;
  eos::common::Mapping::Root(rootvid);
  XrdOucErrInfo lError;
  time_t lKeepTime = 0;
  double lSpaceKeepRatio = 0;
  std::multimap<time_t, std::string> lDeletionMap;
  time_t snoozetime = 10;
  unsigned long long lLowInodesWatermark = 0;
  unsigned long long lLowSpaceWatermark = 0;
  bool show_attribute_missing = true;
  eos_static_info("msg=\"async recycling thread started\"");
  // ---------------------------------------------------------------------------
  // wait that the namespace is initialized
  // ---------------------------------------------------------------------------
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

  std::this_thread::sleep_for(std::chrono::seconds(10));

  while (1) {
    //...........................................................................
    // every now and then we wake up
    //..........................................................................
    eos_static_info("snooze-time=%llu", snoozetime);
    XrdSysThread::SetCancelOn();

    for (int i = 0; i < snoozetime / 10; i++) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      {
        XrdSysMutexHelper lock(mWakeUpMutex);

        if (mWakeUp) {
          mWakeUp = false;
          break;
        }
      }
    }

    snoozetime =
      gRecyclingPollTime; // this will be reconfigured to an appropriate value later
    XrdSysThread::SetCancelOff();
    //...........................................................................
    // read our current policy setting
    //...........................................................................
    eos::IContainerMD::XAttrMap attrmap;

    //...........................................................................
    // check if this path has a recycle attribute
    //...........................................................................
    if (gOFS->_attr_ls(Recycle::gRecyclingPrefix.c_str(), lError, rootvid, "",
                       attrmap)) {
      eos_static_err("msg=\"unable to get attribute on recycle path\" recycle-path=%s",
                     Recycle::gRecyclingPrefix.c_str());
    } else {
      if (attrmap.count(Recycle::gRecyclingKeepRatio)) {
        // One can define a space threshold which actually leaves even older
        // files in the garbage bin until the threshold is reached for
        // simplicity we apply this threshold to volume & inodes
        lSpaceKeepRatio = strtod(attrmap[Recycle::gRecyclingKeepRatio].c_str(), 0);
        // Get group statistics for space and project id
        auto map_quotas = Quota::GetGroupStatistics(Recycle::gRecyclingPrefix,
                          Quota::gProjectId);

        if (!map_quotas.empty()) {
          unsigned long long usedbytes = map_quotas[SpaceQuota::kGroupBytesIs];
          unsigned long long maxbytes = map_quotas[SpaceQuota::kGroupBytesTarget];
          unsigned long long usedfiles = map_quotas[SpaceQuota::kGroupFilesIs];
          unsigned long long maxfiles = map_quotas[SpaceQuota::kGroupFilesTarget];

          if ((lSpaceKeepRatio > (1.0 * usedbytes / (maxbytes ? maxbytes : 999999999))) &&
              (lSpaceKeepRatio > (1.0 * usedfiles / (maxfiles ? maxfiles : 999999999)))) {
            eos_static_debug("msg=\"skipping recycle clean-up - ratio still low\" "
                             "ratio=%.02f space-ratio=%.02f inode-ratio=%.02f",
                             lSpaceKeepRatio,
                             1.0 * usedbytes / (maxbytes ? maxbytes : 999999999),
                             1.0 * usedfiles / (maxfiles ? maxfiles : 999999999));
            continue;
          }

          if ((lSpaceKeepRatio - 0.1) > 0) {
            lSpaceKeepRatio -= 0.1;
          }

          lLowInodesWatermark = (maxfiles * lSpaceKeepRatio);
          lLowSpaceWatermark = (maxbytes * lSpaceKeepRatio);
          eos_static_info("msg=\"cleaning by ratio policy\" low-inodes-mark=%lld "
                          "low-space-mark=%lld mark=%.02f", lLowInodesWatermark,
                          lLowSpaceWatermark, lSpaceKeepRatio);
        }
      }

      if (attrmap.count(Recycle::gRecyclingTimeAttribute)) {
        lKeepTime = strtoull(attrmap[Recycle::gRecyclingTimeAttribute].c_str(), 0, 10);
        eos_static_info("keep-time=%llu deletion-map=%llu", lKeepTime,
                        lDeletionMap.size());

        if (lKeepTime > 0) {
          if (!lDeletionMap.size()) {
            //...................................................................
            //  the deletion map is filled if there is nothing inside with files/
            //  directories found previously in the garbage bin
            //...................................................................
            // the old reyccle bin gid/uid/<contracted>
            {
              std::string subdirs;
              XrdMgmOfsDirectory dirl1;
              XrdMgmOfsDirectory dirl2;
              XrdMgmOfsDirectory dirl3;
              int listrc = dirl1.open(Recycle::gRecyclingPrefix.c_str(), rootvid,
                                      (const char*) 0);

              if (listrc) {
                eos_static_err("msg=\"unable to list the garbage directory level-1\" recycle-path=%s",
                               Recycle::gRecyclingPrefix.c_str());
              } else {
                // loop over all directories = group directories
                const char* dname1;

                while ((dname1 = dirl1.nextEntry())) {
                  {
                    std::string sdname = dname1;

                    if ((sdname == ".") || (sdname == "..")) {
                      continue;
                    }
                  }
                  std::string l2 = Recycle::gRecyclingPrefix;
                  l2 += dname1;
                  // list level-2 user directories
                  listrc = dirl2.open(l2.c_str(), rootvid, (const char*) 0);

                  if (listrc) {
                    eos_static_err("msg=\"unable to list the garbage directory level-2\" recycle-path=%s l2-path=%s",
                                   Recycle::gRecyclingPrefix.c_str(), l2.c_str());
                  } else {
                    const char* dname2;

                    while ((dname2 = dirl2.nextEntry())) {
                      {
                        std::string sdname = dname2;

                        if ((sdname == ".") || (sdname == "..")) {
                          continue;
                        }
                      }
                      std::string l3 = l2;
                      l3 += "/";
                      l3 += dname2;
                      // list the level-3 entries
                      listrc = dirl3.open(l3.c_str(), rootvid, (const char*) 0);

                      if (listrc) {
                        eos_static_err("msg=\"unable to list the garbage directory level-2\" recycle-path=%s l2-path=%s l3-path=%s",
                                       Recycle::gRecyclingPrefix.c_str(), l2.c_str(), l3.c_str());
                      } else {
                        const char* dname3;

                        while ((dname3 = dirl3.nextEntry())) {
                          {
                            std::string sdname = dname3;

                            if ((sdname == ".") || (sdname == "..")) {
                              continue;
                            }
                          }
                          std::string l4 = l3;
                          l4 += "/";
                          l4 += dname3;
                          eos_static_debug("path=%s", l4.c_str());
                          // Stat the directory to get the mtime
                          struct stat buf;

                          if (gOFS->_stat(l4.c_str(), &buf, lError, rootvid, "", 0, false)) {
                            eos_static_err("msg=\"unable to stat a garbage directory entry\" "
                                           "recycle-path=%s l2-path=%s l3-path=%s",
                                           Recycle::gRecyclingPrefix.c_str(), l2.c_str(), l3.c_str());
                          } else {
                            // Add to the garbage fifo deletion multimap
                            lDeletionMap.insert(std::pair<time_t, std::string > (buf.st_ctime, l4));
                          }
                        }

                        dirl3.close();
                      }
                    }

                    dirl2.close();
                  }
                }

                dirl1.close();
              }

              // the new recycle bin
              {
                std::map<std::string, std::set < std::string>> findmap;
                char sdir[4096];
                snprintf(sdir, sizeof(sdir) - 1, "%s/", Recycle::gRecyclingPrefix.c_str());
                XrdOucErrInfo lError;
                XrdOucString stdErr;
                int depth = 6;
                (void) gOFS->_find(sdir, lError, stdErr, rootvid, findmap,
                                   0, 0, false, 0, true, depth);

                for (auto dirit = findmap.begin(); dirit != findmap.end(); ++dirit) {
                  XrdOucString dirname = dirit->first.c_str();

                  if (dirname.endswith(".d/")) {
                    dirname.erase(dirname.length() - 1);
                    eos::common::Path cpath(dirname.c_str());
                    dirname = cpath.GetParentPath();
                    dirit->second.insert(cpath.GetName());
                  }

                  eos_static_debug("dir=%s", dirit->first.c_str());

                  for (auto fileit = dirit->second.begin(); fileit != dirit->second.end();
                       ++fileit) {
                    std::string fullpath = dirname.c_str();
                    fullpath += *fileit;
                    XrdOucString originode;
                    XrdOucString origpath = fileit->c_str();
                    eos_static_debug("path=%s", fileit->c_str());

                    if ((origpath != "/") && !origpath.beginswith("#")) {
                      continue;
                    }

                    struct stat buf;

                    if (gOFS->_stat(fullpath.c_str(), &buf, lError, rootvid, "", 0, false)) {
                      eos_static_err("msg=\"unable to stat a garbage directory entry\" "
                                     "recycle-path=%s path=%s",
                                     Recycle::gRecyclingPrefix.c_str(), fullpath.c_str());
                    } else {
                      // Add to the garbage fifo deletion multimap
                      lDeletionMap.insert(std::pair<time_t, std::string > (buf.st_ctime,
                                          fullpath.c_str()));
                      eos_static_debug("new-bin: adding to deletionmap : %s", fullpath.c_str());
                    }
                  }
                }
              }
            }
          } else {
            snoozetime = 0; // this will be redefined by the oldest entry time
            auto it = lDeletionMap.begin();
            time_t now = time(NULL);

            while (it != lDeletionMap.end()) {
              // take the first element and see if it is exceeding the keep time
              if ((it->first + lKeepTime) < now) {
                // This entry can be removed
                // If there is a keep-ratio policy defined we abort deletion once
                // we are enough under the thresholds
                if (attrmap.count(Recycle::gRecyclingKeepRatio)) {
                  auto map_quotas = Quota::GetGroupStatistics(Recycle::gRecyclingPrefix,
                                    Quota::gProjectId);

                  if (!map_quotas.empty()) {
                    unsigned long long usedbytes = map_quotas[SpaceQuota::kGroupBytesIs];
                    unsigned long long usedfiles = map_quotas[SpaceQuota::kGroupFilesIs];
                    eos_static_debug("low-volume=%lld is-volume=%lld low-inodes=%lld is-inodes=%lld",
                                     usedfiles,
                                     lLowInodesWatermark,
                                     usedbytes,
                                     lLowSpaceWatermark);

                    if ((lLowInodesWatermark >= usedfiles) &&
                        (lLowSpaceWatermark >= usedbytes)) {
                      eos_static_debug("msg=\"skipping recycle clean-up - ratio went under low watermarks\"");
                      break; // leave the deletion loop
                    }
                  }
                }

                XrdOucString delpath = it->second.c_str();

                if ((it->second.length()) &&
                    (delpath.endswith(Recycle::gRecyclingPostFix.c_str()))) {
                  //.............................................................
                  // do a directory deletion - first find all subtree children
                  //.............................................................
                  std::map<std::string, std::set<std::string> > found;
                  std::map<std::string, std::set<std::string> >::const_reverse_iterator rfoundit;
                  std::set<std::string>::const_iterator fileit;
                  XrdOucString stdErr;

                  if (gOFS->_find(it->second.c_str(), lError, stdErr, rootvid, found)) {
                    eos_static_err("msg=\"unable to do a find in subtree\" path=%s stderr=\"%s\"",
                                   it->second.c_str(), stdErr.c_str());
                  } else {
                    //...........................................................
                    // standard way to delete files recursively
                    //...........................................................
                    // delete files starting at the deepest level
                    for (rfoundit = found.rbegin(); rfoundit != found.rend(); rfoundit++) {
                      for (fileit = rfoundit->second.begin(); fileit != rfoundit->second.end();
                           fileit++) {
                        std::string fspath = rfoundit->first;
                        std::string fname = *fileit;
                        size_t lpos;

                        if ((lpos = fname.find(" -> ")) != std::string::npos) {
                          // rewrite link name
                          fname.erase(lpos);
                        }

                        fspath += fname;

                        if (gOFS->_rem(fspath.c_str(), lError, rootvid, (const char*) 0)) {
                          eos_static_err("msg=\"unable to remove file\" path=%s", fspath.c_str());
                        } else {
                          eos_static_info("msg=\"permanently deleted file from recycle bin\" path=%s keep-time=%llu",
                                          fspath.c_str(), lKeepTime);
                        }
                      }
                    }

                    //...........................................................
                    // delete directories starting at the deepest level
                    //...........................................................
                    for (rfoundit = found.rbegin(); rfoundit != found.rend(); rfoundit++) {
                      //.........................................................
                      // don't even try to delete the root directory
                      //.........................................................
                      std::string fspath = rfoundit->first.c_str();

                      if (fspath == "/") {
                        continue;
                      }

                      if (gOFS->_remdir(rfoundit->first.c_str(), lError, rootvid, (const char*) 0)) {
                        eos_static_err("msg=\"unable to remove directory\" path=%s", fspath.c_str());
                      } else {
                        eos_static_info("msg=\"permanently deleted directory from recycle bin\" path=%s keep-time=%llu",
                                        fspath.c_str(), lKeepTime);
                      }
                    }
                  }

                  lDeletionMap.erase(it);
                  it = lDeletionMap.begin();
                } else {
                  //...........................................................
                  // do a single file deletion
                  //...........................................................
                  if (gOFS->_rem(it->second.c_str(), lError, rootvid, (const char*) 0)) {
                    eos_static_err("msg=\"unable to remove file\" path=%s", it->second.c_str());
                  }

                  lDeletionMap.erase(it);
                  it = lDeletionMap.begin();
                }
              } else {
                // This entry has still to be kept
                eos_static_info("oldest entry: %lld sec to deletion",
                                it->first + lKeepTime - now);

                if (!snoozetime) {
                  // define the sleep period from the oldest entry
                  snoozetime = it->first + lKeepTime - now;

                  if (snoozetime < gRecyclingPollTime) {
                    // avoid to activate this thread too many times, 5 minutes
                    // resolution is perfectly fine
                    snoozetime = gRecyclingPollTime;
                  }

                  if (snoozetime > lKeepTime) {
                    eos_static_warning("msg=\"snooze time exceeds keeptime\" snooze-time=%llu keep-time=%llu",
                                       snoozetime, lKeepTime);
                    // That is sort of strange but let's have a fix for that
                    snoozetime = lKeepTime;
                  }
                }

                // we can leave the loop because all other entries don't match anymore the time constraint
                break;
              }
            }

            if (!snoozetime) {
              snoozetime = gRecyclingPollTime;
            }
          }
        } else {
          eos_static_warning("msg=\"parsed '%s' attribute as keep-time of %llu seconds - ignoring!\" recycle-path=%s",
                             Recycle::gRecyclingTimeAttribute.c_str(), Recycle::gRecyclingPrefix.c_str());
        }
      } else {
        if (show_attribute_missing) {
          eos_static_warning("msg=\"unable to read '%s' attribute on recycle path - undefined!\" recycle-path=%s",
                             Recycle::gRecyclingTimeAttribute.c_str(), Recycle::gRecyclingPrefix.c_str());
          show_attribute_missing = false;
        }
      }
    }
  };

  return 0;
}

/*----------------------------------------------------------------------------*/
int
Recycle::ToGarbage(const char* epname, XrdOucErrInfo& error)
{
  eos::common::Mapping::VirtualIdentity rootvid;
  eos::common::Mapping::Root(rootvid);
  char srecyclepath[4096];
  // If path ends with '/' we recycle a full directory tree aka directory
  bool isdir = false;
  // rewrite the file name /a/b/c as #:#a#:#b#:#c
  XrdOucString contractedpath = mPath.c_str();

  if (contractedpath.endswith("/")) {
    isdir = true;
    mPath.erase(mPath.length() - 1);
    // remove the '/' indicating a recursive directory recycling
    contractedpath.erase(contractedpath.length() - 1);
  }

  if (mRecycleDir.length() > 1) {
    if (mRecycleDir[mRecycleDir.length() - 1] == '/') {
      mRecycleDir.erase(mRecycleDir.length() - 1);
    }
  }

  while (contractedpath.replace("/", "#:#")) {
  }

  // For dir's we add a '.d' in the end of the recycle path
  std::string lPostFix = "";

  if (isdir) {
    lPostFix = Recycle::gRecyclingPostFix;
  }

  std::string rpath;
  int rc = 0;

  // retrieve the current valid index directory
  if ((rc = GetRecyclePrefix(epname, error, rpath))) {
    return rc;
  }

  snprintf(srecyclepath, sizeof(srecyclepath) - 1, "%s/%s.%016llx%s",
           rpath.c_str(),
           contractedpath.c_str(),
           mId, lPostFix.c_str());
  mRecyclePath = srecyclepath;

  // Finally do the rename
  if (gOFS->_rename(mPath.c_str(), srecyclepath, error, rootvid, "", "", true,
                    true, false)) {
    return gOFS->Emsg(epname, error, EIO, "rename file/directory", srecyclepath);
  }

  // store the recycle path in the error object
  error.setErrInfo(0, srecyclepath);
  return SFS_OK;
}

/*----------------------------------------------------------------------------*/
void
Recycle::Print(XrdOucString& stdOut, XrdOucString& stdErr,
               eos::common::Mapping::VirtualIdentity_t& vid, bool monitoring,
               bool translateids, bool details,
               std::string date,
               bool global
              )
{
  XrdOucString uids;
  XrdOucString gids;
  std::map<uid_t, bool> printmap;
  eos::common::Mapping::VirtualIdentity rootvid;
  eos::common::Mapping::Root(rootvid);

  if (global && ((!vid.uid) ||
                 (eos::common::Mapping::HasUid(3, vid.uid_list)) ||
                 (eos::common::Mapping::HasGid(4, vid.gid_list)))) {
    // add everything found in the recycle directory structure to the printmap
    std::string subdirs;
    XrdMgmOfsDirectory dirl;
    int listrc = dirl.open(Recycle::gRecyclingPrefix.c_str(), rootvid,
                           (const char*) 0);

    if (listrc) {
      eos_static_err("msg=\"unable to list the garbage directory level-1\" recycle-path=%s",
                     Recycle::gRecyclingPrefix.c_str());
    } else {
      // loop over all directories = group directories
      const char* dname1;

      while ((dname1 = dirl.nextEntry())) {
        std::string sdname = dname1;

        if ((sdname == ".") || (sdname == "..")) {
          continue;
        }

        if (sdname.substr(0, 4) == "uid:") {
          uid_t uid = std::stoull(sdname.substr(4));
          printmap[uid] = true;
        }
      }

      dirl.close();
    }
  } else {
    // add only the virtual user to the printmap
    printmap[vid.uid] = true;
  }

  eos::common::Path dPath(std::string("/") + date);

  if (details) {
    size_t count = 0;

    for (auto ituid = printmap.begin(); ituid != printmap.end(); ituid++) {
      std::map<std::string, std::set < std::string>> findmap;
      char sdir[4096];
      snprintf(sdir, sizeof(sdir) - 1, "%s/uid:%u/%s",
               Recycle::gRecyclingPrefix.c_str(),
               (unsigned int) ituid->first, date.c_str());
      XrdOucErrInfo lError;
      int depth = 5 ;

      if (dPath.GetSubPathSize()) {
        if (depth > (int) dPath.GetSubPathSize()) {
          depth -= dPath.GetSubPathSize();
        }
      }

      int retc = gOFS->_find(sdir, lError, stdErr, rootvid,
                             findmap,
                             0, 0, false, 0, true, depth);

      if (retc && errno != ENOENT) {
        eos_static_err("find command failed in dir='%s'", sdir);
      }

      for (auto dirit = findmap.begin(); dirit != findmap.end(); ++dirit) {
        XrdOucString dirname = dirit->first.c_str();

        if (dirname.endswith(".d/")) {
          dirname.erase(dirname.length() - 1);
          eos::common::Path cpath(dirname.c_str());
          dirname = cpath.GetParentPath();
          dirit->second.insert(cpath.GetName());
        }

        eos_static_debug("dir=%s", dirit->first.c_str());

        for (auto fileit = dirit->second.begin(); fileit != dirit->second.end();
             ++fileit) {
          std::string fullpath = dirname.c_str();
          fullpath += *fileit;
          XrdOucString originode;
          XrdOucString origpath = fileit->c_str();
          eos_static_debug("file=%s", fileit->c_str());

          if ((origpath != "/") && !origpath.beginswith("#")) {
            continue;
          }

          // demangle the original pathname
          while (origpath.replace("#:#", "/")) {
          }

          XrdOucString type = "file";
          struct stat buf;
          XrdOucErrInfo error;

          if (!gOFS->_stat(fullpath.c_str(), &buf, error, vid, "")) {
            if (translateids) {
              int errc = 0;
              uids = eos::common::Mapping::UidToUserName(buf.st_uid, errc).c_str();

              if (errc) {
                uids = eos::common::Mapping::UidAsString(buf.st_uid).c_str();
              }

              gids = eos::common::Mapping::GidToGroupName(buf.st_gid, errc).c_str();

              if (errc) {
                gids = eos::common::Mapping::GidAsString(buf.st_gid).c_str();
              }
            } else {
              uids = eos::common::Mapping::UidAsString(buf.st_uid).c_str();
              gids = eos::common::Mapping::GidAsString(buf.st_gid).c_str();
            }

            if (origpath.endswith(Recycle::gRecyclingPostFix.c_str())) {
              type = "recursive-dir";
              origpath.erase(origpath.length() - Recycle::gRecyclingPostFix.length());
            }

            originode = origpath;
            originode.erase(0, origpath.length() - 16);
            origpath.erase(origpath.length() - 17);

            if (monitoring) {
              XrdOucString sizestring;
              stdOut += "recycle=ls ";
              stdOut += " recycle-bin=";
              stdOut += Recycle::gRecyclingPrefix.c_str();
              stdOut += " uid=";
              stdOut += uids.c_str();
              stdOut += " gid=";
              stdOut += gids.c_str();
              stdOut += " size=";
              stdOut += eos::common::StringConversion::GetSizeString(sizestring,
                        (unsigned long long) buf.st_size);
              stdOut += " deletion-time=";
              char deltime[256];
              snprintf(deltime, sizeof(deltime) - 1, "%llu",
                       (unsigned long long) buf.st_ctime);
              stdOut += deltime;
              stdOut += " type=";
              stdOut += type.c_str();
              stdOut += " keylength.restore-path=";
              stdOut += (int) origpath.length();
              stdOut += " restore-path=";
              stdOut += origpath.c_str();
              stdOut += " restore-key=";
              stdOut += originode.c_str();
              stdOut += "\n";
            } else {
              char sline[4096];
              XrdOucString sizestring;

              if (count == 0) {
                // print a header
                snprintf(sline, sizeof(sline) - 1,
                         "# %-24s %-8s %-8s %-12s %-13s %-16s %-64s\n", "Deletion Time", "UID", "GID",
                         "SIZE", "TYPE", "RESTORE-KEY", "RESTORE-PATH");
                stdOut += sline;
                stdOut += "# ==============================================================================================================================\n";
              }

              char tdeltime[4096];
              std::string deltime = ctime_r(&buf.st_ctime, tdeltime);
              deltime.erase(deltime.length() - 1);
              snprintf(sline, sizeof(sline) - 1, "%-26s %-8s %-8s %-12s %-13s %-16s %-64s",
                       deltime.c_str(), uids.c_str(), gids.c_str(),
                       eos::common::StringConversion::GetSizeString(sizestring,
                           (unsigned long long) buf.st_size), type.c_str(), originode.c_str(),
                       origpath.c_str());

              if (stdOut.length() > 1 * 1024 * 1024 * 1024) {
                stdOut += "... (truncated after 1G of output)\n";
                retc = E2BIG;
                stdErr += "warning: list too long - truncated after 1GB of output!\n";
                return;
              }

              stdOut += sline;
              stdOut += "\n";
            }

            count++;

            if ((vid.uid) && (!vid.sudoer) && (count > 100000)) {
              stdOut += "... (truncated)\n";
              retc = E2BIG;
              stdErr += "warning: list too long - truncated after 100000 entries!\n";
              return;
            }
          }
        }
      }
    }
  } else {
    auto map_quotas = Quota::GetGroupStatistics(Recycle::gRecyclingPrefix,
                      Quota::gProjectId);

    if (!map_quotas.empty()) {
      unsigned long long usedbytes = map_quotas[SpaceQuota::kGroupBytesIs];
      unsigned long long maxbytes = map_quotas[SpaceQuota::kGroupBytesTarget];
      unsigned long long usedfiles = map_quotas[SpaceQuota::kGroupFilesIs];
      unsigned long long maxfiles = map_quotas[SpaceQuota::kGroupFilesTarget];
      char sline[1024];
      XrdOucString sizestring1;
      XrdOucString sizestring2;
      eos::IContainerMD::XAttrMap attrmap;
      XrdOucErrInfo error;

      //...........................................................................
      // check if this path has a recycle attribute
      //...........................................................................
      if (gOFS->_attr_ls(Recycle::gRecyclingPrefix.c_str(), error, rootvid, "",
                         attrmap)) {
        eos_static_err("msg=\"unable to get attribute on recycle path\" "
                       "recycle-path=%s", Recycle::gRecyclingPrefix.c_str());
      }

      if (!monitoring) {
        stdOut += "# _________________________________________________________"
                  "__________________________________________________________________\n";
        snprintf(sline, sizeof(sline) - 1, "# used %s out of %s (%.02f%% volume "
                 "/ %.02f%% inodes used) Object-Lifetime %s [s] Keep-Ratio %s",
                 eos::common::StringConversion::GetReadableSizeString(sizestring1, usedbytes,
                     "B"),
                 eos::common::StringConversion::GetReadableSizeString(sizestring2, maxbytes,
                     "B"),
                 usedbytes * 100.0 / maxbytes,
                 usedfiles * 100.0 / maxfiles,
                 attrmap.count(Recycle::gRecyclingTimeAttribute) ?
                 attrmap[Recycle::gRecyclingTimeAttribute].c_str() : "not configured",
                 attrmap.count(Recycle::gRecyclingKeepRatio) ?
                 attrmap[Recycle::gRecyclingKeepRatio].c_str() : "not configured");
        stdOut += sline;
        stdOut += "\n";
        stdOut += "# _________________________________________________________"
                  "__________________________________________________________________\n";
      } else {
        snprintf(sline, sizeof(sline) - 1, "recycle-bin=%s usedbytes=%s "
                 "maxbytes=%s volumeusage=%.02f%% inodeusage=%.02f%% lifetime=%s ratio=%s",
                 Recycle::gRecyclingPrefix.c_str(),
                 eos::common::StringConversion::GetSizeString(sizestring1, usedbytes),
                 eos::common::StringConversion::GetSizeString(sizestring2, maxbytes),
                 usedbytes * 100.0 / maxbytes,
                 usedfiles * 100.0 / maxfiles,
                 attrmap.count(Recycle::gRecyclingTimeAttribute) ?
                 attrmap[Recycle::gRecyclingTimeAttribute].c_str() : "-1",
                 attrmap.count(Recycle::gRecyclingKeepRatio) ?
                 attrmap[Recycle::gRecyclingKeepRatio].c_str() : "-1");
        stdOut += sline;
        stdOut += "\n";
      }
    }
  }
}

/*----------------------------------------------------------------------------*/
void
Recycle::PrintOld(XrdOucString& stdOut, XrdOucString& stdErr,
                  eos::common::Mapping::VirtualIdentity_t& vid, bool monitoring,
                  bool translateids, bool details)
{
  XrdOucString uids;
  XrdOucString gids;
  std::map<gid_t, std::map<uid_t, bool> > printmap;
  eos::common::Mapping::VirtualIdentity rootvid;
  eos::common::Mapping::Root(rootvid);

  if ((!vid.uid) ||
      (eos::common::Mapping::HasUid(3, vid.uid_list)) ||
      (eos::common::Mapping::HasGid(4, vid.gid_list))) {
    // add everything found in the recycle directory structure to the printmap
    std::string subdirs;
    XrdMgmOfsDirectory dirl1;
    XrdMgmOfsDirectory dirl2;
    int listrc = dirl1.open(Recycle::gRecyclingPrefix.c_str(), rootvid,
                            (const char*) 0);

    if (listrc) {
      eos_static_err("msg=\"unable to list the garbage directory level-1\" recycle-path=%s",
                     Recycle::gRecyclingPrefix.c_str());
    } else {
      // loop over all directories = group directories
      const char* dname1;

      while ((dname1 = dirl1.nextEntry())) {
        std::string sdname = dname1;

        if ((sdname == ".") || (sdname == "..")) {
          continue;
        }

        if (sdname.substr(0, 4) == "uid:") {
          continue;
        }

        gid_t gid = strtoull(dname1, 0, 10);
        std::string l2 = Recycle::gRecyclingPrefix;
        l2 += dname1;
        // list level-2 user directories
        listrc = dirl2.open(l2.c_str(), rootvid, (const char*) 0);

        if (listrc) {
          eos_static_err("msg=\"unable to list the garbage directory level-2\" recycle-path=%s l2-path=%s",
                         Recycle::gRecyclingPrefix.c_str(), l2.c_str());
        } else {
          const char* dname2;

          while ((dname2 = dirl2.nextEntry())) {
            std::string sdname = dname2;

            if ((sdname == ".") || (sdname == "..")) {
              continue;
            }

            uid_t uid = strtoull(dname2, 0, 10);
            printmap[gid][uid] = true;
          }

          dirl2.close();
        }
      }

      dirl1.close();
    }
  } else {
    // add only the virtual user to the printmap
    printmap[vid.gid][vid.uid] = true;
  }

  if (details) {
    size_t count = 0;

    for (auto itgid = printmap.begin(); itgid != printmap.end(); itgid++) {
      for (auto ituid = itgid->second.begin(); ituid != itgid->second.end();
           ituid++) {
        XrdMgmOfsDirectory dirl;
        char sdir[4096];
        snprintf(sdir, sizeof(sdir) - 1, "%s/%u/%u/", Recycle::gRecyclingPrefix.c_str(),
                 (unsigned int) itgid->first, (unsigned int) ituid->first);
        int retc = dirl.open(sdir, vid, "");

        if (!retc) {
          const char* dname;

          while ((dname = dirl.nextEntry())) {
            std::string sdname = dname;

            if ((sdname == ".") || (sdname == "..")) {
              continue;
            }

            std::string fullpath = sdir;
            fullpath += dname;
            XrdOucString originode;
            XrdOucString origpath = dname;

            // demangle the original pathname
            while (origpath.replace("#:#", "/")) {
            }

            XrdOucString type = "file";
            struct stat buf;
            XrdOucErrInfo error;

            if (!gOFS->_stat(fullpath.c_str(), &buf, error, vid, "")) {
              if (translateids) {
                int errc = 0;
                uids = eos::common::Mapping::UidToUserName(buf.st_uid, errc).c_str();

                if (errc) {
                  uids = eos::common::Mapping::UidAsString(buf.st_uid).c_str();
                }

                gids = eos::common::Mapping::GidToGroupName(buf.st_gid, errc).c_str();

                if (errc) {
                  gids = eos::common::Mapping::GidAsString(buf.st_gid).c_str();
                }
              } else {
                uids = eos::common::Mapping::UidAsString(buf.st_uid).c_str();
                gids = eos::common::Mapping::GidAsString(buf.st_gid).c_str();
              }

              if (origpath.endswith(Recycle::gRecyclingPostFix.c_str())) {
                type = "recursive-dir";
                origpath.erase(origpath.length() - Recycle::gRecyclingPostFix.length());
              }

              originode = origpath;
              originode.erase(0, origpath.length() - 16);
              origpath.erase(origpath.length() - 17);

              if (monitoring) {
                XrdOucString sizestring;
                stdOut += "recycle=ls ";
                stdOut += " recycle-bin=";
                stdOut += Recycle::gRecyclingPrefix.c_str();
                stdOut += " uid=";
                stdOut += uids.c_str();
                stdOut += " gid=";
                stdOut += gids.c_str();
                stdOut += " size=";
                stdOut += eos::common::StringConversion::GetSizeString(sizestring,
                          (unsigned long long) buf.st_size);
                stdOut += " deletion-time=";
                char deltime[256];
                snprintf(deltime, sizeof(deltime) - 1, "%llu",
                         (unsigned long long) buf.st_ctime);
                stdOut += deltime;
                stdOut += " type=";
                stdOut += type.c_str();
                stdOut += " keylength.restore-path=";
                stdOut += (int) origpath.length();
                stdOut += " restore-path=";
                stdOut += origpath.c_str();
                stdOut += " restore-key=";
                stdOut += originode.c_str();
                stdOut += "\n";
              } else {
                char sline[4096];
                XrdOucString sizestring;

                if (count == 0) {
                  // print a header
                  snprintf(sline, sizeof(sline) - 1,
                           "# %-24s %-8s %-8s %-12s %-13s %-16s %-64s\n", "Deletion Time", "UID", "GID",
                           "SIZE", "TYPE", "RESTORE-KEY", "RESTORE-PATH");
                  stdOut += sline;
                  stdOut += "# ==============================================================================================================================\n";
                }

                char tdeltime[4096];
                std::string deltime = ctime_r(&buf.st_ctime, tdeltime);
                deltime.erase(deltime.length() - 1);
                snprintf(sline, sizeof(sline) - 1, "%-26s %-8s %-8s %-12s %-13s %-16s %-64s",
                         deltime.c_str(), uids.c_str(), gids.c_str(),
                         eos::common::StringConversion::GetSizeString(sizestring,
                             (unsigned long long) buf.st_size), type.c_str(), originode.c_str(),
                         origpath.c_str());

                if (stdOut.length() > 1 * 1024 * 1024 * 1024) {
                  stdOut += "... (truncated after 1G of output)\n";
                  retc = E2BIG;
                  stdErr += "warning: list too long - truncated after 1GB of output!\n";
                  return;
                }

                stdOut += sline;
                stdOut += "\n";
              }

              count++;

              if ((vid.uid) && (!vid.sudoer) && (count > 100000)) {
                stdOut += "... (truncated)\n";
                retc = E2BIG;
                stdErr += "warning: list too long - truncated after 100000 entries!\n";
                return;
              }
            }
          }
        }
      }
    }
  }
}

/*----------------------------------------------------------------------------*/
int
Recycle::Restore(XrdOucString& stdOut, XrdOucString& stdErr,
                 eos::common::Mapping::VirtualIdentity_t& vid, const char* key,
                 XrdOucString& option)
{
  eos::common::Mapping::VirtualIdentity rootvid;
  eos::common::Mapping::Root(rootvid);

  if (!key) {
    stdErr += "error: invalid argument as recycle key\n";
    return EINVAL;
  }

  XrdOucString skey = key;
  bool force_file = false;
  bool force_directory = false;

  if (skey.beginswith("fxid:")) {
    skey.erase(0, 5);
    force_file = true;
  }

  if (skey.beginswith("pxid:")) {
    skey.erase(0, 5);
    force_directory = true;
  }

  unsigned long long fid = strtoull(skey.c_str(), 0, 16);
  // convert the hex inode number into decimal and retrieve path name
  std::shared_ptr<eos::IFileMD> fmd;
  std::shared_ptr<eos::IContainerMD> cmd;
  std::string recyclepath;
  XrdOucString repath;
  XrdOucString rprefix = Recycle::gRecyclingPrefix.c_str();
  rprefix += "/";
  rprefix += (int) vid.gid;
  rprefix += "/";
  rprefix += (int) vid.uid;
  XrdOucString newrprefix = Recycle::gRecyclingPrefix.c_str();
  newrprefix += "/uid:";
  newrprefix += (int) vid.uid;

  while (rprefix.replace("//", "/")) {
  }

  while (newrprefix.replace("//", "/")) {
  }

  {
    // TODO(gbitzes): This could be more precise...
    eos::Prefetcher::prefetchFileMDWithParentsAndWait(gOFS->eosView, fid);
    eos::Prefetcher::prefetchContainerMDWithParentsAndWait(gOFS->eosView, fid);
    eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);

    if (!force_directory) {
      try {
        fmd = gOFS->eosFileService->getFileMD(fid);
        recyclepath = gOFS->eosView->getUri(fmd.get());
        repath = recyclepath.c_str();

        if (!repath.beginswith(rprefix.c_str()) &&
            !repath.beginswith(newrprefix.c_str())) {
          stdErr = "error: this is not a file in your recycle bin - try to prefix the key with pxid:<key>\n";
          return EPERM;
        }
      } catch (eos::MDException& e) {
      }
    }

    if (!force_file && !fmd) {
      try {
        cmd = gOFS->eosDirectoryService->getContainerMD(fid);
        recyclepath = gOFS->eosView->getUri(cmd.get());
        repath = recyclepath.c_str();

        if (!repath.beginswith(rprefix.c_str()) &&
            !repath.beginswith(newrprefix.c_str())) {
          stdErr = "error: this is not a directory in your recycle bin\n";
          return EPERM;
        }
      } catch (eos::MDException& e) {
      }
    }

    if (!recyclepath.length()) {
      stdErr = "error: cannot find object referenced by recycle-key=";
      stdErr += key;
      return ENOENT;
    }
  }

  // reconstruct original file name
  eos::common::Path cPath(recyclepath.c_str());
  XrdOucString originalpath = cPath.GetName();

  // Demangle path
  while (originalpath.replace("#:#", "/")) {
  }

  if (originalpath.endswith(Recycle::gRecyclingPostFix.c_str())) {
    originalpath.erase(originalpath.length() - Recycle::gRecyclingPostFix.length() -
                       16 - 1);
  } else {
    originalpath.erase(originalpath.length() - 16 - 1);
  }

  // Check that this is a path to recycle
  if (!repath.beginswith(Recycle::gRecyclingPrefix.c_str())) {
    stdErr = "error: referenced object cannot be recycled\n";
    return EINVAL;
  }

  eos::common::Path oPath(originalpath.c_str());
  // Check if the client is the owner of the object to recycle
  struct stat buf;
  XrdOucErrInfo lError;

  if (gOFS->_stat(cPath.GetPath(), &buf, lError, rootvid, "")) {
    stdErr += "error: unable to stat path to be recycled\n";
    return EIO;
  }

  // check that the client is the owner of that object
  if (vid.uid != buf.st_uid) {
    stdErr += "error: to recycle this file you have to have the role of the file owner: uid=";
    stdErr += (int) buf.st_uid;
    stdErr += "\n";
    return EPERM;
  }

  // check if original parent path exists
  if (gOFS->_stat(oPath.GetParentPath(), &buf, lError, rootvid, "")) {
    stdErr = "error: you have to recreate the restore directory path=";
    stdErr += oPath.GetParentPath();
    stdErr += " to be able to restore this file/tree\n";
    stdErr += "hint: retry after creating the mentioned directory\n";
    return ENOENT;
  }

  // check if original path is existing
  if (!gOFS->_stat(oPath.GetPath(), &buf, lError, rootvid, "")) {
    if ((option.find("--force-original-name") == STR_NPOS) &&
        (option.find("-f") == STR_NPOS)) {
      stdErr += "error: the original path is already existing - use '--force-original-name' "
                "or '-f' to put the deleted file/tree back and rename the file/tree in place to <name>.<inode>\n";
      return EEXIST;
    } else {
      std::string newold = oPath.GetPath();
      char sp[256];
      snprintf(sp, sizeof(sp) - 1, "%016llx",
               (unsigned long long)(S_ISDIR(buf.st_mode) ? buf.st_ino :
                                    eos::common::FileId::InodeToFid(buf.st_ino)));
      newold += ".";
      newold += sp;

      if (gOFS->_rename(oPath.GetPath(), newold.c_str(), lError, rootvid, "", "",
                        true, true)) {
        stdErr += "error: failed to rename the existing file/tree where we need to restore path=";
        stdErr += oPath.GetPath();
        stdErr += "\n";
        stdErr += lError.getErrText();
        return EIO;
      } else {
        stdOut += "warning: renamed restore path=";
        stdOut += oPath.GetPath();
        stdOut += " to backup-path=";
        stdOut += newold.c_str();
        stdOut += "\n";
      }
    }
  }

  // do the 'undelete' aka rename
  if (gOFS->_rename(cPath.GetPath(), oPath.GetPath(), lError, rootvid, "", "",
                    true)) {
    stdErr += "error: failed to undelete path=";
    stdErr += oPath.GetPath();
    stdErr += "\n";
    return EIO;
  } else {
    stdOut += "success: restored path=";
    stdOut += oPath.GetPath();
    stdOut += "\n";
  }

  if ((option.find("--restore-versions") == STR_NPOS) &&
      (option.find("-r") == STR_NPOS)) {
    // don't restore old versions
    return 0;
  }

  XrdOucString vkey;

  if (gOFS->_attr_get(oPath.GetPath(), lError, rootvid, "",
                      Recycle::gRecyclingVersionKey.c_str(), vkey)) {
    // no version directory to restore
    return 0;
  }

  int retc = Restore(stdOut, stdErr, vid, vkey.c_str(), option);

  // mask an non existant version reference
  if (retc == ENOENT) {
    return 0;
  }

  return retc;
}

/*----------------------------------------------------------------------------*/
int
Recycle::Purge(XrdOucString& stdOut, XrdOucString& stdErr,
               eos::common::Mapping::VirtualIdentity_t& vid,
               std::string date,
               bool global)
{
  eos::common::Mapping::VirtualIdentity rootvid;
  eos::common::Mapping::Root(rootvid);
  XrdMgmOfsDirectory dirl;
  char sdir[4096];
  XrdOucErrInfo lError;
  int nfiles_deleted = 0;
  int nbulk_deleted = 0;
  std::string rpath;

  if (vid.uid &&
      !vid.sudoer &&
      !(eos::common::Mapping::HasUid(3, vid.uid_list)) &&
      !(eos::common::Mapping::HasGid(4, vid.gid_list))) {
    stdErr = "error: you cannot purge your new recycle bin without being a sudor or having an admin role";
    return EPERM;
  }

  if (!global || (global && vid.uid)) {
    snprintf(sdir, sizeof(sdir) - 1, "%s/uid:%u/%s",
             Recycle::gRecyclingPrefix.c_str(),
             (unsigned int) vid.uid,
             date.c_str());
  } else {
    snprintf(sdir, sizeof(sdir) - 1, "%s/", Recycle::gRecyclingPrefix.c_str());
  }

  std::map<std::string, std::set < std::string>> findmap;
  int depth = 5 + (int) global;
  eos::common::Path dPath(std::string("/") + date);

  if (dPath.GetSubPathSize()) {
    if (depth > (int) dPath.GetSubPathSize()) {
      depth -= dPath.GetSubPathSize();
    }
  }

  int retc = gOFS->_find(sdir, lError, stdErr, vid,
                         findmap,
                         0, 0, false, 0, true, depth);

  if (retc && errno != ENOENT) {
    eos_static_err("find command failed in dir='%s'", sdir);
  }

  for (auto dirit = findmap.begin(); dirit != findmap.end(); ++dirit) {
    eos_static_debug("dir=%s", dirit->first.c_str());
    XrdOucString dirname = dirit->first.c_str();

    if (dirname.endswith(".d/")) {
      dirname.erase(dirname.length() - 1);
      eos::common::Path cpath(dirname.c_str());
      dirname = cpath.GetParentPath();
      dirit->second.insert(cpath.GetName());
    }

    for (auto fileit = dirit->second.begin(); fileit != dirit->second.end();
         ++fileit) {
      XrdOucString fname = fileit->c_str();
      std::string pathname = dirname.c_str();
      pathname += *fileit;
      struct stat buf;
      XrdOucErrInfo lError;

      if ((fname != "/") && !fname.beginswith("#")) {
        continue;
      }

      if (!gOFS->_stat(pathname.c_str(), &buf, lError, vid, "")) {
        // execute a proc command
        ProcCommand Cmd;
        XrdOucString info;

        if (S_ISDIR(buf.st_mode)) {
          // we need recursive deletion
          info = "mgm.cmd=rm&mgm.option=r&mgm.path=";
        } else {
          info = "mgm.cmd=rm&mgm.path=";
        }

        info += pathname.c_str();
        int result = Cmd.open("/proc/user", info.c_str(), rootvid, &lError);
        Cmd.AddOutput(stdOut, stdErr);

        if (!stdOut.endswith("\n")) {
          stdOut += "\n";
        }

        if (!stdErr.endswith("\n")) {
          stdErr += "\n";
        }

        Cmd.close();

        if (!result) {
          if (S_ISDIR(buf.st_mode)) {
            nbulk_deleted++;
          } else {
            nfiles_deleted++;
          }
        }
      }
    }
  }

  stdOut += "success: purged ";
  stdOut += (int) nbulk_deleted;
  stdOut += " bulk deletions and ";
  stdOut += (int) nfiles_deleted;
  stdOut += " individual files from the recycle bin!";
  return 0;
}

/*----------------------------------------------------------------------------*/
int
Recycle::PurgeOld(XrdOucString& stdOut, XrdOucString& stdErr,
                  eos::common::Mapping::VirtualIdentity_t& vid)
{
  eos::common::Mapping::VirtualIdentity rootvid;
  eos::common::Mapping::Root(rootvid);
  XrdMgmOfsDirectory dirl;
  char sdir[4096];
  snprintf(sdir, sizeof(sdir) - 1, "%s/%u/%u/", Recycle::gRecyclingPrefix.c_str(),
           (unsigned int) vid.gid, (unsigned int) vid.uid);
  int retc = dirl.open(sdir, vid, "");

  if (retc) {
    stdOut = "success: nothing has been purged in the old recycle bin!\n";
    return 0;
  }

  const char* dname;
  int nfiles_deleted = 0;
  int nbulk_deleted = 0;

  while ((dname = dirl.nextEntry())) {
    std::string sdname = dname;

    if ((sdname == ".") || (sdname == "..")) {
      continue;
    }

    std::string pathname = sdir;
    pathname += dname;
    struct stat buf;
    XrdOucErrInfo lError;

    if (!gOFS->_stat(pathname.c_str(), &buf, lError, vid, "")) {
      // execute a proc command
      ProcCommand Cmd;
      XrdOucString info;

      if (S_ISDIR(buf.st_mode)) {
        // we need recursive deletion
        info = "mgm.cmd=rm&mgm.option=r&mgm.path=";
      } else {
        info = "mgm.cmd=rm&mgm.path=";
      }

      info += pathname.c_str();
      int result = Cmd.open("/proc/user", info.c_str(), rootvid, &lError);
      Cmd.AddOutput(stdOut, stdErr);

      if (!stdOut.endswith("\n")) {
        stdOut += "\n";
      }

      if (!stdErr.endswith("\n")) {
        stdErr += "\n";
      }

      Cmd.close();

      if (!result) {
        if (S_ISDIR(buf.st_mode)) {
          nbulk_deleted++;
        } else {
          nfiles_deleted++;
        }
      }
    }
  }

  dirl.close();
  stdOut += "success: purged ";
  stdOut += (int) nbulk_deleted;
  stdOut += " bulk deletions and ";
  stdOut += (int) nfiles_deleted;
  stdOut += " individual files from the old recycle bin!\n";
  return 0;
}

/*----------------------------------------------------------------------------*/
int
Recycle::Config(XrdOucString& stdOut, XrdOucString& stdErr,
                eos::common::Mapping::VirtualIdentity_t& vid, const char* arg,
                XrdOucString& option)
{
  XrdOucErrInfo lError;
  eos::common::Mapping::VirtualIdentity rootvid;
  eos::common::Mapping::Root(rootvid);

  if (vid.uid != 0) {
    stdErr = "error: you need to be root to configure the recycle bin and/or recycle polcies";
    stdErr += "\n";
    return EPERM;
  }

  if (option == "--add-bin") {
    if (!arg) {
      stdErr = "error: missing subtree argument\n";
      return EINVAL;
    }

    // execute a proc command
    ProcCommand Cmd;
    XrdOucString info;
    info = "eos.rgid=0&eos.ruid=0&mgm.cmd=attr&mgm.subcmd=set&mgm.option=r&mgm.path=";
    info += arg;
    info += "&mgm.attr.key=";
    info += Recycle::gRecyclingAttribute.c_str();
    info += "&mgm.attr.value=";
    info += Recycle::gRecyclingPrefix.c_str();
    int result = Cmd.open("/proc/user", info.c_str(), rootvid, &lError);
    Cmd.AddOutput(stdOut, stdErr);
    Cmd.close();
    return result;
  }

  if (option == "--remove-bin") {
    if (!arg) {
      stdErr = "error: missing subtree argument\n";
      return EINVAL;
    }

    // execute a proc command
    ProcCommand Cmd;
    XrdOucString info;
    info = "eos.rgid=0&eos.ruid=0&mgm.cmd=attr&mgm.subcmd=rm&mgm.option=r&mgm.path=";
    info += arg;
    info += "&mgm.attr.key=";
    info += Recycle::gRecyclingAttribute.c_str();
    int result = Cmd.open("/proc/user", info.c_str(), rootvid, &lError);
    Cmd.AddOutput(stdOut, stdErr);
    Cmd.close();
    return result;
  }

  if (option == "--lifetime") {
    if (!arg) {
      stdErr = "error: missing lifetime argument";
      return EINVAL;
    }

    XrdOucString ssize = arg;
    unsigned long long size = eos::common::StringConversion::GetSizeFromString(
                                ssize);

    if (!size) {
      stdErr = "error: lifetime has been converted to 0 seconds - probably you made a type!";
      return EINVAL;
    }

    if (size < 60) {
      stdErr = "error: a recycle bin lifetime less than 60s is not accepted!";
      return EINVAL;
    }

    char csize[256];
    snprintf(csize, sizeof(csize) - 1, "%llu", size);

    if (gOFS->_attr_set(Recycle::gRecyclingPrefix.c_str(),
                        lError,
                        rootvid,
                        "",
                        Recycle::gRecyclingTimeAttribute.c_str(),
                        csize)) {
      stdErr = "error: failed to set extended attribute '";
      stdErr += Recycle::gRecyclingTimeAttribute.c_str();
      stdErr += "'";
      stdErr += " at '";
      stdErr += Recycle::gRecyclingPrefix.c_str();
      stdErr += "'";
      return EIO;
    } else {
      stdOut += "success: recycle bin lifetime configured!\n";
    }

    gOFS->Recycler->WakeUp();
  }

  if (option == "--ratio") {
    if (!arg) {
      stdErr = "error: missing ratio argument\n";
      return EINVAL;
    }

    double ratio = strtod(arg, 0);

    if (!ratio) {
      stdErr = "error: ratio must be != 0";
      return EINVAL;
    }

    if ((ratio <= 0) || (ratio > 0.99)) {
      stdErr = "error: a recycle bin ratio has to be 0 < ratio < 1.0!";
      return EINVAL;
    }

    char dratio[256];
    snprintf(dratio, sizeof(dratio) - 1, "%0.2f", ratio);

    if (gOFS->_attr_set(Recycle::gRecyclingPrefix.c_str(),
                        lError,
                        rootvid,
                        "",
                        Recycle::gRecyclingKeepRatio.c_str(),
                        dratio)) {
      stdErr = "error: failed to set extended attribute '";
      stdErr += Recycle::gRecyclingKeepRatio.c_str();
      stdErr += "'";
      stdErr += " at '";
      stdErr += Recycle::gRecyclingPrefix.c_str();
      stdErr += "'";
      return EIO;
    } else {
      stdOut += "success: recycle bin ratio configured!";
    }

    gOFS->Recycler->WakeUp();
  }

  return 0;
}

/*----------------------------------------------------------------------------*/
int
Recycle::GetRecyclePrefix(const char* epname, XrdOucErrInfo& error,
                          std::string& recyclepath, int i_index)
/*----------------------------------------------------------------------------*/
{
  eos::common::Mapping::VirtualIdentity rootvid;
  eos::common::Mapping::Root(rootvid);
  char srecycleuser[4096];
  time_t now = time(NULL);
  struct tm nowtm;
  localtime_r(&now, &nowtm);
  size_t index = (i_index == -1) ? 0 : i_index;

  do {
    snprintf(srecycleuser, sizeof(srecycleuser) - 1, "%s/uid:%u/%04u/%02u/%02u/%lu",
             mRecycleDir.c_str(),
             mOwnerUid,
             1900 + nowtm.tm_year,
             nowtm.tm_mon + 1,
             nowtm.tm_mday,
             index);
    struct stat buf;

    // if i_index is not -1, we just compute the path for the given index and return if it exists already
    if (i_index >= 0) {
      if (!gOFS->_stat(srecycleuser, &buf, error, rootvid, "")) {
        recyclepath = srecycleuser;
      } else {
        return gOFS->Emsg(epname, error, ENOENT, "stat index directory - "
                          "the computed index recycle directory does not exist");
      }

      return SFS_OK;
    }

    // check in case the index directory exists, that it has not more than 1M files,
    // otherwise increment the index by one
    if (!gOFS->_stat(srecycleuser, &buf, error, rootvid, "")) {
      if (buf.st_blksize > 100000) {
        index++;
        continue;
      }
    }

    // Verify/create group/user directory
    if (gOFS->_mkdir(srecycleuser, S_IRUSR | S_IXUSR | SFS_O_MKPTH, error, rootvid,
                     "")) {
      return gOFS->Emsg(epname, error, EIO, "remove existing file - the "
                        "recycle space user directory couldn't be created");
    }

    // Check the user recycle directory

    if (gOFS->_stat(srecycleuser, &buf, error, rootvid, "")) {
      return gOFS->Emsg(epname, error, EIO, "remove existing file - could not "
                        "determine ownership of the recycle space user directory",
                        srecycleuser);
    }

    // Check the ownership of the user directory
    if ((buf.st_uid != mOwnerUid) || (buf.st_gid != mOwnerGid)) {
      // Set the correct ownership
      if (gOFS->_chown(srecycleuser, mOwnerUid, mOwnerGid, error, rootvid, "")) {
        return gOFS->Emsg(epname, error, EIO, "remove existing file - could not "
                          "change ownership of the recycle space user directory",
                          srecycleuser);
      }
    }

    recyclepath = srecycleuser;
    return SFS_OK;
  } while (1);
}

EOSMGMNAMESPACE_END
