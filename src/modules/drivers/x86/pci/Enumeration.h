/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_ENUMERATION_H
#define PEDIGREE_PCI_ENUMERATION_H

class Device;

namespace PciEnumeration {
// Sleepable, serialized slot-worker calls. Existing bridge buses/windows stay
// fixed. Hardware remains present until removeSlot has returned successfully.
bool probeSlot(Device* port);
bool prepareRemoveSlot(Device* port);
void cancelRemoveSlot(Device* port);
bool removeSlot(Device* port);
}  // namespace PciEnumeration

#endif
