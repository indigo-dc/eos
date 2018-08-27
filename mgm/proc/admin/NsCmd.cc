//------------------------------------------------------------------------------
//! @file NsCmd.cc
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

#include "NsCmd.hh"
#include "common/LinuxMemConsumption.hh"
#include "common/LinuxStat.hh"
#include "common/LinuxFds.hh"
#include "namespace/interface/IChLogFileMDSvc.hh"
#include "namespace/interface/IChLogContainerMDSvc.hh"
#include "namespace/interface/IContainerMDSvc.hh"
#include "namespace/interface/IFileMDSvc.hh"
#include "namespace/interface/IView.hh"
#include "namespace/interface/ContainerIterators.hh"
#include "namespace/ns_quarkdb/Constants.hh"
#include "namespace/ns_quarkdb/explorer/NamespaceExplorer.hh"
#include "namespace/ns_quarkdb/BackendClient.hh"
#include "namespace/Resolver.hh"
#include "namespace/Constants.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Quota.hh"
#include "mgm/Stat.hh"
#include "mgm/Master.hh"
#include "mgm/ZMQ.hh"
#include <sstream>

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Method implementing the specific behvior of the command executed by the
// asynchronous thread
//------------------------------------------------------------------------------
eos::console::ReplyProto
NsCmd::ProcessRequest()
{
  eos::console::ReplyProto reply;
  eos::console::NsProto ns = mReqProto.ns();
  eos::console::NsProto::SubcmdCase subcmd = ns.subcmd_case();

  if (subcmd == eos::console::NsProto::kStat) {
    reply.set_std_out(StatSubcmd(ns.stat()));
  } else if (subcmd == eos::console::NsProto::kMutex) {
    MutexSubcmd(ns.mutex(), reply);
  } else if (subcmd == eos::console::NsProto::kCompact) {
    CompactSubcmd(ns.compact(), reply);
  } else if (subcmd == eos::console::NsProto::kMaster) {
    MasterSubcmd(ns.master(), reply);
  } else if (subcmd == eos::console::NsProto::kTree) {
    TreeSizeSubcmd(ns.tree(), reply);
  } else if (subcmd == eos::console::NsProto::kCache) {
    CacheSubcmd(ns.cache(), reply);
  } else if (subcmd == eos::console::NsProto::kQuota) {
    QuotaSizeSubcmd(ns.quota(), reply);
  } else {
    reply.set_retc(EINVAL);
    reply.set_std_err("error: not supported");
  }

  return reply;
}

//------------------------------------------------------------------------------
// Execute mutex subcommand
//------------------------------------------------------------------------------
void
NsCmd::MutexSubcmd(const eos::console::NsProto_MutexProto& mutex,
                   eos::console::ReplyProto& reply)
{
  if (mVid.uid == 0) {
    bool no_option = true;
    std::ostringstream oss;

    if (mutex.sample_rate1() || mutex.sample_rate10() ||
        mutex.sample_rate100() || mutex.toggle_timing() ||
        mutex.toggle_order()) {
      no_option = false;
    }

    eos::common::RWMutex* fs_mtx = &FsView::gFsView.ViewMutex;
    eos::common::RWMutex* quota_mtx = &Quota::pMapMutex;
    eos::common::RWMutex* ns_mtx = &gOFS->eosViewRWMutex;

    if (no_option) {
      size_t cycleperiod = eos::common::RWMutex::GetLockUnlockDuration();
      std::string line = "# ------------------------------------------------------"
                         "------------------------------";
      oss << line << std::endl
          << "# Mutex Monitoring Management" << std::endl
          << line << std::endl
          << "order checking is : "
          << (eos::common::RWMutex::GetOrderCheckingGlobal() ? "on " : "off")
          << " (estimated order checking latency for 1 rule ";
      size_t orderlatency = eos::common::RWMutex::GetOrderCheckingLatency();
      oss <<  orderlatency << " nsec / "
          << int(double(orderlatency) / cycleperiod * 100)
          << "% of the mutex lock/unlock cycle duration)" << std::endl
          << "deadlock checking is : "
          << (eos::common::RWMutex::GetDeadlockCheckingGlobal() ? "on" : "off")
          << std::endl
          << "timing         is : "
          << (fs_mtx->GetTiming() ? "on " : "off")
          << " (estimated timing latency for 1 lock ";
      size_t timinglatency = eos::common::RWMutex::GetTimingLatency();
      oss << timinglatency << " nsec / "
          << int(double(timinglatency) / cycleperiod * 100)
          << "% of the mutex lock/unlock cycle duration)" << std::endl
          << "sampling rate  is : ";
      float sr = fs_mtx->GetSampling();
      char ssr[32];
      sprintf(ssr, "%f", sr);
      oss << (sr < 0 ? "NA" : ssr);

      if (sr > 0) {
        oss << " (estimated average timing latency "
            << int(double(timinglatency) * sr) << " nsec / "
            << int((timinglatency * sr) / cycleperiod * 100)
            << "% of the mutex lock/unlock cycle duration)";
      }

      oss << std::endl;
    }

    if (mutex.toggle_timing()) {
      if (fs_mtx->GetTiming()) {
        fs_mtx->SetTiming(false);
        quota_mtx->SetTiming(false);
        ns_mtx->SetTiming(false);
        oss << "mutex timing is off" << std::endl;
      } else {
        fs_mtx->SetTiming(true);
        quota_mtx->SetTiming(true);
        ns_mtx->SetTiming(true);
        oss << "mutex timing is on" << std::endl;
      }
    }

    if (mutex.toggle_order()) {
      if (eos::common::RWMutex::GetOrderCheckingGlobal()) {
        eos::common::RWMutex::SetOrderCheckingGlobal(false);
        oss << "mutex order checking is off" << std::endl;
      } else {
        eos::common::RWMutex::SetOrderCheckingGlobal(true);
        oss << "mutex order checking is on" << std::endl;
      }
    }

    if (mutex.toggle_deadlock()) {
      if (eos::common::RWMutex::GetDeadlockCheckingGlobal()) {
        eos::common::RWMutex::SetDeadlockCheckingGlobal(false);
        oss << "mutex deadlock checking is off" << std::endl;
      } else {
        eos::common::RWMutex::SetDeadlockCheckingGlobal(true);
        oss << "mutex deadlock checking is on" << std::endl;
      }
    }

    if (mutex.sample_rate1() || mutex.sample_rate10() ||
        mutex.sample_rate100()) {
      float rate = 0.0;

      if (mutex.sample_rate1()) {
        rate = 0.01;
      } else if (mutex.sample_rate10()) {
        rate = 0.1;
      } else if (mutex.sample_rate100()) {
        rate = 1.0;
      }

      fs_mtx->SetSampling(true, rate);
      quota_mtx->SetSampling(true, rate);
      ns_mtx->SetSampling(true, rate);
    }

    reply.set_std_out(oss.str());
  } else {
    reply.set_std_err("error: you have to take role 'root' to execute this"
                      " command");
    reply.set_retc(EPERM);
  }
}

//------------------------------------------------------------------------------
// Execute stat command
//------------------------------------------------------------------------------
std::string
NsCmd::StatSubcmd(const eos::console::NsProto_StatProto& stat)
{
  using eos::common::StringConversion;
  std::ostringstream oss;

  if (stat.reset()) {
    gOFS->MgmStats.Clear();
    oss << "success: all counters have been reset" << std::endl;
  }

  uint64_t f = gOFS->eosFileService->getNumFiles();
  uint64_t d = gOFS->eosDirectoryService->getNumContainers();
  eos::common::FileId::fileid_t fid_now = gOFS->eosFileService->getFirstFreeId();
  eos::common::FileId::fileid_t cid_now =
    gOFS->eosDirectoryService->getFirstFreeId();
  struct stat statf;
  struct stat statd;
  memset(&statf, 0, sizeof(struct stat));
  memset(&statd, 0, sizeof(struct stat));
  XrdOucString clfsize = "";
  XrdOucString cldsize = "";
  XrdOucString clfratio = "";
  XrdOucString cldratio = "";
  XrdOucString sizestring = "";

  // Statistics for the changelog files if they exist
  if ((!::stat(gOFS->MgmNsFileChangeLogFile.c_str(), &statf)) &&
      (!::stat(gOFS->MgmNsDirChangeLogFile.c_str(), &statd))) {
    StringConversion::GetReadableSizeString(clfsize,
                                            (unsigned long long) statf.st_size, "B");
    StringConversion::GetReadableSizeString(cldsize,
                                            (unsigned long long) statd.st_size, "B");
    StringConversion::GetReadableSizeString(clfratio,
                                            (unsigned long long) f ?
                                            (1.0 * statf.st_size) / f : 0, "B");
    StringConversion::GetReadableSizeString(cldratio,
                                            (unsigned long long) d ?
                                            (1.0 * statd.st_size) / d : 0, "B");
  }

  time_t fboot_time = 0;
  time_t boot_time = 0;
  XrdOucString bootstring = gOFS->gNameSpaceState[gOFS->mInitialized];

  if (bootstring == "booting") {
    fboot_time = time(nullptr) - gOFS->mFileInitTime;
    boot_time = time(nullptr) - gOFS->mStartTime;
  } else {
    fboot_time = gOFS->mFileInitTime;
    boot_time = gOFS->mTotalInitTime;
  }

  // Statistics for memory usage
  eos::common::LinuxMemConsumption::linux_mem_t mem;

  if (!eos::common::LinuxMemConsumption::GetMemoryFootprint(mem)) {
    oss << "error: failed to get the memory usage information" << std::endl;
  }

  eos::common::LinuxStat::linux_stat_t pstat;

  if (!eos::common::LinuxStat::GetStat(pstat)) {
    oss << "error: failed to get the process stat information" << std::endl;
  }

  eos::common::LinuxFds::linux_fds_t fds;

  if (!eos::common::LinuxFds::GetFdUsage(fds)) {
    oss << "error: failed to get the process fd information" << std::endl;
  }

  int64_t latencyf = 0, latencyd = 0, latencyp = 0;
  auto chlog_file_svc = dynamic_cast<eos::IChLogFileMDSvc*>(gOFS->eosFileService);
  auto chlog_dir_svc = dynamic_cast<eos::IChLogContainerMDSvc*>
                       (gOFS->eosDirectoryService);

  if (chlog_file_svc && chlog_dir_svc) {
    latencyf = statf.st_size - chlog_file_svc->getFollowOffset();
    latencyd = statd.st_size - chlog_dir_svc->getFollowOffset();
    latencyp = chlog_file_svc->getFollowPending();
  }

  std::string master_status;
  XrdOucString compact_status = "";
  eos::mgm::Master* master = dynamic_cast<eos::mgm::Master*>(gOFS->mMaster.get());
  master_status = master->PrintOut();

  if (master) {
    master->PrintOutCompacting(compact_status);
  }

  if (stat.monitor()) {
    oss << "uid=all gid=all ns.total.files=" << f << std::endl
        << "uid=all gid=all ns.total.directories=" << d << std::endl
        << "uid=all gid=all ns.current.fid=" << fid_now
        << " ns.current.cid=" << cid_now
        << " ns.generated.fid=" << (int)(fid_now - gOFS->mBootFileId)
        << " ns.generated.cid=" << (int)(cid_now - gOFS->mBootContainerId) << std::endl
        << "uid=all gid=all ns.total.files.changelog.size="
        << StringConversion::GetSizeString(clfsize, (unsigned long long) statf.st_size)
        << std::endl
        << "uid=all gid=all ns.total.directories.changelog.size="
        << StringConversion::GetSizeString(cldsize, (unsigned long long) statd.st_size)
        << std::endl
        << "uid=all gid=all ns.total.files.changelog.avg_entry_size="
        << StringConversion::GetSizeString(clfratio, (unsigned long long) f ?
                                           (1.0 * statf.st_size) / f : 0)
        << std::endl
        << "uid=all gid=all ns.total.directories.changelog.avg_entry_size="
        << StringConversion::GetSizeString(cldratio, (unsigned long long) d ?
                                           (1.0 * statd.st_size) / d : 0)
        << std::endl
        << "uid=all gid=all " << compact_status.c_str() << std::endl
        << "uid=all gid=all ns.boot.status=" << bootstring.c_str() << std::endl
        << "uid=all gid=all ns.boot.time=" << boot_time << std::endl
        << "uid=all gid=all ns.boot.file.time=" << fboot_time << std::endl
        << "uid=all gid=all ns.latency.files=" << latencyf << std::endl
        << "uid=all gid=all ns.latency.dirs=" << latencyd << std::endl
        << "uid=all gid=all ns.latency.pending.updates=" << latencyp << std::endl
        << "uid=all gid=all " << master_status.c_str() << std::endl
        << "uid=all gid=all ns.memory.virtual=" << mem.vmsize << std::endl
        << "uid=all gid=all ns.memory.resident=" << mem.resident << std::endl
        << "uid=all gid=all ns.memory.share=" << mem.share << std::endl
        << "uid=all gid=all ns.stat.threads=" << pstat.threads << std::endl
        << "uid=all gid=all ns.fds.all=" << fds.all << std::endl
        << "uid=all gid=all ns.fusex.caps=" << gOFS->zMQ->gFuseServer.Cap().ncaps() <<
        std::endl
        << "uid=all gid=all ns.fusex.clients=" <<
        gOFS->zMQ->gFuseServer.Client().nclients() << std::endl;

    if (pstat.vsize > gOFS->LinuxStatsStartup.vsize) {
      oss << "uid=all gid=all ns.memory.growth=" << (unsigned long long)
          (pstat.vsize - gOFS->LinuxStatsStartup.vsize) << std::endl;
    } else {
      oss << "uid=all gid=all ns.memory.growth=-" << (unsigned long long)
          (-pstat.vsize + gOFS->LinuxStatsStartup.vsize) << std::endl;
    }

    oss << "uid=all gid=all ns.uptime="
        << (int)(time(NULL) - gOFS->mStartTime)
        << std::endl;
  } else {
    std::string line = "# ------------------------------------------------------"
                       "------------------------------";
    oss << line << std::endl
        << "# Namespace Statistics" << std::endl
        << line << std::endl
        << "ALL      Files                            "
        << f << " [" << bootstring << "] (" << fboot_time << "s)" << std::endl
        << "ALL      Directories                      "
        << d <<  std::endl
        << "ALL      Total boot time                  "
        << boot_time << " s" << std::endl
        << line << std::endl
        << "ALL      Compactification                 "
        << compact_status.c_str() << std::endl
        << line << std::endl
        << "ALL      Replication                      "
        << master_status.c_str() << std::endl;

    if (!gOFS->mMaster->IsMaster()) {
      oss << "ALL      Namespace Latency Files          " << latencyf << std::endl
          << "ALL      Namespace Latency Directories    " << latencyd << std::endl
          << "ALL      Namespace Pending Updates        " << latencyp << std::endl;
    }

    oss << line << std::endl
        << "ALL      File Changelog Size              " << clfsize << std::endl
        << "ALL      Dir  Changelog Size              " << cldsize << std::endl
        << line << std::endl
        << "ALL      avg. File Entry Size             " << clfratio << std::endl
        << "ALL      avg. Dir  Entry Size             " << cldratio << std::endl
        << line << std::endl
        << "ALL      files created since boot         "
        << (int)(fid_now - gOFS->mBootFileId) << std::endl
        << "ALL      container created since boot     "
        << (int)(cid_now - gOFS->mBootContainerId) << std::endl
        << line << std::endl
        << "ALL      current file id                  " << fid_now
        << std::endl
        << "ALL      current container id             " << cid_now
        << std::endl
        << line << std::endl
        << "ALL      eosxd caps                       " <<
        gOFS->zMQ->gFuseServer.Cap().ncaps() << std::endl
        << "ALL      eosxd clients                    " <<
        gOFS->zMQ->gFuseServer.Client().nclients() << std::endl
        << line << std::endl;
    CacheStatistics fileCacheStats = gOFS->eosFileService->getCacheStatistics();
    CacheStatistics containerCacheStats =
      gOFS->eosDirectoryService->getCacheStatistics();

    if (fileCacheStats.enabled || containerCacheStats.enabled) {
      oss << "ALL      File cache max num               " << fileCacheStats.maxNum <<
          std::endl
          << "ALL      File cache occupancy             " << fileCacheStats.occupancy <<
          std::endl
          << "ALL      Container cache max num          " << containerCacheStats.maxNum
          << std::endl
          << "ALL      Container cache occupancy        " << containerCacheStats.occupancy
          << std::endl
          << line << std::endl;
    }

    // Do them one at a time otherwise sizestring is saved only the first time
    oss << "ALL      memory virtual                   "
        << StringConversion::GetReadableSizeString(sizestring, (unsigned long long)
            mem.vmsize, "B")
        << std::endl;
    oss << "ALL      memory resident                  "
        << StringConversion::GetReadableSizeString(sizestring, (unsigned long long)
            mem.resident, "B")
        << std::endl;
    oss << "ALL      memory share                     "
        << StringConversion::GetReadableSizeString(sizestring, (unsigned long long)
            mem.share, "B")
        << std::endl
        << "ALL      memory growths                   ";

    if (pstat.vsize > gOFS->LinuxStatsStartup.vsize) {
      oss << StringConversion::GetReadableSizeString
          (sizestring, (unsigned long long)(pstat.vsize - gOFS->LinuxStatsStartup.vsize),
           "B")
          << std::endl;
    } else {
      oss << StringConversion::GetReadableSizeString
          (sizestring, (unsigned long long)(-pstat.vsize + gOFS->LinuxStatsStartup.vsize),
           "B")
          << std::endl;
    }

    oss << "ALL      threads                          " <<  pstat.threads
        << std::endl
        << "ALL      fds                              " << fds.all
        << std::endl
        << "ALL      uptime                           "
        << (int)(time(NULL) - gOFS->mStartTime) << std::endl
        << line << std::endl;
  }

  if (!stat.summary()) {
    XrdOucString stats_out;
    gOFS->MgmStats.PrintOutTotal(stats_out, stat.groupids(), stat.monitor(),
                                 stat.numericids());
    oss << stats_out.c_str();
  }

  return oss.str();
}

//------------------------------------------------------------------------------
// Execute master comand
//------------------------------------------------------------------------------
void
NsCmd::MasterSubcmd(const eos::console::NsProto_MasterProto& master,
                    eos::console::ReplyProto& reply)
{
  using eos::console::NsProto_MasterProto;

  if (master.op() == NsProto_MasterProto::DISABLE) {
    // Disable the master heart beat thread to do remote checks
    eos::mgm::Master* master = dynamic_cast<eos::mgm::Master*>(gOFS->mMaster.get());

    if (master) {
      if (!master->DisableRemoteCheck()) {
        reply.set_std_err("warning: master heartbeat was already disabled!");
        reply.set_retc(EINVAL);
        retc = EINVAL;
      } else {
        reply.set_std_out("success: disabled master heartbeat check");
      }
    } else {
      reply.set_std_err("error: operation supported by master object");
      reply.set_retc(ENOTSUP);
      retc = ENOTSUP;
    }
  } else if (master.op() == NsProto_MasterProto::ENABLE) {
    // Enable the master heart beat thread to do remote checks
    eos::mgm::Master* master = dynamic_cast<eos::mgm::Master*>(gOFS->mMaster.get());

    if (master) {
      if (!master->EnableRemoteCheck()) {
        reply.set_std_err("warning: master heartbeat was already enabled!");
        reply.set_retc(EINVAL);
      } else {
        reply.set_std_out("success: enabled master heartbeat check");
      }
    } else {
      reply.set_std_err("error: operation supported by master object");
      reply.set_retc(ENOTSUP);
      retc = ENOTSUP;
    }
  } else if (master.op() == NsProto_MasterProto::LOG) {
    std::string out;
    gOFS->mMaster->GetLog(out);
    reply.set_std_out(out.c_str());
  } else if (master.op() == NsProto_MasterProto::LOG_CLEAR) {
    gOFS->mMaster->ResetLog();
    reply.set_std_out("success: cleaned the master log");
  } else if (master.host().length()) {
    std::string out, err;

    if (!gOFS->mMaster->SetMasterId(master.host(), 1094, err)) {
      reply.set_std_err(err.c_str());
      reply.set_retc(EIO);
    } else {
      out += "success: <";
      out += gOFS->mMaster->GetMasterId();
      out += "> is now the master\n";
      reply.set_std_out(out.c_str());
    }
  }
}

//------------------------------------------------------------------------------
// Execute compact comand
//------------------------------------------------------------------------------
void
NsCmd::CompactSubcmd(const eos::console::NsProto_CompactProto& compact,
                     eos::console::ReplyProto& reply)
{
  using eos::console::NsProto_CompactProto;
  eos::mgm::Master* master = dynamic_cast<eos::mgm::Master*>(gOFS->mMaster.get());

  if (master == nullptr) {
    reply.set_std_err("error: operation supported by master object");
    reply.set_retc(ENOTSUP);
    retc = ENOTSUP;
    return;
  }

  if (mVid.uid == 0) {
    if (compact.on()) {
      master->ScheduleOnlineCompacting((time(NULL) + compact.delay()),
                                       compact.interval());

      if (compact.type() == NsProto_CompactProto::FILES) {
        master->SetCompactingType(true, false, false);
      } else if (compact.type() == NsProto_CompactProto::DIRS) {
        master->SetCompactingType(false, true, false);
      } else if (compact.type() == NsProto_CompactProto::ALL) {
        master->SetCompactingType(true, true, false);
      } else if (compact.type() == NsProto_CompactProto::FILES_REPAIR) {
        master->SetCompactingType(true, false, true);
      } else if (compact.type() == NsProto_CompactProto::DIRS_REPAIR) {
        master->SetCompactingType(false, true, true);
      } else if (compact.type() == NsProto_CompactProto::ALL_REPAIR) {
        master->SetCompactingType(true, true, true);
      }

      std::ostringstream oss;
      oss << "success: configured online compacting to run in "
          << compact.delay()
          << " seconds from now (might be delayed up to 60 seconds)";

      if (compact.interval()) {
        oss << " (re-compact every " << compact.interval() << " seconds)"
            << std::endl;
      } else {
        oss << std::endl;
      }

      reply.set_std_out(oss.str());
    } else {
      master->ScheduleOnlineCompacting(0, 0);
      reply.set_std_out("success: disabled online compacting\n");
    }
  } else {
    reply.set_std_err("error: you have to take role 'root' to execute this command");
    reply.set_retc(EPERM);
  }
}

//------------------------------------------------------------------------------
// Execute tree size recompute comand
//------------------------------------------------------------------------------
void
NsCmd::TreeSizeSubcmd(const eos::console::NsProto_TreeSizeProto& tree,
                      eos::console::ReplyProto& reply)
{
  eos::common::RWMutexWriteLock ns_wr_lock(gOFS->eosViewRWMutex);
  std::shared_ptr<IContainerMD> cont;

  try {
    cont = eos::Resolver::resolveContainer(gOFS->eosView, tree.container());
  } catch (const eos::MDException& e) {
    reply.set_std_err(SSTR(e.what()));
    reply.set_retc(e.getErrno());
    return;
  }

  if (cont == nullptr) {
    reply.set_std_err("error: container not found");
    reply.set_retc(ENOENT);
    return;
  }

  std::shared_ptr<eos::IContainerMD> tmp_cont {nullptr};
  std::list< std::list<eos::IContainerMD::id_t> > bfs =
    BreadthFirstSearchContainers(cont.get(), tree.depth());

  for (auto it_level = bfs.crbegin(); it_level != bfs.crend(); ++it_level) {
    for (const auto& id : *it_level) {
      try {
        tmp_cont = gOFS->eosDirectoryService->getContainerMD(id);
      } catch (const eos::MDException& e) {
        eos_err("error=\"%s\"", e.what());
        continue;
      }

      UpdateTreeSize(tmp_cont);
    }
  }
}

//------------------------------------------------------------------------------
// Filtering class for NamespaceExplorer to ignore sub-quotanodes when
// recomputing a quotanode.
//------------------------------------------------------------------------------
class QuotaNodeFilter : public ExpansionDecider
{
public:
  virtual bool shouldExpandContainer(const eos::ns::ContainerMdProto& proto)
  override
  {
    if ((proto.flags() & eos::QUOTA_NODE_FLAG) == 0) {
      return true; // not a quota node, continue
    }

    return false; // quota node, ignore
  }
};

//------------------------------------------------------------------------------
// Execute quota size recompute comand
//------------------------------------------------------------------------------
void
NsCmd::QuotaSizeSubcmd(const eos::console::NsProto_QuotaSizeProto& tree,
                       eos::console::ReplyProto& reply)
{
  std::shared_ptr<IContainerMD> cont;

  try {
    cont = eos::Resolver::resolveContainer(gOFS->eosView, tree.container());
  } catch (const eos::MDException& e) {
    reply.set_std_err(SSTR(e.what()));
    reply.set_retc(e.getErrno());
    return;
  }

  if ((cont->getFlags() & eos::QUOTA_NODE_FLAG) == 0) {
    reply.set_std_err("Specified directory is not a quota node.");
    reply.set_retc(EINVAL);
    return;
  }

  if (gOFS->eosView->inMemory()) {
    reply.set_std_err("Command only available for QDB namespace.");
    reply.set_retc(EINVAL);
    return;
  }

  ExplorationOptions options;
  options.depthLimit = 2048;
  options.expansionDecider.reset(new QuotaNodeFilter());
  NamespaceExplorer explorer(gOFS->eosView->getUri(cont.get()),
                             options,
                             *eos::BackendClient::getInstance(gOFS->mQdbContactDetails,
                                 "quota-recomputation")
                            );
  NamespaceItem item;
  QuotaNodeCore qnc;

  while (explorer.fetch(item)) {
    if (item.isFile) {
      // Calculate physical size
      uint64_t logicalSize = item.fileMd.size();
      uint64_t physicalSize = item.fileMd.size() *
                              eos::common::LayoutId::GetSizeFactor(item.fileMd.layout_id());
      // Account file.
      qnc.addFile(
        item.fileMd.uid(),
        item.fileMd.gid(),
        logicalSize,
        physicalSize
      );
    }
  }

  try {
    eos::common::RWMutexWriteLock ns_wr_lock(gOFS->eosViewRWMutex);
    eos::IQuotaNode* quotaNode = gOFS->eosView->getQuotaNode(cont.get());
    quotaNode->replaceCore(qnc);
  } catch (const eos::MDException& e) {
    reply.set_std_err(SSTR(e.what()));
    reply.set_retc(e.getErrno());
    return;
  }

  reply.set_retc(0);
  return;
}

//------------------------------------------------------------------------------
// Recompute and update tree size of the given container assumming its
// subcontainers tree size values are correct and adding the size of files
// attached directly to the current container
//------------------------------------------------------------------------------
void
NsCmd::UpdateTreeSize(eos::IContainerMDPtr cont) const
{
  eos_debug("cont name=%s, id=%llu", cont->getName().c_str(), cont->getId());
  std::shared_ptr<eos::IFileMD> tmp_fmd {nullptr};
  std::shared_ptr<eos::IContainerMD> tmp_cont {nullptr};
  uint64_t tree_size = 0u;

  for (auto fit = FileMapIterator(cont); fit.valid(); fit.next()) {
    try {
      tmp_fmd = gOFS->eosFileService->getFileMD(fit.value());
    } catch (const eos::MDException& e) {
      eos_err("error=\"%s\"", e.what());
      continue;
    }

    tree_size += tmp_fmd->getSize();
  }

  for (auto cit = ContainerMapIterator(cont); cit.valid(); cit.next()) {
    try {
      tmp_cont = gOFS->eosDirectoryService->getContainerMD(cit.value());
    } catch (const eos::MDException& e) {
      eos_err("error=\"%s\"", e.what());
      continue;
    }

    tree_size += tmp_cont->getTreeSize();
  }

  cont->setTreeSize(tree_size);
  gOFS->eosDirectoryService->updateStore(cont.get());
  gOFS->FuseXCastContainer(cont->getIdentifier());
}

//------------------------------------------------------------------------------
// Execute cache update command
//------------------------------------------------------------------------------
void
NsCmd::CacheSubcmd(const eos::console::NsProto_CacheProto& cache,
                   eos::console::ReplyProto& reply)
{
  using namespace eos::constants;
  using eos::console::NsProto_CacheProto;
  std::map<std::string, std::string> map_cfg;

  if (cache.op() == NsProto_CacheProto::SET_FILE) {
    map_cfg[sMaxNumCacheFiles] = std::to_string(cache.max_num());
    map_cfg[sMaxSizeCacheFiles] = std::to_string(cache.max_size());
    gOFS->eosFileService->configure(map_cfg);
  } else if (cache.op() == NsProto_CacheProto::SET_DIR) {
    map_cfg[sMaxNumCacheDirs] = std::to_string(cache.max_num());
    map_cfg[sMaxSizeCacheDirs] = std::to_string(cache.max_size());
    gOFS->eosDirectoryService->configure(map_cfg);
  } else if (cache.op() == NsProto_CacheProto::DROP_FILE) {
    map_cfg[sMaxNumCacheFiles] = "0";
    map_cfg[sMaxSizeCacheFiles] = "0";
    gOFS->eosFileService->configure(map_cfg);
  } else if (cache.op() == NsProto_CacheProto::DROP_DIR) {
    map_cfg[sMaxNumCacheDirs] = "0";
    map_cfg[sMaxSizeCacheDirs] = "0";
    gOFS->eosDirectoryService->configure(map_cfg);
  } else if (cache.op() == NsProto_CacheProto::DROP_ALL) {
    map_cfg[sMaxNumCacheFiles] = "0";
    map_cfg[sMaxSizeCacheFiles] = "0";
    map_cfg[sMaxNumCacheDirs] = "0";
    map_cfg[sMaxSizeCacheDirs] = "0";
    gOFS->eosFileService->configure(map_cfg);
    gOFS->eosDirectoryService->configure(map_cfg);
  }
}

//------------------------------------------------------------------------------
// Do a breadth first search of all the subcontainers under the given
// container
//------------------------------------------------------------------------------
std::list< std::list<eos::IContainerMD::id_t> >
NsCmd::BreadthFirstSearchContainers(eos::IContainerMD* cont,
                                    uint32_t max_depth) const
{
  uint32_t num_levels = 0u;
  std::shared_ptr<eos::IContainerMD> tmp_cont;
  std::list< std::list<eos::IContainerMD::id_t> > depth(256);
  auto it_lvl = depth.begin();
  it_lvl->push_back(cont->getId());

  while (it_lvl->size() && (it_lvl != depth.end())) {
    auto it_next_lvl = it_lvl;
    ++it_next_lvl;

    for (const auto& cid : *it_lvl) {
      try {
        tmp_cont = gOFS->eosDirectoryService->getContainerMD(cid);
      } catch (const eos::MDException& e) {
        // ignore error
        eos_err("error=\"%s\"", e.what());
        continue;
      }

      for (auto subcont_it = ContainerMapIterator(tmp_cont); subcont_it.valid();
           subcont_it.next()) {
        it_next_lvl->push_back(subcont_it.value());
      }
    }

    it_lvl = it_next_lvl;
    ++num_levels;

    if (max_depth && (num_levels == max_depth)) {
      break;
    }
  }

  depth.resize(num_levels);
  return depth;
}
EOSMGMNAMESPACE_END
