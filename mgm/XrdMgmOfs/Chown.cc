// ----------------------------------------------------------------------
// File: Chown.cc
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

/*----------------------------------------------------------------------------*/
int
XrdMgmOfs::_chown(const char* path,
                  uid_t uid,
                  gid_t gid,
                  XrdOucErrInfo& error,
                  eos::common::Mapping::VirtualIdentity& vid,
                  const char* ininfo)
/*----------------------------------------------------------------------------*/
/*
 * @brief change the owner of a file or directory
 *
 * @param path directory path to change
 * @param uid user id to set
 * @param gid group id to set
 * @param error error object
 * @param vid virtual identity of the client
 * @param ininfo CGI
 * @return SFS_OK on success otherwise SFS_ERROR
 *
 * Chown has only an internal implementation because XRootD does not support
 * this operation in the Ofs interface. root can alwasy run the operation.
 * Users with the admin role can run the operation. Normal users can run the operation
 * if they have the 'c' permissions in 'sys.acl'. File ownership can only be changed
 * with the root or admin role. If uid,gid=0xffffffff, we don't set the uid/group
 */
/*----------------------------------------------------------------------------*/

{
  static const char* epname = "chown";
  EXEC_TIMING_BEGIN("Chown");
  // ---------------------------------------------------------------------------
  eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
  std::shared_ptr<eos::IContainerMD> cmd;
  std::shared_ptr<eos::IFileMD> fmd;
  errno = 0;
  gOFS->MgmStats.Add("Chown", vid.uid, vid.gid, 1);
  eos_info("path=%s uid=%u gid=%u", path, uid, gid);

  // try as a directory
  try {
    eos::IContainerMD::XAttrMap attrmap;
    eos::common::Path cPath(path);
    cmd = gOFS->eosView->getContainer(path);
    eos::common::Path pPath(gOFS->eosView->getUri(cmd.get()).c_str());
    eos::IContainerMD::XAttrMap::const_iterator it;
    // ACL and permission check
    Acl acl(pPath.GetParentPath(), error, vid, attrmap, false);
    cmd = gOFS->eosView->getContainer(path);

    if (((vid.uid) && (!eos::common::Mapping::HasUid(3, vid) &&
                       !eos::common::Mapping::HasGid(4, vid)) &&
         !acl.CanChown()) ||
        ((vid.uid) && !acl.IsMutable())) {
      errno = EPERM;
    } else {
      if ((unsigned int) uid != 0xffffffff) {
        // Change the owner
        cmd->setCUid(uid);
      }

      if (((!vid.uid) || (vid.uid == 3) || (vid.gid == 4)) &&
          ((unsigned int)gid != 0xffffffff)) {
        // Change the group
        cmd->setCGid(gid);
      }

      cmd->setCTimeNow();
      eosView->updateContainerStore(cmd.get());
      gOFS->FuseXCastContainer(cmd->getIdentifier());
      errno = 0;
    }
  } catch (eos::MDException& e) {
    errno = e.getErrno();
  }

  if (!cmd) {
    errno = 0;

    try {
      // Try as a file
      eos::common::Path cPath(path);
      cmd = gOFS->eosView->getContainer(cPath.GetParentPath());
      // Translate to path without symlinks
      std::string uri_cmd = eosView->getUri(cmd.get());
      cmd = eosView->getContainer(uri_cmd);
      eos::IQuotaNode* ns_quota = gOFS->eosView->getQuotaNode(cmd.get());

      if ((vid.uid) && (!vid.sudoer) && (vid.uid != 3) && (vid.gid != 4)) {
        errno = EPERM;
      } else {
        fmd = gOFS->eosView->getFile(path);

        // Substract the file
        if (ns_quota) {
          ns_quota->removeFile(fmd.get());
        }

        // Change the owner
        if ((unsigned int) uid != 0xffffffff) {
          fmd->setCUid(uid);
        }

        // Change the group
        if (!vid.uid && ((unsigned int) gid != 0xffffffff)) {
          fmd->setCGid(gid);
        }

        // Re-add the file
        if (ns_quota) {
          ns_quota->addFile(fmd.get());
        }

        fmd->setCTimeNow();
        eosView->updateFileStore(fmd.get());
      }
    } catch (eos::MDException& e) {
      errno = e.getErrno();
    }
  }

  // ---------------------------------------------------------------------------
  if (cmd && (!errno)) {
    EXEC_TIMING_END("Chmod");
    return SFS_OK;
  }

  return Emsg(epname, error, errno, "chown", path);
}
