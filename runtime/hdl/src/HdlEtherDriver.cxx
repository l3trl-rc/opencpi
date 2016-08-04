 /*
 *  Copyright (c) Mercury Federal Systems, Inc., Arlington VA., 2009-2010
 *
 *    Mercury Federal Systems, Incorporated
 *    1901 South Bell Street
 *    Suite 402
 *    Arlington, Virginia 22202
 *    United States of America
 *    Telephone 703-413-0781
 *    FAX 703-413-0784
 *
 *  This file is part of OpenCPI (www.opencpi.org).
 *     ____                   __________   ____
 *    / __ \____  ___  ____  / ____/ __ \ /  _/ ____  _________ _
 *   / / / / __ \/ _ \/ __ \/ /   / /_/ / / /  / __ \/ ___/ __ `/
 *  / /_/ / /_/ /  __/ / / / /___/ ____/_/ / _/ /_/ / /  / /_/ /
 *  \____/ .___/\___/_/ /_/\____/_/    /___/(_)____/_/   \__, /
 *      /_/                                             /____/
 *
 *  OpenCPI is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  OpenCPI is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with OpenCPI.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "HdlEtherDriver.h"

namespace OCPI {
  namespace HDL {
    namespace Ether {
      namespace OS = OCPI::OS;
      namespace OU = OCPI::Util;
      namespace OE = OCPI::OS::Ether;

      class Device
	: public OCPI::HDL::Net::Device {
	friend class Driver;
      protected:
	Device(Driver &driver, OS::Ether::Interface &ifc, std::string &a_name,
	       OE::Address &a_addr, bool discovery, std::string &error)
	  : Net::Device(driver, ifc, a_name, a_addr, discovery, "ocpi-ether-rdma", 0,
			(uint64_t)1 << 32, ((uint64_t)1 << 32) - sizeof(OccpSpace), 0, error) {
	}
      public:
	~Device() {
	}
	// Load a bitstream via jtag
	bool load(const char *, std::string &error) {
	  return OU::eformat(error, "Can't load bitstreams for ethernet devices yet");
	}
	bool unload(std::string &error) {
	  return OU::eformat(error, "Can't unload bitstreams for ethernet devices yet");
	}
      };
      Driver::
      ~Driver() {
      }
      Net::Device *Driver::
      createDevice(OS::Ether::Interface &ifc, OS::Ether::Address &addr, bool discovery,
		   std::string &error) {
	std::string name("Ether:" + ifc.name + "/" + addr.pretty());
	Device *d = new Device(*this, ifc, name, addr, discovery, error);
	if (error.empty())
	  return d;
	delete d;
	return NULL;
      }
    } // namespace Ether
  } // namespace HDL
} // namespace OCPI
