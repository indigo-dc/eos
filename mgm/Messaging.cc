// ----------------------------------------------------------------------
// File: Messaging.cc
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

#include "mgm/Messaging.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/FsView.hh"
#include "mq/XrdMqTiming.hh"

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
Messaging::Messaging(const char* url, const char* defaultreceiverqueue,
                     bool advisorystatus, bool advisoryquery,
                     XrdMqSharedObjectManager* som)
{
  mSom = som;

  // Add to a broker with the flushbacklog flag since we don't want to
  // block message flow in case of a master/slave MGM where one got stuck or
  // is too slow
  if (gMessageClient.AddBroker(url, advisorystatus, advisoryquery , true)) {
    mIsZombie = false;
  } else {
    mIsZombie = true;
  }

  int spos;
  XrdOucString clientid = url;
  spos = clientid.find("//");

  if (spos != STR_NPOS) {
    spos = clientid.find("//", spos + 1);
    clientid.erase(0, spos + 1);
    gMessageClient.SetClientId(clientid.c_str());
  }

  gMessageClient.Subscribe();
  gMessageClient.SetDefaultReceiverQueue(defaultreceiverqueue);
}

//------------------------------------------------------------------------------
// Infinite loop processing messages
//------------------------------------------------------------------------------
void
Messaging::Listen()
{
  std::unique_ptr<XrdMqMessage> new_msg;
  XrdSysThread::SetCancelDeferred();

  while (true) {
    //eos_static_debug("RecvMessage");
    new_msg.reset(XrdMqMessaging::gMessageClient.RecvMessage());
    // if (new_msg) new_msg->Print();

    if (new_msg) {
      Process(new_msg.get());
    } else {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    XrdSysThread::CancelPoint();
  }

  XrdSysThread::SetCancelOn();
}

//------------------------------------------------------------------------------
// Update based on advisory message
//------------------------------------------------------------------------------
bool
Messaging::Update(XrdAdvisoryMqMessage* advmsg)
{
  if (!advmsg) {
    return false;
  }

  std::string nodequeue = advmsg->kQueue.c_str();
  eos_static_debug("View Lock");
  // new implementations uses mainly read locks
  FsView::gFsView.ViewMutex.LockRead(); // =========| LockRead
  eos_static_debug("View Locked");

  if (!FsView::gFsView.mNodeView.count(nodequeue)) {
    // Rare case where a node is not yet known
    FsView::gFsView.ViewMutex.UnLockRead(); // |========= UnLockRead
    // register the node to the global view and config
    {
      // =========| LockWrite
      eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);

      if (FsView::gFsView.RegisterNode(advmsg->kQueue.c_str())) {
        std::string nodeconfigname = eos::common::GlobalConfig::gConfig.QueuePrefixName(
                                       gOFS->NodeConfigQueuePrefix.c_str(), advmsg->kQueue.c_str());

        if (!eos::common::GlobalConfig::gConfig.Get(nodeconfigname.c_str())) {
          if (!eos::common::GlobalConfig::gConfig.AddConfigQueue(nodeconfigname.c_str(),
              advmsg->kQueue.c_str())) {
            eos_static_crit("cannot add node config queue %s", nodeconfigname.c_str());
          }
        }
      }

      if (FsView::gFsView.mNodeView.count(nodequeue)) {
        if (advmsg->kOnline) {
          FsView::gFsView.mNodeView[nodequeue]->SetStatus("online");
          FsView::gFsView.mNodeView[nodequeue]->SetActiveStatus(
            eos::common::FileSystem::kOnline);
        } else {
          FsView::gFsView.mNodeView[nodequeue]->SetStatus("offline");
          FsView::gFsView.mNodeView[nodequeue]->SetActiveStatus(
            eos::common::FileSystem::kOffline);

          // propagate into filesystem states
          for (auto it = FsView::gFsView.mNodeView[nodequeue]->begin();
               it != FsView::gFsView.mNodeView[nodequeue]->end(); ++it) {
            FsView::gFsView.mIdView[*it]->SetStatus(eos::common::FileSystem::kDown, false);
          }
        }

        eos_static_info("Setting heart beat to %llu\n",
                        (unsigned long long) advmsg->kMessageHeader.kSenderTime_sec);
        FsView::gFsView.mNodeView[nodequeue]->SetHeartBeat(
          advmsg->kMessageHeader.kSenderTime_sec);

        // propagate into filesystems
        for (auto it = FsView::gFsView.mNodeView[nodequeue]->begin();
             it != FsView::gFsView.mNodeView[nodequeue]->end(); ++it) {
          FsView::gFsView.mIdView[*it]->SetLongLong("stat.heartbeattime",
              (long long) advmsg->kMessageHeader.kSenderTime_sec, false);
        }
      }

      // =========| UnLockWrite
    }
    return true;
  } else {
    // here we can go just with a read lock
    if (FsView::gFsView.mNodeView.count(nodequeue)) {
      if (advmsg->kOnline) {
        FsView::gFsView.mNodeView[nodequeue]->SetStatus("online");
        FsView::gFsView.mNodeView[nodequeue]->SetActiveStatus(
          eos::common::FileSystem::kOnline);
      } else {
        FsView::gFsView.mNodeView[nodequeue]->SetStatus("offline");
        FsView::gFsView.mNodeView[nodequeue]->SetActiveStatus(
          eos::common::FileSystem::kOffline);
        // propagate into filesystem states

        for (auto it = FsView::gFsView.mNodeView[nodequeue]->begin();
             it != FsView::gFsView.mNodeView[nodequeue]->end(); ++it) {
          FsView::gFsView.mIdView[*it]->SetStatus(eos::common::FileSystem::kDown, false);
        }
      }

      eos_static_debug("Setting heart beat to %llu\n",
                       (unsigned long long) advmsg->kMessageHeader.kSenderTime_sec);
      FsView::gFsView.mNodeView[nodequeue]->SetHeartBeat(
        advmsg->kMessageHeader.kSenderTime_sec);

      // propagate into filesystems
      for (auto it = FsView::gFsView.mNodeView[nodequeue]->begin();
           it != FsView::gFsView.mNodeView[nodequeue]->end(); ++it) {
        FsView::gFsView.mIdView[*it]->SetLongLong("stat.heartbeattime",
            (long long) advmsg->kMessageHeader.kSenderTime_sec, false);
      }
    }

    FsView::gFsView.ViewMutex.UnLockRead(); // |========= UnLockRead
    eos_static_debug("View UnLocked");
    return true;
  }
}

//------------------------------------------------------------------------------
// Process message
//------------------------------------------------------------------------------
void
Messaging::Process(XrdMqMessage* new_msg)
{
  static bool discardmode = false;

  if ((new_msg->kMessageHeader.kType == XrdMqMessageHeader::kStatusMessage) ||
      (new_msg->kMessageHeader.kType == XrdMqMessageHeader::kQueryMessage)) {
    if (discardmode) {
      return;
    }

    XrdAdvisoryMqMessage* advisorymessage = XrdAdvisoryMqMessage::Create(
        new_msg->GetMessageBuffer());

    if (advisorymessage) {
      eos_debug("queue=%s online=%d", advisorymessage->kQueue.c_str(),
                advisorymessage->kOnline);

      if (advisorymessage->kQueue.endswith("/fst")) {
        if (!Update(advisorymessage)) {
          eos_err("cannot update node status for %s", advisorymessage->GetBody());
        }
      }

      delete advisorymessage;
    }
  } else {
    //    XrdMqTiming somTiming("ParseEnvMessage");;
    //    TIMING("ParseEnv-Start",&somTiming);
    //    somTiming.Print();
    // deal with shared object exchange messages
    if (mSom) {
      // do a cut on the maximum allowed delay for shared object messages
      if ((!discardmode) &&
          ((new_msg->kMessageHeader.kReceiverTime_sec -
            new_msg->kMessageHeader.kBrokerTime_sec) > 60)) {
        eos_crit("dropping shared object message because of message delays of %d seconds",
                 (new_msg->kMessageHeader.kReceiverTime_sec -
                  new_msg->kMessageHeader.kBrokerTime_sec));
        discardmode = true;
        return;
      } else {
        // we accept when we catched up
        if ((new_msg->kMessageHeader.kReceiverTime_sec -
             new_msg->kMessageHeader.kBrokerTime_sec) <= 5) {
          discardmode = false;
        } else {
          if (discardmode) {
            eos_crit("dropping shared object message because of message delays of %d seconds",
                     (new_msg->kMessageHeader.kReceiverTime_sec -
                      new_msg->kMessageHeader.kBrokerTime_sec));
            return;
          }
        }
      }

      // parse as shared object manager message
      XrdOucString error = "";
      bool result = mSom->ParseEnvMessage(new_msg, error);

      //      TIMING("ParseEnv-Stop",&somTiming);
      //      somTiming.Print();
      if (!result) {
        if ((error != "no subject in message body") &&
            (error != "no pairs in message body")) {
          //          new_msg->Print();
          eos_err("%s", error.c_str());
        } else {
          eos_debug("%s", error.c_str());
        }

        return;
      } else {
        return;
      }
    }

    XrdOucString saction = new_msg->GetBody();
    //    new_msg->Print();
    // replace the arg separator # with an & to be able to put it into XrdOucEnv
    XrdOucEnv action(saction.c_str());
    XrdOucString cmd = action.Get("mgm.cmd");
    XrdOucString subcmd = action.Get("mgm.subcmd");
  }
}

EOSMGMNAMESPACE_END
