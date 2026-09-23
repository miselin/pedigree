/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef VIRTIO_RNG_H
#define VIRTIO_RNG_H

#include "pedigree/kernel/processor/MemoryRegion.h"

#include "modules/drivers/common/virtio/VirtioPci.h"
#include "modules/drivers/common/virtio/Virtqueue.h"

class Device;

class VirtioRng final {
 public:
  explicit VirtioRng(Device* pci);
  ~VirtioRng();

  bool seedKernel();

 private:
  void shutdown();

  Device* m_Pci;
  Virtio::PciTransport m_Transport;
  Virtio::Queue m_Queue;
  MemoryRegion m_Data;
  bool m_Shutdown;
};

#endif
