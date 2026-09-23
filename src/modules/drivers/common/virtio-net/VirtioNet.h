/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/MemoryRegion.h"

#include "modules/drivers/common/virtio/VirtioPci.h"
#include "modules/drivers/common/virtio/Virtqueue.h"

class VirtioNet : public Network, public IrqHandler {
 public:
  explicit VirtioNet(Device* pciDevice);
  ~VirtioNet() override;

  bool initialise();
  void getName(String& name) override;
  bool send(size_t length, uintptr_t buffer) override;
  bool setStationInfo(const StationInfo& info) override;
  const StationInfo& getStationInfo() override;
  bool isConnected() override;
  IrqDisposition irq(irq_id_t number) override;

 private:
  static constexpr size_t BufferSize = 2048;
  static constexpr size_t MaxRxSlots = 64;
  static constexpr size_t MaxTxSlots = 32;

  struct Slot {
    uint8_t* data;
    physical_uintptr_t physical;
    bool busy;
  };

  void reclaimTransmit();
  bool postReceive(Slot& slot);

  Device* m_PciDevice;
  Virtio::PciTransport m_Transport;
  Virtio::Queue m_RxQueue;
  Virtio::Queue m_TxQueue;
  MemoryRegion m_RxRegion;
  MemoryRegion m_TxRegion;
  Mutex m_Lock;
  Slot m_RxSlots[MaxRxSlots];
  Slot m_TxSlots[MaxTxSlots];
  size_t m_RxCount;
  size_t m_TxCount;
  size_t m_NextTx;
  irq_id_t m_IrqId;
  bool m_HasStatus;
  bool m_NetworkRegistered;
  bool m_Initialised;
  bool m_Stopping;

  VirtioNet(const VirtioNet&) = delete;
  VirtioNet& operator=(const VirtioNet&) = delete;
};

#endif
