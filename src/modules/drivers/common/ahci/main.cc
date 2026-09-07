/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/new"

#include "AhciController.h"
#include "modules/Module.h"

namespace {
List<Device*> candidates;
List<AhciController*> controllers;

void collect(Device* device) {
  for (size_t i = 0; i < device->getNumChildren(); ++i) {
    if (device->getChild(i)->getSpecificType() == String("ahci-controller"))
      return;
  }
  candidates.pushBack(device);
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
  Device::searchByClassSubclassAndProgInterface(0x01, 0x06, 0x01, collect);
  while (candidates.count()) {
    Device* pci = candidates.popFront();
    auto* controller = new AhciController(pci);
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
    NOTICE("AHCI: no supported SATA disks found");
    return false;
  }
  return KernelElf::instance().registerUnloadAdmission(&entry, &admitUnload);
}

void exit() {
  while (controllers.count()) {
    AhciController* controller = controllers.popFront();
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

MODULE_INFO("ahci", &entry, &exit, "pci", "scsi");
