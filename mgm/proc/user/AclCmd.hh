//------------------------------------------------------------------------------
//! @file AclCmd.hh
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2017 CERN/Switzerland                                  *
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
 * You should have received a copy of the AGNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#pragma once
#include "mgm/Namespace.hh"
#include "mgm/proc/ProcCommand.hh"
#include "common/Acl.pb.h"
#include "common/ConsoleRequest.pb.h"
#include <unordered_map>

EOSMGMNAMESPACE_BEGIN

typedef std::pair<std::string, unsigned short> Rule;
typedef std::unordered_map<std::string, unsigned short> RuleMap;

//------------------------------------------------------------------------------
//! Class AclCmd - class hadling acl command from a client
//------------------------------------------------------------------------------
class AclCmd: public ProcCommand
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //!
  //! @param req client ProtocolBuffer request
  //----------------------------------------------------------------------------
  AclCmd(eos::console::RequestProto&& req,
         eos::common::Mapping::VirtualIdentity& vid):
    ProcCommand(vid), mExecRequest(false), mReqProto(std::move(req))
  {}

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~AclCmd() = default;

  //----------------------------------------------------------------------------
  //! Open a proc command e.g. call the appropriate user or admin commmand and
  //! store the output in a resultstream of in case of find in temporary output
  //! files.
  //! @note This method can also stall the client if the response is not ready
  //!       within 5 seconds. This behaviour avoids the scenario in which the
  //!       client resubmitts the same command if he doesn't get a reply within
  //!       the STREAM_TIMEOUT (60 seconds).
  //!
  //! @param inpath path indicating user or admin command
  //! @param info CGI describing the proc command
  //! @param vid_in virtual identity of the user requesting a command
  //! @param error object to store errors
  //!
  //! @return SFS_OK in any case
  //----------------------------------------------------------------------------
  int open(const char* path, const char* info,
           eos::common::Mapping::VirtualIdentity& vid,
           XrdOucErrInfo* error) override;

  //----------------------------------------------------------------------------
  //! Read a part of the result stream created during open
  //!
  //! @param boff offset where to start
  //! @param buff buffer to store stream
  //! @param blen len to return
  //!
  //! @return number of bytes read
  //----------------------------------------------------------------------------
  int read(XrdSfsFileOffset offset, char* buff, XrdSfsXferSize blen) override;

  //----------------------------------------------------------------------------
  //! Method implementing the specific behvior of the command executed by the
  //! asynchronous thread
  //----------------------------------------------------------------------------
  void ProcessRequest() override;

private:
  //! Enumerator defining which bit represents which acl flag.
  enum ACLPos {
    R  = 1 << 0,   // 1    -  r
    W  = 1 << 1,   // 2    -  w
    X  = 1 << 2,   // 4    -  x
    M  = 1 << 3,   // 8    -  m
    nM = 1 << 4,   // 16   - !m
    nD = 1 << 5,   // 32   - !d
    pD = 1 << 6,   // 64   - +d
    nU = 1 << 7,   // 128  - !u
    pU = 1 << 8,   // 256  - +u
    Q  = 1 << 9,   // 512  -  q
    C  = 1 << 10   // 1024 -  c
  };

  std::string mTmpResp; ///< String used for streaming the response
  bool mExecRequest; ///< Indicate if request is launched asynchronously
  eos::console::RequestProto mReqProto; ///< Client request protobuf object
  std::string mId; ///< Rule identifier extracted from command line
  ///< ACL rule bitmasks for adding and removing
  unsigned short mAddRule, mRmRule;
  bool mSet; ///< Rule is set operations i.e contains =

  //----------------------------------------------------------------------------
  //! Get sys.acl and user.acl for a given path
  //!
  //! @param path path to get the ACLs for
  //! @param acls ACL VALUE
  //! @param is_sys if true return sys.acl, otherwise user.acl
  //! @param take_lock if true take namespace lock, otherwise don't
  //----------------------------------------------------------------------------
  void GetAcls(const std::string& path, std::string& acls, bool is_sys = false,
               bool take_lock = true);

  //----------------------------------------------------------------------------
  //! Modify the acls for a path
  //!
  //! @param acl acl ProtoBuf object
  //!
  //! @return 0 if successful, otherwise errno
  //----------------------------------------------------------------------------
  int ModifyAcls(const eos::console::AclProto& acl);

  //----------------------------------------------------------------------------
  //! Get ACL rule from string by creating a pair of identifier for the ACL and
  //! the bitmask representation.
  //!
  //! @param in ACL string
  //!
  //! @return std::pair containing ACL identifier (ie. u:user1 or g:group1)
  //! and the bitmask representation
  //----------------------------------------------------------------------------
  Rule GetRuleFromString(const std::string& in) const;

  //----------------------------------------------------------------------------
  //! Generate rule map from the string representation of the acls. If there
  //! are no acls then the rmap will be empty.
  //!
  //! @param acl_string string containing acl
  //! @param rmap map to be filled with acl rules
  //----------------------------------------------------------------------------
  void GenerateRuleMap(const std::string& acl_string, RuleMap& rmap) const;

  //----------------------------------------------------------------------------
  //! Generate acl string representation from a rule map
  //!
  //! @param rmap map of rules to be used for conversion
  //!
  //! @return true if conversion successful, otherwise false
  //----------------------------------------------------------------------------
  std::string GenerateAclString(const RuleMap& rmap) const;

  //----------------------------------------------------------------------------
  //! Convert acl modification command into bitmask rule format
  //!
  //! @param input string containing the modifications of the acls
  //! @param set if true "set" mode is active, otherwise flase
  //!
  //! @return bool true if conversion successful, otherwise false
  //----------------------------------------------------------------------------
  bool GetRuleBitmask(const std::string& input, bool set = false);

  //----------------------------------------------------------------------------
  //! Parse command line (modification) rule given by the client. This specifies
  //! the modifications to be operated on the current acls of the dir(s).
  //!
  //! @param input string rule from command line
  //!
  //! @return bool true if rule is correct, otherwise false
  //----------------------------------------------------------------------------
  bool ParseRule(const std::string& input);

  //----------------------------------------------------------------------------
  //! Check if id has the correct format
  //!
  //! @param id string containing id
  //!
  //! @return bool true if correct, otherwise false
  //----------------------------------------------------------------------------
  bool CheckCorrectId(const std::string& id) const;

  //----------------------------------------------------------------------------
  //! Apply client modification rule(s) to the acls of the current entry
  //!
  //! @param rules map of acl rules for the current entry (directory)
  //----------------------------------------------------------------------------
  void ApplyRule(RuleMap& rules);

  //----------------------------------------------------------------------------
  //! Convert ACL bitmask to string representation
  //!
  //! @param in ACL bitmask
  //!
  //! @return std::string representation of ACL
  //----------------------------------------------------------------------------
  std::string AclBitmaskToString(const unsigned short in) const;
};

EOSMGMNAMESPACE_END