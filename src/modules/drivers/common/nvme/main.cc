/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/new"

#include "NvmeController.h"
#include "modules/Module.h"
namespace {
List<Device*> candidates;
List<NvmeController*> controllers;
void collect(Device* pci) {
  for (size_t i = 0; i < pci->getNumChildren(); ++i) {
    if (pci->getChild(i)->getSpecificType() == String("nvme-controller"))
      return;
  }
  candidates.pushBack(pci);
}
Module::UnloadAdmission admitUnload(bool terminal) {
  for (auto* controller : controllers) {
    if (!controller->prepareDiskRemoval()) {
      for (auto* rollback : controllers)
        rollback->cancelDiskRemoval();
      return terminal ? Module::UnloadAdmission::KeepMapped : Module::UnloadAdmission::Busy;
    }
  }
  return Module::UnloadAdmission::Ready;
}
bool entry() {
  Device::searchByClassSubclassAndProgInterface(1, 8, 2, collect);
  while (candidates.count()) {
    Device* pci = candidates.popFront();
    auto* controller = new NvmeController(pci);
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
  if (!controllers.count())
    return false;
  return KernelElf::instance().registerUnloadAdmission(&entry, &admitUnload);
}
void exit() {
  while (controllers.count()) {
    NvmeController* controller = controllers.popFront();
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
MODULE_INFO("nvme", &entry, &exit, "pci", "scsi");
