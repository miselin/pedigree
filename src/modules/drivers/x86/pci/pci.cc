/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "modules/Module.h"

namespace {
bool entry() {
  return true;
}

void exit() {}
}  // namespace

// Existing drivers require "pci" to mean discovery has completed.
#if ARM64
MODULE_INFO_NON_UNLOADABLE("pci", &entry, &exit, "pci-enumeration");
#else
MODULE_INFO_NON_UNLOADABLE("pci", &entry, &exit, "chipset");
#endif
