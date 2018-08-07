/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2018 CERN/Switzerland                                  *
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

//------------------------------------------------------------------------------
//! @author Georgios Bitzes <georgios.bitzes@cern.ch>
//! @brief  Quota node core logic, shared between the two namespaces
//------------------------------------------------------------------------------

#include "namespace/Namespace.hh"
#include "namespace/interface/Identifiers.hh"
#include <map>

EOSNSNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! QuotaNode core logic, which keeps track of user/group volume/inode use for
//! a single quotanode.
//------------------------------------------------------------------------------
class QuotaNodeCore {
public:

  struct UsageInfo {
    UsageInfo(): space(0), physicalSpace(0), files(0) {}
    uint64_t space;
    uint64_t physicalSpace;
    uint64_t files;
  };

  //----------------------------------------------------------------------------
  //! Constructor. The object is initially empty, no files whatsoever are being
  //! accounted.
  //----------------------------------------------------------------------------
  QuotaNodeCore() {}

  //----------------------------------------------------------------------------
  //! Get the amount of space occupied by the given user
  //----------------------------------------------------------------------------
  uint64_t getUsedSpaceByUser(uid_t uid);

  //----------------------------------------------------------------------------
  //! Get the amount of space occupied by the given group
  //----------------------------------------------------------------------------
  uint64_t getUsedSpaceByGroup(gid_t gid);

  //----------------------------------------------------------------------------
  //! Get the amount of space occupied by the given user
  //----------------------------------------------------------------------------
  uint64_t getPhysicalSpaceByUser(uid_t uid);

  //----------------------------------------------------------------------------
  //! Get the amount of space occupied by the given group
  //----------------------------------------------------------------------------
  uint64_t getPhysicalSpaceByGroup(gid_t gid);

  //----------------------------------------------------------------------------
  //! Get the amount of space occupied by the given user
  //----------------------------------------------------------------------------
  uint64_t getNumFilesByUser(uid_t uid);

  //----------------------------------------------------------------------------
  //! Get the amount of space occupied by the given group
  //----------------------------------------------------------------------------
  uint64_t getNumFilesByGroup(gid_t gid);

  //----------------------------------------------------------------------------
  //! Account a new file.
  //----------------------------------------------------------------------------
  void addFile(uid_t uid, gid_t gid, uint64_t size, uint64_t physicalSize);

  //----------------------------------------------------------------------------
  //! Remove a file.
  //----------------------------------------------------------------------------
  void removeFile(uid_t uid, gid_t gid, uint64_t size, uint64_t physicalSize);

private:
  std::map<uid_t, UsageInfo> mUserInfo;
  std::map<gid_t, UsageInfo> mGroupInfo;
};

EOSNSNAMESPACE_END
