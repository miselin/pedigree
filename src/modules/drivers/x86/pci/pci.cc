/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "modules/Module.h"

namespace {
bool entry() {
  return true;
}

void exit() {}
}  // namespace

// Existing drivers require "pci" to mean discovery and interrupt routing are
// complete, including external modules built against that dependency contract.
MODULE_INFO_NON_UNLOADABLE("pci", &entry, &exit, "chipset");
