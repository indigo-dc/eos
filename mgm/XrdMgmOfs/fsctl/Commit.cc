// ----------------------------------------------------------------------
// File: Commit.cc
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

  EXEC_TIMING_BEGIN("Commit");

  char* asize = env.Get("mgm.size");
  char* spath = env.Get("mgm.path");
  char* afid = env.Get("mgm.fid");
  char* afsid = env.Get("mgm.add.fsid");
  char* amtime = env.Get("mgm.mtime");
  char* amtimensec = env.Get("mgm.mtime_ns");
  char* alogid = env.Get("mgm.logid");

  if (alogid)
  {
    ThreadLogId.SetLogId(alogid, tident);
  }

  XrdOucString averifychecksum = env.Get("mgm.verify.checksum");
  XrdOucString acommitchecksum = env.Get("mgm.commit.checksum");
  XrdOucString averifysize = env.Get("mgm.verify.size");
  XrdOucString acommitsize = env.Get("mgm.commit.size");
  XrdOucString adropfsid = env.Get("mgm.drop.fsid");
  XrdOucString areplication = env.Get("mgm.replication");
  XrdOucString areconstruction = env.Get("mgm.reconstruction");
  XrdOucString aismodified = env.Get("mgm.modified");
  XrdOucString afusex = env.Get("mgm.fusex");

  bool verifychecksum = (averifychecksum == "1");
  bool commitchecksum = (acommitchecksum == "1");
  bool verifysize = (averifysize == "1");
  bool commitsize = (acommitsize == "1");
  bool replication = (areplication == "1");
  bool reconstruction = (areconstruction == "1");
  bool modified = (aismodified == "1");
  bool fusex = (afusex == "1");
  bool isAbort = false;
  eos::common::Path atomic_path("");
  bool isVersioning = false;
  bool isAtomic = false;

  int envlen;
  int oc_n = 0;
  int oc_max = 0;
  XrdOucString oc_uuid = "";
  bool occhunk = eos::common::OwnCloud::GetChunkInfo(env.Env(envlen), oc_n,
      oc_max, oc_uuid);

  // Indicate when the last chunk of a chunked OC upload has been committed
  bool ocdone = false;
  char* checksum = env.Get("mgm.checksum");
  char binchecksum[SHA_DIGEST_LENGTH];
  memset(binchecksum, 0, sizeof(binchecksum));
  unsigned long dropfsid = 0;

  static const char* epname = "commit";

  if (adropfsid.length())
  {
    dropfsid = strtoul(adropfsid.c_str(), 0, 10);
  }

  if (reconstruction)
  {
    // remove the checksum we don't care about it
    checksum = 0;
    verifysize = false;
    verifychecksum = false;
    commitsize = false;
    commitchecksum = false;
    replication = false;
  }

  if (checksum)
  {
    for (unsigned int i = 0; i < strlen(checksum); i += 2) {
      // hex2binary conversion
      char hex[3];
      hex[0] = checksum[i];
      hex[1] = checksum[i + 1];
      hex[2] = 0;
      binchecksum[i / 2] = strtol(hex, 0, 16);
    }
  }

  if (asize && afid && spath && afsid && amtime && amtimensec)
  {
    unsigned long long size = strtoull(asize, 0, 10);
    unsigned long long fid = strtoull(afid, 0, 16);
    unsigned long fsid = strtoul(afsid, 0, 10);
    unsigned long mtime = strtoul(amtime, 0, 10);
    unsigned long mtimens = strtoul(amtimensec, 0, 10);
    {
      // Check that the file system is still allowed to accept replica's
      eos::common::RWMutexReadLock vlock(FsView::gFsView.ViewMutex);
      eos::mgm::FileSystem* fs = 0;

      if (FsView::gFsView.mIdView.count(fsid)) {
        fs = FsView::gFsView.mIdView[fsid];
      }

      if ((!fs) || (fs->GetConfigStatus() < eos::common::FileSystem::kDrain)) {
        eos_thread_err("msg=\"commit suppressed\" configstatus=%s subcmd=commit "
                       "path=%s size=%s fid=%s fsid=%s dropfsid=%llu checksum=%s"
                       " mtime=%s mtime.nsec=%s oc-chunk=%d oc-n=%d oc-max=%d "
                       "oc-uuid=%s", (fs ? eos::common::FileSystem::GetConfigStatusAsString(
                                        fs->GetConfigStatus()) :
                                      "deleted"), spath, asize, afid, afsid,
                       dropfsid, checksum, amtime, amtimensec, occhunk, oc_n,
                       oc_max, oc_uuid.c_str());
        return Emsg(epname, error, EIO,
                    "commit file metadata - filesystem is in non-operational state [EIO]", "");
      }
    }
    eos::Buffer checksumbuffer;
    checksumbuffer.putData(binchecksum, SHA_DIGEST_LENGTH);

    if (checksum) {
      eos_thread_info("subcmd=commit path=%s size=%s fid=%s fsid=%s dropfsid=%llu"
                      " checksum=%s mtime=%s mtime.nsec=%s oc-chunk=%d  oc-n=%d "
                      "oc-max=%d oc-uuid=%s", spath, asize, afid, afsid, dropfsid,
                      checksum, amtime, amtimensec, occhunk, oc_n, oc_max,
                      oc_uuid.c_str());
    } else {
      eos_thread_info("subcmd=commit path=%s size=%s fid=%s fsid=%s dropfsid=%llu "
                      "mtime=%s mtime.nsec=%s oc-chunk=%d  oc-n=%d oc-max=%d oc-uuid=%s",
                      spath, asize, afid, afsid, dropfsid, amtime, amtimensec, occhunk,
                      oc_n, oc_max, oc_uuid.c_str());
    }

    // get the file meta data if exists
    std::shared_ptr<eos::IFileMD> fmd;
    std::shared_ptr<eos::IContainerMD> cmd;
    eos::IContainerMD::id_t cid = 0;
    std::string fmdname;
    {
      // Keep the lock order View=>Namespace=>Quota
      eos::common::RWMutexWriteLock nslock(gOFS->eosViewRWMutex);
      XrdOucString emsg = "";

      try {
        fmd = gOFS->eosFileService->getFileMD(fid);
      } catch (eos::MDException& e) {
        errno = e.getErrno();
        eos_thread_debug("msg=\"exception\" ec=%d emsg=\"%s\"\n", e.getErrno(),
                         e.getMessage().str().c_str());
        emsg = "retc=";
        emsg += e.getErrno();
        emsg += " msg=";
        emsg += e.getMessage().str().c_str();
      }

      if (!fmd) {
        // uups, no such file anymore
        if (errno == ENOENT) {
          return Emsg(epname, error, ENOENT,
                      "commit filesize change - file is already removed [EIDRM]", "");
        } else {
          emsg.insert("commit filesize change [EIO] ", 0);
          return Emsg(epname, error, errno, emsg.c_str(), spath);
        }
      } else {
        unsigned long lid = fmd->getLayoutId();

        // check if fsid and fid are ok
        if (fmd->getId() != fid) {
          eos_thread_notice("commit for fid=%lu but fid=%lu", fmd->getId(), fid);
          gOFS->MgmStats.Add("CommitFailedFid", 0, 0, 1);
          return Emsg(epname, error, EINVAL,
                      "commit filesize change - file id is wrong [EINVAL]", spath);
        }

        // check if this file is already unlinked from the visible namespace
        if (!(cid = fmd->getContainerId())) {
          eos_thread_debug("commit for fid=%lu but file is disconnected from any container",
                           fmd->getId());
          gOFS->MgmStats.Add("CommitFailedUnlinked", 0, 0, 1);
          return Emsg(epname, error, EIDRM,
                      "commit filesize change - file is already removed [EIDRM]", "");
        }

        // check if this commit comes from a transfer and if the size/checksum is ok
        if (replication) {
          // we remote this file NOW from the scheduling maps
          {
            XrdSysMutexHelper sLock(ScheduledToDrainFidMutex);

            if (ScheduledToDrainFid.count(fid)) {
              ScheduledToDrainFid.erase(fid);
            }
          }
          {
            XrdSysMutexHelper sLock(ScheduledToBalanceFidMutex);

            if (ScheduledToBalanceFid.count(fid)) {
              ScheduledToBalanceFid.erase(fid);
            }
          }

          // check if we have this replica in the unlink list
          if (fusex && fmd->hasUnlinkedLocation((unsigned short) fsid)) {
            eos_thread_err("suppressing possible recovery replica for fid=%lu on unlinked fsid=%llu- rejecting replica",
                           fmd->getId(), fsid);
            // this happens when a FUSEX recovery has been triggered, to avoid to reattach replicas,
            // we clean them up here
            return Emsg(epname, error, EBADE,
                        "commit replica - file size is wrong [EBADE] - suppressing recovery replica",
                        "");
          }

          if (eos::common::LayoutId::GetLayoutType(lid) ==
              eos::common::LayoutId::kReplica) {
            // we check filesize and the checksum only for replica layouts
            eos_thread_debug("fmd size=%lli, size=%lli", fmd->getSize(), size);

            if (fmd->getSize() != size) {
              eos_thread_err("replication for fid=%lu resulted in a different file "
                             "size on fsid=%llu - rejecting replica", fmd->getId(), fsid);
              gOFS->MgmStats.Add("ReplicaFailedSize", 0, 0, 1);

              // -----------------------------------------------------------
              // if we come via FUSE, we have to remove this replica
              // -----------------------------------------------------------
              if (fmd->hasLocation((unsigned short) fsid)) {
                fmd->unlinkLocation((unsigned short) fsid);
                fmd->removeLocation((unsigned short) fsid);

                try {
                  gOFS->eosView->updateFileStore(fmd.get());
                  // this call is not needed, since it is just a new replica location
                  // gOFS->FuseXCast(eos::common::FileId::FidToInode(fmd->getId()));
                } catch (eos::MDException& e) {
                  errno = e.getErrno();
                  std::string errmsg = e.getMessage().str();
                  eos_thread_crit("msg=\"exception\" ec=%d emsg=\"%s\"\n",
                                  e.getErrno(), e.getMessage().str().c_str());
                }
              }

              return Emsg(epname, error, EBADE, "commit replica - file size is wrong [EBADE]",
                          "");
            }

            bool cxError = false;
            size_t cxlen = eos::common::LayoutId::GetChecksumLen(fmd->getLayoutId());

            for (size_t i = 0; i < cxlen; i++) {
              if (fmd->getChecksum().getDataPadded(i) != checksumbuffer.getDataPadded(i)) {
                cxError = true;
              }
            }

            if (cxError) {
              eos_thread_err("replication for fid=%lu resulted in a different checksum "
                             "on fsid=%llu - rejecting replica", fmd->getId(), fsid);
              gOFS->MgmStats.Add("ReplicaFailedChecksum", 0, 0, 1);

              // -----------------------------------------------------------
              // if we come via FUSE, we have to remove this replica
              // -----------------------------------------------------------
              if (fmd->hasLocation((unsigned short) fsid)) {
                fmd->unlinkLocation((unsigned short) fsid);
                fmd->removeLocation((unsigned short) fsid);

                try {
                  gOFS->eosView->updateFileStore(fmd.get());
                  // this call is not be needed, since it is just a new replica location
                  //gOFS->FuseXCast(eos::common::FileId::FidToInode(fmd->getId()));
                } catch (eos::MDException& e) {
                  errno = e.getErrno();
                  std::string errmsg = e.getMessage().str();
                  eos_thread_crit("msg=\"exception\" ec=%d emsg=\"%s\"\n",
                                  e.getErrno(), e.getMessage().str().c_str());
                }
              }

              return Emsg(epname, error, EBADR,
                          "commit replica - file checksum is wrong [EBADR]", "");
            }
          }
        }

        if (verifysize) {
          // check if we saw a file size change or checksum change
          if (fmd->getSize() != size) {
            eos_thread_err("commit for fid=%lu gave a file size change after "
                           "verification on fsid=%llu", fmd->getId(), fsid);
          }
        }

        if (checksum) {
          if (verifychecksum) {
            bool cxError = false;
            size_t cxlen = eos::common::LayoutId::GetChecksumLen(fmd->getLayoutId());

            for (size_t i = 0; i < cxlen; i++) {
              if (fmd->getChecksum().getDataPadded(i) != checksumbuffer.getDataPadded(i)) {
                cxError = true;
              }
            }

            if (cxError) {
              eos_thread_err("commit for fid=%lu gave a different checksum after "
                             "verification on fsid=%llu", fmd->getId(), fsid);
            }
          }
        }

        // For changing the modification time we have to figure out if we
        // just attach a new replica or if we have a change of the contents
        bool isUpdate = false;
        {
          std::shared_ptr<eos::IContainerMD> dir;

          try {
            dir = gOFS->eosDirectoryService->getContainerMD(cid);
          } catch (eos::MDException& e) {
            eos_thread_err("parent_id=%llu not found", cid);
            gOFS->MgmStats.Add("CommitFailedUnlinked", 0, 0, 1);
            return Emsg(epname, error, EIDRM,
                        "commit file, parent contrainer removed [EIDRM]", "");
          }

          eos::IQuotaNode* ns_quota = eosView->getQuotaNode(dir.get());

          // Free previous quota
          if (ns_quota) {
            ns_quota->removeFile(fmd.get());
          }

          fmd->addLocation(fsid);

          // If fsid is in the deletion list, we try to remove it if there
          // is something in the deletion list
          if (fmd->getNumUnlinkedLocation()) {
            fmd->removeLocation(fsid);
          }

          if (dropfsid) {
            eos_thread_debug("commit: dropping replica on fs %lu", dropfsid);
            fmd->unlinkLocation((unsigned short) dropfsid);
          }

          if (commitsize) {
            fmdname = fmd->getName();

            if ((fmd->getSize() != size) || modified) {
              eos_thread_debug("size difference forces mtime %lld %lld or "
                               "ismodified=%d", fmd->getSize(), size, modified);
              isUpdate = true;
            }

            fmd->setSize(size);
          }

          if (ns_quota) {
            ns_quota->addFile(fmd.get());
          }
        }

        if (occhunk && commitsize) {
          // store the index in flags;
          fmd->setFlags(oc_n + 1);
          eos_thread_info("subcmd=commit max-chunks=%d commited-chunks=%d", oc_max,
                          fmd->getFlags());

          // The last chunk terminates all
          if (oc_max == (oc_n + 1)) {
            // we are done with chunked upload, remove the flags counter
            fmd->setFlags((S_IRWXU | S_IRWXG | S_IRWXO));
            ocdone = true;
          }
        }

        if (commitchecksum) {
          if (!isUpdate) {
            for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
              if (fmd->getChecksum().getDataPadded(i) != checksumbuffer.getDataPadded(i)) {
                eos_thread_debug("checksum difference forces mtime");
                isUpdate = true;
              }
            }
          }

          fmd->setChecksum(checksumbuffer);
        }

        eos::IFileMD::ctime_t mt;
        mt.tv_sec = mtime;
        mt.tv_nsec = mtimens;
        atomic_path.Init(fmd->getName().c_str());
        atomic_path.DecodeAtomicPath(isVersioning);
        isAtomic = (atomic_path.GetName() != fmd->getName());

        if (isUpdate && mtime) {
          // Update the modification time only if the file contents changed and
          // mtime != 0 (FUSE clients will commit mtime=0 to indicated that they
          // call utimes anyway
          // OC clients set the mtime during a commit!
          if (!isAtomic || occhunk) {
            fmd->setMTime(mt);
          }
        }

        eos_thread_debug("commit: setting size to %llu", fmd->getSize());

        try {
          // check for a temporary Etag and remove it
          std::string tmpEtag = "sys.tmp.etag";

          if (fmd->hasAttribute(tmpEtag) && (!isAtomic || occhunk)
              && (commitsize || commitchecksum)) {
            fmd->removeAttribute(tmpEtag);
          }

          gOFS->eosView->updateFileStore(fmd.get());
          cmd = gOFS->eosDirectoryService->getContainerMD(cid);

          if (isUpdate) {
            if (cmd->hasAttribute(tmpEtag)) {
              cmd->removeAttribute(tmpEtag);
            }

            // update parent mtime
            cmd->setMTimeNow();
            gOFS->eosView->updateContainerStore(cmd.get());

            // Broadcast to the fusex network only if the change has been
            // triggered outside the fusex client network e.g. xrdcp etc.
            if (!fusex) {
              gOFS->FuseXCastContainer(cmd->getIdentifier());
            }

            cmd->notifyMTimeChange(gOFS->eosDirectoryService);
          }
        } catch (eos::MDException& e) {
          errno = e.getErrno();
          std::string errmsg = e.getMessage().str();
          eos_thread_debug("msg=\"exception\" ec=%d emsg=\"%s\"\n",
                           e.getErrno(), e.getMessage().str().c_str());
          gOFS->MgmStats.Add("CommitFailedNamespace", 0, 0, 1);
          return Emsg(epname, error, errno, "commit filesize change",
                      errmsg.c_str());
        }
      }
    }
    {
      // check if this is an atomic path
      std::string dname;
      eos::common::Mapping::VirtualIdentity rootvid;
      eos::common::Mapping::Root(rootvid);
      // Path of a previous version existing before an atomic/versioning upload
      std::string delete_path = "";
      eos_thread_info("commitsize=%d n1=%s n2=%s occhunk=%d ocdone=%d", commitsize,
                      fmdname.c_str(), atomic_path.GetName(), occhunk, ocdone);

      if ((commitsize) && (fmdname != atomic_path.GetName()) && (!occhunk ||
          ocdone)) {
        eos_thread_info("commit: de-atomize file %s => %s", fmdname.c_str(),
                        atomic_path.GetName());
        std::shared_ptr<eos::IContainerMD> dir;
        std::shared_ptr<eos::IContainerMD> versiondir;
        XrdOucString versionedname = "";
        unsigned long long vfid = 0;
        {
          eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);
          std::shared_ptr<eos::IFileMD> versionfmd;

          try {
            dname = gOFS->eosView->getUri(fmd.get());
            eos::common::Path dPath(dname.c_str());
            dname = dPath.GetParentPath();

            if (isVersioning) {
              versionfmd = gOFS->eosView->getFile(dname + atomic_path.GetPath());
              vfid = versionfmd->getId();
            }
          } catch (eos::MDException& e) {
            errno = e.getErrno();
            eos_debug("msg=\"exception\" ec=%d emsg=\"%s\"\n",
                      e.getErrno(), e.getMessage().str().c_str());
          }
        }

        // check if we want versioning
        if (isVersioning) {
          eos_static_info("checked  %s%s vfid=%llu", dname.c_str(), atomic_path.GetPath(),
                          vfid);

          // We purged the versions before during open, so we just simulate a new
          // one and do the final rename in a transaction
          if (vfid) {
            gOFS->Version(vfid, error, rootvid, 0xffff, &versionedname, true);
          }
        }

        eos::common::Path version_path(versionedname.c_str());
        {
          eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);

          // We have to de-atomize the fmd name here e.g. make the temporary
          // atomic name a persistent name
          try {
            dir = eosView->getContainer(dname);
            fmd = gOFS->eosFileService->getFileMD(fid);

            if (isVersioning && (std::string(version_path.GetPath()) != "/")) {
              std::shared_ptr<eos::IFileMD> versionfmd;

              try {
                versiondir = eosView->getContainer(version_path.GetParentPath());
                // rename the existing path to the version path
                versionfmd = gOFS->eosView->getFile(dname + atomic_path.GetPath());
                dir->removeFile(atomic_path.GetName());
                versionfmd->setName(version_path.GetName());
                versionfmd->setContainerId(versiondir->getId());
                versiondir->addFile(versionfmd.get());
                versiondir->setMTimeNow();
                eosView->updateFileStore(versionfmd.get());
                gOFS->FuseXCastContainer(versiondir->getIdentifier());
                // Update the ownership and mode of the new file to the original
                // one
                fmd->setCUid(versionfmd->getCUid());
                fmd->setCGid(versionfmd->getCGid());
                fmd->setFlags(versionfmd->getFlags());
                eosView->updateFileStore(fmd.get());
              } catch (eos::MDException& e) {
                errno = e.getErrno();
                eos_thread_err("msg=\"exception\" ec=%d emsg=\"%s\"\n",
                               e.getErrno(), e.getMessage().str().c_str());
              }
            }

            std::shared_ptr<eos::IFileMD> pfmd;

            // Rename the temporary upload path to the final path
            if ((pfmd = dir->findFile(atomic_path.GetName()))) {
              // Check if we are tagged as that 'latest' atomic upload
              std::string atomic_tag;

              try {
                atomic_tag = pfmd->getAttribute("sys.tmp.atomic");
              } catch (eos::MDException& e) {
              }

              if ((!ocdone) && (atomic_tag != fmd->getName())) {
                // this is not our atomic upload, just abort that and delete the temporary artefact
                delete_path = fmd->getName();
                eos_thread_err("msg=\"we are not the last atomic upload - cleaning %s\"",
                               delete_path.c_str());
                isAbort = true;
              } else {
                eos_thread_info("msg=\"found final path\" %s", atomic_path.GetName());
                // If the target exists we swap the two and then delete the
                // previous one
                delete_path = fmd->getName();
                delete_path += ".delete";
                eosView->renameFile(pfmd.get(), delete_path);
              }
            } else {
              eos_thread_info("msg=\"didn't find path\" %s", atomic_path.GetName());
            }

            if (!isAbort) {
              eosView->renameFile(fmd.get(), atomic_path.GetName());
              eos_thread_info("msg=\"de-atomize file\" fid=%llu atomic-name=%s "
                              "final-name=%s", fmd->getId(), fmd->getName().c_str(),
                              atomic_path.GetName());
            }
          } catch (eos::MDException& e) {
            delete_path = "";
            errno = e.getErrno();
            std::string errmsg = e.getMessage().str();
            eos_thread_err("msg=\"exception\" ec=%d emsg=\"%s\"\n",
                           e.getErrno(), e.getMessage().str().c_str());
          }
        }
      }

      // If there was a previous target file we have to delete the renamed
      // atomic left-over
      if (delete_path.length()) {
        delete_path.insert(0, dname.c_str());
        eos_thread_info("msg=\"delete path\" %s", delete_path.c_str());

        if (gOFS->_rem(delete_path.c_str(), error, rootvid, "")) {
          eos_thread_err("msg=\"failed to remove atomic left-over\" path=%s",
                         delete_path.c_str());
        }
      }

      if (isAbort) {
        return Emsg(epname, error, EREMCHG, "commit replica - overlapping "
                    "atomic upload [EREMCHG] - discarding atomic upload", "");
      }
    }
  } else
  {
    int envlen = 0;
    eos_thread_err("commit message does not contain all meta information: %s",
                   env.Env(envlen));
    gOFS->MgmStats.Add("CommitFailedParameters", 0, 0, 1);

    if (spath) {
      return Emsg(epname, error, EINVAL,
                  "commit filesize change - size,fid,fsid,mtime not complete", spath);
    } else {
      return Emsg(epname, error, EINVAL,
                  "commit filesize change - size,fid,fsid,mtime,path not complete", "unknown");
    }
  }

  gOFS->MgmStats.Add("Commit", 0, 0, 1);
  const char* ok = "OK";
  error.setErrInfo(strlen(ok) + 1, ok);
  EXEC_TIMING_END("Commit");
  return SFS_DATA;
}
