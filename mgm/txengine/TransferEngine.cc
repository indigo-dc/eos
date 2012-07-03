// ----------------------------------------------------------------------
// File: TransferEngine.cc
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

/*----------------------------------------------------------------------------*/
#include "mgm/txengine/TransferEngine.hh"
#include "mgm/txengine/TransferFsDB.hh"
#include "mgm/FsView.hh"
#include "common/StringConversion.hh"
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_BEGIN

TransferEngine gTransferEngine;
const char* TransferEngine::gConfigSchedule = "transfer.schedule";

/*----------------------------------------------------------------------------*/
TransferEngine::TransferEngine()
{
  xDB = (TransferDB*) new TransferFsDB();
  thread = 0;
}

/*----------------------------------------------------------------------------*/
TransferEngine::~TransferEngine()
{
  Stop(false);
  if (xDB) {
    delete xDB;
    xDB=0;
  }
}

/*----------------------------------------------------------------------------*/
bool TransferEngine::Init(const char* connectstring)
{
  if (xDB) return xDB->Init(connectstring);
  return false;
}

/*----------------------------------------------------------------------------*/
int TransferEngine::Run(bool store)
{
  if (!thread) {
    if (store) {
      FsView::gFsView.SetGlobalConfig(TransferEngine::gConfigSchedule, "true");
    }
    XrdSysThread::Run(&thread, TransferEngine::StaticSchedulerProc, static_cast<void *>(this), XRDSYSTHREAD_HOLD, "Transfer Scheduler Thread started");
    return 0;
  }
  return EINVAL;
}

/* ------------------------------------------------------------------------- */
int
TransferEngine::Stop(bool store) {
  if (thread) {
    XrdSysThread::Cancel(thread);
    XrdSysThread::Join(thread,NULL);
    thread = 0;
    if (store) {
      FsView::gFsView.SetGlobalConfig(TransferEngine::gConfigSchedule, "false");
    }
    eos_static_info("Stop transfer engine");
    return 0;
  }
  return EINVAL;
}

/* ------------------------------------------------------------------------- */
void* TransferEngine::StaticSchedulerProc(void* arg){
  return reinterpret_cast<TransferEngine*>(arg)->Scheduler();
}

/* ------------------------------------------------------------------------- */
int
TransferEngine::ApplyTransferEngineConfig() {
  int retc=0;
  std::string scheduling = FsView::gFsView.GetGlobalConfig(TransferEngine::gConfigSchedule);
  if ( (scheduling == "true") || (scheduling == "") ) {
    retc = Run(false);
  }
  if (scheduling == "false") {
    retc = Stop(false);
  }
  return retc;
}

/*----------------------------------------------------------------------------*/
int 
TransferEngine::Submit(XrdOucString& src, XrdOucString& dst, XrdOucString& rate, XrdOucString& streams, XrdOucString& group, XrdOucString& stdOut, XrdOucString& stdErr, eos::common::Mapping::VirtualIdentity& vid, time_t exptime, XrdOucString credentials)
{
  if ( ((!src.beginswith("root://")) &&
	(!src.beginswith("/eos/"))) ||
       ((!dst.beginswith("root://")) &&
	(!dst.beginswith("/eos/"))) ) {
    stdErr += "error: invalid source or destination URL!";
    return EINVAL;
  }
  
  int  irate = atoi(rate.c_str());
  XrdOucString sirate = ""; sirate += irate;
  
  if ( (irate <0) || 
       (sirate != rate) || 
       (irate > 1000000) ) {
    stdErr += "error: rate has to be a positive integer value!";
    return EINVAL;
  }

  int  istreams = atoi(streams.c_str());
  XrdOucString sistreams = ""; sistreams += istreams;
  
  if ( (istreams <0) || 
       (sistreams != streams) || 
       (istreams > 64) ) {
    stdErr += "error: streams has to be a positive integer value and <= 64!";
    return EINVAL;
  }

  if ( group.length() > 128 ) {
    stdErr += "error: the maximum group string can have 128 characters!";
    return EINVAL;
  }
  
  XrdOucString submissionhost=vid.tident.c_str();
  return xDB->Submit(src,dst,rate,streams,group,stdOut,stdErr,vid.uid,vid.gid,time(NULL)+exptime, credentials, submissionhost);
}

/*----------------------------------------------------------------------------*/
int 
TransferEngine::Ls(XrdOucString& option, XrdOucString& group, XrdOucString& stdOut, XrdOucString& stdErr, eos::common::Mapping::VirtualIdentity& vid)
{
  // forbid the 'a' option for non root
  if ( (vid.uid) && (option.find("a") != STR_NPOS) ) {
    stdErr += "error: you have to be root to query transfers of all users\n";
    return EPERM;
  } 
  return xDB->Ls(option,group,stdOut,stdErr,vid.uid,vid.gid);
}

/*----------------------------------------------------------------------------*/
int 
TransferEngine::Cancel(XrdOucString& sid, XrdOucString& group, XrdOucString& stdOut, XrdOucString& stdErr, eos::common::Mapping::VirtualIdentity& vid)
{
  long long id = strtoll(sid.c_str(),0,10);
  if (id) {
    TransferDB::transfer_t transfer = xDB->GetTransfer(id);
    if (transfer.count("error")) {
      stdErr += "error: "; stdErr += transfer["error"].c_str();
      return EINVAL;
    }
    if (transfer.count("uid")) {
      uid_t uid = (uid_t)atoi(transfer["uid"].c_str());
      if ( (vid.uid > 4) && (vid.uid != uid) ) {
	stdErr = "error: you are not the owner of this transfer";
	return EPERM;
    }
    }
    return xDB->Cancel(id,stdOut, stdErr);
  } else {
    // cancel by group
    // query all transfers in a group
    std::vector<long long> ids = xDB->QueryByGroup(group);
    for (size_t i=0; i< ids.size(); i++) {
      TransferDB::transfer_t transfer = xDB->GetTransfer(ids[i]);
      if (transfer.count("uid")) {
	if ( (!vid.uid) || (((int)vid.uid == atoi(transfer["uid"].c_str())) )) {
	  xDB->Cancel(ids[i],stdOut, stdErr);
	} else {
	  stdOut+="warning: skipping id="; stdOut += transfer["id"].c_str(); stdOut += " - you are not the owner!";
	}
      }
    }
    return 0;
  }

}

/*----------------------------------------------------------------------------*/
int 
TransferEngine::Kill(XrdOucString& sid, XrdOucString& group, XrdOucString& stdOut, XrdOucString& stdErr, eos::common::Mapping::VirtualIdentity& vid)
{
  stdErr += "error: 'kill' is currently not supported";
  return EOPNOTSUPP;
}

/*----------------------------------------------------------------------------*/
int 
TransferEngine::Log(XrdOucString& sid, XrdOucString& group, XrdOucString& stdOut, XrdOucString& stdErr, eos::common::Mapping::VirtualIdentity& vid)
{
  long long id = strtoll(sid.c_str(),0,10);
  TransferDB::transfer_t transfer = xDB->GetTransfer(id);
  
  if (transfer.count("log")) {
    stdOut += transfer["log"].c_str();
    return 0;
  } else {
    stdErr += "error: there is no log available for id=%s";stdErr += sid; stdErr += "\n";
    return EINVAL;
  }
}

/*----------------------------------------------------------------------------*/
int 
TransferEngine::Resubmit(XrdOucString& sid, XrdOucString& group, XrdOucString& stdOut, XrdOucString& stdErr, eos::common::Mapping::VirtualIdentity& vid)
{
  long long id = strtoll(sid.c_str(),0,10);
  TransferDB::transfer_t transfer = xDB->GetTransfer(id);
  if (transfer["status"] == "failed") {
    SetState(id,kInserted);
    stdOut += "success: resubmitted transfer id="; stdOut += sid.c_str(); stdOut += "\n";
    return 0;
  } else {
    if (transfer["status"] == "done") {
      stdErr += "error: cannot resubmit <done> transfer with id="; stdErr += sid.c_str(); stdErr += "\n";
    } else {
      stdErr += "error: cannot get a transfer with id="; stdErr += sid.c_str(); stdErr += "\n";
    }
    return EINVAL;
  }
}

/*----------------------------------------------------------------------------*/
int 
TransferEngine::Reset(XrdOucString& stdOut, XrdOucString& stdErr, eos::common::Mapping::VirtualIdentity& vid)
{
  if (vid.uid == 0) {
    SetState(0,kInserted);
    stdOut += "success: all transfers have been reset\n";
    return 0;
  } else {
    stdErr += "error: you have to be 'root' to reset transfers\n";
    return EPERM;
  }
}

/*----------------------------------------------------------------------------*/
int 
TransferEngine::Clear(XrdOucString& stdOut, XrdOucString& stdErr, eos::common::Mapping::VirtualIdentity& vid )
{
  if (vid.uid == 0) {
    stdOut += "success: all transfers have been cleared\n";
    xDB->Clear(stdOut, stdErr);
    return 0;
  } else {
    stdErr += "error: you have to be 'root' to clear transfers\n";
    return EPERM;
  }
}

/*----------------------------------------------------------------------------*/
void*
TransferEngine::Scheduler()
{
  eos_static_info("running transfer scheduler");
  size_t loopsleep=2000000;
  sleep(10);
  size_t gwpos=0;
  while (1) {
    XrdSysThread::SetCancelOff();
    // schedule here
    {
      eos_static_debug("gettaing next transfer");
      TransferDB::transfer_t transfer = GetNextTransfer(kInserted);
      if (transfer.count("error")) {
	eos_static_debug("GetNextTransfer(kInserted) returned %s", transfer["error"].c_str());
      } else {
	if (transfer.count("id")) {
	  long long id = strtoll(transfer["id"].c_str(),0,10);
	  eos_static_info("received transfer id=%lld", id);
	  SetState(id, kValidated);

	  // ------------------------------------------------------------
	  // trivial scheduling engine
	  // ------------------------------------------------------------

	  // ------------------------------------------------------------
	  // select random a gw to deal with it
	  // ------------------------------------------------------------
	  std::set<std::string>::const_iterator it;
	  eos::common::RWMutexReadLock viewlock(FsView::gFsView.GwMutex);
	  gwpos++;
	  size_t gwnpos = gwpos%FsView::gFsView.mGwNodes.size();
	  it = FsView::gFsView.mGwNodes.begin();
	  std::advance(it, gwnpos);
	  eos_static_info("selected gw: %s", it->c_str());

	  // ------------------------------------------------------------------------
	  // assemble a transfer job
	  // ------------------------------------------------------------------------
	  XrdOucString transferjob="";
	  TransferDB::transfer_t transfer = GetTransfer(id);
	  
	  if (transfer.count("id")) {
	    std::vector<std::string> src_tok;
	    std::vector<std::string> dst_tok;
	    eos::common::StringConversion::EmptyTokenize(transfer["src"], src_tok,"?");
	    eos::common::StringConversion::EmptyTokenize(transfer["dst"], dst_tok,"?");
	    if (src_tok.size() && dst_tok.size()) {
	      transferjob    ="source.url="; transferjob += src_tok[0].c_str();
	      if (src_tok.size()==2) {
		// the opaque information has to be sealead against the use of '& '
		XrdOucString env = src_tok[1].c_str();
		transferjob +="&source.env="; transferjob += XrdMqMessage::Seal(env,"_AND_");
	      }
	      transferjob  +="&target.url="; transferjob += dst_tok[0].c_str();
	      if (dst_tok.size()==2) {
		// the opaque information has to be sealead against the use of '&' 
		XrdOucString env = dst_tok[1].c_str();
 		transferjob += "&target.env="; transferjob += XrdMqMessage::Seal(env,"_AND_");
	      }
	      transferjob  +="&tx.id="; transferjob += transfer["id"].c_str();
	      transferjob  +="&tx.streams="; transferjob += transfer["streams"].c_str();
	      transferjob  +="&tx.rate="; transferjob += transfer["rate"].c_str();
	      transferjob  +="&tx.exp="; transferjob += transfer["expires"].c_str();
	      transferjob  +="&tx.uid="; transferjob += transfer["uid"].c_str();
	      transferjob  +="&tx.gid="; transferjob += transfer["gid"].c_str();
	      fprintf(stderr,"%s\n", transferjob.c_str());
	      // now encrypt the security credential 
	      XrdOucString credential = transfer["credential"].c_str();
	      XrdOucString enccredential;
	      eos::common::SymKey* symkey = eos::common::gSymKeyStore.GetCurrentKey();
	      if (credential.length() && symkey && XrdMqMessage::SymmetricStringEncrypt(credential, enccredential, (char*)symkey->GetKey())) {
		transferjob += "&tx.auth.cred=";transferjob += enccredential;
		transferjob += "&tx.auth.digest=";transferjob += symkey->GetDigest64();
	      } 

	      eos::common::TransferJob* txjob = new eos::common::TransferJob(transferjob.c_str());
	      if (FsView::gFsView.mNodeView.count(*it)) {
		if ( FsView::gFsView.mNodeView[*it]->mGwQueue->Add(txjob)) {
		  eos_static_info("Submitted id=%lld to node=%s\n", id, it->c_str());
		  SetState(id, kScheduled);
		}
	      }
	      
	      if (txjob) {
		delete txjob;
	      }

	      }
	  } else {
	    eos_static_err("GetTransfer(id) failed");
	  }
	} else {
	  eos_static_debug("GetNextTransfer(kInserted) returnd no id");
	}
      }
    }
    XrdSysThread::SetCancelOn();
    for (size_t i=0; i< loopsleep/10000; i++) {
      usleep(10000);
      XrdSysThread::CancelPoint();
    }
  }
  return 0;
}

EOSMGMNAMESPACE_END
