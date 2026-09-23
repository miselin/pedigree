/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/utilities/List.h"

#include "VirtioRng.h"
#include "modules/Module.h"

namespace {
List<Device*> candidates;

void collect(Device* pci) {
  candidates.pushBack(pci);
}

bool entry() {
  Device::searchByVendorIdAndDeviceId(0x1af4, 0x1044, collect);
  Device::searchByVendorIdAndDeviceId(0x1af4, 0x1005, collect);
  bool seeded = false;
  while (candidates.count()) {
    VirtioRng rng(candidates.popFront());
    seeded |= rng.seedKernel();
  }
  return seeded;
}

void exit() {}
}  // namespace

MODULE_INFO("virtio-rng", &entry, &exit, "pci", "virtio-pci");
