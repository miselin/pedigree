/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_RESOURCES_H
#define PEDIGREE_PCI_RESOURCES_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

class Device;

namespace PciResources {
// Call once after PCI enumeration, before publishing drivers. Existing BARs
// and bridge windows are recorded in PCI bus address space.
EXPORTED_PUBLIC bool initialize(Device* root = nullptr);

// Runtime registration accepts only Type 0 leaves with unassigned BARs. Both
// operations require decoding/BME off and no DMA domain or child controllers.
// The caller serializes driver/topology changes and retains the node until
// removeFunction() succeeds. Group-owned VFs use their PF's reservations.
EXPORTED_PUBLIC bool addFunction(Device* device);
EXPORTED_PUBLIC bool removeFunction(Device* device);

// Reserve an already assigned MMIO interval, or allocate one in configured
// firmware and upstream bridge windows. allocate() also reserves its result.
EXPORTED_PUBLIC bool reserve(Device* owner, uint64_t base, uint64_t size, bool prefetchable);
EXPORTED_PUBLIC bool allocate(Device* owner, uint64_t size, uint64_t alignment, bool prefetchable,
                              uint64_t maxAddress, uint64_t& outBase);
EXPORTED_PUBLIC bool release(Device* owner, uint64_t base, uint64_t size);
}  // namespace PciResources

#endif
