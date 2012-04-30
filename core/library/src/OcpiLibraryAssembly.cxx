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
#include "OcpiLibraryAssembly.h"

namespace OCPI {
  namespace Library {
    namespace OU = OCPI::Util;
    Assembly::Assembly(const char *file)
      : OU::Assembly(file), m_refCount(1) {
      findImplementations();
    }
    Assembly::Assembly(const std::string &string)
      : OU::Assembly(string), m_refCount(1) {
      findImplementations();
    }
    Assembly::~Assembly() {
      delete [] m_candidates;
    }

    void
    Assembly::
    operator ++( int )
    {
      m_refCount++;
    }

    void
    Assembly::
    operator --( int )
    {
      if ( --m_refCount == 0 ) {
	delete this;
      }
    }

    // The callback for the findImplementations() method below.
    bool Assembly::foundImplementation(const Implementation &i, unsigned score) {
      ocpiDebug("Considering implementation for instance \"%s\" spec \"%s\" named \"%s%s%s\" "
		"from artifact \"%s\"",
		m_instances[m_instance].m_name.c_str(),
		i.m_metadataImpl.specName().c_str(),
		i.m_metadataImpl.name().c_str(),
		i.m_instance ? "/" : "",
		i.m_instance ? ezxml_cattr(i.m_instance, "name") : "",
		i.m_artifact.name().c_str());
      // The util::assembly only knows port names, not worker port ordinals
      // (because it has not been correlated with any implementations).
      // Here is where we process the matchup between the port names in the util::assembly
      // and the port names in implementations in the libraries
      if (m_nPorts) {
	// We have processed an implementation before, just check for consistency
	if (i.m_metadataImpl.nPorts() != m_nPorts) {
	  ocpiInfo("Port number mismatch (%u vs %u) between implementations of worker %s.",
		   i.m_metadataImpl.specName().c_str());
	  return false;
	}
      } else {
	OU::Port *ports = i.m_metadataImpl.ports(m_nPorts);
	OU::Assembly::Port **ap = m_assyPorts[m_instance] = new OU::Assembly::Port *[m_nPorts];
	for (unsigned n = 0; n < m_nPorts; n++)
	  ap[n] = NULL;
	// build the map from implementation port ordinals to util::assembly::ports
	OU::Assembly::Instance &inst = m_instances[m_instance];
	for (std::list<OU::Assembly::Port*>::const_iterator pi = inst.m_ports.begin(); 
	     pi != inst.m_ports.end(); pi++) {
	  bool found = false;
	  for (unsigned n = 0; n < m_nPorts; n++)
	    if (ports[n].m_name == (*pi)->m_name) {
	      ap[n] = *pi;
	      found = true;
	      break;
	    }
	  if (!found)
	    throw OU::Error("Assembly instance %s of worker %s has no port named %s",
			    inst.m_name.c_str(), i.m_metadataImpl.specName().c_str(),
			    (*pi)->m_name.c_str());
	}
	// Now we know, for any worker port ordinal, the port in the util::assembly of it
      }
      // Here is where we know about the assembly and thus can check for
      // some connectivity constraints.  If the implementation has hard-wired connections
      // that are incompatible with the assembly, we ignore it.
      if (i.m_externals) {
	OU::Port::Mask m = 1;
	for (unsigned n = 0; n < OU::Port::c_maxPorts; n++, m <<= 1)
	  if (m & i.m_externals) {
	    // This port cannot be connected to another instance in the same container.
	    // Thus it PRECLUDES other connected instances on the same container.
	    // So the connected instance cannot have an INTERNAL requirement.
	    // But we can't check is here because it is a constraint about
	    // a pair of choices, not just one choice.
	  }
      }
      if (i.m_internals) {
	OCPI::Library::Connection *c = i.m_connections;
	unsigned nPorts;
	OU::Port *p = i.m_metadataImpl.ports(nPorts);
	OU::Port::Mask m = 1;
	unsigned bump = 0;
	for (unsigned n = 0; n < nPorts; n++, m <<= 1, c++, p++)
	  if (m & i.m_internals) {
	    // Find the assembly connection port for this instance and this 
	    // internally/statically connected port
	    OU::Assembly::Port *ap = m_assyPorts[m_instance][n];
	    if (ap) {
	      // We found the assembly connection port
	      // Now check that the port connected in the assembly has the same
	      // name as the port connected in the artifact
	      if (!ap->m_connectedPort ||
		  ap->m_connectedPort->m_name != c->port->m_name ||
		  m_instances[ap->m_connectedPort->m_instance].m_specName != i.m_metadataImpl.specName())
		return false; // avoid this implementation due to an incompatible static connection
	      bump = 1;; // An implementation with hardwired connections gets a score bump
	    } else
	      // There is no connection in the assembly for a statically connected impl port
	      return false;
	  }
	score += bump;
      }
      // FIXME:  Check consistency between implementations here...
      m_tempCandidates->push_back(Candidate(i, score));
      ocpiDebug("Accepted implementation with score %u", score);
      return false;
    }
    // A common method used by constructors
    void Assembly::findImplementations() {
      m_tempCandidates = m_candidates = new Candidates[m_instances.size()];
      m_instance = 0;
      m_assyPorts = new OU::Assembly::Port **[m_instances.size()];
      for (unsigned n = 0; n < m_instances.size(); n++, m_tempCandidates++, m_instance++) {
	OU::Assembly::Instance &inst = m_instances[m_instance];
	m_assyPorts[n] = NULL;
	m_nPorts = 0;
	if (!Manager::findImplementations(*this, inst.m_specName.c_str(),
					  inst.m_selection.empty() ?
					  NULL : inst.m_selection.c_str()))
	  throw OU::Error(inst.m_selection.empty() ?
			  "No implementations found in any libraries for \"%s\"" :
			  "No acceptable implementations found in any libraries "
			  "for \"%s\" (for selection: '%s')",
			  inst.m_specName.c_str(), inst.m_selection.c_str());
      }
      // Check for interface compatibility.
      // We assume all implementations have the same protocol metadata
      unsigned nConns = m_connections.size();
      for (unsigned n = 0; n < nConns; n++) {
	const OU::Assembly::Connection &c = m_connections[n];
	if (c.m_ports.size() == 2) {
	  const OU::Implementation // implementations on both sides of the connection
	    &i0 = m_candidates[c.m_ports[0].m_instance][0].impl->m_metadataImpl,
	    &i1 = m_candidates[c.m_ports[1].m_instance][0].impl->m_metadataImpl;
	  OU::Port // ports on both sides of the connection
	    *ap0 = i0.findPort(c.m_ports[0].m_name),
	    *ap1 = i1.findPort(c.m_ports[1].m_name);
	  if (!ap0 || !ap1)
	    throw OU::Error("Port name (\"%s\") in connection does not match any port in implementation",
			    (ap0 ? c.m_ports[1] : c.m_ports[0]).m_name.c_str());
	  if (ap0->m_provider == ap1->m_provider)
	    throw OU::Error("Port roles (ports \"%s\" and \"%s\") in connection are incompatible",
			    ap0->m_name.c_str(), ap1->m_name.c_str());
	  // Protocol on both sides of the connection
	  OU::Protocol &p0 = *ap0, &p1 = *ap1;
	  if (p0.m_name.size()  && p1.m_name.size() && p0.m_name != p1.m_name)
	    throw OU::Error("Protocols (ports \"%s\" protocol \%s\" vs. port \"%s\" protocol \"%s\") "
			    "in connection are incompatible",
			    p0.m_name.c_str(), p1.m_name.c_str());
	  
	  // FIXME:  more robust naming, namespacing, UUIDs, hash etc.
	    
	}
      }
      // Consolidate properties
    }
    // A port is connected in the assembly, and the port it is connected to is on an instance with
    // an already chosen implementation. Now we can check whether this impl conflicts with that one
    // or not
    bool Assembly::checkConnection(const Implementation &impl, const Implementation &otherImpl,
				    const OU::Assembly::Port &ap, unsigned port) {
      const OU::Port
	&p = impl.m_metadataImpl.port(port),
	&other = *otherImpl.m_metadataImpl.findPort(ap.m_connectedPort->m_name);
      if (impl.m_internals & (1 << port)) {
	// This port is preconnected and the other port is not preconnected to us: we're incompatible
	if (!(otherImpl.m_internals & (1 << other.m_ordinal)) ||
	    otherImpl.m_connections[other.m_ordinal].port != &p) {
	ocpiDebug("other %p %u %s  m_internals %x, other internals %x", &other, other.m_ordinal, other.m_name.c_str(),
		  impl.m_internals, otherImpl.m_internals);
	ocpiDebug("me %p %u %s", &p, p.m_ordinal, p.m_name.c_str());
	return true;
	}
      } else if (otherImpl.m_internals & (1 << other.m_ordinal))
	// This port is external.  If the other port is connected, we're incompatible
	return true;
      return false;
    }
  }
}