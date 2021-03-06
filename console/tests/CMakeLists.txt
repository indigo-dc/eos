# ----------------------------------------------------------------------
# File: CMakeLists.txt
# Author: Stefan Isidorovic <stefan.isidorovic@comtrade.com>
# ----------------------------------------------------------------------

# ************************************************************************
# * EOS - the CERN Disk Storage System                                   *
# * Copyright (C) 2016 CERN/Switzerland                                  *
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

include_directories(
  ${CMAKE_SOURCE_DIR}
  ${OPENSSL_INCLUDE_DIRS}
  ${CPPUNIT_INCLUDE_DIRS})

add_definitions(-DBUILD_TESTS)
#-------------------------------------------------------------------------------
# EosConsoleTests library
#-------------------------------------------------------------------------------
add_library(
  EosConsoleTests SHARED
  ${CMAKE_SOURCE_DIR}/fst/Fmd.cc
  $<TARGET_OBJECTS:EosConsoleCommands-Objects>)

target_link_libraries(
  EosConsoleTests
  EosFstIo-Static
  ${CMAKE_THREAD_LIBS_INIT}
  ${UUID_LIBRARIES}
  ${XROOTD_SERVER_LIBRARY}
  ${CPPUNIT_LIBRARIES}
  ${Z_LIBRARY}
  ${UUID_LIBRARIES}
  ${NCURSES_LIBRARY}
  ${READLINE_LIBRARY}
  ${PROTOBUF_LIBRARY}
  ${XROOTD_CL_LIBRARY}
  ${XROOTD_POSIX_LIBRARY}
  ${XROOTD_UTILS_LIBRARY}
  ${OPENSSL_CRYPTO_LIBRARY}
  ${CMAKE_THREAD_LIBS_INIT})

install(
  TARGETS EosConsoleTests
  LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR})
