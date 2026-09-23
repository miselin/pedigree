/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "modules/Module.h"

namespace {
bool entry() {
  return true;
}
void exit() {}
}  // namespace

MODULE_INFO_NON_UNLOADABLE("virtio-pci", &entry, &exit, "pci");
