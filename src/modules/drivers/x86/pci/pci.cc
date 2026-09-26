/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/machine/PciDrivers.h"

#include "modules/Module.h"
#if !ARM64 && !ARMV7
#include "PciePorts.h"
#endif

namespace {
bool entry() {
  if (!PciDrivers::initialize()) {
    return false;
  }
#if !ARM64 && !ARMV7
  PciePorts::initialize();
#endif
  return true;
}

void exit() {}
}  // namespace

// Existing drivers require "pci" to mean discovery has completed.
#if ARM64 || ARMV7
MODULE_INFO_NON_UNLOADABLE("pci", &entry, &exit, "pci-enumeration");
#else
MODULE_INFO_NON_UNLOADABLE("pci", &entry, &exit, "chipset");
#endif
