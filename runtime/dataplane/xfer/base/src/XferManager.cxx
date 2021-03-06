/*
 * This file is protected by Copyright. Please refer to the COPYRIGHT file
 * distributed with this source distribution.
 *
 * This file is part of OpenCPI <http://www.opencpi.org>
 *
 * OpenCPI is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * OpenCPI is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <ezxml.h>
#include <OcpiOsAssert.h>
#include <OcpiOsMisc.h>
#include <OcpiUtilHash.h>
#include <OcpiUtilAutoMutex.h>
#include <OcpiUtilEzxml.h>
#include <OcpiUtilMisc.h>
#include <OcpiPValue.h>
#include "XferEndPoint.h"
#include "XferServices.h"
#include "XferPioInternal.h"
#include "XferManager.h"

namespace OX = OCPI::Util::EzXml;
namespace OU = OCPI::Util;
namespace OS = OCPI::OS;
namespace OD = OCPI::Driver;
namespace DDT = DtOsDataTypes;
namespace DataTransfer {

// Configure this manager.  The drivers will be configured by the base class

void XferManager::
configure(ezxml_t x) {
  if (m_configured)
    return;
  m_configured = true;
  ocpiDebug("Before configuration, SMB size is %zu", m_SMBSize);
  parse(NULL, x); // this is the actual local manager configuration
  // Allow the environment to override config files here
  ocpiDebug("After configuration, SMB size is %zu", m_SMBSize);
  const char *env = getenv("OCPI_SMB_SIZE");
  if (env && OX::getUNum(env, &m_SMBSize))
    throw OU::Error("Invalid OCPI_SMB_SIZE value: %s", env);
  ocpiDebug("After environment, SMB size is %zu", m_SMBSize);
  // Now configure the drivers
  OD::Manager::configure(x);
  for (XferFactory* d = firstDriver(); d; d = d->nextDriver())
    ocpiInfo("Transfer Driver manager has protocol %s", d->getProtocol());
}

// This method is used to retreive all of the available endpoints that have
// been registered in the system.  Note that some of the endpoints may not
// be finalized.
std::vector<std::string> XferManager::
getListOfSupportedProtocols() {
  parent().configure();
  std::vector<std::string> l;
  for (XferFactory* d = firstDriver(); d; d = d->nextDriver())
    l.push_back(d->getProtocol());
  return l;
}

#if 0
std::string XferManager::null;

// Find the Data transfer class
XferFactory* XferManager::
find(const char* ep1, const char* ep2) {
  std::string sep1, sep2;
  if ( ep1 && ep2 ) {
    sep1 = ep1;
    sep2 = ep2;
  }
  else if ( ep1 && !ep2 ) {
    sep1 = ep1;
  }
  else {
    sep2 = ep2;
  }
  return XferManager::find( sep1 , sep2);
}

XferFactory* XferManager::
find(std::string& ep1, std::string& ep2) {
  parent().configure();
  OU::AutoMutex guard ( m_mutex, true );
  for (XferFactory* d = firstDriver(); d; d = d->nextDriver())
    if (d->supportsEndPoints(ep1, ep2))
      return d;
  return NULL;
}
#endif
XferFactory* XferManager::
getDriver(const char *name) {
  parent().configure();
  OU::AutoMutex guard(m_mutex, true);
  for (XferFactory* d = firstDriver(); d; d = d->nextDriver())
    if (d->supportsEndPoint(name))
      return d;
  return NULL;
}

const char *xfer = "transfer";
static OCPI::Driver::Registration<XferManager> xfm;
XferManager::
XferManager()
  : m_mutex(true), m_configured(false)
{
}

XferManager::
~XferManager()
{
}

// Create an endpoint for some other (hardware) container.
// If only a protocol, then its actually local...
EndPoint& XferManager::
allocateProxyEndPoint(const char *loc, bool local, size_t size) {
  XferFactory* tfactory = find(loc);
  if (!tfactory)
    throw OU::Error("No driver/factory for endpoint string: '%s'", loc);
  return tfactory->getEndPoint(loc, local, true, size);
}

EndPoint &XferManager::
getEndPoint(std::string &s) {
  parent().configure();
  XferFactory* factory = find(s);
  if (!factory)
    throw OU::EmbeddedException( UNSUPPORTED_ENDPOINT, s.c_str());
  return factory->getEndPoint(s.c_str());
}

XferServices* XferManager::
getService(EndPoint *s_endpoint, EndPoint *t_endpoint) {
  return &s_endpoint->factory().getTemplate(*s_endpoint, *t_endpoint);
}
}
