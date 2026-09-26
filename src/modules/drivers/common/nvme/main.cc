/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/machine/PciDrivers.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/utilities/new"

#include "NvmeController.h"
#include "modules/Module.h"
namespace {
Device* attach(Device* pci) {
  auto* controller = new NvmeController(pci);
  if (!controller) {
    return nullptr;
  }
  if (!controller->initialiseController()) {
    controller->shutdown();
    delete controller;
    return nullptr;
  }
  {
    Device::TreeLockGuard guard;
    controller->setParent(pci);
    pci->addChild(controller);
  }
  auto& services = ServiceManager::instance();
  for (size_t i = 0; i < controller->getNumChildren(); ++i) {
    auto* disk = static_cast<Disk*>(controller->getChild(i));
    services.serve(String("partition"), ServiceFeatures::touch, disk, sizeof(disk));
  }
  return controller;
}

bool prepareRemove(Device* controller) {
  return static_cast<NvmeController*>(controller)->prepareDiskRemoval();
}

void cancelRemove(Device* controller) {
  static_cast<NvmeController*>(controller)->cancelDiskRemoval();
}

void remove(Device* device) {
  auto* controller = static_cast<NvmeController*>(device);
  controller->shutdown();
  {
    Device::TreeLockGuard guard;
    controller->getParent()->removeChild(controller);
    controller->setParent(nullptr);
  }
  delete controller;
}

const PciDrivers::Driver driver{1, 8, 2, attach, prepareRemove, cancelRemove, remove};

Module::UnloadAdmission admitUnload(bool) {
  return PciDrivers::prepareUnregisterDriver(&driver) ? Module::UnloadAdmission::Ready
                                                      : Module::UnloadAdmission::Busy;
}

bool entry() {
  if (!KernelElf::instance().registerUnloadAdmission(&entry, &admitUnload)) {
    return false;
  }
  return PciDrivers::registerDriver(&driver);
}

void exit() {
  if (!PciDrivers::unregisterDriver(&driver)) {
    panic("NVMe: PCI driver registration still owns live bindings during unload");
  }
}
}  // namespace
MODULE_INFO("nvme", &entry, &exit, "pci", "scsi");
