//------------------------------------------------------------------------------
//! @file IProcCommand.cc
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
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#include "common/Path.hh"
#include "common/CommentLog.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/proc/IProcCommand.hh"
#include "mgm/proc/ProcInterface.hh"
#include "mgm/Access.hh"
#include "mgm/Macros.hh"
#include "namespace/interface/IView.hh"
#include "json/json.h"
#include <google/protobuf/util/json_util.h>

EOSMGMNAMESPACE_BEGIN

std::atomic_uint_least64_t IProcCommand::uuid{0};
std::map<eos::console::RequestProto::CommandCase, std::atomic<uint64_t>>
    IProcCommand::mCmdsExecuting;

//------------------------------------------------------------------------------
// Open a proc command e.g. call the appropriate user or admin command and
// store the output in a resultstream or in case of find in a temporary output
// file.
//------------------------------------------------------------------------------
int
IProcCommand::open(const char* path, const char* info,
                   eos::common::Mapping::VirtualIdentity& vid,
                   XrdOucErrInfo* error)
{
  // @todo (esindril): configure delay based on the type of command
  int delay = 5;

  if (!mExecRequest) {
    if (HasSlot(mReqProto)) {
      LaunchJob();
      mExecRequest = true;
    } else {
      eos_notice("%s", SSTR("cmd_type=" << mReqProto.command_case() <<
                            " no more slots, stall client 3 seconds").c_str());
      return delay - 2;
    }
  }

  if (mFuture.wait_for(std::chrono::seconds(delay)) !=
      std::future_status::ready) {
    // Stall the client
    std::string msg = "command not ready, stall the client 5 seconds";
    eos_notice("%s", msg.c_str());
    error->setErrInfo(0, msg.c_str());
    return delay;
  } else {
    eos::console::ReplyProto reply = mFuture.get();

    // Output is written in file
    if (!ofstdoutStreamFilename.empty() && !ofstderrStreamFilename.empty()) {
      ifstdoutStream.open(ofstdoutStreamFilename, std::ifstream::in);
      ifstderrStream.open(ofstderrStreamFilename, std::ifstream::in);
      iretcStream.str(std::string("&mgm.proc.retc=") + std::to_string(reply.retc()));
      readStdOutStream = true;
    } else {
      std::ostringstream oss;

      if (mReqProto.format() == eos::console::RequestProto::JSON) {
        ConvertToJsonFormat(reply, oss);
      } else if (mReqProto.format() == eos::console::RequestProto::FUSE) {
        // @todo (esindril) This format should be dropped and the client should
        // just parse the stdout response. For example the FST dumpmd should do
        // this.
        oss << reply.std_out();
      } else {
        oss << "mgm.proc.stdout=" << reply.std_out()
            << "&mgm.proc.stderr=" << reply.std_err()
            << "&mgm.proc.retc=" << reply.retc();
      }

      mTmpResp = oss.str();
    }

    // Store the client's command comment in the comments logbook
    if ((vid.uid <= 2) || (vid.sudoer)) {
      // Only instance users or sudoers can add to the logbook
      if (mComment.length() && gOFS->mCommentLog) {
        std::string argsJson;
        (void) google::protobuf::util::MessageToJsonString(mReqProto, &argsJson);

        if (!gOFS->mCommentLog->Add(mTimestamp, "", "", argsJson.c_str(),
                                    mComment.c_str(), stdErr.c_str(),
                                    reply.retc())) {
          eos_err("failed to log to comments logbook");
        }
      }
    }
  }

  --mCmdsExecuting[mReqProto.command_case()];
  return SFS_OK;
}

//------------------------------------------------------------------------------
// Read a part of the result stream created during open
//------------------------------------------------------------------------------
size_t
IProcCommand::read(XrdSfsFileOffset offset, char* buff, XrdSfsXferSize blen)
{
  size_t cpy_len = 0;

  if (readStdOutStream && ifstdoutStream.is_open() && ifstderrStream.is_open()) {
    ifstdoutStream.read(buff, blen);
    cpy_len = (size_t)ifstdoutStream.gcount();

    if (cpy_len < (size_t)blen) {
      readStdOutStream = false;
      readStdErrStream = true;
      ifstderrStream.read(buff + cpy_len, blen - cpy_len);
      cpy_len += (size_t)ifstderrStream.gcount();
    }
  } else if (readStdErrStream && ifstderrStream.is_open()) {
    ifstderrStream.read(buff, blen);
    cpy_len = (size_t)ifstderrStream.gcount();

    if (cpy_len < (size_t)blen) {
      readStdErrStream = false;
      readRetcStream = true;
      iretcStream.read(buff + cpy_len, blen - cpy_len);
      cpy_len += (size_t)iretcStream.gcount();
    }
  } else if (readRetcStream) {
    iretcStream.read(buff, blen);
    cpy_len = (size_t)iretcStream.gcount();

    if (cpy_len < (size_t)blen) {
      readRetcStream = false;
    }
  } else if ((size_t)offset < mTmpResp.length()) {
    cpy_len = std::min((size_t)(mTmpResp.size() - offset), (size_t)blen);
    memcpy(buff, mTmpResp.data() + offset, cpy_len);
  }

  return cpy_len;
}

//------------------------------------------------------------------------------
// Launch command asynchronously, creating the corresponding promise and future
//------------------------------------------------------------------------------
void
IProcCommand::LaunchJob()
{
  if (mDoAsync) {
    mFuture = ProcInterface::sProcThreads.PushTask<eos::console::ReplyProto>
    ([this]() -> eos::console::ReplyProto {
      return ProcessRequest();
    });

    if (EOS_LOGS_DEBUG) {
      eos_debug("%s", ProcInterface::sProcThreads.GetInfo().c_str());
    }
  } else {
    std::promise<eos::console::ReplyProto> promise;
    mFuture = promise.get_future();
    promise.set_value(ProcessRequest());
  }
}

//------------------------------------------------------------------------------
// Check if we can safely delete the current object as there is no async
// thread executing the ProcessResponse method
//------------------------------------------------------------------------------
bool
IProcCommand::KillJob()
{
  if (!mDoAsync) {
    return true;
  }

  mForceKill.store(true);

  if (mFuture.valid()) {
    return (mFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
  } else {
    return true;
  }
}

//------------------------------------------------------------------------------
// Open temporary output files for file based results
//------------------------------------------------------------------------------
bool
IProcCommand::OpenTemporaryOutputFiles()
{
  ostringstream tmpdir;
  tmpdir << "/tmp/eos.mgm/";
  tmpdir << uuid++;
  ofstdoutStreamFilename = tmpdir.str();
  ofstdoutStreamFilename += ".stdout";
  ofstderrStreamFilename = tmpdir.str();
  ofstderrStreamFilename += ".stderr";
  eos::common::Path cPath(ofstdoutStreamFilename.c_str());

  if (!cPath.MakeParentPath(S_IRWXU)) {
    eos_err("Unable to create temporary outputfile directory %s",
            tmpdir.str().c_str());
    return false;
  }

  // own the directory by daemon
  if (::chown(cPath.GetParentPath(), 2, 2)) {
    eos_err("Unable to own temporary outputfile directory %s",
            cPath.GetParentPath());
  }

  ofstdoutStream.open(ofstdoutStreamFilename, std::ofstream::out);
  ofstderrStream.open(ofstderrStreamFilename, std::ofstream::out);

  if ((!ofstdoutStream) || (!ofstderrStream)) {
    if (ofstdoutStream.is_open()) {
      ofstdoutStream.close();
    }

    if (ofstderrStream.is_open()) {
      ofstderrStream.close();
    }

    return false;
  }

  ofstdoutStream << "mgm.proc.stdout=";
  ofstderrStream << "&mgm.proc.stderr=";
  return true;
}

//------------------------------------------------------------------------------
// Open temporary output files for file based results
//------------------------------------------------------------------------------
bool
IProcCommand::CloseTemporaryOutputFiles()
{
  ofstdoutStream.close();
  ofstderrStream.close();
  return !(ofstdoutStream.is_open() || ofstderrStream.is_open());
}

//------------------------------------------------------------------------------
// Format input string as json
//------------------------------------------------------------------------------
void
IProcCommand::ConvertToJsonFormat(eos::console::ReplyProto& reply,
                                  std::ostringstream& oss)
{
  Json::Value json;
  Json::Value jsonresult;
  json["errormsg"] = reply.std_err();
  std::stringstream ss;
  ss << reply.retc();
  json["retc"] = ss.str();
  ss.str(reply.std_out());
  std::string line;

  do {
    Json::Value jsonentry;
    line.clear();

    if (!std::getline(ss, line)) {
      break;
    }

    if (!line.length()) {
      continue;
    }

    XrdOucString sline = line.c_str();

    while (sline.replace("<n>", "n")) {}

    while (sline.replace("?configstatus@rw", "_rw")) {}

    line = sline.c_str();
    std::map <std::string , std::string> map;
    eos::common::StringConversion::GetKeyValueMap(line.c_str(), map, "=", " ");
    // These values violate the JSON hierarchy and have to be rewritten
    eos::common::StringConversion::ReplaceMapKey(map, "cfg.balancer",
        "cfg.balancer.status");
    eos::common::StringConversion::ReplaceMapKey(map, "cfg.geotagbalancer",
        "cfg.geotagbalancer.status");
    eos::common::StringConversion::ReplaceMapKey(map, "cfg.geobalancer",
        "cfg.geobalancer.status");
    eos::common::StringConversion::ReplaceMapKey(map, "cfg.groupbalancer",
        "cfg.groupbalancer.status");
    eos::common::StringConversion::ReplaceMapKey(map, "cfg.wfe", "cfg.wfe.status");
    eos::common::StringConversion::ReplaceMapKey(map, "cfg.lru", "cfg.lru.status");
    eos::common::StringConversion::ReplaceMapKey(map, "stat.health",
        "stat.health.status");
    eos::common::StringConversion::ReplaceMapKey(map, "balancer",
        "balancer.status");
    eos::common::StringConversion::ReplaceMapKey(map, "converter",
        "converter.status");
    eos::common::StringConversion::ReplaceMapKey(map, "geotagbalancer",
        "geotagbalancer.status");
    eos::common::StringConversion::ReplaceMapKey(map, "geobalancer",
        "geobalancer.status");
    eos::common::StringConversion::ReplaceMapKey(map, "groupbalancer",
        "groupbalancer.status");

    for (auto it = map.begin(); it != map.end(); ++it) {
      std::vector<std::string> token;
      eos::common::StringConversion::Tokenize(it->first, token, ".");
      char* conv;
      double val;
      errno = 0;
      val = strtod(it->second.c_str(), &conv);
      std::string value;

      if (it->second.length()) {
        value = it->second.c_str();
      } else {
        value = "NULL";
        continue;
      }

      if (token.empty()) {
        continue;
      }

      auto* jep = &(jsonentry[token[0]]);

      for (int i = 1; i < (int)token.size(); i++) {
        jep = &((*jep)[token[i]]);
      }

      if (errno || (!val && (conv  == it->second.c_str())) ||
          ((conv - it->second.c_str()) != (long long)it->second.length())) {
        // non numeric
        (*jep) = value;
      } else {
        // numeric
        (*jep) = val;
      }
    }

    jsonresult.append(jsonentry);
  } while (true);

  json["result"] = jsonresult;
  oss << "mgm.proc.stdout=" << json
      << "&mgm.proc.stderr=" << reply.std_err()
      << "&mgm.proc.retc=" << reply.retc();
}

//------------------------------------------------------------------------------
// Get a file's full path using the fid information stored in the opaque data
//------------------------------------------------------------------------------
void
IProcCommand::GetPathFromFid(XrdOucString& path, unsigned long long fid,
                             const std::string& err_msg)
{
  if (path == "") {
    if (fid == 0ULL) {
      stdErr += "error: fid unknown!";
      retc = errno;
      return;
    }

    try {
      std::string temp =
        gOFS->eosView->getUri(gOFS->eosFileService->getFileMD(fid).get());
      path = XrdOucString(temp.c_str());
    } catch (eos::MDException& e) {
      errno = e.getErrno();
      stdErr = err_msg.c_str();
      stdErr += e.getMessage().str().c_str();
      stdErr += "\n";
      eos_debug("caught exception %d %s\n",
                e.getErrno(), e.getMessage().str().c_str());
    }
  }
}

//------------------------------------------------------------------------------
// Get a directory full path using the cid information stored in the opaque data
//------------------------------------------------------------------------------
void
IProcCommand::GetPathFromCid(XrdOucString& path, unsigned long long cid,
                             const std::string& err_msg)
{
  if (path == "") {
    if (cid == 0ULL) {
      stdErr += "error: cid unknown!";
      retc = errno;
      return;
    }

    try {
      std::string temp =
        gOFS->eosView->getUri(gOFS->eosDirectoryService->getContainerMD(cid).get());
      path = XrdOucString(temp.c_str());
    } catch (eos::MDException& e) {
      errno = e.getErrno();
      stdErr = err_msg.c_str();
      stdErr += e.getMessage().str().c_str();
      stdErr += "\n";
      eos_debug("caught exception %d %s\n",
                e.getErrno(), e.getMessage().str().c_str());
    }
  }
}

//------------------------------------------------------------------------------
// Check if operation forbidden
//------------------------------------------------------------------------------
int
IProcCommand::IsOperationForbidden(const char* inpath)
{
  PROC_BOUNCE_NOT_ALLOWED;
  return SFS_ERROR;
}

//------------------------------------------------------------------------------
// Check if there is still an available slot for the current type of command
//------------------------------------------------------------------------------
bool
IProcCommand::HasSlot(const eos::console::RequestProto& req_proto)
{
  static bool init = false;

  // Initialize only once in the beginning
  if (!init) {
    init = true;

    for (const auto& type : {
    eos::console::RequestProto::kAcl,
        eos::console::RequestProto::kNs,
        eos::console::RequestProto::kDrain,
        eos::console::RequestProto::kFind,
        eos::console::RequestProto::kFs,
        eos::console::RequestProto::kRm,
        eos::console::RequestProto::kStagerRm,
        eos::console::RequestProto::kRoute
  }) {
      mCmdsExecuting.emplace(type, 0ull);
    }
  }

  uint64_t slot_limit {50};
  auto it = mCmdsExecuting.find(req_proto.command_case());

  if (it == mCmdsExecuting.end()) {
    // This should not happen unless you forgot to populate the map in the
    // section above
    mCmdsExecuting[req_proto.command_case()] = 1;
  } else {
    if (it->second >= slot_limit) {
      return false;
    } else {
      ++it->second;
    }
  }

  return true;
}

EOSMGMNAMESPACE_END
