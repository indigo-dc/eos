// ----------------------------------------------------------------------
// File: ParseUtils.hh
// Author: Georgios Bitzes - CERN
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

//------------------------------------------------------------------------------
//! @author Georgios Bitzes <georgios.bitzes@cern.ch>
//! @brief Parse utilities with proper error checking
//------------------------------------------------------------------------------

#ifndef EOSCOMMON_PARSE_UTILS_HH
#define EOSCOMMON_PARSE_UTILS_HH

#include "common/Namespace.hh"
#include <climits>

EOSCOMMONNAMESPACE_BEGIN

inline bool parseInt64(const std::string &str, int64_t &ret, int base = 10) {
  char *endptr = NULL;
  ret = std::strtoll(str.c_str(), &endptr, base);
  if(endptr != str.c_str() + str.size() || ret == LLONG_MIN || ret == LONG_LONG_MAX) {
    return false;
  }
  return true;
}

EOSCOMMONNAMESPACE_END

#endif
