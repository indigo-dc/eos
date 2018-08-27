// ----------------------------------------------------------------------
// File: XrdMgmOfsDirectory.cc
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

#include "XrdOuc/XrdOucEnv.hh"
/*----------------------------------------------------------------------------*/
#include "mgm/XrdMgmOfsDirectory.hh"
#include "mgm/Stat.hh"
#include "mgm/XrdMgmOfsTrace.hh"
#include "mgm/XrdMgmOfsSecurity.hh"
#include "mgm/Macros.hh"
#include "mgm/Access.hh"
#include "mgm/Acl.hh"
#include "common/Path.hh"
/*----------------------------------------------------------------------------*/
#include "namespace/interface/IContainerMD.hh"
#include "namespace/interface/IView.hh"
#include "namespace/Prefetcher.hh"
#include "namespace/interface/ContainerIterators.hh"
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/

#ifdef __APPLE__
#define ECOMM 70
#endif

#ifndef S_IAMB
#define S_IAMB  0x1FF
#endif


/*----------------------------------------------------------------------------*/

/******************************************************************************/
/******************************************************************************/
/* MGM Directory Interface                                                    */
/******************************************************************************/
/******************************************************************************/


//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
XrdMgmOfsDirectory::XrdMgmOfsDirectory(char* user, int MonID):
  XrdSfsDirectory(user, MonID)
{
  dirName = "";
  dh.reset();
  d_pnt = &dirent_full.d_entry;
  eos::common::Mapping::Nobody(vid);
  eos::common::LogId();
}

/*----------------------------------------------------------------------------*/
int
XrdMgmOfsDirectory::open(const char* inpath,
                         const XrdSecEntity* client,
                         const char* ininfo)
/*----------------------------------------------------------------------------*/
/*
 * @brief open a directory object with bouncing/mapping & namespace mapping
 *
 * @param inpath directory path to open
 * @param client XRootD authentication object
 * @param ininfo CGI
 *
 * @return SFS_OK otherwise SFS_ERROR
 */
/*----------------------------------------------------------------------------*/
{
  static const char* epname = "opendir";
  const char* tident = error.getErrUser();
  NAMESPACEMAP;
  BOUNCE_ILLEGAL_NAMES;
  XrdOucEnv Open_Env(ininfo);
  AUTHORIZE(client, &Open_Env, AOP_Readdir, "open directory", inpath, error);
  EXEC_TIMING_BEGIN("IdMap");
  eos::common::Mapping::IdMap(client, ininfo, tident, vid);
  EXEC_TIMING_END("IdMap");
  gOFS->MgmStats.Add("IdMap", vid.uid, vid.gid, 1);
  BOUNCE_NOT_ALLOWED;
  ACCESSMODE_R;
  MAYSTALL;
  MAYREDIRECT;
  return _open(path, vid, ininfo);
}

/*----------------------------------------------------------------------------*/
int
XrdMgmOfsDirectory::open(const char* inpath,
                         eos::common::Mapping::VirtualIdentity& vid,
                         const char* ininfo)
/*----------------------------------------------------------------------------*/
/*
 * @brief open a directory object with bouncing & namespace mapping
 *
 * @param dir_path directory path to open
 * @param vid EOS virtual identity
 * @param info CGI
 *
 * @return SFS_OK otherwise SFS_ERROR
 *
 * We create during the open the full directory listing which then is retrieved
 * via nextEntry() and cleaned up with close().
 */
/*----------------------------------------------------------------------------*/
{
  static const char* epname = "opendir";
  NAMESPACEMAP;
  BOUNCE_ILLEGAL_NAMES;
  XrdOucEnv Open_Env(ininfo);
  BOUNCE_NOT_ALLOWED;
  ACCESSMODE_R;
  MAYSTALL;
  MAYREDIRECT;
  return _open(path, vid, ininfo);
}

/*----------------------------------------------------------------------------*/
int
XrdMgmOfsDirectory::_open(const char* dir_path,
                          eos::common::Mapping::VirtualIdentity& vid,
                          const char* info)
/*----------------------------------------------------------------------------*/
/*
 * @brief open a directory object (without bouncing/mapping)
 *
 * @param dir_path directory path to open
 * @param vid EOS virtual identity
 * @param info CGI
 *
 * @return SFS_OK otherwise SFS_ERROR
 *
 * We create during the open the full directory listing which then is retrieved
 * via nextEntry() and cleaned up with close().
 */
/*----------------------------------------------------------------------------*/
{
  static const char* epname = "opendir";
  XrdOucEnv Open_Env(info);
  errno = 0;
  EXEC_TIMING_BEGIN("OpenDir");
  eos::common::Path cPath(dir_path);

  // Skip printout when listing the /eos/<instance/proc/conversion dir
  if ((strstr(dir_path, "/proc/conversion") == nullptr) && (info != nullptr)) {
    eos_info("name=opendir path=%s", cPath.GetPath());
  }

  gOFS->MgmStats.Add("OpenDir", vid.uid, vid.gid, 1);
  // Open the directory
  bool permok = false;
  eos::Prefetcher::prefetchContainerMDWithChildrenAndWait(gOFS->eosView,
      cPath.GetPath());
  // ---------------------------------------------------------------------------
  eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);

  try {
    eos::IContainerMD::XAttrMap attrmap;
    dh = gOFS->eosView->getContainer(cPath.GetPath());
    permok = dh->access(vid.uid, vid.gid, R_OK | X_OK);

    if (!permok) {
      eos::common::Mapping::VirtualIdentity rootvid;
      eos::common::Mapping::Root(rootvid);
      // ACL and permission check
      Acl acl(cPath.GetPath(),
              error,
              vid,
              attrmap,
              false);
      eos_info("acl=%d r=%d w=%d wo=%d x=%d egroup=%d",
               acl.HasAcl(), acl.CanRead(), acl.CanWrite(), acl.CanWriteOnce(),
               acl.CanBrowse(), acl.HasEgroup());

      // browse permission by ACL
      if (acl.HasAcl()) {
        if (acl.CanBrowse()) {
          permok = true;
        }
      }
    }

    if (permok) {
      // Add all the files and subdirectories
      gOFS->MgmStats.Add("OpenDir-Entry", vid.uid, vid.gid,
                         dh->getNumContainers() + dh->getNumFiles());

      // Collect all file names
      for (auto it = eos::FileMapIterator(dh); it.valid(); it.next()) {
        dh_list.insert(it.key());
      }

      // Collect all subcontainers
      for (auto it = eos::ContainerMapIterator(dh); it.valid(); it.next()) {
        dh_list.insert(it.key());
      }

      dh_list.insert(".");

      // The root dir has no .. entry
      if (strcmp(dir_path, "/")) {
        dh_list.insert("..");
      }
    }
  } catch (eos::MDException& e) {
    dh.reset();
    errno = e.getErrno();
    eos_debug("msg=\"exception\" ec=%d emsg=\"%s\"\n",
              e.getErrno(), e.getMessage().str().c_str());
  }

  // check permissions

  if (dh) {
    eos_debug("msg=\"access\" uid=%d gid=%d retc=%d mode=%o",
              vid.uid, vid.gid, (dh->access(vid.uid, vid.gid, R_OK | X_OK)),
              dh->getMode());
  }

  // Verify that this object is not already associated with an open directory
  //
  if (!dh)
    return Emsg(epname, error, errno,
                "open directory", cPath.GetPath());

  if (!permok) {
    errno = EPERM;
    return Emsg(epname, error, errno,
                "open directory", cPath.GetPath());
  }

  dirName = dir_path;
  // Set up values for this directory object
  //
  dh_it = dh_list.begin();
  EXEC_TIMING_END("OpenDir");
  return SFS_OK;
}

/*----------------------------------------------------------------------------*/
const char*
XrdMgmOfsDirectory::nextEntry()
/*----------------------------------------------------------------------------*/
/*
 * @brief read the next directory entry
 *
 * @return name of the next directory entry
 *
 * Upon success, returns the contents of the next directory entry as
 * a null terminated string. Returns a null pointer upon EOF or an
 * error. To differentiate the two cases, getErrorInfo will return
 * 0 upon EOF and an actual error code (i.e., not 0) on error.
 */
/*----------------------------------------------------------------------------*/
{
  if (dh_it == dh_list.end()) {
    // no more entry
    return (const char*) 0;
  }

  auto tmp_it = dh_it;
  dh_it++;
  return tmp_it->c_str();
}

/*----------------------------------------------------------------------------*/
int
XrdMgmOfsDirectory::close()
/*----------------------------------------------------------------------------*/
/*
 * @brief close a directory object
 *
 * @return SFS_OK
 */
/*----------------------------------------------------------------------------*/
{
  //  static const char *epname = "closedir";
  dh_list.clear();
  return SFS_OK;
}


/*----------------------------------------------------------------------------*/
int
XrdMgmOfsDirectory::Emsg(const char* pfx,
                         XrdOucErrInfo& einfo,
                         int ecode,
                         const char* op,
                         const char* target)
/*----------------------------------------------------------------------------*/
/*
 * @brief create an error message for a directory object
 *
 * @param pfx message prefix value
 * @param einfo error text/code object
 * @param ecode error code
 * @param op name of the operation performed
 * @param target target of the operation e.g. file name etc.
 *
 * @return SFS_ERROR in all cases
 *
 * This routines prints also an error message into the EOS log.
 */
/*----------------------------------------------------------------------------*/
{
  char* etext, buffer[4096], unkbuff[64];

  // ---------------------------------------------------------------------------
  // Get the reason for the error
  // ---------------------------------------------------------------------------
  if (ecode < 0) {
    ecode = -ecode;
  }

  if (!(etext = strerror(ecode))) {
    sprintf(unkbuff, "reason unknown (%d)", ecode);
    etext = unkbuff;
  }

  // ---------------------------------------------------------------------------
  // Format the error message
  // ---------------------------------------------------------------------------
  snprintf(buffer, sizeof(buffer), "Unable to %s %s; %s", op, target, etext);

  if (ecode == ENOENT) {
    eos_debug("Unable to %s %s; %s", op, target, etext);
  } else {
    eos_err("Unable to %s %s; %s", op, target, etext);
  }

  // ---------------------------------------------------------------------------
  // Print it out if debugging is enabled
  // ---------------------------------------------------------------------------
#ifndef NODEBUG
  //   XrdMgmOfs::eDest->Emsg(pfx, buffer);
#endif
  // ---------------------------------------------------------------------------
  // Place the error message in the error object and return
  // ---------------------------------------------------------------------------
  einfo.setErrInfo(ecode, buffer);
  return SFS_ERROR;
}
