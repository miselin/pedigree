/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/new"

#include "VirtioNet.h"
#include "modules/Module.h"

namespace {
List<Device*> candidates;
List<VirtioNet*> cards;

void collect(Device* pci) {
  for (size_t i = 0; i < pci->getNumChildren(); ++i) {
    if (pci->getChild(i)->getSpecificType() == String("virtio-net-card")) {
      return;
    }
  }
  candidates.pushBack(pci);
}

bool entry() {
  Device::searchByVendorIdAndDeviceId(0x1af4, 0x1041, collect);
  Device::searchByVendorIdAndDeviceId(0x1af4, 0x1000, collect);
  while (candidates.count()) {
    Device* pci = candidates.popFront();
    auto* card = new VirtioNet(pci);
    if (!card->initialise()) {
      delete card;
      continue;
    }
    {
      Device::TreeLockGuard guard;
      card->setParent(pci);
      pci->addChild(card);
    }
    cards.pushBack(card);
  }
  return cards.count() != 0;
}

void exit() {
  while (cards.count()) {
    auto* card = cards.popFront();
    {
      Device::TreeLockGuard guard;
      card->getParent()->removeChild(card);
      card->setParent(nullptr);
    }
    delete card;
  }
}
}  // namespace

MODULE_INFO("virtio-net", &entry, &exit, "virtio-pci", "network-stack");
