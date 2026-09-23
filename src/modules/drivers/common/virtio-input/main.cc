/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/new"

#include "VirtioInput.h"
#include "modules/Module.h"

namespace {
List<Device*> candidates;
List<VirtioInputDevice*> devices;

void collect(Device* pci) {
  for (size_t i = 0; i < pci->getNumChildren(); ++i) {
    if (pci->getChild(i)->getSpecificType() == String("virtio-input-device")) {
      return;
    }
  }
  candidates.pushBack(pci);
}

bool entry() {
  Device::searchByVendorIdAndDeviceId(0x1af4, 0x1052, collect);
  while (candidates.count()) {
    Device* pci = candidates.popFront();
    auto* device = new VirtioInputDevice(pci);
    if (!device->initialise()) {
      delete device;
      continue;
    }
    {
      Device::TreeLockGuard guard;
      device->setParent(pci);
      pci->addChild(device);
    }
    devices.pushBack(device);
  }
  return devices.count() != 0;
}

void exit() {
  while (devices.count()) {
    auto* device = devices.popFront();
    {
      Device::TreeLockGuard guard;
      device->getParent()->removeChild(device);
      device->setParent(nullptr);
    }
    delete device;
  }
}
}  // namespace

MODULE_INFO("virtio-input", &entry, &exit, "pci", "virtio-pci");
