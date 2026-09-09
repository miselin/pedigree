/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/machine/Device.h"

#include "Qm67.h"
#include "modules/Module.h"

namespace {
bool entry() {
  Vector<Device*> devices;
  auto collect = [](Device* device, Vector<Device*>* candidates) {
    if (device->getParent() && device->getParent()->getSpecificType() == String("pci"))
      candidates->pushBack(device);
    return device;
  };
  auto collector = pedigree_std::make_callable(collect);
  Device::foreach (collector, nullptr, &devices);
  Qm67::routeInterrupts(devices);
  return true;
}

void exit() {}
}  // namespace

// PIRQ configuration and PIC reservations remain owned for the kernel lifetime.
MODULE_INFO_NON_UNLOADABLE("chipset", &entry, &exit, "pci-enumeration");
