/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/new"

#include "VirtioBlkController.h"
#include "modules/Module.h"

namespace {
List<Device*> candidates;
List<VirtioBlkController*> controllers;

void collect(Device* pci) {
  for (size_t i = 0; i < pci->getNumChildren(); ++i) {
    if (pci->getChild(i)->getSpecificType() == String("virtio-blk-controller")) {
      return;
    }
  }
  candidates.pushBack(pci);
}

Module::UnloadAdmission admitUnload(bool) {
  for (auto* controller : controllers) {
    if (!controller->prepareDiskRemoval()) {
      for (auto* rollback : controllers) {
        rollback->cancelDiskRemoval();
      }
      return Module::UnloadAdmission::Busy;
    }
  }
  return Module::UnloadAdmission::Ready;
}

bool entry() {
  Device::searchByVendorIdAndDeviceId(0x1af4, 0x1042, collect);
  Device::searchByVendorIdAndDeviceId(0x1af4, 0x1001, collect);
  while (candidates.count()) {
    Device* pci = candidates.popFront();
    auto* controller = new VirtioBlkController(pci);
    if (!controller->initialiseController()) {
      controller->shutdown();
      delete controller;
      continue;
    }
    {
      Device::TreeLockGuard guard;
      controller->setParent(pci);
      pci->addChild(controller);
    }
    controllers.pushBack(controller);
  }
  if (!controllers.count()) {
    return false;
  }
  return KernelElf::instance().registerUnloadAdmission(&entry, &admitUnload);
}

void exit() {
  while (controllers.count()) {
    VirtioBlkController* controller = controllers.popFront();
    controller->shutdown();
    {
      Device::TreeLockGuard guard;
      controller->getParent()->removeChild(controller);
      controller->setParent(nullptr);
    }
    delete controller;
  }
}
}  // namespace

MODULE_INFO("virtio-blk", &entry, &exit, "pci", "virtio-pci", "scsi");
