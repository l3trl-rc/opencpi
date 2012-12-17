
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

#include <errno.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <arpa/inet.h>
#include <uuid/uuid.h>
#include <dirent.h>
// FIXME: integrate this into our UUID utility properly
#ifndef _UUID_STRING_T
#define _UUID_STRING_T
typedef char uuid_string_t[50]; // darwin has 37 - lousy unsafe interface
#endif
#include "zlib.h"
#include "OcpiOsMisc.h"
#include "OcpiPValue.h"
#include "OcpiUtilMisc.h"
#include "DtSharedMemoryInterface.h"
#include "DtTransferInternal.h"
#include "EtherDriver.h"
#include "SimDriver.h"
#include "HdlDriver.h"
#include "HdlContainer.h"
#include "HdlOCCP.h"
#include "HdlOCDP.h"

namespace OE = OCPI::OS::Ether;
namespace HE = OCPI::HDL::Ether;
namespace OH = OCPI::HDL;
namespace OU = OCPI::Util;
namespace OP = OCPI::HDL::PCI;
namespace OA = OCPI::API;
namespace OD = OCPI::DataTransport;
namespace OC = OCPI::Container;
namespace OS = OCPI::OS;
namespace OX = DataTransfer;
/*
  usage message
  verbose
  set log level
  subsume others
  swtcl
  dump all workers: with metadata?
  abbreviated parsers - pci and ether
  tpci - check for PCI access
  ethertest functions - emulator, list
  reset
  jtag load
  flash load
  introduce bitstream to cache
  ocfrp_check: show more stuff like swctl
  perhaps "verb" syntax like git
  use gnu options?
  fix our options?
 */
typedef void Function(const char **ap);
static Function
  search, emulate, ethers, probe, testdma, admin, bram, unbram, uuid, reset, 
  radmin, wadmin, rmeta, settime, deltatime, wdump, wreset, wunreset, wop, wwctl, wclear, wwpage,
  wread, wwrite, sendData, receiveData, receiveRDMA, sendRDMA, simulate;
static bool verbose = false, parseable = false;
static int log = -1;
static const char
*interface = NULL, *device = NULL, *part = NULL, *platform = NULL, *simexec = NULL, *simDump = NULL;
static unsigned
  sleepUsecs = 100000,  // how much time between quota injections
  spinCount = 255,      // how much quote per timeout AND per control message
  simTicks = 10000000;   // how much quote to run
static std::string name, error, endpoint;
static OH::Driver *driver;
static OH::Device *dev;
static OH::Access *cAccess, *dAccess, wAccess, confAccess;
static unsigned worker;
static const unsigned
  WORKER = 1,
  DEVICE = 2,
  INTERFACE = 4,
  SUDO = 8,
  DISCOVERY = 16;
struct Command {
  const char *name;
  Function *func;
  unsigned options;
} commands [] = {
  { "admin", admin, DEVICE },
  { "bram", bram, 0 },
  { "deltatime", deltatime, DEVICE},
  { "dump", 0, 0 },
  { "emulate", emulate, SUDO | INTERFACE },
  { "ethers", ethers, INTERFACE},
  { "probe", probe, SUDO | DEVICE | DISCOVERY},
  { "radmin", radmin, DEVICE },
  { "receiveData", receiveData, INTERFACE},
  { "receiveRDMA", receiveRDMA, 0}, // might want device depending on args
  { "reset", reset, DEVICE },
  { "rmeta", rmeta, DEVICE },
  { "search", search, SUDO | INTERFACE | DISCOVERY},
  { "sendData", sendData, INTERFACE},
  { "sendRDMA", sendRDMA, 0},  // might want device depending on args
  { "settime", settime, DEVICE},
  { "simulate", simulate, 0},
  { "testdma", testdma, 0},
  { "unbram", unbram, 0},
  { "uuid", uuid, 0},
  { "wadmin", wadmin, DEVICE},
  { "wclear", wclear, DEVICE|WORKER},
  { "wdump", wdump, DEVICE|WORKER},
  { "wop", wop, DEVICE|WORKER},
  { "wread", wread, DEVICE|WORKER},
  { "wreset", wreset, DEVICE|WORKER},
  { "wunreset", wunreset, DEVICE|WORKER},
  { "wwctl", wwctl, DEVICE|WORKER},
  { "wwpage", wwpage, DEVICE|WORKER},
  { "wwrite", wwrite, DEVICE|WORKER},
  { 0, 0, 0},
};

static int
usage(const char *name) {
  fprintf(stderr,
	  "Usage is: %s <command> [<options>...] [<command args>...]\n"
          "  Major commands/modes:\n"
	  "    search [-i <interface>]      # search for OpenCPI HDL devices, limit ethernet to <interface>\n"
	  "    emulate [-i <interface>]     # emulate an HDL device on ethernet, on first or specified interface\n"
          "    ethers                       # list available ethernet interfaces\n"
          "    probe <hdl-dev>              # see if a specific HDL device is available\n"
          "    testdma                      # test for DMA memory setup\n"
          "    admin <hdl-dev>              # dump admin information (reading only) for <hdl-device>\n"
          "    radmin <hdl-dev> <offset>    # read admin space\n"
	  "                                 # write admin word <value> for <hdl-device> at <offset>\n"
          "    wadmin <hdl-dev> <offset> <value>\n"
	  "                                 # write admin word <value> for <hdl-device> at <offset>\n"
          "    radmin <hdl-dev> <offset>    # read admin word for <hdl-device> at <offset>\n"
	  "    settime <hdl-dev>            # set the GPT time of the device to syste time\n"
	  "    deltatime <hdl-dev>          # measure round trip and difference between host and device\n"
          "    dump <hdl-dev>               # dump all state/status of <platform> including all workers\n"
          "    reset <hdl-dev>              # reset platform\n"
          "    flash <hdl-dev>              # flash load platform\n"
	  "    bram <infile> <outfile>      # create a BRAM file from in input file\n"
	  "    unbram <infile> <outfile>    # recreate the original file from a BRAM file\n"
	  "    uuid -p <platform> -c <part> <outputfilename>\n"
	  "    wclear <hdl-dev> <worker>    # clear worker status errors\n"
	  "    wdump <hdl-dev> <worker>     # dump worker's control plane registers\n"
	  "    wop <hdl-dev> <worker> <op>  # perform control operation on worker\n"
	  "          ops are: initialize, start, stop, release, after, before\n"
	  "    wread <hdl-dev> <worker> <offset>[/size]\n"
	  "                                 # perform config space read of size bytes (default 4) at offset\n"
	  "    wreset <hdl-dev> <worker>    # assert reset for worker\n"
	  "    wunreset <hdl-dev> <worker>  # deassert (enable) worker\n"
	  "    wwctl <hdl-dev> <worker> <val>\n"
	  "                                 # write worker control register\n"
	  "    wwpage <hdl-dev> <worker> <val>\n"
	  "                                 # write worker window register\n"
	  "    wwrite <hdl-dev> <worker> <offset>[/size] <value>"
	  "                                 # perform config space write of size bytes (default 4) at offset\n"
	  "                                 # generate UUID verilog file\n"
          "    simulate                     # run simulator inside created sim: device\n"
          "  Options: (values are either directly after the letter or in the next argument)\n"
	  "    -l <level>                   # set log levels\n"
	  "    -i <interface>               # set ethernet interface to use\n"
	  "    -d <hdl-device>              # identify a specific hdl device to use\n"
	  "    -p <hdl-platform>            # specify a particular hdl platform (e.g. ml605)\n"
          "    -c <hdl-part>                # specify a particular part/chip (e.g. xc6vlx240t\n"
	  "    -s <spin-clocks>             # clocks to credit/run the sim between control messages\n"
	  "    -t <sleep-usecs>             # delay time letting sim run between credits\n"
	  "    -T <sim-time>                # total simulation time before terminating\n"
	  "    -D                           # turn off simulation dumping\n"
	  "    -e <sim-executable>          # simulator executable - default is \"./runsim\"\n"
	  "    -v                           # be verbose\n"
	  "  <worker> can be multiple workers such as 1,2,3,4,5.  No ranges.\n"
	  "  <hdl-dev> examples: 3                            # PCI device 3 (i.e. 0000:03:00.0)\n"
	  "                      0000:04:00.0                 # PCI device 0000:04:00.0\n"
          "                      PCI:0001:05:04.2             # fully specified PCI device\n"
	  "                      a0:00:b0:34:55:67            # ethernet MAC address on first up+connected interface\n"
	  "                      eth0/a0:00:b0:34:55:67       # ethernet address on a specific interface\n"
	  "                      Ether:eth1/a0:00:b0:34:55:67 # fully specified Ethernet-based device\n",
	  name);
  return 1;
}

static void
bad(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "Exiting for problem: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "%s%s\n", error.size() ? ": " : "", error.c_str());
  va_end(ap);
  exit(1);
}

static void setupDevice(bool discovery) {
  if (!device)
    bad("a device option is required with this command");
  
  driver = &OCPI::HDL::Driver::getSingleton();
  std::string error;
  if (!(dev = driver->open(device, discovery, error)))
    bad("error opening device %s", device);
  cAccess = &dev->cAccess();
  dAccess = &dev->dAccess();
  name = dev->name();
  endpoint = dev->endpointSpecific();
}

static void
doFlags(const char **&ap) {
  for (; *ap && ap[0][0] == '-'; ap++)
    switch (ap[0][1]) {
    case 'i':
      //      if (!(exact->options & INTERFACE))
      //	bad("An interface cannot be specified with this command");
      interface = ap[0][2] ? &ap[0][2] : *++ap;
      break;
    case 'd':
      //      if (!(exact->options & DEVICE))
      //	bad("An interface cannot be specified with this command");
      device = ap[0][2] ? &ap[0][2] : *++ap;
      break;
    case 'e':
      simexec = ap[0][2] ? &ap[0][2] : *++ap;
      break;
    case 'p':
      platform = ap[0][2] ? &ap[0][2] : *++ap;
      break;
    case 'c':
      part = ap[0][2] ? &ap[0][2] : *++ap;
      break;
    case 'l':
      log = atoi(ap[0][2] ? &ap[0][2] : *++ap);
      break;
    case 's':
      spinCount = atoi(ap[0][2] ? &ap[0][2] : *++ap);
      break;
    case 't':
      sleepUsecs = atoi(ap[0][2] ? &ap[0][2] : *++ap);
      break;
    case 'T':
      simTicks = atoi(ap[0][2] ? &ap[0][2] : *++ap);
      break;
    case 'D':
      simDump = ap[0][2] ? &ap[0][2] : *++ap;
      break;
    case 'v':
      verbose = true;
      break;
    case 'P':
      parseable = true;
      break;
    default:
      bad("unknown flag: %s", *ap);
    }
}

int
main(int argc, const char **argv)
{
  signal(SIGPIPE, SIG_IGN);
  OC::Manager::getSingleton().suppressDiscovery();
  const char *argv0 = strrchr(argv[0], '/');
  if (argv0)
    argv0++;
  else
    argv0 = argv[0];
  if (argc == 1)
    return usage(argv0);
  argv++;
  doFlags(argv);
  Command *found = NULL, *exact = NULL;
  bool ambiguous = false;
  for (Command *c = commands; c->name; c++)
    if (!strncmp(argv[0], c->name, strlen(argv[0])))
      if (!strcmp(argv[0], c->name)) {
	exact = c;
	break;
      } else if (found)
	ambiguous = true;
      else
	found = c;
  if (!exact)
    if (!found)
      bad("unknown command: %s", argv[1]);
    else if (ambiguous)
      bad("ambiguous command: %s", argv[1]);
    else
      exact = found;
  argv++;
  doFlags(argv);
#if 0
  if ((exact->options & SUDO) && geteuid()) {
    int dfd = ::open(OCPI_DRIVER_MEM, O_RDONLY);
    if (dfd >= 0)
      close(dfd);
    else
      bad("This command requires running with \"sudo -E ./%s ...\"", argv0);
  }
#endif
  if (!exact->func)
    bad("command %s not implemented", exact->name);
  try {
    if (exact->options & DEVICE) {
      if (!device)
	device = *argv++;
      setupDevice((exact->options & DISCOVERY) != 0);
    }
    if (exact->options & WORKER) {
      if (!*argv)
	bad("Missing worker specifier for command");
      const char *cp = *argv++;
      char *ep;
      do {
	worker = strtoul(cp, &ep, 10);
	if (worker < 1 || worker > OCCP_MAX_WORKERS)
	  bad("Worker number `%s' invalid", cp);
	if (*ep)
	  ep++;
	cAccess->offsetRegisters(wAccess, (intptr_t)(&((OH::OccpSpace*)0)->worker[worker]));
	cAccess->offsetRegisters(confAccess, (intptr_t)(&((OH::OccpSpace*)0)->config[worker]));
	exact->func(argv);
      } while (*(cp = ep));
    } else
      exact->func(argv);
  } catch (std::string &e) {
    bad(e.c_str());
  } catch (const char *e) {
    bad(e);
  } catch (...) {
    bad("Unexpected exception");
  }
  return 0;
}
static void search(const char **) {
#if 0
  OA::PValue vals[] = {
    OA::PVBool("printOnly", true),
    interface ? OA::PVString("interface", interface) : OA::PVEnd,
    OA::PVEnd};
#else
    OA::PVarray vals(5);
    unsigned n = 0;
    vals[n++] = OA::PVBool("printOnly", true);
    if (interface)
      vals[n++] = OA::PVString("interface", interface);
    vals[n++] = OA::PVEnd;
#endif

  OCPI::HDL::Driver::getSingleton().search(vals, NULL);
}
static void probe(const char **) {
  dev->print();
}

static void initAdmin(OH::OccpAdminRegisters &admin, const char *platform) {
  memset(&admin, 0, sizeof(admin));
#define unconst32(a) (*(uint32_t *)&(a))
#define unconst64(a) (*(uint64_t *)&(a))
  unconst64(admin.magic) = OCCP_MAGIC;
  unconst32(admin.revision) = 0;
  unconst32(admin.birthday) = time(0);
  unconst32(admin.config) = 0xf0;
  unconst32(admin.pciDevice) = 0;
  unconst32(admin.attention) = 0;
  unconst32(admin.status) = 0;
  admin.scratch20 = 0xf00dface;
  admin.scratch24 = 0xdeadbeef;
  admin.control = 0;
  unconst32(admin.reset) = 0;
  unconst32(admin.timeStatus) = 0;
  admin.timeControl = 0;
  admin.time = 0;
  admin.timeDelta = 0;
  unconst32(admin.timeClksPerPps) = 0;
  unconst64(admin.dna) = 0;
  unconst32(admin.numRegions) = 1;
  unconst32(admin.regions[0]) = 0;
  uuid_t uuid;
  uuid_generate(uuid);
  uuid_string_t textUUID;
  uuid_unparse_lower(uuid, textUUID);
  ocpiDebug("Emulator UUID: %s", textUUID);
  OH::HdlUUID temp;
  temp.birthday = time(0) + 1;
  memcpy(temp.uuid, uuid, sizeof(admin.uuid.uuid));
  strcpy(temp.platform, platform);
  strcpy(temp.device, "devemu");
  strcpy(temp.load, "ld");
  strcpy(temp.dna, "\001\002\003\004\005\006\007");
  for (unsigned n = 0; n < sizeof(OH::HdlUUID); n++)
    ((uint8_t*)&admin.uuid)[n] = ((uint8_t *)&temp)[(n & ~3) + (3 - (n&3))];
}

static const char *ops[] =
  { "initialize", "start", "stop", "release", "test", "before", "after", "reserved7", 0};

static void emulate(const char **) {
  OE::IfScanner ifs(error);
  if (error.size())
    bad("Error establishing interface scanner");
  OE::Interface eif;
  while (error.empty() && ifs.getNext(eif, error, interface)) {
    if (eif.up && eif.connected) {
      printf("Using interface %s with address %s\n", eif.name.c_str(), eif.addr.pretty());
      OE::Socket s(eif, ocpi_slave, NULL, 0, error);
      if (error.size())
	bad("Failed to open slave socket");
      OE::Packet rFrame, sFrame;
      static char cadmin[sizeof(OH::OccpSpace)];
      memset(cadmin, 0, sizeof(cadmin));
      initAdmin(*(OH::OccpAdminRegisters *)cadmin, "emulator");
      HE::EtherControlHeader &ech_out =  *(HE::EtherControlHeader *)(sFrame.payload);
      bool haveTag = false;
      OE::Address to;
      do {
	unsigned length;
	OE::Address from;
	if (s.receive(rFrame, length, 0, from, error)) {
	  if (from == eif.addr)
	    continue;
	  ocpiDebug("Received packet from %s, length %u\n", from.pretty(), length);
	  bool sent;
	  HE::EtherControlHeader &ech_in =  *(HE::EtherControlHeader *)(rFrame.payload);
	  HE::EtherControlMessageType type = OCCP_ETHER_MESSAGE_TYPE(ech_in.typeEtc);
	  unsigned uncache = OCCP_ETHER_UNCACHED(ech_in.typeEtc) ? 1 : 0;
	  OE::Packet &out = uncache ? rFrame : sFrame;
	  HE::EtherControlHeader *echp = (HE::EtherControlHeader *)out.payload;
	  HE::EtherControlRead &ecr =  *(HE::EtherControlRead *)(rFrame.payload);
	  uint32_t offset = ntohl(ecr.address); // for read or write
	  HE::EtherControlWrite &ecw =  *(HE::EtherControlWrite *)(rFrame.payload);
	  printf("Received type %u, length 0x%x ntohs 0x%x tag %3d from %s, offset 0x%8" PRIx32
		 ", data 0x%8" PRIx32 "\n",
		 type, ech_in.length, ntohs(ech_in.length), ech_in.tag, from.pretty(), offset,
		 ntohl(ecw.data));
	  switch (type) {
	  case HE::OCCP_READ:
	  case HE::OCCP_WRITE:
	    if (uncache || !haveTag || ech_in.tag != ech_out.tag || from != to) {
	      if (!uncache) {
		to = from;
		haveTag = true;
		echp->tag = ech_in.tag;
	      }
	      if (type == HE::OCCP_READ) {
		ocpiAssert(ntohs(ech_in.length) == sizeof(ecr)-2);
		HE::EtherControlReadResponse &ecrr =  *(HE::EtherControlReadResponse *)(echp);
		ecrr.data = htonl(*(uint32_t *)&cadmin[offset]);
		if (offset > sizeof(cadmin)) {
		  ocpiDebug("Read offset out of range: 0x%" PRIx32 ", returning 0\n", offset);
		  ecrr.data = 0;
		} else if (offset >= offsetof(OH::OccpSpace, config)) {
		  unsigned
		    worker = (offset - offsetof(OH::OccpSpace, config)) / OCCP_WORKER_CONFIG_SIZE,
		    woffset = (offset - offsetof(OH::OccpSpace, config)) % OCCP_WORKER_CONFIG_SIZE;
		  printf("Worker %2d config read 0x%x\n", worker, woffset);
		  if (worker == 13 && woffset == 0x4c)
		    ecrr.data = htonl(0x8000);
		} else if (offset >= offsetof(OH::OccpSpace, worker)) {
		  unsigned
		    worker = (offset - offsetof(OH::OccpSpace, worker)) / sizeof(OH::OccpWorker),
		    woffset = (offset - offsetof(OH::OccpSpace, worker)) % sizeof(OH::OccpWorker);
		  if (woffset < offsetof(OH::OccpWorkerRegisters, control)) {
		    printf("Worker %2d control operation read: %s\n", worker, ops[woffset/sizeof(uint32_t)]);
		    ecrr.data = htonl(OCCP_SUCCESS_RESULT);
		  } else
		    printf("Worker %2d register read: %s\n", worker, 
			   woffset == offsetof(OH::OccpWorkerRegisters, control) ? "control" :
			   woffset == offsetof(OH::OccpWorkerRegisters, window) ? "window" :
			   woffset == offsetof(OH::OccpWorkerRegisters, clearError) ? "clearError" :
			   woffset == offsetof(OH::OccpWorkerRegisters, lastConfig) ? "lastConfig" :
			   "unknown");
		}
		echp->length = htons(sizeof(ecrr)-2);
	      } else {
		HE::EtherControlWrite &ecw =  *(HE::EtherControlWrite *)(rFrame.payload);
		ocpiAssert(ntohs(ech_in.length) == sizeof(ecw)-2);
		uint32_t data = ntohl(ecw.data);
		if (offset > sizeof(cadmin)) {
		  ocpiDebug("Write offset out of range: 0x%" PRIx32 "\n", offset);
		} else if (offset > offsetof(OH::OccpSpace, config)) {
		  unsigned
		    worker = (offset - offsetof(OH::OccpSpace, config)) / OCCP_WORKER_CONFIG_SIZE,
		    woffset = (offset - offsetof(OH::OccpSpace, config)) % OCCP_WORKER_CONFIG_SIZE;
		  printf("Worker %2d config write 0x%x: 0x%" PRIx32 "\n", worker, woffset, data);
		  *(uint32_t *)&cadmin[offset] = data;
		} else if (offset > offsetof(OH::OccpSpace, worker)) {
		  unsigned
		    worker = (offset - offsetof(OH::OccpSpace, worker)) / sizeof(OH::OccpWorker),
		    woffset = (offset - offsetof(OH::OccpSpace, worker)) % sizeof(OH::OccpWorker);
		  if (woffset < offsetof(OH::OccpWorkerRegisters, control)) {
		    printf("Worker %2d control operation write???: %s\n",
			   worker, ops[woffset/sizeof(uint32_t)]);
		  } else {
		    printf("Worker %2d register write of 0x%" PRIx32 " to %s\n", worker, data,
			   woffset == offsetof(OH::OccpWorkerRegisters, control) ? "control" :
			   woffset == offsetof(OH::OccpWorkerRegisters, window) ? "window" :
			   woffset == offsetof(OH::OccpWorkerRegisters, clearError) ? "clearError" :
			   woffset == offsetof(OH::OccpWorkerRegisters, lastConfig) ? "lastConfig" :
			   "unknown");
		    *(uint32_t *)&cadmin[offset] = data;
		  }
		} else
		  *(uint32_t *)&cadmin[offset] = data;
		HE::EtherControlWriteResponse &ecwr =  *(HE::EtherControlWriteResponse *)(echp);
		echp->length = htons(sizeof(ecwr)-2);
	      }
	      // Modify the outgoing packet last since we might be doing it in the
	      // received buffer (for uncached mode).
	      echp->typeEtc = OCCP_ETHER_TYPE_ETC(HE::OCCP_RESPONSE, HE::OK, uncache);
	      ocpiDebug("Sending read/write response packet: length is htons %x, ntohs %u tag %u",
			echp->length, ntohs(echp->length), echp->tag);
	    }
	    sent = s.send(out, ntohs(echp->length) + 2, from, 0, error);
	    break;
	  case HE::OCCP_NOP:
	    {
	      HE::EtherControlNop &ecn =  *(HE::EtherControlNop *)(rFrame.payload);
	      ocpiDebug("Received NOP: sizeof h %zd sizeof nop %zd offset %zd from %s\n",
			sizeof(HE::EtherControlHeader), sizeof(HE::EtherControlNop),
			offsetof(HE::EtherControlNop, mbx80), from.pretty());
	      ocpiAssert(ntohs(ech_in.length) == sizeof(ecn)-2);
	      HE::EtherControlNopResponse &ecnr =  *(HE::EtherControlNopResponse *)(rFrame.payload);
	      // Tag is the same
	      ech_in.length = htons(sizeof(ecnr)-2);
	      ech_in.typeEtc = OCCP_ETHER_TYPE_ETC(HE::OCCP_RESPONSE, HE::OK, uncache);
	      ecnr.mbx40 = 0x40;
	      ecnr.mbz0 = 0;
	      ecnr.mbz1 = 0;
	      ecnr.maxCoalesced = 1;
	      ocpiDebug("Sending nop response packet: length is sizeof %zu, htons %u, ntohs %u",
			sizeof(HE::EtherControlNopResponse), ech_in.length, ntohs(ech_in.length));
	      sent = s.send(rFrame, ntohs(ech_in.length)+2, from, 0, error);
	    }
	    break;
	  default:
	    bad("Invalid control packet type: typeEtc = 0x%x", ech_in.typeEtc);
	  }
	  if (sent)
	    ocpiDebug("response sent ok to %s %x %x %x\n",
		   from.pretty(), echp->length, echp->typeEtc, echp->tag);
	  else
	    ocpiDebug("response send error: %s\n", error.c_str());
	} else
	  bad("Slave Recv failed");
      } while (1);
    } else if (interface)
      OU::formatString(error, "interface %s is %s and %s", interface,
		       eif.up ? "up" : "down",
		       eif.connected ? "connected" : "not connected");
  }	  
  if (error.size())
    bad("Error getting interface %s", interface ? interface : "that is up and connected");
}

static void
ethers(const char **) {
  OE::IfScanner ifs(error);
  if (error.size())
    bad("Error establishing interface scanner");
  OE::Interface eif;
  while (ifs.getNext(eif, error))
    printf("Interface %s: address %s, %s, %s\n",
	   eif.name.c_str(), eif.addr.pretty(),
	   eif.up ? "up" : "down", eif.connected ? "connected" : "disconnected");
  if (error.size())
    bad("Error scanning for interfaces");
}
static void
testdma(const char **) {
  const char *dmaEnv;
  unsigned dmaMeg;
  unsigned long long dmaBase, dmaSize;
  int fd;
  volatile uint8_t *cpuBase;

  if (!(dmaEnv = getenv("OCPI_DMA_MEMORY")))
    bad("Warning: You must set the OCPI_DMA_MEMORY environment variable before using any OpenCPI FPGA device.\n"
	"         Use \"sudo -E\" to allow this program to have access to environmant variables");
  if (sscanf(dmaEnv, "%uM$0x%llx", &dmaMeg, (unsigned long long *) &dmaBase) != 2)
    bad("The OCPI_DMA_MEMORY environment variable is not formatted correctly");
  dmaSize = dmaMeg * 1024ull * 1024ull;
  printf("The OCPI_DMA_MEMORY environment variable indicates %uMB at 0x%llx, ending at 0x%llx\n",
	 dmaMeg, dmaBase, dmaBase + dmaSize);
  int pageSize = getpagesize();
  if (dmaBase & (pageSize - 1))
    bad("DMA memory starting address 0x%llx does not start on a page boundary, %u (0x%x)", 
	dmaBase, pageSize, pageSize);
  if (dmaSize & (pageSize -1))
    bad("DMA memory size %ull (0x%llx) does not start on a page boundary, %u (0x%x)", 
	dmaSize, dmaSize, pageSize, pageSize);
  if ((fd = open("/dev/mem", O_RDWR|O_SYNC)) < 0 ||
	     (cpuBase = (uint8_t*)mmap(NULL, (unsigned long long)dmaMeg * 1024 * 1024,
				       PROT_READ|PROT_WRITE, MAP_SHARED, fd, dmaBase)) ==
	     MAP_FAILED)
    bad("Can't map to local DMA memory defined in OCPI_DMA_MEMORY using /dev/mem (%s/%d). Forgot sudo -E?",
	strerror(errno), errno);
  uint8_t save = *cpuBase;

  if ((*cpuBase = 1, *cpuBase != 1) || (*cpuBase = 2, *cpuBase != 2)) {
    *cpuBase = save; // do this before any sys calls etc.
    bad("Can't write to start of local DMA memory defined in OCPI_DMA_MEMORY using /dev/mem");
  }
  *cpuBase = save; // on the wild chance that we shouldn't actually be touching this?
  cpuBase += dmaSize - 1ull;
  save = *cpuBase;
  if ((*cpuBase = 1, *cpuBase != 1) || (*cpuBase = 2, *cpuBase != 2)) {
    *cpuBase = save; // do this before any sys calls etc.
    bad("Can't write to end of local DMA memory defined in OCPI_DMA_MEMORY using /dev/mem");
  }
  *cpuBase = save; // on the wild chance that we shouldn't actually be touching this?
  printf("Successfully wrote and read back start and end of DMA memory\n");
}
static void
admin(const char **) {
  uint32_t i, j, k;
  time_t epochtime;
  struct tm *etime, *ntime;
  static union {
    uint64_t uint;
    char c[sizeof(uint64_t) + 1];
  } u;

  epochtime = (time_t)cAccess->get32Register(birthday, OH::OccpAdminRegisters);

  etime = gmtime(&epochtime); 
  //printf("%lld%lld\n", (long long)x, (long long)y);
  printf("OCCP Admin Space\n");
  u.uint = cAccess->get64Register(magic, OH::OccpAdminRegisters);
  printf(" OpenCpi:      0x%016llx \"%s\"\n", (unsigned long long)u.uint, u.c);
  printf(" revision:     0x%08x\n", cAccess->get32Register(revision, OH::OccpAdminRegisters));
  printf(" birthday:     0x%08x %s", (uint32_t)epochtime, asctime(etime));
  printf(" workerMask:   0x%08x workers", j = cAccess->get32Register(config, OH::OccpAdminRegisters));
  for (i = 0; i < sizeof(uint32_t) * 8; i++)
    if (j & (1 << i))
      printf(" %d", i);
  printf(" exist\n");
  printf(" pci_dev_id:   0x%08x\n", cAccess->get32Register(pciDevice, OH::OccpAdminRegisters));
  printf(" attention:    0x%08x\n", cAccess->get32Register(attention, OH::OccpAdminRegisters));
  printf(" cpStatus:     0x%08x\n", cAccess->get32Register(status, OH::OccpAdminRegisters));
  printf(" scratch20:    0x%08x\n", cAccess->get32Register(scratch20, OH::OccpAdminRegisters));
  printf(" scratch24:    0x%08x\n", cAccess->get32Register(scratch24, OH::OccpAdminRegisters));
  printf(" cpControl:    0x%08x\n", cAccess->get32Register(control, OH::OccpAdminRegisters));

  //  nowtime = (time_t)(cAccess.get32Register(time, OH::OccpAdminRegisters)); // FIXME WRONG ENDIAN IN FPGA
  i = cAccess->get32Register(timeStatus, OH::OccpAdminRegisters);
  printf(" timeStatus:   0x%08x ", i);
  if(i & 0x80000000) printf("ppsLostSticky ");
  if(i & 0x40000000) printf("gpsInSticky ");
  if(i & 0x20000000) printf("ppsInSticky ");
  if(i & 0x10000000) printf("timeSetSticky ");
  if(i & 0x08000000) printf("ppsOK ");
  if(i & 0x04000000) printf("ppsLost ");
  printf("\n");
  printf(" timeControl:  0x%08x\n", cAccess->get32Register(timeControl, OH::OccpAdminRegisters));
  uint64_t gpsTime = cAccess->get64Register(time, OH::OccpAdminRegisters);
  //  cAccess.set64Register(timeDelta, OH::OccpAdminRegisters, gpsTime);
  uint32_t gpsTimeLS = gpsTime >> 32;
  uint32_t gpsTimeMS = gpsTime & 0xffffffffll;
  time_t gpsNow = gpsTimeMS;
  ntime = gmtime(&gpsNow); 
  uint64_t deltaTime = cAccess->get64Register(timeDelta, OH::OccpAdminRegisters);
  uint32_t deltaTimeLS = deltaTime >> 32;
  uint32_t deltaTimeMS = deltaTime & 0xffffffffll;
  uint32_t deltaTimeNS = ((deltaTimeLS * 1000000000ull) + (1ull << 31)) / (1ull << 32);
  printf(" gpsTime:      0x%16llx (%llu)\n", (unsigned long long)gpsTime, (unsigned long long)gpsTime);
  printf(" gpsTimeMS:    0x%08x (%u) %s", gpsTimeMS,  gpsTimeMS, asctime(ntime));
  printf(" gpsTimeLS:    0x%08x (%u)\n",  gpsTimeLS,  gpsTimeLS);
  printf(" deltaTimeMS:  0x%08x (%u)\n", deltaTimeMS, deltaTimeMS);
  printf(" deltaTimeLS:  0x%08x (%u) (%uns)\n", deltaTimeLS, deltaTimeLS, deltaTimeNS);
  i = cAccess->get32Register(timeClksPerPps, OH::OccpAdminRegisters);
  printf(" refPerPPS:    0x%08x (%u)\n", i, i);
  i = cAccess->get32Register(readCounter, OH::OccpAdminRegisters);
  printf(" readCounter:  0x%08x (%u)\n", i, i);
  i = cAccess->get32Register(numRegions, OH::OccpAdminRegisters);
  printf(" numDPMemReg:  0x%08x (%u)\n", i, i);
  uint32_t regions[OCCP_MAX_REGIONS];
  cAccess->getRegisterBytes(regions, regions, OH::OccpAdminRegisters);
  if (i < 16) 
    for (k=0; k<i; k++)
      printf("    DP%2d:      0x%08x\n", k, regions[k]);

  // Print out the 64B 16DW UUID in little-endian looking format...
  uint32_t uuid[16];
  cAccess->getRegisterBytes(uuid, uuid, OH::OccpAdminRegisters);
  for (k=0;k<16;k+=4)
    printf(" UUID[%2d:%2d]:  0x%08x 0x%08x 0x%08x 0x%08x\n",
	   k+3, k, uuid[k+3], uuid[k+2], uuid[k+1], uuid[k]);
}
static voidpf zalloc(voidpf , uInt items, uInt size) {
  return malloc(items * size);
}
static void zfree(voidpf , voidpf data) {
  free(data);
}
static void
bram(const char **ap) {
  if (!ap[0])
    bad("No input filename specified for bram");
  if (!ap[1])
    bad("No output filename specified for bram");
  unsigned char *in = 0, *out;
  int fd = open(*ap, O_RDONLY);
  if (fd < 0)
    bad("Cannot open file: \"%s\"", *ap);
  FILE *ofp = fopen(ap[1], "w");
  if (!ofp)
    bad("Cannot open output file '%s'", ap[1]);
  off_t length;
  if (fd >= 0 &&
      (length = lseek(fd, 0, SEEK_END)) != -1 &&
      lseek(fd, 0, SEEK_SET) == 0 &&
      (in = (unsigned char*)malloc(length)) &&
      read(fd, in, length) == length &&
      (out = (unsigned char*)malloc(length))) {
    z_stream zs;
    zs.zalloc = zalloc;
    zs.zfree = zfree;
    zs.next_in = in;
    zs.avail_in = length;
    zs.next_out = out;
    zs.avail_out = length;
    zs.data_type = Z_TEXT;
    if (deflateInit(&zs, 9) == Z_OK &&
	deflate(&zs, Z_FINISH) == Z_STREAM_END &&
	deflateEnd(&zs) == Z_OK) {
      unsigned oWords = (zs.total_out + 3)/4;
      fprintf(ofp, "%08lx\n%08lx\n%08lx\n%08lx\n", 1ul, zs.total_out, (unsigned long)length, zs.adler);
      for (uint32_t *u32p = (uint32_t *)out; oWords; oWords--)
	fprintf(ofp, "%08x\n", *u32p++);
      if (fclose(ofp)) {
	unlink(ap[1]);
	bad("Error writing output file '%s'", ap[1]);
      }
      printf("Wrote bram file '%s' (%lu bytes) from file '%s' (%lu bytes)\n",
	     ap[1], zs.total_out, *ap, (unsigned long)length);
    }
  }
}
static void
unbram(const char **ap) {
  if (!*ap)
    bad("No input bram filename specified for unbram");
  if (!ap[1])
    bad("No output filename specified for unbram");
  FILE *ifp = fopen(ap[0], "r");
  if (!ifp)
    bad("Cannot open input file '%s'", ap[0]);
  FILE *ofp = fopen(ap[1], "wb");
  if (!ofp)
    bad("Cannot open output file: \"%s\"", ap[1]);
  size_t version, bytes, length, adler;
  if (fscanf(ifp, "%zx\n%zx\n%zx\n%zx\n", &version, &bytes, &length, &adler) != 4)
    bad("Input file has bad format");
  unsigned
    nWords = (bytes + 3)/4,
    nBytes = nWords * sizeof(uint32_t);
  unsigned char *in = (unsigned char *)malloc(nBytes);
  if (!in)
    bad("Error allocating %zu bytes for input file", nBytes);
  unsigned char *out = (unsigned char *)malloc(length);
  if (!out)
    bad("Error allocating %zu bytes for input file", length);
  for (uint32_t *u32p = (uint32_t *)in; nWords; nWords--) {
    size_t n;
    if (fscanf(ifp, "%zx\n", &n) != 1)
      bad("Error reading input file");
    *u32p++ = n;
  }
  z_stream zs;  
  zs.zalloc = zalloc;
  zs.zfree = zfree;
  zs.data_type = Z_TEXT;
  zs.next_in = in;
  zs.avail_in = nBytes;
  zs.next_out = out;
  zs.avail_out = length;
  if (inflateInit(&zs) == Z_OK &&
      inflate(&zs, Z_FINISH) == Z_STREAM_END &&
      inflateEnd(&zs) == Z_OK) {
    if (zs.adler != adler || zs.total_out != length)
      bad("bad checksum on decompressed data: is %lx, should be %zx", zs.adler, adler);
    if (fwrite(out, 1, length, ofp) != length || fclose(ofp)) {
      unlink(ap[1]);
      bad("Error writing output file '%s'", ap[1]);
    }
    printf("Wrote unbram file '%s' (%zu bytes) from file '%s' (%zu bytes)\n",
	   ap[1], length, ap[0], bytes);
  }
}

static void
uuid(const char **ap) {
  OCPI::HDL::HdlUUID uuidRegs;
  if (!platform || !part)
    bad("both platform and part/chip must be specified for the uuid command");
  if (!*ap)
    bad("No output uuid verilog specified for uuid command");
  FILE *ofp = fopen(ap[0], "wb");
  if (!ofp)
    bad("Cannot open output file: \"%s\"", ap[1]);
  if (strlen(platform) > sizeof(uuidRegs.platform) - 1)
    bad("Platform: '%s' is too long.  Must be < %u long",
	platform, sizeof(uuidRegs.platform) - 1);
  if (strlen(part) > sizeof(uuidRegs.device) - 1)
    bad("Part/chip: '%s' is too long.  Must be < %u long",
	part, sizeof(uuidRegs.device) - 1);
  uuid_t uuid;
  uuid_generate(uuid);
  memcpy(uuidRegs.uuid, uuid, sizeof(uuidRegs.uuid));
  uuidRegs.birthday = time(0);
  strncpy(uuidRegs.platform, platform, sizeof(uuidRegs.platform));
  strncpy(uuidRegs.device, part, sizeof(uuidRegs.device));
  strncpy(uuidRegs.load, "", sizeof(uuidRegs.load));
  assert(sizeof(uuidRegs) * 8 == 512);
  uuid_string_t textUUID;
  uuid_unparse_lower(uuid, textUUID);
  fprintf(ofp,
	  "// UUID generated for platform '%s', device '%s', uuid '%s'\n"
	  "module mkUUID(uuid);\n"
	  "output [511 : 0] uuid;\nwire [511 : 0] uuid = 512'h",
	  platform, part, textUUID);
  for (unsigned n = 0; n < sizeof(uuidRegs); n++)
    fprintf(ofp, "%02x", ((char*)&uuidRegs)[n]&0xff);
  fprintf(ofp, ";\nendmodule // mkUUID\n");
  if (fclose(ofp))
    bad("Could close output file '%s'. No space?", ap[0]);
}
  
static void
reset(const char **) {
  // FIXME:  need to copy PCI config.
  cAccess->set32Register(reset, OH::OccpAdminRegisters, 0xc0deffff);
}
static uint64_t
atoi_any(const char *arg, unsigned *sizep)
{
  uint64_t value ;

  int n = sscanf(arg, strncmp(arg,"0x",2) != 0 ? "%" SCNu64 : "0x%" SCNx64, &value);
  if (n != 1)
    bad("Bad numeric value: '%s'", arg);
  if (sizep) {
    const char *sp;
    if ((sp = strchr(arg, '/')))
      switch(*++sp) {
      default:
	bad("Bad size specifier in %s: must be 1, 2, or 4", arg);
	abort();
      case '1':
      case '2':
      case '4':
      case '8':
	*sizep = *sp - '0';
      }
    else
      *sizep = 4;
  }
  return value ;
}

static void
radmin(const char **ap) {
  unsigned size;
  unsigned off = (unsigned)atoi_any(*ap, &size);
  if (size == 4) {
    uint32_t x = cAccess->get32RegisterOffset(off);
    if (parseable)
      printf("0x%" PRIx32 "\n", x);
    else
      printf("Admin for hdl-device '%s' at offset 0x%x is 0x%x (%u)\n",
	     device, off, x, x);
  } else if (size == 8) {
    uint64_t x = cAccess->get64RegisterOffset(off);
    if (parseable)
      printf("0x%" PRIx64 "\n", x);
    else
      printf("Admin for hdl-device '%s' at offset 0x%x is 0x%"PRIx64" (%"PRIi64")\n",
	     device, off, x, x);
  } else
    bad("bad size for radmin");
}

static void
rmeta(const char **ap) {
  unsigned size;
  unsigned off = (unsigned)atoi_any(*ap, &size);
  if (size == 4) {
    uint32_t x = cAccess->get32RegisterOffset(off + offsetof(OH::OccpSpace,configRam));
    if (parseable)
      printf("0x%" PRIx32 "\n", x);
    else
      printf("Metadata for hdl-device '%s' at offset 0x%x is 0x%x (%u)\n",
	     device, off, x, x);
  } else if (size == 8) {
    uint64_t x = cAccess->get64RegisterOffset(off + offsetof(OH::OccpSpace,configRam));
    if (parseable)
      printf("0x%" PRIx64 "\n", x);
    else
      printf("Metadata for hdl-device '%s' at offset 0x%x is 0x%"PRIx64" (%"PRIi64")\n",
	     device, off, x, x);
  } else
    bad("bad size for rmeta");
}

static void
wadmin(const char **ap) {
  unsigned size;
  unsigned off = (unsigned)atoi_any(*ap++, &size);
  uint64_t val = atoi_any(*ap, 0);
  if (size == 4)
    cAccess->set32RegisterOffset(off, (uint32_t)val);
  else if (size == 8)
    cAccess->set64RegisterOffset(off, val);
  else
    bad("bad size for wadmin");
}
static void
settime(const char **) {
  struct timeval tv;
  gettimeofday(&tv, NULL); 
  uint32_t fraction =
    (((uint64_t)tv.tv_usec * 1000 * (1ull << 32) ) + 500000000ull) / 1000000000ull;
  // Write 64 bit value
  // When it goes on the PCIe wire, it will be "endianized".
  // On intel, first DW will be LSB.  On PPC, first DW will be MSB.
  
#define FPGA_IS_OPPOSITE_ENDIAN_FROM_CPU() 1

  cAccess->set64Register(time, OH::OccpAdminRegisters, 
#if FPGA_IS_OPPOSITE_ENDIAN_FROM_CPU()
			((uint64_t)fraction << 32) | tv.tv_sec
#else
			((uint64_t)tv.tv_sec << 32) | fraction
#endif
			);
}
typedef unsigned long long ull; 
static inline int64_t ticks2ns(uint64_t ticks) {
  return ((ticks) * 1000000000ull + (1ull << 31))/ (1ull << 32);
}
static inline int64_t dticks2ns(int64_t ticks) {
  return ((ticks) * 1000000000ll + (1ll << 31))/ (1ll << 32);
}
static inline ull ns2ticks(uint32_t sec, uint32_t nsec) {
  return (((uint64_t)(sec)) << 32ull) + ((nsec) + 500000000ull) * (1ull<<32) /1000000000;
}

static int compu32(const void *a, const void *b) { return *(int32_t*)a - *(int32_t*)b; }
static inline uint64_t swap32(uint64_t x) {return (x <<32) | (x >> 32); }
static void
deltatime(const char **) {
  unsigned n;
  int32_t delta[100];
  int64_t sum = 0;
  
  for (n = 0; n < 100; n++) {
    uint64_t time = cAccess->get64Register(time, OH::OccpAdminRegisters);
    cAccess->set64Register(timeDelta, OH::OccpAdminRegisters, time);
    delta[n] = (int32_t)swap32(cAccess->get64Register(timeDelta, OH::OccpAdminRegisters));
  }
  qsort(delta, 100, sizeof(int32_t), compu32);
  
  for (n = 0; n < 90; n++)
    sum += delta[n];
  sum = (sum + 45) / 90;
  // we have average delay
  printf("Delta ns min %"PRIi64" max %"PRIi64". average: (of best 90 out of 100) %"PRIi64"\n",
	 ticks2ns(delta[0]), ticks2ns(delta[99]), ticks2ns(sum));
  uint64_t time = swap32(cAccess->get64Register(time, OH::OccpAdminRegisters));
  cAccess->set64Register(timeDelta, OH::OccpAdminRegisters, swap32(time + sum));
  int32_t deltatime = swap32(cAccess->get64Register(timeDelta, OH::OccpAdminRegisters));
  printf("Now after correction, delta is: %"PRIi64"ns\n", dticks2ns(deltatime));
}
static void
wdump(const char **) {
  printf("Worker %u on device %s\n", worker, device);
  uint32_t i = wAccess.get32Register(status, OH::OccpWorkerRegisters);
  printf(" Status:     0x%08x", i);
  if (i & OCCP_STATUS_CONTROL_ERROR)
    printf(" ctlError");
  if (i & OCCP_STATUS_READ_ERROR)
    printf(" rdError");
  if (i & OCCP_STATUS_WRITE_ERROR)
    printf(" wrtError");
  if (i & OCCP_STATUS_CONTROL_TIMEOUT)
    printf(" ctlTimeout");
  if (i & OCCP_STATUS_READ_TIMEOUT)
    printf(" rdTimeout");
  if (i & OCCP_STATUS_SFLAG)
    printf(" sflag");
  if (i & OCCP_STATUS_CONFIG_ADDR_VALID)
    printf(" addrValid");
  if (i & OCCP_STATUS_CONFIG_BE_VALID)
    printf(" beValid:0x%x", (i & OCCP_STATUS_CONFIG_BE) >> 20);
  static const char *ops[] = { "init", "start", "stop", "release", "test", "bQuery", "aConfig", "rsvd7"};
  if (i & OCCP_STATUS_CONFIG_OP_VALID)
    printf(" opValid:0x%x:%s", (i & OCCP_STATUS_CONFIG_OP) >> 24, ops[ (i & OCCP_STATUS_CONFIG_OP) >> 24]);
  if (i & OCCP_STATUS_CONFIG_WRITE_VALID)
    printf(" wrtValid:%d", (i & OCCP_STATUS_CONFIG_WRITE) ? 1 : 0);
  printf("\n");
  printf(" Control:    0x%08x\n", wAccess.get32Register(control, OH::OccpWorkerRegisters));
  printf(" ConfigAddr: 0x%08x\n", wAccess.get32Register(lastConfig, OH::OccpWorkerRegisters));
  printf(" pageWindow: 0x%08x\n", wAccess.get32Register(window, OH::OccpWorkerRegisters));
}

static int
wwreset(OH::Access &w) {
  uint32_t i = w.get32Register(control, OH::OccpWorkerRegisters);
  w.set32Register(control, OH::OccpWorkerRegisters, i & ~OCCP_WORKER_CONTROL_ENABLE);
  return i;
}
static void
wreset(const char **) {
  uint32_t i = wwreset(wAccess);
  printf("Worker %u on device %s: reset asserted, was %s\n", 
	 worker, device, (i & OCCP_WORKER_CONTROL_ENABLE) ? "deasserted" : "already asserted");
}
static int
wwunreset(OH::Access &w) {
  uint32_t i = w.get32Register(control, OH::OccpWorkerRegisters);
  w.set32Register(control, OH::OccpWorkerRegisters, i  | OCCP_WORKER_CONTROL_ENABLE);
  return i;
}
static void
wunreset(const char **) {
  uint32_t i = wwunreset(wAccess);
  printf("Worker %u on device %s: reset deasserted, was %s\n", 
	 worker, device, (i & OCCP_WORKER_CONTROL_ENABLE) ? "already deasserted" : "asserted");
}

#define WOP(name) offsetof(OH::OccpWorkerRegisters, name)
static uint32_t
wwop(OH::Access &w, const char *op) {
  for (unsigned i = 0; ops[i]; i++) 
    if (!strcasecmp(ops[i], op)) {
      ocpiDebug("Worker control op: %s, %d offset %lx", op, i,
		offsetof(OH::OccpWorkerRegisters, initialize) + i * sizeof(uint32_t));
     return w.get32RegisterOffset(offsetof(OH::OccpWorkerRegisters, initialize) +
				   i * sizeof(uint32_t));
    }
  bad("Unknown control operation: `%s'", op);
  return 0;
}      

static void
wop(const char **ap) {
  if (!*ap)
    bad("Missing control operation name for this command");
  uint32_t r = wwop(wAccess, *ap);
  if (parseable)
    printf("0x%" PRIx32 "\n", r);
  else
    printf("Worker %u on device %s: the '%s' control operation was performed with result '%s' (0x%x)\n",
	   worker, device, *ap,
	   r == OCCP_ERROR_RESULT ? "error" :
	   r == OCCP_TIMEOUT_RESULT ? "timeout" :
	   r == OCCP_RESET_RESULT ? "reset" :
	   r == OCCP_SUCCESS_RESULT ? "success" : 
	   r == OCCP_FATAL_RESULT  ? "fatal" : "unknown",
	   r);
}

static void
wwctl(const char **ap) {
  uint32_t i = (uint32_t)atoi_any(*ap++, 0);
  wAccess.set32Register(control, OH::OccpWorkerRegisters, i);
  printf("Worker %u on device %s: writing control register value: 0x%x\n",
	 worker, device, i);
}
static void
wclear(const char **) {
  uint32_t i = wAccess.get32Register(control, OH::OccpWorkerRegisters);
  wAccess.set32Register(control, OH::OccpWorkerRegisters,
			i | OCCP_STATUS_SFLAG | OCCP_STATUS_WRITE_TIMEOUT);
  printf("Worker %u on device %s: clearing errors from status register\n",
	 worker, device);
}
static void
wwpage(const char **ap) {
  uint32_t i = (uint32_t)atoi_any(*ap++, 0);
  wAccess.set32Register(window, OH::OccpWorkerRegisters, i);
  printf("Worker %u on device %s: setting window register to 0x%x (%u)\n",
	 worker, device, i, i);
}
static void
wread(const char **ap) {
  unsigned size;
  unsigned off = (unsigned)atoi_any(*ap++, &size);
  unsigned n = *ap ? (unsigned)atoi_any(*ap, 0) : 1;
  if (!parseable)
    printf("Worker %u on device %s: read config offset 0x%x size %u count %u\n",
	   worker, device, off, size, n);

  for (unsigned n = *ap ? (unsigned)atoi_any(*ap, 0) : 1; n--; off += size) {
    uint64_t i;
    switch (size) {
    case 1:
      i = confAccess.get8RegisterOffset(off); break;
    case 2:
      i = confAccess.get16RegisterOffset(off); break;
    case 4:
      i = confAccess.get32RegisterOffset(off); break;
    case 8:
      i = confAccess.get64RegisterOffset(off); break;
    }
    if (parseable)
      printf("0x%08" PRIx64"\n", i);
    else
      printf("  offset 0x%08x (%8u), value 0x%08" PRIx64 " (%10" PRIu64 ")\n",
	     off, off, i, i);
  }
}
static void
wwrite(const char **ap) {
  unsigned size;
  unsigned off = (unsigned)atoi_any(*ap++, &size);
  uint64_t val = atoi_any(*ap, 0);

  switch (size) {
  case 1:
    confAccess.set8RegisterOffset(off, (uint8_t)val); break;
  case 2:
    confAccess.set16RegisterOffset(off, (uint16_t)val); break;
  case 4:
    confAccess.set32RegisterOffset(off, (uint32_t)val); break;
  case 8:
    confAccess.set64RegisterOffset(off, val); break;
  }
  printf("Worker %u on device %s: wrote config offset 0x%x size %u: value is 0x%" PRIx64 "(%" PRIu64 ")\n",
	 worker, device, off, size, val, val);
}
static OE::Socket *getSock() {
  OE::Interface i(interface, error);
  if (error.size())
    bad("Opening interface");
  OE::Socket *s = new OE::Socket(i, ocpi_data, NULL, 123, error);
  if (error.size()) {
    delete s;
    bad("opening ethernet socket for data");
  }
  return s;
}

static void
sendData(const char **ap) {
  OE::Address a(ap[0]);
  if (a.error())
    bad("Establishing remote address as %s", ap[0]);
  OE::Socket *s = getSock();
  OE::Packet p;
  ((uint16_t *)p.payload)[1] = 123;
  for (unsigned n = 0; n < 10; n++) {
    p.payload[4] = n;
    if (!s->send(p, 5, a, 0, error))
      bad("sending");
  }
}
static void
receiveData(const char **/*ap*/) {
  OE::Socket *s = getSock();
  OE::Packet p;
  OE::Address from;
  for (unsigned n = 0; n < 10; n++) {
    unsigned len;
    if (!s->receive(p, len, 0, from, error))
      bad("receiving");
    if (p.payload[2] != 123 || p.payload[4] != n)
      bad("received %d/%d, wanted %d/%d", p.payload[2], p.payload[4], 123, n);
  }
  printf("Got 10 good packets\n");
}
// Receive an RDMA stream using the ethernet datagram transport
// If there is a file name argument, we are exchanging descriptors between software endpoints
// otherwise we are dealing directly with an HDL hardware endpoint
#define EDP_WORKER 13
#define SMA_WORKER 6
#define GBE_WORKER 9
#define GEN_WORKER 5
static void
receiveRDMA(const char **ap) {
  if (!*ap)
    setupDevice(false);
  std::string file;
  OD::TransportGlobal &global(OC::Manager::getTransportGlobal());
  OD::Transport transport(&global, false);
  if (endpoint.empty())
    endpoint = "ocpi-ether-rdma";
  OU::PValue params[2] = { 
    OA::PVString("remote", endpoint.c_str()),
    OA::PVEnd
  };
  OD::Descriptor myInputDesc;
  // Initialize out input descriptor before it is processed and
  // completed by the transport system.
  myInputDesc.type = OD::ConsumerDescT;
  myInputDesc.role = OD::ActiveFlowControl;
  myInputDesc.options = 1 << OD::MandatedRole;
  myInputDesc.desc.nBuffers = 10;
  myInputDesc.desc.dataBufferBaseAddr = 0;
  myInputDesc.desc.dataBufferPitch = 0;
  myInputDesc.desc.dataBufferSize = 4096;
  myInputDesc.desc.metaDataBaseAddr = 0;
  myInputDesc.desc.metaDataPitch = 0;
  myInputDesc.desc.fullFlagBaseAddr =  0;
  myInputDesc.desc.fullFlagSize = 0;
  myInputDesc.desc.fullFlagPitch = 0;
  myInputDesc.desc.fullFlagValue = 0;
  myInputDesc.desc.emptyFlagBaseAddr = 0;
  myInputDesc.desc.emptyFlagSize = 0;
  myInputDesc.desc.emptyFlagPitch = 0;
  myInputDesc.desc.emptyFlagValue = 0;
  myInputDesc.desc.oob.port_id = 0;
  memset( myInputDesc.desc.oob.oep, 0, sizeof(myInputDesc.desc.oob.oep));
  myInputDesc.desc.oob.cookie = 0;
  OD::Port &port = *transport.createInputPort(myInputDesc, params);
  ocpiDebug("Our input descriptor: %s", myInputDesc.desc.oob.oep);
  // Now the normal transport/transfer driver has initialized (not finalized) the input port.
  OD::Descriptor theOutputDesc;
  OH::Access
    edpAccess, edpConfAccess, smaAccess, smaConfAccess, gbeAccess, gbeConfAccess,
    genAccess, genConfAccess;
  if (*ap) {
    // If we are exchanging descriptors, write it out to the given file with ".in" suffix
    // Pause (getchar), and then read in the output descriptor from the other side.
    file = *ap;
    file += ".in";
    // Strip out our ether interface.
    OD::Descriptor sendDesc = myInputDesc;
    const char *sp = strchr(myInputDesc.desc.oob.oep,'/');
    std::string clean("ocpi-ether-rdma:");
    clean.append(sp+1);
    strcpy(sendDesc.desc.oob.oep, clean.c_str());
    int fd = creat(file.c_str(), 0666);
    if (fd < 0 ||
	write(fd, &sendDesc, sizeof(sendDesc)) != sizeof(sendDesc) ||
	close(fd) < 0)
      bad("Creating temp file for RDMA descriptor");
    printf("Input Descriptor written to %s.\nWaiting for a keystroke...", file.c_str());
    fflush(stdout);
    getchar();
    file = *ap;
    file += ".out";
    fd = open(file.c_str(), O_RDONLY);
    if (fd < 0 ||
	read(fd, &theOutputDesc, sizeof(theOutputDesc)) != sizeof(theOutputDesc) ||
	close(fd) < 0)
      bad("Reading file for RDMA output descriptor");
    // Now we have the output descriptor for the remote output port
  } else {
    // The output port is a hardware port.  We need to program the hardware 
    // as well as produce/synthesize the output descriptor for the remote hardware port.
    // 1. Get acccessors to all workers with control and configuration accessors for each.
    cAccess->offsetRegisters(edpAccess, (intptr_t)(&((OH::OccpSpace*)0)->worker[EDP_WORKER]));
    cAccess->offsetRegisters(edpConfAccess, (intptr_t)(&((OH::OccpSpace*)0)->config[EDP_WORKER]));
    cAccess->offsetRegisters(smaAccess, (intptr_t)(&((OH::OccpSpace*)0)->worker[SMA_WORKER]));
    cAccess->offsetRegisters(smaConfAccess, (intptr_t)(&((OH::OccpSpace*)0)->config[SMA_WORKER]));
    cAccess->offsetRegisters(gbeAccess, (intptr_t)(&((OH::OccpSpace*)0)->worker[GBE_WORKER]));
    cAccess->offsetRegisters(gbeConfAccess, (intptr_t)(&((OH::OccpSpace*)0)->config[GBE_WORKER]));
    cAccess->offsetRegisters(genAccess, (intptr_t)(&((OH::OccpSpace*)0)->worker[GEN_WORKER]));
    cAccess->offsetRegisters(genConfAccess, (intptr_t)(&((OH::OccpSpace*)0)->config[GEN_WORKER]));
    wwreset(edpAccess);
    wwreset(smaAccess);
    wwreset(gbeAccess);
    wwreset(genAccess);
    wwunreset(edpAccess);
    wwunreset(smaAccess);
    wwunreset(gbeAccess);
    wwunreset(genAccess);
    ocpiCheck(wwop(edpAccess, "initialize") == OCCP_SUCCESS_RESULT);
    ocpiCheck(wwop(smaAccess, "initialize") == OCCP_SUCCESS_RESULT);
    ocpiCheck(wwop(gbeAccess, "initialize") == OCCP_SUCCESS_RESULT);
    ocpiCheck(wwop(genAccess, "initialize") == OCCP_SUCCESS_RESULT);
    // 2. Synthesize the output descriptor
    theOutputDesc.type = OD::ProducerDescT;
    theOutputDesc.role = OD::ActiveMessage;
    theOutputDesc.options = 1 << OD::MandatedRole;
    theOutputDesc.desc.nBuffers = 3;
    theOutputDesc.desc.dataBufferBaseAddr = 0;
    theOutputDesc.desc.dataBufferPitch = 4096;
    theOutputDesc.desc.dataBufferSize = 4096;
    theOutputDesc.desc.metaDataBaseAddr = 4096 * theOutputDesc.desc.nBuffers;
    theOutputDesc.desc.metaDataPitch = sizeof(OH::OcdpMetadata);
    theOutputDesc.desc.fullFlagBaseAddr = 
      theOutputDesc.desc.metaDataBaseAddr + theOutputDesc.desc.metaDataPitch * theOutputDesc.desc.nBuffers;
    theOutputDesc.desc.fullFlagSize = 4;
    theOutputDesc.desc.fullFlagPitch = 4;
    theOutputDesc.desc.fullFlagValue = 1;
    theOutputDesc.desc.emptyFlagBaseAddr = edpConfAccess.physOffset(offsetof(OH::OcdpProperties, nRemoteDone));
    theOutputDesc.desc.emptyFlagSize = 4;
    theOutputDesc.desc.emptyFlagPitch = 0;
    theOutputDesc.desc.emptyFlagValue = 1;
    theOutputDesc.desc.oob.port_id = 0;
    theOutputDesc.desc.oob.cookie = 0;
    uint32_t outputEndPointSize = edpConfAccess.get32Register(memoryBytes, OH::OcdpProperties);
    OX::EndPoint &outputEndPoint =
      OX::getManager().allocateProxyEndPoint(endpoint.c_str(), outputEndPointSize);
    OD::Transport::fillDescriptorFromEndPoint(outputEndPoint, theOutputDesc);
  }
  // Finalizing the input port takes: role, type flow, emptyflagbase, size, pitch, value
  // This makes the input port ready to data from the output port
  port.finalize(theOutputDesc, myInputDesc);
  ocpiDebug("Our output descriptor is: %s", theOutputDesc.desc.oob.oep);
  if (!*ap) {
    // If the other side is software (sendRDMA below), it will start when told.
    // If it is hardware we setup and start the workers now
    // We aren't passing descriptors, but programming the hardware and
    // synthesizing the output descriptor

    // Configure the 4 workers, upstream to downstream:
    // 1. The message generator set up to send 10 messages of 16 bytes each.
    struct Generator {
      uint32_t
        enable,         // 0x00
	wrapCount,      // 0x04
	messagesSent,   // 0x08
	dataSent,       // 0x0c
	messagesToSend, // 0x10
	status;         // 0x14
    };
    genAccess.set32Register(window, OH::OccpWorkerRegisters, 0);     // for normal config
    //    genConfAccess.set32Register(enable, Generator, 1);
    genConfAccess.set32Register(wrapCount, Generator, 1);
    genConfAccess.set32Register(messagesToSend, Generator, 10);
    genAccess.set32Register(window, OH::OccpWorkerRegisters, 0x400); // setup for metadata
    genConfAccess.set32Register(length, OH::OcdpMetadata, 16);
    genConfAccess.set32Register(opCode, OH::OcdpMetadata, 1);
    genAccess.set32Register(window, OH::OccpWorkerRegisters, 0x800); // setup for data
    genConfAccess.set32RegisterOffset(0, 1);
    genConfAccess.set32RegisterOffset(4, 2);
    genConfAccess.set32RegisterOffset(8, 3);
    genConfAccess.set32RegisterOffset(0xc, 4);
    genAccess.set32Register(window, OH::OccpWorkerRegisters, 0);     // for normal config
    // 2. The SMA adapter converting from WSI to WMI
    smaConfAccess.set32RegisterOffset(0, 2); // WSI to WMI
    // 3. The EDP, set up to perform datagram RDMA
    // Tell the output side about the input side, as determined by createInputPort
    edpConfAccess.set32Register(control, OH::OcdpProperties,
				OCDP_CONTROL(OCDP_CONTROL_PRODUCER, OH::OCDP_ACTIVE_MESSAGE));
    edpConfAccess.set32Register(nRemoteBuffers, OH::OcdpProperties, myInputDesc.desc.nBuffers);
    edpConfAccess.set32Register(remoteBufferBase, OH::OcdpProperties, myInputDesc.desc.dataBufferBaseAddr);
    edpConfAccess.set32Register(remoteMetadataBase, OH::OcdpProperties, myInputDesc.desc.metaDataBaseAddr);
    edpConfAccess.set32Register(remoteBufferSize, OH::OcdpProperties, myInputDesc.desc.dataBufferPitch);
    edpConfAccess.set32Register(remoteMetadataSize, OH::OcdpProperties, myInputDesc.desc.metaDataPitch);
    edpConfAccess.set32Register(remoteFlagBase, OH::OcdpProperties, myInputDesc.desc.fullFlagBaseAddr);
    edpConfAccess.set32Register(remoteFlagPitch, OH::OcdpProperties, myInputDesc.desc.fullFlagPitch);
    // Program the source/destination ids for remote DMA
    uint32_t val = myInputDesc.desc.oob.mailBox | (theOutputDesc.desc.oob.mailBox << 16);
    ocpiDebug("DW with DestId in LSB and SourceID in MSB is 0x%x", val);
    edpConfAccess.set32Register(remoteBufferHi, OH::OcdpProperties, val);

    // Program the local part of the output side - how data is placed in local buffers
    edpConfAccess.set32Register(nLocalBuffers, OH::OcdpProperties, theOutputDesc.desc.nBuffers);
    edpConfAccess.set32Register(localBufferSize, OH::OcdpProperties, theOutputDesc.desc.dataBufferPitch);
    edpConfAccess.set32Register(localBufferBase, OH::OcdpProperties, theOutputDesc.desc.dataBufferBaseAddr);
    edpConfAccess.set32Register(localMetadataBase, OH::OcdpProperties, theOutputDesc.desc.metaDataBaseAddr);
    edpConfAccess.set32Register(localMetadataSize, OH::OcdpProperties, theOutputDesc.desc.metaDataPitch);
    // 4. The GBE worker that inserts the MAC header fields.
    OE::Address addr(strchr(myInputDesc.desc.oob.oep, '/') + 1);
    union {
      struct {
	uint8_t addr[6];
	uint16_t ethertype;
      };
      uint64_t value;
    } value;
    memcpy(value.addr, addr.addr(), addr.s_size);
    value.ethertype = OCDP_ETHER_TYPE;
    ocpiDebug("Telling device to send to %s with ethertype %x",
	      addr.pretty(), OCDP_ETHER_TYPE);
    gbeConfAccess.set64RegisterOffset(0x10, value.value);
    // Now that they are all configured, start from downstream to upstream.
    ocpiCheck(wwop(gbeAccess, "start") == OCCP_SUCCESS_RESULT);
    ocpiCheck(wwop(edpAccess, "start") == OCCP_SUCCESS_RESULT);
    ocpiCheck(wwop(smaAccess, "start") == OCCP_SUCCESS_RESULT);
    ocpiCheck(wwop(genAccess, "start") == OCCP_SUCCESS_RESULT);
    genConfAccess.set32Register(enable, Generator, 1);
  }
  for (unsigned n = 0; n < 10; n++) {
    uint8_t opCode;
    uint32_t length;
    void *vdata;
    OD::BufferUserFacet *buf;
    unsigned t;
    for (t = 0; t < 1000000 && !(buf = port.getNextFullInputBuffer(vdata, length, opCode)); t++)
      OS::sleep(1);
    if (t == 1000000)
      ocpiBad("Receive DMA timeout\n");
    else {
      ocpiDebug("Received RDMA buffer: first DW 0x%" PRIx32 ", length %" PRIu32 ", op %u\n",
		*(uint32_t *)vdata, length, opCode);
      if (opCode != n)
	ocpiBad("Bad opcode: wanted %u, got %u", n, opCode);
      port.releaseInputBuffer(buf);
    }
  }
  printf("Received 10 RDMA buffers\n");
  port.reset();
}
// Send an RDMA stream using a datagram transport
static void
sendRDMA(const char **ap) {
  if (!*ap)
    setupDevice(false);
  std::string file;
  OD::TransportGlobal &global(OC::Manager::getTransportGlobal());
  OD::Transport transport(&global, false);
  OD::Descriptor myOutputDesc;
  myOutputDesc.type = OD::ProducerDescT;
  myOutputDesc.role = OD::ActiveMessage;
  myOutputDesc.options = 1 << OD::MandatedRole;
  myOutputDesc.desc.nBuffers = 10;
  myOutputDesc.desc.dataBufferBaseAddr = 0;
  myOutputDesc.desc.dataBufferPitch = 0;
  myOutputDesc.desc.dataBufferSize = 4096;
  myOutputDesc.desc.metaDataBaseAddr = 0;
  myOutputDesc.desc.metaDataPitch = 0;
  myOutputDesc.desc.fullFlagBaseAddr =  0;
  myOutputDesc.desc.fullFlagSize = 0;
  myOutputDesc.desc.fullFlagPitch = 0;
  myOutputDesc.desc.fullFlagValue = 0;
  myOutputDesc.desc.emptyFlagBaseAddr = 0;
  myOutputDesc.desc.emptyFlagSize = 0;
  myOutputDesc.desc.emptyFlagPitch = 0;
  myOutputDesc.desc.emptyFlagValue = 0;
  myOutputDesc.desc.oob.port_id = 0;
  memset( myOutputDesc.desc.oob.oep, 0, sizeof(myOutputDesc.desc.oob.oep));
  myOutputDesc.desc.oob.cookie = 0;

  OD::Descriptor theInputDesc;
  if (*ap) {
    file = *ap;
    file += ".in";
    int fd = open(file.c_str(), O_RDONLY);
    if (fd < 0 ||
	read(fd, &theInputDesc, sizeof(theInputDesc)) != sizeof(theInputDesc) ||
	close(fd) < 0)
      bad("Reading file for RDMA input descriptor");
    ocpiDebug("Received descriptor for: %s", myOutputDesc.desc.oob.oep);
  } else {
    theInputDesc.type = OD::ProducerDescT;
    theInputDesc.role = OD::ActiveMessage;
    theInputDesc.options = 1 << OD::MandatedRole;
    theInputDesc.desc.nBuffers = 10;
    theInputDesc.desc.dataBufferBaseAddr = 0;
    theInputDesc.desc.dataBufferPitch = 0;
    theInputDesc.desc.dataBufferSize = 0;
    theInputDesc.desc.metaDataBaseAddr = 0;
    theInputDesc.desc.metaDataPitch = 0;
    theInputDesc.desc.fullFlagBaseAddr =  0;
    theInputDesc.desc.fullFlagSize = 0;
    theInputDesc.desc.fullFlagPitch = 0;
    theInputDesc.desc.fullFlagValue = 0;
    theInputDesc.desc.emptyFlagBaseAddr = 0;
    theInputDesc.desc.emptyFlagSize = 0;
    theInputDesc.desc.emptyFlagPitch = 0;
    theInputDesc.desc.emptyFlagValue = 0;
    theInputDesc.desc.oob.port_id = 0;
    memset( theInputDesc.desc.oob.oep, 0, sizeof(theInputDesc.desc.oob.oep));
    theInputDesc.desc.oob.cookie = 0;
  }
  OD::Port &port = *transport.createOutputPort(myOutputDesc, theInputDesc);
  OD::Descriptor localShadowPort, feedback;
  const OD::Descriptor &outDesc = *port.finalize(theInputDesc, myOutputDesc, &localShadowPort);
  if (*ap) {
    file = *ap;
    file += ".out";
    OD::Descriptor sendDesc = outDesc;
    const char *sp = strchr(outDesc.desc.oob.oep, '/');
    std::string clean("ocpi-ether-rdma:");
    clean.append(sp+1);
    strcpy(sendDesc.desc.oob.oep, clean.c_str());
    int fd = creat(file.c_str(), 0666);
    if (fd < 0 ||
	write(fd, &sendDesc, sizeof(sendDesc)) != sizeof(sendDesc) ||
	close(fd) < 0)
      bad("Writing output RDMA descriptor");
    printf("Flow Control Descriptor in %s.\nWaiting for a keystroke...", file.c_str());
    fflush(stdout);
    getchar();
  }
  for (unsigned n = 0; n < 10; n++) {
    void *vdata;
    uint32_t length;
    OD::BufferUserFacet *buf;
    unsigned t;
    for (t = 0; t < 1000000 && !(buf = port.getNextEmptyOutputBuffer(vdata, length)); t++)
      OS::sleep(1);
    if (t == 1000000)
      bad("Receive DMA timeout\n");
    else {
      for (unsigned d = 0; d < length/sizeof(uint32_t); d++)
	((uint32_t *)vdata)[d] = n + d;
      port.sendOutputBuffer(buf, 4096, n);
      ocpiDebug("Sent RDMA buffer: first DW 0x%" PRIx32 ", length %" PRIu32 ", op %u\n",
		*(uint32_t *)vdata, length, n);
    }
  }
  printf("Sent 10 buffers, sleeping to allow frames to get through and be ack'd...\n");
  fflush(stdout);
  OS::sleep(10000);
  printf("Exiting\n");
  port.reset();
}

// Our added-value wait-for-process call.
// If "hang", we wait for the process to end, and if it stops, we term+kill it.
// Return true on bad unexpected things
static bool
mywait(pid_t pid, bool hang, std::string &error) {
  int status;
  pid_t wpid;
  do
    wpid = waitpid(pid, &status, (hang ? 0 : WNOHANG) | WUNTRACED);
  while (wpid == -1 && errno == EINTR);
  if (wpid == 0) // can't happen if hanging
    ocpiDebug("Wait returned 0 - subprocess running");
  else if ((int)wpid == -1) {
    error = "waitpid error";
    return true;
  } else if (WIFEXITED(status)) {
    int exitStatus = WEXITSTATUS(status);
    if (exitStatus > 10)
      OU::formatString(error, "Simulation subprocess couldn't execute simulator \"%s\" (got %s - %d)",
		       simexec, strerror(exitStatus - 10), exitStatus - 10);
    else
      OU::formatString(error, "Simulation subprocess terminated with exit status %d", exitStatus);
    if (hang) {
      // If waiting for termination, its not our error
      ocpiInfo("%s", error.c_str());
      error.clear();
    } else
      return true;
  } else if (WIFSIGNALED(status)) {
    int termSig = WTERMSIG(status);
    OU::formatString(error, "Simulation subprocess terminated with signal %s (%d)",
		     strsignal(termSig), termSig);
    if (hang) {
      // If waiting for termination, its not our error
      ocpiInfo("%s", error.c_str());
      error.clear();
    } else
      return true;
  } else if (WIFSTOPPED(status)) {
    int stopSig = WSTOPSIG(status);
    ocpiInfo("Simulator subprocess stopped with signal %s (%d)",
	     strsignal(stopSig), stopSig);
    if (hang) {
      kill(pid, SIGTERM);
      sleep(1);
      kill(pid, SIGKILL);
      return mywait(pid, true, error);
    }
  }
  return false;
}

// return true on error, otherwise read the amount requested
bool
myread(int fd, char *buf, unsigned nRequested, std::string &error) {
  ocpiDebug("SIM reading %u from  fd %d", nRequested, fd);
  do {
    ssize_t nread = read(fd, buf, nRequested);
    if (nread == 0)
      // shouldn't happen because we have it open for writing too
      error = "eof on FIFO"; 
    else if (nread > 0) {
      nRequested -= nread;
      buf += nread;
    } else if (errno != EINTR)
      error = "error reading FIFO";
  } while (error.empty() && nRequested);
  return !error.empty();
}

// Our named pipes.
struct Fifo {
  std::string m_name;
  int m_rfd, m_wfd;
  bool m_read;
  Fifo(std::string strName, bool iRead, const char *old, std::string &error)
    : m_name(strName), m_rfd(-1), m_wfd(-1), m_read(iRead) {
    if (error.size())
      return;
    const char *name = m_name.c_str();
    if (mkfifo(name, 0666))
      OU::formatString(error, "can't create fifo: %s (%s %d)",
		       name, strerror(errno), errno);
    else if ((m_rfd = open(name, O_RDONLY | O_NONBLOCK)) < 0)
      OU::formatString(error, "can't open fifo %s for reading (%s %d)",
		       name, strerror(errno), errno);
    else if (fcntl(m_rfd, F_SETFL, 0) != 0)
      OU::formatString(error, "can't set blocking flags on reading fifo %s (%s %d)", name,
		       strerror(errno), errno);
    else if ((m_wfd = open(name, O_WRONLY | O_NONBLOCK)) < 0)
      OU::formatString(error, "can't open fifo %s for writing (%s %d)",
		       name, strerror(errno), errno);
    else if (fcntl(m_wfd, F_SETFL, 0) != 0)
      OU::formatString(error, "can't set blocking flags on writing fifo %s (%s %d)", name,
		       strerror(errno), errno);
    if (old) {
      unlink(old);
      if (symlink(name, old) != 0)
	OU::formatString(error, "can't symlink from %s to %s (%s %d)", old, name,
		       strerror(errno), errno);
    }
    ocpiDebug("Fifo %s created and opened for %s", name, iRead ? "reading" : "writing");
  }    
  ~Fifo() {
    ocpiDebug("Destroying fifo %s", m_name.c_str());
    if (m_rfd >= 0) close(m_rfd);
    if (m_wfd >= 0) close(m_wfd);
    unlink(m_name.c_str());
  }
  // Get rid of any state in the pipe
  void
  flush() {
    int r, n;
    char buf[256];
    while (ioctl(m_rfd, FIONREAD, &n) >= 0 && n > 0 &&
	   ((r = read(m_rfd, buf, sizeof(buf))) > 0 ||
	    r < 0 && errno == EINTR))
      ;
  }
};


struct Sim {
  enum Action {
    SPIN_CREDIT = 0,
    DCP_CREDIT = 1,
    DUMP_OFF = 253,
    DUMP_ON = 254,
    TERMINATE = 255
  };

  // This fifo is read by the sim and we write it.
  // It carries CP traffic from us to the sim
  Fifo m_req;
  // This fifo is created and we open it for reading, but never read it.
  // It carries CP traffic from the sim back to the client.
  Fifo m_resp;
  // This fifo is read by the sim and we write it.
  // It carries sim controls from us to the sim.
  Fifo m_ctl;
  // This fifo is read by us and written by the client
  // It carries CP traffic from the client to us, on the way to the sim
  Fifo m_ext;
  // This fifo is for client->server control requests
  Fifo m_server;
  // This fifo is for server->client control responses
  Fifo m_client;
  int m_nfds;
  static pid_t s_pid;
  char *m_buffer;
  unsigned m_bufferSize;
  uint64_t m_dcp;
  char m_admin[sizeof(OH::OccpAdminRegisters)];
  Sim(std::string &simDir, std::string &error)
    : m_req(simDir + "/request", false, "/tmp/OpenCPI0_Req", error),
      m_resp(simDir + "/response", true, "/tmp/OpenCPI0_Resp", error),
      m_ctl(simDir + "/control", false, "/tmp/OpenCPI0_IOCtl", error),
      m_ext(simDir + "/external", true, "/tmp/OpenCPI0_Ext", error),
      m_server(simDir + "/server", true, NULL, error),
      m_client(simDir + "/client", true, NULL, error),
      m_nfds((m_ext.m_rfd > m_server.m_rfd ? m_ext.m_rfd : m_server.m_rfd) + 1),
      m_buffer(NULL), m_bufferSize(0), m_dcp(0)
  {
    initAdmin(*(OH::OccpAdminRegisters*)m_admin, "ocpiIsim");
  }
  ~Sim() {
    delete [] m_buffer;
  }
  // Establish a new simulator from a new executable.
  // Return error in the string and return true
  bool
  loadRun(const char *file, std::string &err) {
    if (verbose)
      fprintf(stderr, "Initializing simulator from executable/bitstream: %s\n", file);
    ocpiDebug("Starting the simulator load of bitstream: %s", file);
    // First establish a directory for the simulation based on the name of the file
    const char 
      *slash = strrchr(file, '/'),
      *suff = strrchr(file, '-');
    if (!suff) {
      OU::formatString(err, "simulator file name %s is not formatted properly", file);
      return true;
    }
    const char *dot = strchr(suff + 1, '.');
    if (!dot) {
      OU::formatString(err, "simulator file name %s is not formatted properly", file);
      return true;
    }
    slash = slash ? slash + 1 : file;
    std::string
      app(slash, suff - slash),
      platform(suff + 1, (dot - suff) - 1),
      dir(app);
    
    char date[100];
    time_t now = time(NULL);
    struct tm nowtm;
    localtime_r(&now, &nowtm);
    strftime(date, sizeof(date), ":%Y:%m:%d:%T", &nowtm);
    dir += date;
    if (mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST) {
      OU::formatString(err, "Can't create directory %s to run simulation", dir.c_str());
      return true;
    }
    std::string untar;
    OU::formatString(untar,
		     "set -e; file=%s; "
		     "xlength=`tail -1 $file | sed 's/^.*X//'` && "
		     "trimlength=$(($xlength + ${#xlength} + 2)) && "
		     "head -c -$trimlength < $file | tar -xzf - -C %s",
		     file, dir.c_str());
    ocpiDebug("Executing command to load bit stream for sim: %s", untar.c_str());
    int rc = system(untar.c_str());
    switch (rc) {
    case 127:
      OU::formatString(err, "Couldn't start execution of command: %s", untar.c_str());
      return 0;
    case -1:
      OU::formatString(err, "System error (%s, errno %d) while executing bitstream loading command",
		       strerror(errno), errno);
      return 0;
    default:
      OU::formatString(err, "Error return %u while executing bitstream loading command", rc);
      return 0;
    case 0:
      if (verbose)
	fprintf(stderr, "Executable/bitstream is installed and ready, in directory %s.\n",
		dir.c_str());
      ocpiInfo("Successfully loaded bitstream file: \"%s\" for simulation", file);
      break;
    }
    std::string cmd;
    if (platform == "ocpiIsim") {
      const char *xenv = getenv("OCPI_XILINX_TOOLS_DIR");
      if (!xenv) {
	err = "the OCPI_XILINX_TOOLS_DIR environment variable is not set";
	return true;
      }
      OU::formatString(cmd,
		       "echo run 1s | "
		       "(set -e ; cd %s; . $OCPI_XILINX_TOOLS_DIR/settings64.sh > sim.out 2>&1; "
		       " exec ./%s-ocpiIsim.exe -testplusarg bscvcd >> sim.out 2>&1)",
		       dir.c_str(), app.c_str());;
    } else {
      OU::formatString(err, "Unsupported simulator platform %s for %s", platform.c_str(), file);
      return true;
    }
    if (verbose)
      fprintf(stderr, "Starting execution of simulator for HDL assembly: %s.\n", app.c_str());
    switch ((s_pid = fork())) {
    case 0:
      if (execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), NULL))
	exit(10 + errno);
      break; // not used.
    case -1:
      OU::formatString(err, "Could not create simulator sub-process for: %s", simexec);
      ocpiBad("%s", err.c_str());
      return true;
    default:
      ocpiInfo("Simluator subprocess has pid: %u.", s_pid);
    }
    if (verbose)
      fprintf(stderr, "Simulator process (process id %u) started, with its output in %s/sim.out\n",
	      s_pid, dir.c_str());
    // just improve the odds of an immediate error giving a good error message
    char msg[2];
    msg[0] = SPIN_CREDIT;
    msg[1] = spinCount;
    assert(write(m_ctl.m_wfd, msg, 2) == 2);
    if (simDump) {
      msg[0] = !strcasecmp(simDump, "on") ? DUMP_ON : DUMP_OFF;
      msg[1] = 0;
      assert(write(m_ctl.m_wfd, msg, 2) == 2);
    } 
    ocpiInfo("Waiting 5 seconds for simlator to start before issueing any more credits.");
    sleep(5);
    if (mywait(s_pid, false, error))
      return true;
    if (verbose)
      fprintf(stderr, "Simulator process is running.\n");
    error.clear();
    return false;
  }
  // Flush all comms to the sim process since we have a new client.
  void
  flush() {
    m_req.flush();
    m_resp.flush();
    m_ext.flush();
  }
  void
  shutdown() {
    if (s_pid) {
      char msg[2];
      std::string error;
      msg[0] = TERMINATE;
      msg[1] = 0;
      assert(write(m_ctl.m_wfd, msg, 2) == 2);
      ocpiInfo("Waiting for simulator process to exit");
      mywait(s_pid, true, error);
      m_ctl.flush();
      flush();
      if (error.size())
	ocpiBad("Error when shutting down simulator: %s", error.c_str());
      m_dcp = 0;
      s_pid = 0;
    }
  }
  bool
  doServer(std::string &error) {
    uint16_t c[2];
    // This protocol has a count byte and a command byte.
    if (myread(m_server.m_rfd, (char *)c, sizeof(c), error)) {
      ocpiBad("Error reading server fifo for header: %s", error.c_str());
      return true;
    }
    if (c[0] > m_bufferSize) {
      delete [] m_buffer;
      m_buffer = new char[c[0]];
      m_bufferSize = c[0];
    }
    if (c[0] && myread(m_server.m_rfd, m_buffer, c[0], error)) {
      ocpiBad("Error reading server fifo for name: %s", error.c_str());
      return true;
    }
    ocpiDebug("Server fifo received %u %u: %s", c[0], c[1], c[0] ? m_buffer : "");
    switch (c[1]) {
    case 0:
      shutdown();
      loadRun(m_buffer, error);
      break;
    case 1:
      flush();
      break;
    }
    c[0] = error.empty() ? 0 : (uint16_t)error.size() + 1;
    if (write(m_client.m_wfd, c, sizeof(c)) != sizeof(c)) {
      error = "serverFifo write header failed";
      return true;
    }
    if (c[0] && write(m_client.m_wfd, error.c_str(), c[0]) != c[0]) {
      error = "serverFifo write data failed";
      return true;
    }
    ocpiDebug("serverFifo sent: %s", c[0] ? "<success>" : error.c_str());
    return false;
  }

  // There is no simulator running, handle it ourselves.
  bool
  doEmulate(std::string &error) {
    HE::EtherControlPacket pkt;
    if (myread(m_ext.m_rfd, (char*)&pkt + 2, sizeof(HE::EtherControlHeader) - 2, error)) {
      ocpiBad("Error reading header from client fifo: %s", error.c_str());
      return true;
    }
    unsigned n = ntohs(pkt.header.length);
    //    ocpiDebug("Got header.  Need %u header %u", n, sizeof(HE::EtherControlHeader)-2);
    if (n < sizeof(HE::EtherControlHeader) - 2) {
      OU::formatString(error, "bad client message length: %u", n);
      return true;
    } else if (n > sizeof(HE::EtherControlHeader) - 2 &&
	       myread(m_ext.m_rfd, (char*)(&pkt.header + 1),
		      n - (sizeof(HE::EtherControlHeader) - 2), error)) {
      ocpiBad("Error reading message from client fifo: %s", error.c_str());
      return true;
    }
    unsigned uncache = OCCP_ETHER_UNCACHED(pkt.header.typeEtc) ? 1 : 0;
    HE::EtherControlMessageType action = OCCP_ETHER_MESSAGE_TYPE(pkt.header.typeEtc);
    pkt.header.typeEtc = OCCP_ETHER_TYPE_ETC(HE::OCCP_RESPONSE, HE::OK, uncache);
    //ocpiDebug("Got message");
    uint32_t offset;
    ssize_t len;
    switch (action) {
    default:
      OU::formatString(error, "Invalid control message received when no sim executable %x",
		       pkt.header.typeEtc);
      return true;
    case HE::OCCP_WRITE:
      offset = ntohl(pkt.write.address); // for read or write
      len = sizeof(HE::EtherControlWriteResponse) - 2;
      if (offset > sizeof(m_admin))
	ocpiDebug("Write offset out of range: 0x%" PRIx32 "\n", offset);
      else
	*(uint32_t *)(&((char *)&m_admin) [offset]) = ntohl(pkt.write.data);
      break;
    case HE::OCCP_READ:
      offset = ntohl(pkt.read.address); // for read or write
      len = sizeof(HE::EtherControlReadResponse) - 2;
      pkt.readResponse.data = htonl(*(uint32_t *)(&((char *)&m_admin) [offset]));
    }
    pkt.header.length = htons(len);
    //ocpiDebug("Writing message %u", len);
    if (write(m_resp.m_wfd, (char *)&pkt + 2, len) != len) {
      error = "Error writing response fifo to client";
      return true;
    }
    return false;
  }

  bool
  sendToSim(unsigned n, std::string &error) {
    while (n) {
      char buf[250], *bp = buf;
      ssize_t nactual = read(m_ext.m_rfd, buf, n > sizeof(buf) ? sizeof(buf) : n);
      if (nactual < 0)
	if (errno == EINTR)
	  continue;
	else {
	  error = "read error from external fifo";
	  return true;
	}
      ssize_t nn;
      for (unsigned nw = 0; nw < nactual; nw += nn, bp += nn) {
	if ((nn = write(m_req.m_wfd, bp, nactual - nw)) < 0) {
	  if (errno != EINTR) {
	    error = "write error to request fifo";
	    return true;
	  }
	} else if (nn == 0) {
	  error = "write zero bytes to request fifo";
	  return true;
	} else {
	  char msg[2];
	  msg[0] = DCP_CREDIT;
	  msg[1] = nn;
	  if (write(m_ctl.m_wfd, msg, 2) != 2) {
	    error = "write error to control fifo";
	    return true;
	  }
	  m_dcp += nn;
	}
      }
      ocpiDebug("have written to ioctl crediting %zu bytes", nactual);
      n -= nactual;
    }
    return false;
  }
  
  // Perform one action: wait for data or timeout, and act accordingly.
  // set error on fatal problems
  // return any execution ticks provided.
  uint64_t
  doit(uint64_t cum, std::string &error) {
    char msg[2];
    uint64_t ticks = 0;
    if (m_dcp) {
      msg[0] = SPIN_CREDIT;
      msg[1] = spinCount;
      assert(write(m_ctl.m_wfd, msg, 2) == 2);
      ticks = spinCount;
    }
    fd_set fds[1];
    FD_ZERO(fds);
    FD_SET(m_ext.m_rfd, fds);
    FD_SET(m_server.m_rfd, fds);
    struct timeval timeout[1];
    timeout[0].tv_sec = sleepUsecs/1000000;
    timeout[0].tv_usec = sleepUsecs%1000000;
    switch (select(m_nfds, fds, NULL, NULL, timeout)) {
    case 0: // timeout
      ocpiDebug("Select timeout. cumulative simulator ticks: %" PRIu64, cum);
      return ticks;
    default:
      if (errno == EINTR)
	return ticks;
      OU::formatString(error, "Select failed: %s %u", strerror(errno), errno);
      return ticks;
    case 2: case 1:
      ;
    }
    if (FD_ISSET(m_server.m_rfd, fds))
      return doServer(error);
    if (FD_ISSET(m_ext.m_rfd, fds)) {
      unsigned n = 0;
      if (ioctl(m_ext.m_rfd, FIONREAD, &n) == -1) {
	error = "fionread syscall";
	return ticks;
      }
      if (n == 0) {
	error = "Return from select with 0 bytes";
	return ticks;
      }
      ocpiDebug("Client sent %u bytes", n);
      if (!s_pid)
	return doEmulate(error);
      return sendToSim(n, error);
    }
    return ticks;
  }
};
pid_t Sim::s_pid;

static bool stopped = false;
static void sigint(int /* signal */) {
  if (Sim::s_pid) {
    if (stopped) {
      fflush(stderr);
      fprintf(stderr, "\nSimulator process pid %u still running.\n", Sim::s_pid);
      fflush(stderr);
      signal(SIGINT, SIG_DFL);
    }
    stopped = true;
  }
  else
    _exit(1);
}

static void
simulate(const char **ap) {
  pid_t pid = getpid();
  std::string simDir;
  OU::formatString(simDir, "%s/%s", OH::Sim::TMPDIR, OH::Sim::SIMDIR);
  if (mkdir(simDir.c_str(), 0777) != 0 && errno != EEXIST)
    bad("Can't create directory for all OpenCPI simulator containers: %s", simDir.c_str());
  if (*ap)
    OU::formatString(simDir, "%s/%s/%s.%s", OH::Sim::TMPDIR, OH::Sim::SIMDIR, OH::Sim::SIMPREF, *ap);
  else
    OU::formatString(simDir, "%s/%s/%s.%u", OH::Sim::TMPDIR, OH::Sim::SIMDIR, OH::Sim::SIMPREF, pid);
  if (mkdir(simDir.c_str(), 0777) != 0)
    if (errno == EEXIST)
      bad("Diretory for this simulator, \"%s\", already exists (/tmp not cleared?)", simDir.c_str());
    else
      bad("Can't create the new diretory for this simulator: %s", simDir.c_str());
  do {
    if (verbose)
      fprintf(stderr, "Registering simulation platform in directory: %s\n", simDir.c_str());
    Sim sim(simDir, error);
    if (error.size())
      break;
    ocpiInfo("Simulator named \"sim:%s\" created in %s. All fifos are open.",
	     strrchr(simDir.c_str(), '.') + 1, simDir.c_str());
    assert(signal(SIGINT, sigint) != SIG_ERR);
    // If we were given an executable, start sim with it.
    if (simexec && sim.loadRun(simexec, error))
      bad("failed to start simulator");
    uint64_t cum, last, ticks;
    for (last = cum = 0; !stopped && error.empty() && cum < simTicks; cum += ticks) {
      if (cum - last > 1000) {
	ocpiInfo("Spin credit at: %20" PRIu64, cum);
	last = cum;
      }
      ticks = sim.doit(cum, error);
    }
    if (stopped) {
      if (verbose)
	fprintf(stderr, "Stopping simulator due to signal\n");
      ocpiInfo("Stopping simulator due to signal");
    } else if (cum >= simTicks) {
      if (verbose)
	fprintf(stderr, "Simulator credits at %" PRIu64 " exceeded %u, stopping simulation\n",
		cum, simTicks);
      ocpiInfo("Simulator credits at %" PRIu64 " exceeded %u, stopping simulation",
	       cum, simTicks);
    }
    sim.shutdown();
    // destroy all automatic objects
  } while (0);
  ocpiDebug("Removing sim directory: %s", simDir.c_str());
  assert(rmdir(simDir.c_str()) == 0);
  if (error.length())
    bad("simulator error");
}
