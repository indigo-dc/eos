//------------------------------------------------------------------------------
// File: FindCmd.cc
// Author: Jozsef Makai - CERN
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


#include "FindCmd.hh"
#include "common/Path.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Acl.hh"

EOSMGMNAMESPACE_BEGIN

eos::console::ReplyProto
eos::mgm::FindCmd::ProcessRequest() {
  eos::console::ReplyProto reply;

  auto& findRequest = mReqProto.find();
  auto& spath = findRequest.path();
  auto& filematch = findRequest.name();
  auto& attributekey = findRequest.attributekey();
  auto& attributevalue = findRequest.attributevalue();
  auto& printkey = findRequest.printkey();
  auto finddepth = findRequest.maxdepth();
  auto olderthan = findRequest.olderthan();
  auto youngerthan = findRequest.youngerthan();
  auto& purgeversion = findRequest.purge();

  if (!OpenTemporaryOutputFiles()) {
    std::ostringstream error;
    error << "error: cannot write find result files on MGM" << std::endl;

    reply.set_retc(EIO);
    reply.set_std_err(error.str());
    return reply;
  }

  // this hash is used to calculate the balance of the found files over the filesystems involved
  google::dense_hash_map<unsigned long, unsigned long long> filesystembalance;
  google::dense_hash_map<std::string, unsigned long long> spacebalance;
  google::dense_hash_map<std::string, unsigned long long> schedulinggroupbalance;
  google::dense_hash_map<int, unsigned long long> sizedistribution;
  google::dense_hash_map<int, unsigned long long> sizedistributionn;
  filesystembalance.set_empty_key(0);
  spacebalance.set_empty_key("");
  schedulinggroupbalance.set_empty_key("");
  sizedistribution.set_empty_key(-1);
  sizedistributionn.set_empty_key(-1);

  bool calcbalance = findRequest.balance();
  bool findzero = findRequest.zerosizefiles();
  bool findgroupmix = findRequest.mixedgroups();
  bool printsize = findRequest.size();
  bool printfid = findRequest.fid();
  bool printuid = findRequest.uid();
  bool printgid = findRequest.gid();
  bool printfs = findRequest.fs();
  bool printchecksum = findRequest.checksum();
  bool printctime = findRequest.ctime();
  bool printmtime = findRequest.mtime();
  bool printrep = findRequest.nrep();
  bool selectrepdiff = findRequest.stripediff();
  bool selectonehour = findRequest.onehourold();
  bool printunlink = findRequest.nunlink();
  bool printcounter = findRequest.count();
  bool printchildcount = findRequest.childcount();
  bool printhosts = findRequest.hosts();
  bool printpartition = findRequest.partition();
  bool selectonline = findRequest.online();
  bool printfileinfo = findRequest.fileinfo();
  bool selectfaultyacl = findRequest.faultyacl();
  bool purge = false;
  bool purge_atomic = purgeversion == "atomic";
  bool printxurl = findRequest.xurl();

  auto max_version = 999999ul;
  time_t selectoldertime = (time_t) olderthan;
  time_t selectyoungertime = (time_t) youngerthan;

  if(!purge_atomic) {
    try {
      max_version = std::stoul(purgeversion);
    } catch (std::logic_error& err) {

    }
  }

  XrdOucString url = "root://";
  url += gOFS->MgmOfsAlias;
  url += "/";

  eos::common::Path cPath(spath.c_str());
  bool deepquery = cPath.GetSubPathSize() < 5 && (!findRequest.directories() || findRequest.files());
  static eos::common::RWMutex deepQueryMutex;
  static std::map<std::string, std::set<std::string>>* globalfound = nullptr;
  eos::common::RWMutexWriteLock deepQueryMutexGuard;

  std::unique_ptr<std::map<std::string, std::set<std::string>>,
    std::function<void(std::map<std::string, std::set<std::string>>*)>> found;

  XrdOucErrInfo errInfo;

  if (deepquery) {
    // we use a single once allocated map for deep searches to store the results to avoid memory explosion
    deepQueryMutexGuard.Grab(deepQueryMutex);

    if (globalfound == nullptr) {
      globalfound = new std::map<std::string, std::set<std::string>>;
    }

    found = std::unique_ptr<std::map<std::string, std::set<std::string>>,
      std::function<void(std::map<std::string, std::set<std::string>>*)>>
      (globalfound, [](std::map<std::string, std::set<std::string>>* ptr) {});
  } else {
    found = std::unique_ptr<std::map<std::string, std::set<std::string>>,
      std::function<void(std::map<std::string, std::set<std::string>>*)>>
      (new std::map<std::string, std::set<std::string>>,
       [](std::map<std::string, std::set<std::string>>* ptr) {
         delete ptr;
       });
  }

  bool nofiles = findRequest.directories() && !findRequest.files();
  bool nodirs = findRequest.files();

  // check what <path> actually is ...
  XrdSfsFileExistence file_exists;

  if ((gOFS->_exists(spath.c_str(), file_exists, errInfo, mVid, nullptr))) {
    std::ostringstream error;
    error << "error: failed to run exists on '" << spath << "'";
    ofstderrStream << error.str();

    if (deepquery) {
      deepQueryMutexGuard.Release();
    }

    reply.set_retc(errno);
    reply.set_std_err(error.str());
    return reply;
  } else {
    if (file_exists == XrdSfsFileExistIsFile) {
      // if this is already a file name, we switch off to find directories
      nodirs = true;
    }

    if (file_exists == XrdSfsFileExistNo) {
      std::ostringstream error;
      error << "error: no such file or directory";
      ofstderrStream << error.str();

      if (deepquery) {
        deepQueryMutexGuard.Release();
      }

      reply.set_retc(ENOENT);
      reply.set_std_err(error.str());
      return reply;
    }
  }

  errInfo.clear();
  if (gOFS->_find(spath.c_str(), errInfo, stdErr, mVid, (*found),
                  attributekey.c_str(), attributevalue.c_str(), nofiles, 0, true, finddepth,
                  filematch.length() ? filematch.c_str() : nullptr)) {
    std::ostringstream error;
    error << stdErr;
    error << "error: unable to run find in directory";
    ofstderrStream << error.str();

    if (deepquery) {
      deepQueryMutexGuard.Release();
    }

    reply.set_retc(errno);
    reply.set_std_err(error.str());
    return reply;
  } else {
    if (stdErr.length()) {
      ofstderrStream << stdErr;
      reply.set_retc(E2BIG);
    }
  }

  unsigned int cnt = 0;
  unsigned long long filecounter = 0;
  unsigned long long dircounter = 0;

  if (findRequest.files() || nodirs || !findRequest.directories()) {
    for (auto& foundit : *found) {
      if (!findRequest.directories() && !findRequest.files()) {
        if (!printcounter) {
          if (printxurl) {
            ofstdoutStream << url;
          }

          ofstdoutStream << foundit.first << std::endl;
        }

        dircounter++;
      }

      for (auto& fileit : foundit.second) {
        cnt++;
        std::string fspath = foundit.first;
        fspath += fileit;

        if (!calcbalance) {
          if (findgroupmix || findzero || printsize || printfid || printuid ||
              printgid || printfileinfo || printchecksum || printctime ||
              printmtime || printrep || printunlink || printhosts ||
              printpartition || selectrepdiff || selectonehour ||
              selectoldertime > 0 || selectyoungertime > 0 || purge_atomic) {
            //-------------------------------------------
            gOFS->eosViewRWMutex.LockRead();
            std::shared_ptr<eos::IFileMD> fmd;

            try {
              bool selected = true;
              unsigned long long filesize = 0;
              fmd = gOFS->eosView->getFile(fspath);
              gOFS->eosViewRWMutex.UnLockRead();
              //-------------------------------------------

              if (selectonehour) {
                eos::IFileMD::ctime_t mtime;
                fmd->getMTime(mtime);

                if (mtime.tv_sec > (time(nullptr) - 3600)) {
                  selected = false;
                }
              }

              if (selectoldertime > 0) {
                eos::IFileMD::ctime_t mtime;
                fmd->getMTime(mtime);

                if (mtime.tv_sec > selectoldertime) {
                  selected = false;
                }
              }

              if (selectyoungertime > 0) {
                eos::IFileMD::ctime_t mtime;
                fmd->getMTime(mtime);

                if (mtime.tv_sec < selectyoungertime) {
                  selected = false;
                }
              }

              if (!attributekey.empty() && !attributevalue.empty()) {
                XrdOucString attr;
                errInfo.clear();
                gOFS->_attr_get(fspath.c_str(), errInfo, mVid, (const char*) nullptr,
                                attributekey.c_str(), attr);
                if (attributevalue != std::string(attr.c_str())) {
                  selected = false;
                }
              }

              if (selected && (findzero || findgroupmix)) {
                if (findzero) {
                  filesize = fmd->getSize();
                  if (filesize == 0) {
                    if (!printcounter) {
                      if (printxurl) {
                        ofstdoutStream << url;
                      }

                      ofstdoutStream << fspath << std::endl;
                    }
                  }
                }

                if (selected && findgroupmix) {
                  // find files which have replicas on mixed scheduling groups
                  XrdOucString sGroupRef = "";
                  XrdOucString sGroup = "";
                  bool mixed = false;

                  for (auto lociter : fmd->getLocations()) {
                    // ignore filesystem id 0
                    if (!lociter) {
                      eos_err("fsid 0 found fid=%lld", fmd->getId());
                      continue;
                    }

                    eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
                    eos::common::FileSystem* filesystem = nullptr;

                    if (FsView::gFsView.mIdView.count(lociter)) {
                      filesystem = FsView::gFsView.mIdView[lociter];
                    }

                    if (filesystem != nullptr) {
                      sGroup = filesystem->GetString("schedgroup").c_str();
                    } else {
                      sGroup = "none";
                    }

                    if (sGroupRef.length()) {
                      if (sGroup != sGroupRef) {
                        mixed = true;
                        break;
                      }
                    } else {
                      sGroupRef = sGroup;
                    }
                  }

                  if (mixed) {
                    if (!printcounter) {
                      if (printxurl) {
                        ofstdoutStream << url;
                      }

                      ofstdoutStream << fspath << endl;
                    }
                  }
                }
              } else {
                if (selected &&
                    (selectonehour || selectoldertime > 0 || selectyoungertime > 0 ||
                     printsize || printfid || printuid || printgid ||
                     printchecksum || printfileinfo || printfs || printctime ||
                     printmtime || printrep || printunlink || printhosts ||
                     printpartition || selectrepdiff || purge_atomic)) {
                  XrdOucString sizestring;
                  bool printed = selectrepdiff &&
                    fmd->getNumLocation() != eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId() + 1);

                  if (purge_atomic) {
                    printed = false;
                  }

                  if (printed) {
                    if (!printfileinfo) {
                      if (!printcounter) {
                        ofstdoutStream << "path=";

                        if (printxurl) {
                          ofstdoutStream << url;
                        }

                        ofstdoutStream << fspath;
                      }

                      if (printsize) {
                        if (!printcounter) {
                          ofstdoutStream << fmd->getSize();
                        }
                      }

                      if (printfid) {
                        if (!printcounter) {
                          ofstdoutStream << fmd->getId();
                        }
                      }

                      if (printuid) {
                        if (!printcounter) {
                          ofstdoutStream << fmd->getCUid();
                        }
                      }

                      if (printgid) {
                        if (!printcounter) {
                          ofstdoutStream << fmd->getCGid();
                        }
                      }

                      if (printfs) {
                        if (!printcounter) {
                          ofstdoutStream << " fsid=";
                        }

                        eos::IFileMD::LocationVector loc_vect = fmd->getLocations();
                        eos::IFileMD::LocationVector::const_iterator lociter;

                        for (lociter = loc_vect.begin(); lociter != loc_vect.end(); ++lociter) {
                          if (lociter != loc_vect.begin()) {
                            if (!printcounter) {
                              ofstdoutStream << ',';
                            }
                          }

                          if (!printcounter) {
                            ofstdoutStream << *lociter;
                          }
                        }
                      }

                      if ((printpartition) && (!printcounter)) {
                        ofstdoutStream << " partition=";
                        std::set<std::string> fsPartition;

                        for (auto lociter : fmd->getLocations()) {
                          // get host name for fs id
                          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
                          eos::common::FileSystem* filesystem = nullptr;

                          if (FsView::gFsView.mIdView.count(lociter)) {
                            filesystem = FsView::gFsView.mIdView[lociter];
                          }

                          if (filesystem != nullptr) {
                            eos::common::FileSystem::fs_snapshot_t fs;

                            if (filesystem->SnapShotFileSystem(fs, true)) {
                              std::string partition = fs.mHost;
                              partition += ":";
                              partition += fs.mPath;

                              if ((!selectonline) ||
                                  (filesystem->GetActiveStatus(true) == eos::common::FileSystem::kOnline)) {
                                fsPartition.insert(partition);
                              }
                            }
                          }
                        }

                        for (auto partitionit = fsPartition.begin(); partitionit != fsPartition.end();
                             partitionit++) {
                          if (partitionit != fsPartition.begin()) {
                            ofstdoutStream << ',';
                          }

                          ofstdoutStream << partitionit->c_str();
                        }
                      }

                      if ((printhosts) && (!printcounter)) {
                        ofstdoutStream << " hosts=";
                        std::set<std::string> fsHosts;

                        for (auto lociter : fmd->getLocations()) {
                          // get host name for fs id
                          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
                          eos::common::FileSystem* filesystem = nullptr;

                          if (FsView::gFsView.mIdView.count(lociter)) {
                            filesystem = FsView::gFsView.mIdView[lociter];
                          }

                          if (filesystem != nullptr) {
                            eos::common::FileSystem::fs_snapshot_t fs;

                            if (filesystem->SnapShotFileSystem(fs, true)) {
                              fsHosts.insert(fs.mHost);
                            }
                          }
                        }

                        for (auto hostit = fsHosts.begin(); hostit != fsHosts.end(); hostit++) {
                          if (hostit != fsHosts.begin()) {
                            ofstdoutStream << ',';
                          }

                          ofstdoutStream << hostit->c_str();
                        }
                      }

                      if (printchecksum) {
                        if (!printcounter) {
                          ofstdoutStream << " checksum=";
                        }

                        for (unsigned int i = 0;
                             i < eos::common::LayoutId::GetChecksumLen(fmd->getLayoutId()); i++) {
                          if (!printcounter) {
                            ofstdoutStream << std::right << setfill('0') << std::setw(2) << (unsigned char)(fmd->getChecksum().getDataPadded(i));
                          }
                        }
                      }

                      if (printctime) {
                        eos::IFileMD::ctime_t ctime;
                        fmd->getCTime(ctime);

                        if (!printcounter)
                        ofstdoutStream << " ctime=" << (unsigned long long) ctime.tv_sec;
                        ofstdoutStream << '.' << (unsigned long long) ctime.tv_nsec;
                      }

                      if (printmtime) {
                        eos::IFileMD::ctime_t mtime;
                        fmd->getMTime(mtime);

                        if (!printcounter)
                        ofstdoutStream << " mtime=" << (unsigned long long) mtime.tv_sec;
                        ofstdoutStream << '.' << (unsigned long long) mtime.tv_nsec;
                      }

                      if (printrep) {
                        if (!printcounter) {
                          ofstdoutStream << " nrep=" << fmd->getNumLocation();
                        }
                      }

                      if (printunlink) {
                        if (!printcounter) {
                          ofstdoutStream << " nunlink=" << fmd->getNumUnlinkedLocation();
                        }
                      }
                    } else {
                      // print fileinfo -m
                      ProcCommand Cmd;
                      XrdOucString lStdOut = "";
                      XrdOucString lStdErr = "";
                      XrdOucString info = "&mgm.cmd=fileinfo&mgm.path=";
                      info += fspath.c_str();
                      info += "&mgm.file.info.option=-m";
                      Cmd.open("/proc/user", info.c_str(), mVid, &errInfo);
                      Cmd.AddOutput(lStdOut, lStdErr);

                      if (lStdOut.length()) {
                        ofstdoutStream << lStdOut;
                      }

                      if (lStdErr.length()) {
                        ofstdoutStream << lStdErr;
                      }

                      Cmd.close();
                    }

                    if (!printcounter) {
                      ofstdoutStream << std::endl;
                    }
                  }

                  if (purge_atomic &&
                      (fspath.find(EOS_COMMON_PATH_ATOMIC_FILE_PREFIX) != std::string::npos)) {
                    ofstdoutStream << "# found atomic " << fspath << std::endl;
                    struct stat buf;

                    if ((!gOFS->_stat(fspath.c_str(), &buf, errInfo, mVid, (const char*) nullptr, nullptr)) &&
                        ((mVid.uid == 0) || (mVid.uid == buf.st_uid))) {
                      time_t now = time(nullptr);

                      if ((now - buf.st_ctime) > 86400) {
                        if (!gOFS->_rem(fspath.c_str(), errInfo, mVid, (const char*) nullptr)) {
                          ofstdoutStream << "# purging atomic " << fspath;
                        }
                      } else {
                        ofstdoutStream << "# skipping atomic " << fspath << " [< 1d old ]" << std::endl;
                      }
                    }
                  }
                }
              }

              if (selected) {
                filecounter++;
              }
            } catch (eos::MDException& e) {
              eos_debug("caught exception %d %s\n", e.getErrno(),
                        e.getMessage().str().c_str());
              gOFS->eosViewRWMutex.UnLockRead();
              //-------------------------------------------
            }
          } else {
            if ((!printcounter) && (!purge_atomic)) {
              if (printxurl) {
                ofstdoutStream << url;
              }

              ofstdoutStream << fspath << std::endl;
            }

            filecounter++;
          }
        } else {
          // get location
          //-------------------------------------------
          gOFS->eosViewRWMutex.LockRead();
          std::shared_ptr<eos::IFileMD> fmd;

          try {
            fmd = gOFS->eosView->getFile(fspath);
          } catch (eos::MDException& e) {
            eos_debug("caught exception %d %s\n", e.getErrno(),
                      e.getMessage().str().c_str());
          }

          if (fmd) {
            gOFS->eosViewRWMutex.UnLockRead();
            //-------------------------------------------

            for (unsigned int i = 0; i < fmd->getNumLocation(); i++) {
              auto loc = fmd->getLocation(i);
              size_t size = fmd->getSize();

              if (!loc) {
                eos_err("fsid 0 found %s %llu", fmd->getName().c_str(), fmd->getId());
                continue;
              }

              filesystembalance[loc] += size;

              if ((i == 0) && (size)) {
                auto bin = (int) log10((double) size);
                sizedistribution[ bin ] += size;
                sizedistributionn[ bin ]++;
              }

              eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
              eos::common::FileSystem* filesystem = nullptr;

              if (FsView::gFsView.mIdView.count(loc)) {
                filesystem = FsView::gFsView.mIdView[loc];
              }

              if (filesystem != nullptr) {
                eos::common::FileSystem::fs_snapshot_t fs;

                if (filesystem->SnapShotFileSystem(fs, true)) {
                  spacebalance[fs.mSpace] += size;
                  schedulinggroupbalance[fs.mGroup] += size;
                }
              }
            }
          } else {
            gOFS->eosViewRWMutex.UnLockRead();
            //-------------------------------------------
          }
        }
      }
    }

    gOFS->MgmStats.Add("FindEntries", mVid.uid, mVid.gid, cnt);
  }

  eos_debug("Listing directories");

  if (!nodirs && findRequest.directories()) {
    for (auto& foundit : *found) {
      // eventually call the version purge function if we own this version dir or we are root
      if (purge &&
          (foundit.first.find(EOS_COMMON_PATH_VERSION_PREFIX) != std::string::npos)) {
        struct stat buf;

        if ((!gOFS->_stat(foundit.first.c_str(), &buf, errInfo, mVid, (const char*) nullptr,
                          0)) &&
            ((mVid.uid == 0) || (mVid.uid == buf.st_uid))) {
          ofstdoutStream << "# purging " << foundit.first;
          gOFS->PurgeVersion(foundit.first.c_str(), errInfo, max_version);
        }
      }

      if (selectfaultyacl) {
        // get the attributes and call the verify function
        eos::IContainerMD::XAttrMap map;

        if (!gOFS->_attr_ls(foundit.first.c_str(),
                            errInfo,
                            mVid,
                            (const char*) nullptr,
                            map)
          ) {
          if ((map.count("sys.acl") || map.count("user.acl"))) {
            if (map.count("sys.acl")) {
              if (Acl::IsValid(map["sys.acl"].c_str(), errInfo)) {
                continue;
              }
            }

            if (map.count("user.acl")) {
              if (Acl::IsValid(map["user.acl"].c_str(), errInfo)) {
                continue;
              }
            }
          } else {
            continue;
          }
        }
      }

      // print directories
      XrdOucString attr = "";

      if (!printkey.empty()) {
        gOFS->_attr_get(foundit.first.c_str(), errInfo, mVid, (const char*) nullptr,
                        printkey.c_str(), attr);

        if (!printkey.empty()) {
          if (!attr.length()) {
            attr = "undef";
          }

          if (!printcounter) {
            ofstdoutStream << printkey << "=" << std::left << std::setw(32) << attr << " path=";
          }
        }
      }

      if (!purge && !printcounter) {
        if (printchildcount) {
          //-------------------------------------------
          eos::common::RWMutexReadLock nLock(gOFS->eosViewRWMutex);
          std::shared_ptr<eos::IContainerMD> mCmd;
          unsigned long long childfiles = 0;
          unsigned long long childdirs = 0;

          try {
            mCmd = gOFS->eosView->getContainer(foundit.first);
            childfiles = mCmd->getNumFiles();
            childdirs = mCmd->getNumContainers();
            ofstdoutStream << foundit.first << " ndir=" << childdirs << " nfiles=" << childfiles << std::endl;
          } catch (eos::MDException& e) {
            eos_debug("caught exception %d %s\n", e.getErrno(),
                      e.getMessage().str().c_str());
          }
        } else {
          if (!printfileinfo) {
            if (printxurl) {
              ofstdoutStream << url;
            }

            ofstdoutStream << foundit.first;

            if (printuid || printgid) {
              eos::common::RWMutexReadLock nLock(gOFS->eosViewRWMutex);
              std::shared_ptr<eos::IContainerMD> mCmd;

              try {
                mCmd = gOFS->eosView->getContainer(foundit.first.c_str());

                if (printuid) {
                  ofstdoutStream << " uid=" << mCmd->getCUid();
                }

                if (printgid) {
                  ofstdoutStream << " uid=" << mCmd->getCGid();
                }
              } catch (eos::MDException& e) {
                eos_debug("caught exception %d %s\n", e.getErrno(),
                          e.getMessage().str().c_str());
              }
            }
          } else {
            // print fileinfo -m
            ProcCommand Cmd;
            XrdOucString lStdOut = "";
            XrdOucString lStdErr = "";
            XrdOucString info = "&mgm.cmd=fileinfo&mgm.path=";
            info += foundit.first.c_str();
            info += "&mgm.file.info.option=-m";
            Cmd.open("/proc/user", info.c_str(), mVid, &errInfo);
            Cmd.AddOutput(lStdOut, lStdErr);

            if (lStdOut.length()) {
              ofstdoutStream << lStdOut;
            }

            if (lStdErr.length()) {
              ofstderrStream << lStdErr;
            }

            Cmd.close();
          }

          ofstdoutStream << std::endl;
        }
      }
    }

    dircounter++;
  }

  if (deepquery) {
    globalfound->clear();
    deepQueryMutexGuard.Release();
  }

  if (printcounter) {
    ofstdoutStream << "nfiles=" << filecounter << " ndirectories=" << dircounter << std::endl;
  }

  if (calcbalance) {
    XrdOucString sizestring = "";

    for (const auto& it : filesystembalance) {
      ofstdoutStream << "fsid=" << it.first << " \tvolume=";
      ofstdoutStream << std::left << std::setw(12) << eos::common::StringConversion::GetReadableSizeString(sizestring, it.second,"B");
      ofstdoutStream << " \tnbytes=" << it.second << std::endl;
    }

    for (const auto& its : spacebalance) {
      ofstdoutStream << "space=" << its.first << " \tvolume=";
      ofstdoutStream << std::left << std::setw(12) << eos::common::StringConversion::GetReadableSizeString(sizestring, its.second,"B");
      ofstdoutStream << " \tnbytes=" << its.second << std::endl;
    }

    for (const auto& itg : schedulinggroupbalance) {
      ofstdoutStream << "sched=" << itg.first << " \tvolume=";
      ofstdoutStream << std::left << std::setw(12) << eos::common::StringConversion::GetReadableSizeString(sizestring, itg.second,"B");
      ofstdoutStream << " \tnbytes=" << itg.second << std::endl;
    }

    for (const auto& itsd : sizedistribution) {
      unsigned long long lowerlimit = 0;
      unsigned long long upperlimit = 0;

      if (((itsd.first) - 1) > 0) {
        lowerlimit = pow(10, (itsd.first));
      }

      if ((itsd.first) > 0) {
        upperlimit = pow(10, (itsd.first) + 1);
      }

      XrdOucString sizestring1;
      XrdOucString sizestring2;
      XrdOucString sizestring3;
      XrdOucString sizestring4;
      unsigned long long avgsize = (sizedistributionn[itsd.first]
                                    ? itsd.second / sizedistributionn[itsd.first] : 0);
      ofstdoutStream << "sizeorder=" << std::right << setfill('0') << std::setw(2) << itsd.first;
      ofstdoutStream << " \trange=[ " << setfill(' ') << std::left << std::setw(12);
      ofstdoutStream << eos::common::StringConversion::GetReadableSizeString(sizestring1, lowerlimit, "B");
      ofstdoutStream << " ... " << std::left << std::setw(12);
      ofstdoutStream << eos::common::StringConversion::GetReadableSizeString(sizestring2, upperlimit, "B") << " ]";
      ofstdoutStream << " volume=" << std::left << std::setw(12);
      ofstdoutStream << eos::common::StringConversion::GetReadableSizeString(sizestring3, itsd.second, "B");
      ofstdoutStream << " \tavgsize=" << std::left << std::setw(12);
      ofstdoutStream << eos::common::StringConversion::GetReadableSizeString(sizestring4, avgsize, "B");
      ofstdoutStream << " \tnbytes=" << itsd.second;
      ofstdoutStream << " \t avgnbytes=" << avgsize;
      ofstdoutStream << " \t nfiles=" << sizedistributionn[itsd.first];
    }
  }

  if (!CloseTemporaryOutputFiles()) {
    std::ostringstream error;
    error << "error: cannot save find result files on MGM" << std::endl;

    reply.set_retc(EIO);
    reply.set_std_err(error.str());
    return reply;
  }

  return reply;
}

EOSMGMNAMESPACE_END