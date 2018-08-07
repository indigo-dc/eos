// ----------------------------------------------------------------------
// File: Schedule2Delete.cc
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


// -----------------------------------------------------------------------
// This file is included source code in XrdMgmOfs.cc to make the code more
// transparent without slowing down the compilation time.
// -----------------------------------------------------------------------

{
  REQUIRE_SSS_OR_LOCAL_AUTH;
  ACCESSMODE_W;
  MAYSTALL;
  MAYREDIRECT;
  EXEC_TIMING_BEGIN("Scheduled2Delete");
  gOFS->MgmStats.Add("Schedule2Delete", 0, 0, 1);
  XrdOucString nodename = env.Get("mgm.target.nodename");
  eos_static_debug("nodename=%s", nodename.c_str() ? nodename.c_str() : "-none-");
  std::vector<unsigned long> fslist;

  {
    // Get all the filesystems of the current node
    eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
    std::string snodename = nodename.c_str() ? nodename.c_str() : "-none-";

    if (!FsView::gFsView.mNodeView.count(snodename))
    {
      eos_static_warning("msg=\"node is not configured\" name=%s", snodename.c_str());
      return Emsg(epname, error, EINVAL, "unable to schedule - node is not existing");
    }

    for (auto set_it = FsView::gFsView.mNodeView[snodename]->begin();
    set_it != FsView::gFsView.mNodeView[snodename]->end(); ++set_it)
    {
      fslist.push_back(*set_it);
    }
  }

  size_t totaldeleted = 0;

  for (unsigned long i = 0ul; i < fslist.size(); i++)
  {
    // Loop over all file systems
    eos::Prefetcher::prefetchFilesystemUnlinkedFileListAndWait(gOFS->eosView, gOFS->eosFsView, fslist[i]);
    eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
    std::unordered_set<eos::IFileMD::id_t> set_fids;
    {
      eos::common::RWMutexReadLock vlock(gOFS->eosViewRWMutex);
      uint64_t num_files = eosFsView->getNumUnlinkedFilesOnFs(fslist[i]);

      if (num_files == 0) {
        eos_static_debug("nothing to delete from fs %lu", fslist[i]);
        continue;
      }

      set_fids.reserve(num_files);

      // Collect all the file ids to be deleted from the current file system
      for (auto it_fid = gOFS->eosFsView->getUnlinkedFileList(fslist[i]);
           (it_fid && it_fid->valid()); it_fid->next()) {
        set_fids.insert(it_fid->getElement());
      }
    }
    XrdMqMessage message("deletion");
    int ndeleted = 0;
    int msgsize = 0;
    eos::mgm::FileSystem* fs = 0;
    XrdOucString receiver = "";
    XrdOucString msgbody = "mgm.cmd=drop";
    XrdOucString capability = "";
    XrdOucString idlist = "";

    for (auto elem = set_fids.begin(); elem != set_fids.end(); ++elem) {
      eos_static_info("msg=\"add to deletion message\" fxid=%08llx fsid=%lu",
                      *elem, (unsigned long) fslist[i]);

      // loop over all files and emit a deletion message
      if (!fs) {
        // set the file system only for the first file to relax the mutex contention
        if (!fslist[i]) {
          eos_err("no filesystem in deletion list");
          continue;
        }

        if (FsView::gFsView.mIdView.count(fslist[i])) {
          fs = FsView::gFsView.mIdView[fslist[i]];
        } else {
          fs = 0;
        }

        if (fs) {
          eos::common::FileSystem::fsstatus_t bootstatus = fs->GetStatus();

          // check the state of the filesystem (if it can actually delete in this moment!)
          if ((fs->GetConfigStatus() <= eos::common::FileSystem::kOff) ||
              (bootstatus != eos::common::FileSystem::kBooted)) {
            // we don't need to send messages, this one is anyway down or currently booting
            break;
          }

          if ((fs->GetActiveStatus() == eos::common::FileSystem::kOffline)) {
            break;
          }

          capability += "&mgm.access=delete";
          capability += "&mgm.manager=";
          capability += gOFS->ManagerId.c_str();
          capability += "&mgm.fsid=";
          capability += (int) fs->GetId();
          capability += "&mgm.localprefix=";
          capability += fs->GetPath().c_str();
          capability += "&mgm.fids=";
          receiver = fs->GetQueue().c_str();

          // Track the message size
          msgsize = msgbody.length() + capability.length();
        }
      }

      ndeleted++;
      totaldeleted++;
      XrdOucString sfid = "";
      XrdOucString hexfid = "";
      eos::common::FileId::Fid2Hex(*elem, hexfid);
      std::shared_ptr<eos::IFileMD> fmd = gOFS->eosFileService->getFileMD(*elem);

      // IDs within the list follow the pattern -- hexfid[:lpath:ctime]
      idlist += hexfid;
      msgsize += hexfid.length() + 1;

      if (fmd && fmd->hasAttribute("logicalpath")) {
        eos::IFileMD::ctime_t ctime;
        char buff[64];

        XrdOucString lpath;
        eos::common::FileFsPath::GetPhysicalPath(fs->GetId(), fmd, lpath);
        fmd->getCTime(ctime);
        sprintf(buff, "%ld", ctime.tv_sec);

        idlist += ":";
        idlist += lpath.c_str();
        idlist += ":";
        idlist += buff;

        msgsize += lpath.length() + strlen(buff) + 2;
      }
      idlist += ",";

      // Segment the message into chunks of 1024 files
      // or maximum 75% of MqMessage capacity
      if (ndeleted > 1024 ||
          msgsize > (0.75 * XrdMqClient::XrdMqMaxMessageLen)) {
        XrdOucString refcapability = capability;
        refcapability += idlist;
        XrdOucEnv incapability(refcapability.c_str());
        XrdOucEnv* capabilityenv = 0;
        eos::common::SymKey* symkey = eos::common::gSymKeyStore.GetCurrentKey();
        int caprc = 0;

        if ((caprc = gCapabilityEngine.Create(&incapability, capabilityenv, symkey,
                                              mCapabilityValidity))) {
          eos_static_err("unable to create capability - errno=%u", caprc);
        } else {
          int caplen = 0;
          msgbody += capabilityenv->Env(caplen);
          // we send deletions in bunches of max 1024 for efficiency
          message.SetBody(msgbody.c_str());

          if (!Messaging::gMessageClient.SendMessage(message, receiver.c_str())) {
            eos_static_err("unable to send deletion message to %s", receiver.c_str());
          }
        }

        idlist = "";
        ndeleted = 0;
        msgbody = "mgm.cmd=drop";
        msgsize = msgbody.length();

        if (capabilityenv) {
          delete capabilityenv;
        }
      }
    }

    // send the remaining ids
    if (idlist.length()) {
      XrdOucString refcapability = capability;
      refcapability += idlist;
      XrdOucEnv incapability(refcapability.c_str());
      XrdOucEnv* capabilityenv = 0;
      eos::common::SymKey* symkey = eos::common::gSymKeyStore.GetCurrentKey();
      int caprc = 0;

      if ((caprc = gCapabilityEngine.Create(&incapability, capabilityenv, symkey,
                                            mCapabilityValidity))) {
        eos_static_err("unable to create capability - errno=%u", caprc);
      } else {
        int caplen = 0;
        msgbody += capabilityenv->Env(caplen);
        // we send deletions in bunches of max 1000 for efficiency
        message.SetBody(msgbody.c_str());

        if (!Messaging::gMessageClient.SendMessage(message, receiver.c_str())) {
          eos_static_err("unable to send deletion message to %s", receiver.c_str());
        }
      }

      if (capabilityenv) {
        delete capabilityenv;
      }
    }
  }

  // -----------------------------------------------------------------------
  if (totaldeleted)
  {
    EXEC_TIMING_END("Scheduled2Delete");
    gOFS->MgmStats.Add("Scheduled2Delete", 0, 0, totaldeleted);
    error.setErrInfo(0, "submitted");
    return SFS_DATA;
  } else
  {
    error.setErrInfo(0, "");
    return SFS_DATA;
  }
}
