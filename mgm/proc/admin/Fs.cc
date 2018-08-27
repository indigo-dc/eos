// ----------------------------------------------------------------------
// File: proc/admin/Fs.cc
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

#include "mgm/proc/ProcInterface.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Stat.hh"
#include "common/LayoutId.hh"
#include "namespace/interface/IFsView.hh"
#include "namespace/interface/IView.hh"
#include "namespace/Prefetcher.hh"

EOSMGMNAMESPACE_BEGIN

int
ProcCommand::Fs()
{
  if (mSubCmd == "ls") {
    std::string output;
    std::string format;
    std::string mListFormat;
    std::string fqdn;

    if (pOpaque->Get("mgm.outhost")) {
      fqdn = pOpaque->Get("mgm.outhost");
    }

    mListFormat = FsView::GetFileSystemFormat(std::string(mOutFormat.c_str()));

    if (fqdn != "brief") {
      if (format.find("S") != std::string::npos) {
        format.replace(format.find("S"), 1, "s");
      }

      if (mListFormat.find("S") != std::string::npos) {
        mListFormat.replace(mListFormat.find("S"), 1, "s");
      }
    }

    std::string filter(mOutFormat.c_str());
    eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
    FsView::gFsView.PrintSpaces(output, format, mListFormat, mOutDepth, mSelection,
                                filter);
    stdOut += output.c_str();
  }

  if (mAdminCmd) {
    if (mSubCmd == "add") {
      std::string sfsid = (pOpaque->Get("mgm.fs.fsid")) ? pOpaque->Get("mgm.fs.fsid")
                          : "0";
      std::string uuid = (pOpaque->Get("mgm.fs.uuid")) ? pOpaque->Get("mgm.fs.uuid") :
                         "";
      std::string nodename = (pOpaque->Get("mgm.fs.node")) ?
                             pOpaque->Get("mgm.fs.node") : "";
      std::string mountpoint = (pOpaque->Get("mgm.fs.mountpoint")) ?
                               pOpaque->Get("mgm.fs.mountpoint") : "";
      std::string space = (pOpaque->Get("mgm.fs.space")) ?
                          pOpaque->Get("mgm.fs.space") : "";
      std::string configstatus = (pOpaque->Get("mgm.fs.configstatus")) ?
                                 pOpaque->Get("mgm.fs.configstatus") : "";
      retc = proc_fs_add(sfsid, uuid, nodename, mountpoint, space, configstatus,
                         stdOut, stdErr, *pVid);
    }

    if (mSubCmd == "mv") {
      if (pVid->uid == 0) {
        std::string sfsid = (pOpaque->Get("mgm.fs.id") ?
                             pOpaque->Get("mgm.fs.id") : "");
        std::string space = (pOpaque->Get("mgm.space") ?
                             pOpaque->Get("mgm.space") : "");
        retc = proc_fs_mv(sfsid, space, stdOut, stdErr, *pVid);
      } else {
        retc = EPERM;
        stdErr = "error: you have to take role 'root' to execute this command";
      }
    }

    if (mSubCmd == "dumpmd") {
      if ((pVid->uid == 0) || (pVid->prot == "sss")) {
        {
          // Stall if the namespace is still booting
          XrdSysMutexHelper lock(gOFS->InitializationMutex);

          if (gOFS->mInitialized != gOFS->kBooted) {
            return gOFS->Stall(*mError, 60, "Namespace is still booting");
          }
        }
        std::string fsidst = pOpaque->Get("mgm.fsid");
        XrdOucString option = pOpaque->Get("mgm.dumpmd.option");
        XrdOucString dp = pOpaque->Get("mgm.dumpmd.path");
        XrdOucString df = pOpaque->Get("mgm.dumpmd.fid");
        XrdOucString ds = pOpaque->Get("mgm.dumpmd.size");
        XrdOucString dt = pOpaque->Get("mgm.dumpmd.storetime");
        size_t entries = 0;
        retc = proc_fs_dumpmd(fsidst, option, dp, df, ds, stdOut, stdErr,
                              *pVid, entries);

        if (!retc) {
          gOFS->MgmStats.Add("DumpMd", pVid->uid, pVid->gid, entries);
        }

        if (dt == "1") {
          // store the time of this dump
          XrdSysMutexHelper lock(gOFS->DumpmdTimeMapMutex);
          gOFS->DumpmdTimeMap[strtoul(fsidst.c_str(), 0, 10)] = time(NULL);
        }
      } else {
        retc = EPERM;
        stdErr = "error: you have to take role 'root' or connect via 'sss' "
                 "to execute this command";
      }
    }

    if (mSubCmd == "config") {
      std::string identifier = (pOpaque->Get("mgm.fs.identifier")) ?
                               pOpaque->Get("mgm.fs.identifier") : "";
      std::string key = (pOpaque->Get("mgm.fs.key")) ? pOpaque->Get("mgm.fs.key") :
                        "";
      std::string value = (pOpaque->Get("mgm.fs.value")) ?
                          pOpaque->Get("mgm.fs.value") : "";
      retc = proc_fs_config(identifier, key, value, stdOut, stdErr, *pVid);
    }

    if (mSubCmd == "rm") {
      std::string nodename = (pOpaque->Get("mgm.fs.node")) ?
                             pOpaque->Get("mgm.fs.node") : "";
      std::string mountpoint = pOpaque->Get("mgm.fs.mountpoint") ?
                               pOpaque->Get("mgm.fs.mountpoint") : "";
      std::string id = pOpaque->Get("mgm.fs.id") ? pOpaque->Get("mgm.fs.id") : "";
      eos::common::RWMutexWriteLock wr_lock(FsView::gFsView.ViewMutex);
      retc = proc_fs_rm(nodename, mountpoint, id, stdOut, stdErr, *pVid);
    }

    if (mSubCmd == "dropdeletion") {
      std::string id = pOpaque->Get("mgm.fs.id") ? pOpaque->Get("mgm.fs.id") : "";
      eos::common::RWMutexReadLock rd_lock(FsView::gFsView.ViewMutex);
      retc = proc_fs_dropdeletion(id, stdOut, stdErr, *pVid);
    }
  }

  if (mSubCmd == "boot") {
    if ((pVid->uid == 0) || (pVid->prot == "sss")) {
      std::string node = (pOpaque->Get("mgm.fs.node")) ? pOpaque->Get("mgm.fs.node") :
                         "";
      std::string fsids = (pOpaque->Get("mgm.fs.id")) ? pOpaque->Get("mgm.fs.id") :
                          "";
      std::string forcemgmsync = (pOpaque->Get("mgm.fs.forcemgmsync")) ?
                                 pOpaque->Get("mgm.fs.forcemgmsync") : "";
      eos::common::FileSystem::fsid_t fsid = atoi(fsids.c_str());

      if (node == "*") {
        // boot all filesystems
        if (pVid->uid == 0) {
          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
          stdOut += "success: boot message send to";

          for (auto it = FsView::gFsView.mIdView.begin();
               it != FsView::gFsView.mIdView.end(); ++it) {
            if ((it->second->GetConfigStatus() > eos::common::FileSystem::kOff)) {
              if (forcemgmsync.length()) {
                // set the check flag
                it->second->SetLongLong("bootcheck", eos::common::FileSystem::kBootResync);
              } else {
                // set the force flag
                it->second->SetLongLong("bootcheck", eos::common::FileSystem::kBootForced);
              }

              auto now = time(NULL);

              if (now < 0) {
                now = 0;
              }

              it->second->SetLongLong("bootsenttime", (unsigned long long) now);
              stdOut += " ";
              stdOut += it->second->GetString("host").c_str();
              stdOut += ":";
              stdOut += it->second->GetString("path").c_str();
            }
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      } else {
        if (node.length()) {
          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

          if (!FsView::gFsView.mNodeView.count(node)) {
            stdErr = "error: cannot boot node - no node with name=";
            stdErr += node.c_str();
            retc = ENOENT;
          } else {
            stdOut += "success: boot message send to";

            for (auto it = FsView::gFsView.mNodeView[node]->begin();
                 it != FsView::gFsView.mNodeView[node]->end(); ++it) {
              FileSystem* fs = 0;

              if (FsView::gFsView.mIdView.count(*it)) {
                fs = FsView::gFsView.mIdView[*it];
              }

              if (fs) {
                if (forcemgmsync.length()) {
                  // set the check flag
                  fs->SetLongLong("bootcheck", eos::common::FileSystem::kBootResync);
                } else {
                  // set the force flag
                  fs->SetLongLong("bootcheck", eos::common::FileSystem::kBootForced);
                }

                auto now = time(NULL);
                fs->SetLongLong("bootsenttime", ((now > 0) ? now : 0));
                stdOut += " ";
                stdOut += fs->GetString("host").c_str();
                stdOut += ":";
                stdOut += fs->GetString("path").c_str();
              }
            }
          }
        }

        if (fsid) {
          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

          if (FsView::gFsView.mIdView.count(fsid)) {
            stdOut += "success: boot message send to";
            FileSystem* fs = FsView::gFsView.mIdView[fsid];

            if (fs) {
              if (forcemgmsync.length()) {
                // set the check flag
                fs->SetLongLong("bootcheck", eos::common::FileSystem::kBootResync);
                ;
              } else {
                // set the force flag
                fs->SetLongLong("bootcheck", eos::common::FileSystem::kBootForced);
              }

              fs->SetLongLong("bootsenttime", (unsigned long long) time(NULL));
              stdOut += " ";
              stdOut += fs->GetString("host").c_str();
              stdOut += ":";
              stdOut += fs->GetString("path").c_str();
            }
          } else {
            stdErr = "error: cannot boot filesystem - no filesystem with fsid=";
            stdErr += fsids.c_str();
            retc = ENOENT;
          }
        }
      }
    } else {
      retc = EPERM;
      stdErr = "error: you have to take role 'root' to execute this command";
    }
  }

//      stdOut+="\n==== fs done ====";

  if (mSubCmd == "status") {
    if ((pVid->uid == 0) || (pVid->prot == "sss")) {
      std::string fsids = (pOpaque->Get("mgm.fs.id")) ? pOpaque->Get("mgm.fs.id") :
                          "";
      std::string node = (pOpaque->Get("mgm.fs.node")) ? pOpaque->Get("mgm.fs.node") :
                         "";
      std::string mount = (pOpaque->Get("mgm.fs.mountpoint")) ?
                          pOpaque->Get("mgm.fs.mountpoint") : "";
      std::string option = (pOpaque->Get("mgm.fs.option")) ?
                           pOpaque->Get("mgm.fs.option") : "";
      eos::common::FileSystem::fsid_t fsid = atoi(fsids.c_str());
      XrdOucString filelisting = "";
      bool listfile = false;
      bool riskanalysis = false;

      if (option.find("l") != std::string::npos) {
        listfile = true;
        riskanalysis = true;
      }

      if (option.find("r") != std::string::npos) {
        riskanalysis = true;
      }

      if (!fsid) {
        // try to get from the node/mountpoint
        if ((node.find(":") == std::string::npos)) {
          node += ":1095"; // default eos fst port
        }

        if ((node.find("/eos/") == std::string::npos)) {
          node.insert(0, "/eos/");
          node.append("/fst");
        }

        eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

        if (FsView::gFsView.mNodeView.count(node)) {
          for (auto it = FsView::gFsView.mNodeView[node]->begin();
               it != FsView::gFsView.mNodeView[node]->end(); ++it) {
            if (FsView::gFsView.mIdView.count(*it)) {
              if (FsView::gFsView.mIdView[*it]->GetPath() == mount) {
                // this is the filesystem
                fsid = *it;
              }
            }
          }
        }
      }

      if (fsid) {
        eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

        if (FsView::gFsView.mIdView.count(fsid)) {
          FileSystem* fs = FsView::gFsView.mIdView[fsid];

          if (fs) {
            stdOut += "# ------------------------------------------------------------------------------------\n";
            stdOut += "# FileSystem Variables\n";
            stdOut += "# ....................................................................................\n";
            std::vector<std::string> keylist;
            fs->GetKeys(keylist);
            std::sort(keylist.begin(), keylist.end());

            for (size_t i = 0; i < keylist.size(); i++) {
              char line[1024];
              snprintf(line, sizeof(line) - 1, "%-32s := %s\n", keylist[i].c_str(),
                       fs->GetString(keylist[i].c_str()).c_str());
              stdOut += line;
            }

            if (riskanalysis) {
              stdOut += "# ....................................................................................\n";
              stdOut += "# Risk Analysis\n";
              stdOut += "# ....................................................................................\n";
              // get some statistics about the filesystem
              //-------------------------------------------
              unsigned long long nfids = 0;
              unsigned long long nfids_healthy = 0;
              unsigned long long nfids_risky = 0;
              unsigned long long nfids_inaccessible = 0;
              unsigned long long nfids_todelete = 0;
              eos::Prefetcher::prefetchFilesystemFileListWithFileMDsAndParentsAndWait(
                gOFS->eosView, gOFS->eosFsView, fsid);
              eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);

              try {
                nfids_todelete = gOFS->eosFsView->getNumUnlinkedFilesOnFs(fsid);
                nfids = gOFS->eosFsView->getNumFilesOnFs(fsid);

                for (auto it_fid = gOFS->eosFsView->getFileList(fsid);
                     (it_fid && it_fid->valid()); it_fid->next()) {
                  std::shared_ptr<eos::IFileMD> fmd =
                    gOFS->eosFileService->getFileMD(it_fid->getElement());

                  if (fmd) {
                    size_t nloc_ok = 0;
                    size_t nloc = fmd->getNumLocation();
                    eos::IFileMD::LocationVector::const_iterator lociter;
                    eos::IFileMD::LocationVector loc_vect = fmd->getLocations();

                    for (lociter = loc_vect.begin(); lociter != loc_vect.end(); ++lociter) {
                      if (*lociter) {
                        if (FsView::gFsView.mIdView.count(*lociter)) {
                          FileSystem* repfs = FsView::gFsView.mIdView[*lociter];
                          eos::common::FileSystem::fs_snapshot_t snapshot;
                          repfs->SnapShotFileSystem(snapshot, false);

                          if ((snapshot.mStatus == eos::common::FileSystem::kBooted) &&
                              (snapshot.mConfigStatus == eos::common::FileSystem::kRW) &&
                              (snapshot.mErrCode == 0) && // this we probably don't need
                              (fs->GetActiveStatus(snapshot))) {
                            nloc_ok++;
                          }
                        }
                      }
                    }

                    if (eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) ==
                        eos::common::LayoutId::kReplica) {
                      if (nloc_ok == nloc) {
                        nfids_healthy++;
                      } else {
                        if (nloc_ok == 0) {
                          nfids_inaccessible++;

                          if (listfile) {
                            filelisting += "status=offline path=";
                            filelisting += gOFS->eosView->getUri(fmd.get()).c_str();
                            filelisting += "\n";
                          }
                        } else {
                          if (nloc_ok < nloc) {
                            nfids_risky++;

                            if (listfile) {
                              filelisting += "status=atrisk  path=";
                              filelisting += gOFS->eosView->getUri(fmd.get()).c_str();
                              filelisting += "\n";
                            }
                          }
                        }
                      }
                    }

                    if (eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) ==
                        eos::common::LayoutId::kPlain) {
                      if (nloc_ok != nloc) {
                        nfids_inaccessible++;

                        if (listfile) {
                          filelisting += "status=offline path=";
                          filelisting += gOFS->eosView->getUri(fmd.get()).c_str();
                          filelisting += "\n";
                        }
                      }
                    }
                  }
                }

                XrdOucString sizestring;
                char line[1024];
                snprintf(line, sizeof(line) - 1, "%-32s := %10s (%.02f%%)\n", "number of files",
                         eos::common::StringConversion::GetSizeString(sizestring, nfids), 100.0);
                stdOut += line;
                snprintf(line, sizeof(line) - 1, "%-32s := %10s (%.02f%%)\n", "files healthy",
                         eos::common::StringConversion::GetSizeString(sizestring, nfids_healthy),
                         nfids ? (100.0 * nfids_healthy) / nfids : 100.0);
                stdOut += line;
                snprintf(line, sizeof(line) - 1, "%-32s := %10s (%.02f%%)\n", "files at risk",
                         eos::common::StringConversion::GetSizeString(sizestring, nfids_risky),
                         nfids ? (100.0 * nfids_risky) / nfids : 100.0);
                stdOut += line;
                snprintf(line, sizeof(line) - 1, "%-32s := %10s (%.02f%%)\n",
                         "files inaccessbile", eos::common::StringConversion::GetSizeString(sizestring,
                             nfids_inaccessible), nfids ? (100.0 * nfids_inaccessible) / nfids : 100.0);
                stdOut += line;
                snprintf(line, sizeof(line) - 1, "%-32s := %10s\n", "files pending deletion",
                         eos::common::StringConversion::GetSizeString(sizestring, nfids_todelete));
                stdOut += line;
                stdOut += "# ------------------------------------------------------------------------------------\n";

                if (listfile) {
                  stdOut += filelisting;
                }
              } catch (eos::MDException& e) {
                errno = e.getErrno();
                eos_static_err("caught exception %d %s\n", e.getErrno(),
                               e.getMessage().str().c_str());
              }
            }

            //-------------------------------------------
            retc = 0;
          }
        } else {
          stdErr = "error: cannot find filesystem - no filesystem with fsid=";
          stdErr += fsids.c_str();
          retc = ENOENT;
        }
      } else {
        stdErr = "error: cannot find a matching filesystem";
        retc = ENOENT;
      }
    } else {
      retc = EPERM;
      stdErr = "error: you have to take role 'root' to execute this command or connect via sss";
    }
  }

  return SFS_OK;
}

EOSMGMNAMESPACE_END
