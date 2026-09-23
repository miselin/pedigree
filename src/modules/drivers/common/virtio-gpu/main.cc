/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/new"

#include "VirtioGpu.h"
#include "modules/Module.h"

namespace {
List<Device*> candidates;
List<VirtioGpu*> displays;

void collect(Device* pci) {
  for (size_t i = 0; i < pci->getNumChildren(); ++i) {
    if (pci->getChild(i)->getSpecificType() == String("virtio-gpu-display")) {
      return;
    }
  }
  candidates.pushBack(pci);
}

bool entry() {
  Device::searchByVendorIdAndDeviceId(0x1af4, 0x1050, collect);
  while (candidates.count()) {
    Device* pci = candidates.popFront();
    auto* display = new VirtioGpu(pci);
    if (!display) {
      continue;
    }
    if (!display->initialise()) {
      delete display;
      continue;
    }
    {
      Device::TreeLockGuard guard;
      display->setParent(pci);
      pci->addChild(display);
    }
    displays.pushBack(display);
  }
  return displays.count() != 0;
}

void exit() {
  while (displays.count()) {
    auto* display = displays.popFront();
    display->shutdown();
    {
      Device::TreeLockGuard guard;
      display->getParent()->removeChild(display);
      display->setParent(nullptr);
    }
    delete display;
  }
}
}  // namespace

MODULE_INFO("virtio-gpu", &entry, &exit, "pci", "virtio-pci");
