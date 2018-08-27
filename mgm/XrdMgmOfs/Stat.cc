// ----------------------------------------------------------------------
// File: Stat.cc
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
XrdMgmOfs::stat(const char* inpath,
                struct stat* buf,
                XrdOucErrInfo& error,
                const XrdSecEntity* client,
                const char* ininfo
               )
{
  return stat(inpath, buf, error, 0, client, ininfo, false);
}

/*----------------------------------------------------------------------------*/
int
XrdMgmOfs::stat(const char* inpath,
                struct stat* buf,
                XrdOucErrInfo& error,
                std::string* etag,
                const XrdSecEntity* client,
                const char* ininfo,
                bool follow,
                std::string* uri)
/*----------------------------------------------------------------------------*/
/*
 * @brief return stat information for a given path
 *
 * @param inpath path to stat
 * @param buf stat buffer where to store the stat information
 * @param error error object
 * @param client XRootD authentication object
 * @param ininfo CGI
 * @param etag string to return the ETag for that object
 * @param follow to indicate to follow symbolic links on leave nodes
 * @return SFS_OK on success otherwise SFS_ERROR
 *
 * See the internal implemtation _stat for details.
 */
/*----------------------------------------------------------------------------*/

{
  static const char* epname = "stat";
  const char* tident = error.getErrUser();
  // use a thread private vid
  eos::common::Mapping::VirtualIdentity vid;
  eos::common::Mapping::Nobody(vid);
  NAMESPACEMAP;
  BOUNCE_ILLEGAL_NAMES;
  XrdOucEnv Open_Env(ininfo);
  AUTHORIZE(client, &Open_Env, AOP_Stat, "stat", inpath, error);
  EXEC_TIMING_BEGIN("IdMap");
  eos::common::Mapping::IdMap(client, ininfo, tident, vid, false);
  EXEC_TIMING_END("IdMap");
  gOFS->MgmStats.Add("IdMap", vid.uid, vid.gid, 1);
  BOUNCE_NOT_ALLOWED;
  ACCESSMODE_R;
  MAYSTALL;
  eos::common::Path cPath(path);

  // Never redirect stat's for the master mode
  if (cPath.GetFullPath() != gOFS->MgmProcMasterPath) {
    MAYREDIRECT;
  }

  errno = 0;
  int rc = _stat(path, buf, error, vid, ininfo, etag, follow, uri);
  bool onDisk = ((buf->st_mode & EOS_TAPE_MODE_T) ? buf->st_nlink - 1 :
                 buf->st_nlink) > 0;

  if (!onDisk) {
    buf->st_rdev |= XRDSFS_OFFLINE;
  } else {
    buf->st_rdev &= ~XRDSFS_OFFLINE;
  }

  if (rc && (errno == ENOENT)) {
    MAYREDIRECT_ENOENT;
    MAYSTALL_ENOENT;
  }

  return rc;
}

/*----------------------------------------------------------------------------*/
int
XrdMgmOfs::_stat(const char* path,
                 struct stat* buf,
                 XrdOucErrInfo& error,
                 eos::common::Mapping::VirtualIdentity& vid,
                 const char* ininfo,
                 std::string* etag,
                 bool follow,
                 std::string* uri)
/*----------------------------------------------------------------------------*/
/*
 * @brief return stat information for a given path
 *
 * @param inpath path to stat
 * @param buf stat buffer where to store the stat information
 * @param error error object
 * @param vid virtual identity of the client
 * @param ininfo CGI
 * @param follow to indicate to follow symbolic links on leave nodes
 * @return SFS_OK on success otherwise SFS_ERROR
 *
 * We don't apply any access control on stat calls for performance reasons.
 * Modification times of directories are only emulated and returned from an
 * in-memory map.
 */
/*----------------------------------------------------------------------------*/
{
  static const char* epname = "_stat";
  EXEC_TIMING_BEGIN("Stat");
  gOFS->MgmStats.Add("Stat", vid.uid, vid.gid, 1);
  // ---------------------------------------------------------------------------
  // try if that is a file
  errno = 0;
  std::shared_ptr<eos::IFileMD> fmd;
  eos::common::Path cPath(path);

  // Stat on the master proc entry succeeds only if this MGM is in RW master mode
  if (cPath.GetFullPath() == gOFS->MgmProcMasterPath) {
    if (!gOFS->mMaster->IsMaster()) {
      return Emsg(epname, error, ENODEV, "stat", cPath.GetPath());
    }
  }

  // Prefetch path
  // TODO(gbitzes): This could be more precise..
  eos::Prefetcher::prefetchFileMDAndWait(gOFS->eosView, cPath.GetPath(), follow);
  eos::Prefetcher::prefetchContainerMDAndWait(gOFS->eosView, cPath.GetPath(),
      follow);
  eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);

  try {
    fmd = gOFS->eosView->getFile(cPath.GetPath(), follow);

    if (uri) {
      *uri = gOFS->eosView->getUri(fmd.get());
    }
  } catch (eos::MDException& e) {
    errno = e.getErrno();
    eos_debug("msg=\"exception\" ec=%d emsg=\"%s\"", e.getErrno(),
              e.getMessage().str().c_str());

    if (errno == ELOOP) {
      return Emsg(epname, error, errno, "stat", cPath.GetPath());
    }
  }

  if (fmd) {
    memset(buf, 0, sizeof(struct stat));
    buf->st_dev = 0xcaff;
    buf->st_ino = eos::common::FileId::FidToInode(fmd->getId());

    if (fmd->isLink()) {
      buf->st_nlink = 1;
    } else {
      buf->st_nlink = fmd->getNumLocation();
    }

    buf->st_size = fmd->getSize();
    buf->st_mode = eos::modeFromMetadataEntry(fmd);
    buf->st_uid = fmd->getCUid();
    buf->st_gid = fmd->getCGid();
    buf->st_rdev = 0; /* device type (if inode device) */
    buf->st_blksize = 512;
    buf->st_blocks = Quota::MapSizeCB(fmd.get()) / 512; // including layout factor
    eos::IFileMD::ctime_t atime;
    // adding also nanosecond to stat struct
    fmd->getCTime(atime);
#ifdef __APPLE__
    buf->st_ctimespec.tv_sec = atime.tv_sec;
    buf->st_ctimespec.tv_nsec = atime.tv_nsec;
#else
    buf->st_ctime = atime.tv_sec;
    buf->st_ctim.tv_sec = atime.tv_sec;
    buf->st_ctim.tv_nsec = atime.tv_nsec;
#endif
    fmd->getMTime(atime);
#ifdef __APPLE__
    buf->st_mtimespec.tv_sec = atime.tv_sec;
    buf->st_mtimespec.tv_nsec = atime.tv_nsec;
    buf->st_atimespec.tv_sec = atime.tv_sec;
    buf->st_atimespec.tv_nsec = atime.tv_nsec;
#else
    buf->st_mtime = atime.tv_sec;
    buf->st_mtim.tv_sec = atime.tv_sec;
    buf->st_mtim.tv_nsec = atime.tv_nsec;
    buf->st_atime = atime.tv_sec;
    buf->st_atim.tv_sec = atime.tv_sec;
    buf->st_atim.tv_nsec = atime.tv_nsec;
#endif

    if (etag) {
      // if there is a checksum we use the checksum, otherwise we return inode+mtime
      size_t cxlen = eos::common::LayoutId::GetChecksumLen(fmd->getLayoutId());

      if (cxlen) {
        // use inode + checksum
        char setag[256];
        snprintf(setag, sizeof(setag) - 1, "\"%llu:", (unsigned long long) buf->st_ino);

        // if MD5 checksums are used we omit the inode number in the ETag (S3 wants that)
        if (eos::common::LayoutId::GetChecksum(fmd->getLayoutId()) ==
            eos::common::LayoutId::kMD5) {
          *etag = "\"";
        } else {
          *etag = setag;
        }

        for (unsigned int i = 0; i < cxlen; i++) {
          char hb[3];
          sprintf(hb, "%02x", (i < cxlen) ? (unsigned char)(
                    fmd->getChecksum().getDataPadded(i)) : 0);
          *etag += hb;
        }

        *etag += "\"";
      } else {
        // use inode + mtime
        char setag[256];
        snprintf(setag, sizeof(setag) - 1, "\"%llu:%llu\"",
                 (unsigned long long) buf->st_ino,
                 (unsigned long long) buf->st_mtime);
        *etag = setag;
      }

      // check for a forced etag
      std::string tmpEtag = "sys.tmp.etag";

      if (fmd->hasAttribute(tmpEtag)) {
        *etag = fmd->getAttribute(tmpEtag);
      }
    }

    EXEC_TIMING_END("Stat");
    return SFS_OK;
  }

  // Check if it's a directory
  std::shared_ptr<eos::IContainerMD> cmd;
  errno = 0;

  // ---------------------------------------------------------------------------
  try {
    cmd = gOFS->eosView->getContainer(cPath.GetPath(), follow);

    if (uri) {
      *uri = gOFS->eosView->getUri(cmd.get());
    }

    memset(buf, 0, sizeof(struct stat));
    buf->st_dev = 0xcaff;
    buf->st_ino = cmd->getId();
    buf->st_mode = eos::modeFromMetadataEntry(cmd);
    buf->st_nlink = 1;
    buf->st_uid = cmd->getCUid();
    buf->st_gid = cmd->getCGid();
    buf->st_rdev = 0; /* device type (if inode device) */
    buf->st_size = cmd->getTreeSize();
    buf->st_blksize = cmd->getNumContainers() + cmd->getNumFiles();
    buf->st_blocks = 0;
    eos::IContainerMD::ctime_t ctime;
    eos::IContainerMD::ctime_t mtime;
    eos::IContainerMD::ctime_t tmtime;
    cmd->getCTime(ctime);
    cmd->getMTime(mtime);

    if (gOFS->eosSyncTimeAccounting) {
      cmd->getTMTime(tmtime);
    } else
      // if there is no sync time accounting we just use the normal modification time
    {
      tmtime = mtime;
    }

#ifdef __APPLE__
    buf->st_atimespec.tv_sec = tmtime.tv_sec;
    buf->st_mtimespec.tv_sec = mtime.tv_sec;
    buf->st_ctimespec.tv_sec = ctime.tv_sec;
    buf->st_atimespec.tv_nsec = tmtime.tv_nsec;
    buf->st_mtimespec.tv_nsec = mtime.tv_nsec;
    buf->st_ctimespec.tv_nsec = ctime.tv_nsec;
#else
    buf->st_atime = tmtime.tv_sec;
    buf->st_mtime = mtime.tv_sec;
    buf->st_ctime = ctime.tv_sec;
    buf->st_atim.tv_sec = tmtime.tv_sec;
    buf->st_mtim.tv_sec = mtime.tv_sec;
    buf->st_ctim.tv_sec = ctime.tv_sec;
    buf->st_atim.tv_nsec = tmtime.tv_nsec;
    buf->st_mtim.tv_nsec = mtime.tv_nsec;
    buf->st_ctim.tv_nsec = ctime.tv_nsec;
#endif

    if (etag) {
      // use inode + mtime
      char setag[256];
      snprintf(setag, sizeof(setag) - 1, "\"%llx:%llu.%03lu\"",
               (unsigned long long) cmd->getId(), (unsigned long long) buf->st_atime,
               (unsigned long) buf->st_atim.tv_nsec / 1000000);
      *etag = setag;
      // check for a forced etag
      std::string tmpEtag = "sys.tmp.etag";

      if (cmd->hasAttribute(tmpEtag)) {
        *etag = cmd->getAttribute(tmpEtag);
      }
    }

    return SFS_OK;
  } catch (eos::MDException& e) {
    errno = e.getErrno();
    eos_debug("msg=\"exception\" ec=%d emsg=\"%s\"", e.getErrno(),
              e.getMessage().str().c_str());
    return Emsg(epname, error, errno, "stat", cPath.GetPath());
  }
}

// ---------------------------------------------------------------------------
//  get the checksum info of a file
// ---------------------------------------------------------------------------
int
XrdMgmOfs::_getchecksum(const char* Name,
                        XrdOucErrInfo& error,
                        std::string* xstype,
                        std::string* xs,
                        const XrdSecEntity* client,
                        const char* opaque,
                        bool follow)
{
  // ---------------------------------------------------------------------------
  errno = 0;
  std::shared_ptr<eos::IFileMD> fmd;
  eos::common::Path cPath(Name);
  eos::Prefetcher::prefetchFileMDAndWait(gOFS->eosView, cPath.GetPath(), follow);
  eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);

  try {
    fmd = gOFS->eosView->getFile(cPath.GetPath(), follow);
  } catch (eos::MDException& e) {
    errno = e.getErrno();
    eos_debug("msg=\"exception\" ec=%d emsg=\"%s\"", e.getErrno(),
              e.getMessage().str().c_str());
    return errno;
  }

  if (fmd) {
    size_t cxlen = eos::common::LayoutId::GetChecksumLen(fmd->getLayoutId());

    if (cxlen) {
      *xstype = eos::common::LayoutId::GetChecksumStringReal(fmd->getLayoutId());

      for (unsigned int i = 0; i < cxlen; i++) {
        char hb[3];
        sprintf(hb, "%02x", (i < cxlen) ? (unsigned char)(
                  fmd->getChecksum().getDataPadded(i)) : 0);
        *xs += hb;
      }
    }
  }

  return 0;
}
//------------------------------------------------------------------------------
// Stat following links (not existing in EOS - behaves like stat)
//------------------------------------------------------------------------------
int
XrdMgmOfs::lstat(const char* path,
                 struct stat* buf,
                 XrdOucErrInfo& error,
                 const XrdSecEntity* client,
                 const char* info)

{
  return stat(path, buf, error, client, info);
}
