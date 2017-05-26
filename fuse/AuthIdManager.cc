//------------------------------------------------------------------------------
// File: AuthIdManager.cc
// Author: Geoffray Adde - CERN
//------------------------------------------------------------------------------

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
#include "common/Macros.hh"
#include "AuthIdManager.hh"
/*----------------------------------------------------------------------------*/

const unsigned int AuthIdManager::proccachenbins = 32768;

//------------------------------------------------------------------------------
// Get user name from the uid and change the effective user ID of the thread
//------------------------------------------------------------------------------
void*
AuthIdManager::CleanupThread (void* arg)
{
  AuthIdManager* am = static_cast<AuthIdManager*> (arg);
  am->CleanupLoop();
  return static_cast<void*> (am);
}
;

uint64_t AuthIdManager::sConIdCount=0;
