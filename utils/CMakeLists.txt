# ----------------------------------------------------------------------
# File: CMakeLists.txt
# Author: Andreas-Joachim Peters - CERN
# ----------------------------------------------------------------------

# ************************************************************************
# * EOS - the CERN Disk Storage System                                   *
# * Copyright (C) 2011 CERN/Switzerland                                  *
# *                                                                      *
# * This program is free software: you can redistribute it and/or modify *
# * it under the terms of the GNU General Public License as published by *
# * the Free Software Foundation, either version 3 of the License, or    *
# * (at your option) any later version.                                  *
# *                                                                      *
# * This program is distributed in the hope that it will be useful,      *
# * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
# * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
# * GNU General Public License for more details.                         *
# *                                                                      *
# * You should have received a copy of the GNU General Public License    *
# * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
# ************************************************************************

install(
  PROGRAMS eos-log-clean eos-mgm-clean eos-fst-clean eos-uninstall eos-tty-broadcast filter-trace/eos-filter-stacktrace
  DESTINATION ${CMAKE_INSTALL_FULL_SBINDIR})
INSTALL ( FILES eos-el6.repo eos-el6-dev.repo eos-el7.repo eos-el7-dev.repo DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/yum.repos.d PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ )
