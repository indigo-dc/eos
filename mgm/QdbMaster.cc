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

#include "mgm/QdbMaster.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Quota.hh"
#include "mgm/Access.hh"
#include "mgm/WFE.hh"
#include "namespace/interface/IContainerMDSvc.hh"
#include "namespace/interface/IFileMDSvc.hh"
#include "namespace/interface/IFsView.hh"
#include "namespace/interface/IView.hh"
#include "namespace/interface/IQuota.hh"
#include "namespace/ns_quarkdb/BackendClient.hh"
#include "common/plugin_manager/PluginManager.hh"

EOSMGMNAMESPACE_BEGIN

std::string QdbMaster::sLeaseKey {"master_lease"};
std::chrono::milliseconds QdbMaster::sLeaseTimeout {10000};

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
QdbMaster::QdbMaster(const eos::QdbContactDetails& qdb_info,
                     const std::string& host_port):
  mIdentity(host_port), mMasterIdentity(),
  mIsMaster(false),  mConfigLoaded(false),
  mAcquireDelay(0)
{
  mQcl = eos::BackendClient::getInstance(qdb_info, "HA");
}

//------------------------------------------------------------------------------
//! Destructor
//------------------------------------------------------------------------------
QdbMaster::~QdbMaster()
{
  mThread.join();
}

//------------------------------------------------------------------------------
// Init method to determine the current master/slave state
//------------------------------------------------------------------------------
bool
QdbMaster::Init()
{
  gOFS->NsInQDB = true;
  gOFS->mInitialized = gOFS->kBooting;
  mThread.reset(&QdbMaster::Supervisor, this);
  return true;
}

//------------------------------------------------------------------------------
// Boot namespace
//------------------------------------------------------------------------------
bool
QdbMaster::BootNamespace()
{
  using eos::common::PluginManager;
  // Load the namepace implementation from external library
  PluginManager& pm = PluginManager::GetInstance();
  PF_PlatformServices& pm_svc = pm.GetPlatformServices();
  pm_svc.invokeService = &XrdMgmOfs::DiscoverPlatformServices;
  gOFS->eosDirectoryService = static_cast<IContainerMDSvc*>
                              (pm.CreateObject("ContainerMDSvc"));
  gOFS->eosFileService = static_cast<IFileMDSvc*>(pm.CreateObject("FileMDSvc"));
  gOFS->eosView = static_cast<IView*>(pm.CreateObject("HierarchicalView"));
  gOFS->eosFsView = static_cast<IFsView*>(pm.CreateObject("FileSystemView"));
  gOFS->eosContainerAccounting =
    static_cast<IFileMDChangeListener*>(pm.CreateObject("ContainerAccounting"));
  gOFS->eosSyncTimeAccounting =
    static_cast<IContainerMDChangeListener*>(pm.CreateObject("SyncTimeAccounting"));

  if (!gOFS->eosDirectoryService || !gOFS->eosFileService || !gOFS->eosView ||
      !gOFS->eosFsView || !gOFS->eosContainerAccounting ||
      !gOFS->eosSyncTimeAccounting) {
    MasterLog(eos_err("namespace implementation could not be loaded using "
                      "the provided library plugin - one of the required "
                      "namespace views could not be created"));
    gOFS->mInitialized = gOFS->kFailed;
    return false;
  }

  std::map<std::string, std::string> fileSettings;
  std::map<std::string, std::string> contSettings;

  if (gOFS->mQdbCluster.empty()) {
    eos_alert("msg=\"mgmofs.qdbcluster configuration is missing\"");
    gOFS->mInitialized = gOFS->kFailed;
    return false;
  } else {
    std::ostringstream instance_id;
    instance_id << gOFS->MgmOfsInstanceName << ":"
                << gOFS->ManagerPort;
    contSettings["qdb_cluster"] = gOFS->mQdbCluster;
    contSettings["qdb_password"] = gOFS->mQdbPassword;
    contSettings["qdb_flusher_md"] = instance_id.str() + "_md";
    contSettings["qdb_flusher_quota"] = instance_id.str() + "_quota";
    fileSettings["qdb_cluster"] = gOFS->mQdbCluster;
    fileSettings["qdb_password"] = gOFS->mQdbPassword;
    fileSettings["qdb_flusher_md"] = instance_id.str() + "_md";
  }

  time_t tstart = time(nullptr);

  try {
    gOFS->eosDirectoryService->setFileMDService(gOFS->eosFileService);
    gOFS->eosDirectoryService->configure(contSettings);
    gOFS->eosFileService->setContMDService(gOFS->eosDirectoryService);
    gOFS->eosFileService->configure(fileSettings);
    gOFS->eosFsView->configure(fileSettings);
    gOFS->eosView->setContainerMDSvc(gOFS->eosDirectoryService);
    gOFS->eosView->setFileMDSvc(gOFS->eosFileService);
    gOFS->eosView->configure(contSettings);
    gOFS->eosFileService->addChangeListener(gOFS->eosFsView);
    gOFS->eosDirectoryService->addChangeListener(gOFS->eosSyncTimeAccounting);
    gOFS->eosFileService->addChangeListener(gOFS->eosContainerAccounting);
    gOFS->eosFileService->setQuotaStats(gOFS->eosView->getQuotaStats());
    gOFS->eosDirectoryService->setQuotaStats(gOFS->eosView->getQuotaStats());
    gOFS->eosDirectoryService->setContainerAccounting(gOFS->eosContainerAccounting);
    gOFS->eosView->getQuotaStats()->registerSizeMapper(Quota::MapSizeCB);
    gOFS->eosView->initialize1();
    gOFS->mBootContainerId = gOFS->eosDirectoryService->getFirstFreeId();
    MasterLog(eos_notice("msg=\"container initialization done\" duration=%ds",
                         (time(nullptr) - tstart)));
  } catch (eos::MDException& e) {
    MasterLog(eos_crit("msg=\"container initialization failed\" duration=%ds, "
                       "errc=%d, reason=\"%s\"", (time(nullptr) - tstart),
                       e.getErrno(), e.getMessage().str().c_str()));
    gOFS->mInitialized = gOFS->kFailed;
    return false;
  } catch (const std::runtime_error& qdb_err) {
    MasterLog(eos_crit("msg=\"container initialization failed unable to connect to "
                       "QuarkDB cluster\" reason=\"%s\"", qdb_err.what()));
    gOFS->mInitialized = gOFS->kFailed;
    return false;
  }

  // Initialize the file view
  gOFS->mFileInitTime = time(nullptr);

  try {
    eos_notice("%s", "msg=\"eos file view initialize2 starting ...\"");
    eos::common::RWMutexWriteLock wr_view_lock(gOFS->eosViewRWMutex);
    gOFS->eosView->initialize2();
    eos_notice("msg=\"file view initialize2 done\" duration=%ds",
               time(nullptr) - gOFS->mFileInitTime);
    gOFS->mBootFileId = gOFS->eosFileService->getFirstFreeId();
  } catch (eos::MDException& e) {
    eos_crit("msg=\"file view initialize2 failed\" duration=%ds, "
             "errc=%d reason=\"%s\"", (time(nullptr) - gOFS->mFileInitTime),
             e.getErrno(), e.getMessage().str().c_str());
    gOFS->mInitialized = gOFS->kFailed;
    return false;;
  }

  gOFS->mFileInitTime = time(nullptr) - gOFS->mFileInitTime;
  gOFS->mTotalInitTime = time(nullptr) - gOFS->mTotalInitTime;
  gOFS->mInitialized = gOFS->kBooted;
  eos_static_alert("msg=\"QDB namespace booted\"");

  // Get process status after boot
  if (!eos::common::LinuxStat::GetStat(gOFS->LinuxStatsStartup)) {
    eos_err("msg=\"failed to grab /proc/self/stat information\"");
  }

  return true;
}

//------------------------------------------------------------------------------
// Thread supervising the master/slave status
//------------------------------------------------------------------------------
void
QdbMaster::Supervisor(ThreadAssistant& assistant) noexcept
{
  static bool one_off = true;
  bool old_is_master;
  std::string old_master;
  eos_notice("%s", "msg=\"set up booting stall rule\"");
  Access::StallInfo old_stall;
  Access::StallInfo new_stall("*", "100", "namespace is booting", true);
  Access::SetStallRule(new_stall, old_stall);
  // @todo (esindril) handle case when config contains stall rules

  // Wait for the namespace to boot and the config to load
  while (((gOFS->mInitialized != gOFS->kBooted) ||
          (mConfigLoaded == false)) &&
         !assistant.terminationRequested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    eos_info("msg=\"waiting for namespace boot and config load\" "
             "mInitialized=%s mConfigLoaded=%s",
             gOFS->gNameSpaceState[gOFS->mInitialized.load()],
             mConfigLoaded ? "true" : "false");
  }

  // Loop updating the master status
  while (!assistant.terminationRequested()) {
    old_is_master = mIsMaster;
    old_master = GetMasterId();
    mIsMaster = AcquireLeaseWitDelay();
    UpdateMasterId(GetLeaseHolder());
    eos_info("old_is_master=%s, is_master=%s, old_master_id=%s, master_id=%s",
             old_is_master ? "true" : "false",
             mIsMaster.load() ? "true" : "false",
             old_master.c_str(), GetMasterId().c_str());

    // Run one-off after boot
    if (one_off) {
      one_off = false;
      mIsMaster ? SlaveToMaster() : MasterToSlave();
      eos_notice("%s", "msg=\"remove booting stall rule\"");
      Access::SetStallRule(old_stall, new_stall);
    } else {
      // There was a master-slave transition
      if (old_is_master != mIsMaster) {
        old_is_master ? MasterToSlave() : SlaveToMaster();
      } else {
        std::string new_master_id = GetMasterId();

        // Update new master if we released the lease on purpose
        if (!mIsMaster && (new_master_id == mIdentity)) {
          new_master_id.clear();
        }

        // We're still a slave, but there is a new master
        if (old_master != new_master_id) {
          Access::SetMasterToSlaveRules(new_master_id);
        }
      }
    }

    // If there is a master then wait a bit
    if (!GetMasterId().empty()) {
      std::chrono::milliseconds wait_ms(sLeaseTimeout.count() / 2);
      std::this_thread::sleep_for(wait_ms);
    }
  }

  eos_notice("%s", "msg=\"supervisor thread joined\"");
}

//------------------------------------------------------------------------------
// Slave to master transition
//------------------------------------------------------------------------------
void
QdbMaster::SlaveToMaster()
{
  eos_info("%s", "msg=\"slave to master transition\"");
  // ******
  // @todo (esindril): reapply the configuration
  // ******
  // Load all the quota nodes from the namespace
  Quota::LoadNodes();
  WFE::MoveFromRBackToQ();
  // We are the master and we broadcast every configuration change
  gOFS->ObjectManager.EnableBroadCast(true);
  Access::SetSlaveToMasterRules();
}

//------------------------------------------------------------------------------
// Master to slave transition
//------------------------------------------------------------------------------
void
QdbMaster::MasterToSlave()
{
  eos_info("%s", "msg=\"master to slave transition\"");
  // We are the slave and we just listen and don't broad cast anything
  gOFS->ObjectManager.EnableBroadCast(false);
  std::string new_master_id = GetMasterId();

  if (!mIsMaster && (new_master_id == mIdentity)) {
    new_master_id.clear();
  }

  Access::SetMasterToSlaveRules(new_master_id);
}

//------------------------------------------------------------------------------
// Apply configuration setting
//------------------------------------------------------------------------------
bool
QdbMaster::ApplyMasterConfig(std::string& stdOut, std::string& stdErr,
                             Transition::Type transitiontype)
{
  gOFS->ConfEngine->SetConfigDir(gOFS->MgmConfigDir.c_str());

  if (gOFS->MgmConfigAutoLoad.length()) {
    eos_static_info("autoload config=%s", gOFS->MgmConfigAutoLoad.c_str());
    XrdOucString configloader = "mgm.config.file=";
    configloader += gOFS->MgmConfigAutoLoad;
    XrdOucEnv configenv(configloader.c_str());
    XrdOucString stdErr = "";

    if (!gOFS->ConfEngine->LoadConfig(configenv, stdErr)) {
      eos_crit("msg=\"failed config autoload, fix the configuration file!\" "
               "config=\"%s\"", gOFS->MgmConfigAutoLoad.c_str());
      eos_crit("%s", stdErr.c_str());
    } else {
      mConfigLoaded = true;
      eos_static_info("msg=\"successful config autoload\" config=\"%s\"",
                      gOFS->MgmConfigAutoLoad.c_str());
    }
  }

  return mConfigLoaded;
}

//------------------------------------------------------------------------------
// Try to acquire lease
//------------------------------------------------------------------------------
bool
QdbMaster::AcquireLease()
{
  std::future<qclient::redisReplyPtr> f =
    mQcl->exec("lease-acquire", sLeaseKey, mIdentity,
               eos::common::StringConversion::stringify(sLeaseTimeout.count()));
  qclient::redisReplyPtr reply = f.get();

  if (reply == nullptr) {
    return false;
  }

  std::string reply_msg(reply->str, reply->len);

  if ((reply_msg == "ACQUIRED") ||
      (reply_msg == "RENEWED")) {
    return true;
  }

  return false;
}

//------------------------------------------------------------------------------
// Try to acquire lease with delay. If the mAcquireDelay timestamp is set
// then we skip trying to acquire the lease until the delay has expired.
//------------------------------------------------------------------------------
bool
QdbMaster::AcquireLeaseWitDelay()
{
  bool is_master = false;

  if (mAcquireDelay != 0) {
    if (mAcquireDelay >= time(nullptr)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      eos_info("%s", "msg=\"enforce lease acquire delay\"");
    } else {
      mAcquireDelay = 0;
      is_master = AcquireLease();
    }
  } else {
    is_master = AcquireLease();
  }

  return is_master;
}


//----------------------------------------------------------------------------
// Release lease
//----------------------------------------------------------------------------
void
QdbMaster::ReleaseLease()
{
  std::future<qclient::redisReplyPtr> f = mQcl->exec("lease-release", sLeaseKey);
  qclient::redisReplyPtr reply = f.get();
  (void) reply;
}

//----------------------------------------------------------------------------
// Get the identity of the current lease holder
//----------------------------------------------------------------------------
std::string
QdbMaster::GetLeaseHolder()
{
  std::string holder;
  std::future<qclient::redisReplyPtr> f = mQcl->exec("lease-get", sLeaseKey);
  qclient::redisReplyPtr reply = f.get();

  if ((reply == nullptr) || (reply->type == REDIS_REPLY_NIL)) {
    eos_debug("%s", "msg=\"lease-get is NULL\"");
    return holder;
  }

  std::string reply_msg = std::string(reply->element[0]->str,
                                      reply->element[0]->len);
  eos_debug("lease-get reply: %s", reply_msg.c_str());
  std::string tag {"HOLDER: "};
  size_t pos = reply_msg.find(tag);

  if (pos == std::string::npos) {
    return holder;
  }

  pos += tag.length();
  size_t pos_end = reply_msg.find('\n', pos);

  if (pos_end == std::string::npos) {
    holder = reply_msg.substr(pos);
  } else {
    holder = reply_msg.substr(pos, pos_end - pos + 1);
  }

  return holder;
}

//------------------------------------------------------------------------------
// Set the new master hostname
//------------------------------------------------------------------------------
bool
QdbMaster::SetMasterId(const std::string& hostname, int port,
                       std::string& err_msg)
{
  std::string new_id = hostname + std::to_string(port);

  if (new_id != mIdentity) {
    mAcquireDelay = time(nullptr) + 2 *
                    std::chrono::duration_cast<std::chrono::seconds>(sLeaseTimeout).count();
  }

  return true;
}

//------------------------------------------------------------------------------
// Check if remove master is OK
//----------------------------------------------------------------------------
bool
QdbMaster::IsRemoteMasterOk() const
{
  std::string master_id = GetMasterId();

  // If we're master or remote master id is empty then fail
  if ((mIsMaster && (master_id == mIdentity)) || master_id.empty()) {
    return false;
  }

  std::ostringstream oss;
  oss << "root://" << master_id << "//dummy?xrd.wantprot=sss,unix";
  XrdCl::URL url(oss.str());

  if (!url.IsValid()) {
    eos_err("msg=\"invalid remote master\" id=%s", master_id.c_str());
    return false;
  }

  // Check if node is reachable
  XrdCl::FileSystem fs(url);
  XrdCl::XRootDStatus st = fs.Ping(1);

  if (!st.IsOK()) {
    eos_err("msg=\"remote master not reachable\" id=%s", master_id.c_str());
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
// Show the current master/slave run configuration (used by ns stat)
//------------------------------------------------------------------------------
std::string
QdbMaster::PrintOut()
{
  std::ostringstream oss;
  oss << "is_master=" << (mIsMaster ? "true" : "false")
      << " master_id=" << GetMasterId();
  return oss.str();
}

EOSMGMNAMESPACE_END
