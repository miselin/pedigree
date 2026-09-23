/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_VIRTIO_PCI_H
#define PEDIGREE_VIRTIO_PCI_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/PciFunctionState.h"
#include "pedigree/kernel/processor/types.h"

class Device;
class IoBase;

namespace Virtio {

class Queue;

class EXPORTED_PUBLIC PciTransport {
 public:
  explicit PciTransport(Device* device);
  ~PciTransport();

  bool initialise();
  bool negotiate(uint64_t supportedFeatures, uint64_t requiredFeatures = 0);
  bool setupQueue(uint16_t index, Queue& queue);
  bool ready();
  // A successful reset acknowledges DMA quiescence before buffers may be freed.
  bool reset();
  uint8_t readIsr();
  void notify(uint16_t index);

  uint64_t features() const {
    return m_Features;
  }
  bool writeDeviceConfig8(uint16_t offset, uint8_t value);
  bool readDeviceConfig8(uint16_t offset, uint8_t& value);
  bool readDeviceConfig16(uint16_t offset, uint16_t& value);
  bool readDeviceConfig32(uint16_t offset, uint32_t& value);
  bool readDeviceConfig64(uint16_t offset, uint64_t& value);

 private:
  friend class Queue;
  static constexpr size_t MaxQueues = 64;

  struct Capability {
    IoBase* io = nullptr;
    uint32_t offset = 0;
    uint32_t length = 0;
  };

  bool locateCapabilities();
  bool readConfig(uint16_t offset, unsigned width, uint64_t& value);
  bool status(uint8_t bits);
  bool isDmaActive() const {
    return m_DmaActive;
  }

  Device* m_Device;
  Capability m_Common;
  Capability m_Notify;
  Capability m_Isr;
  Capability m_DeviceConfig;
  PciFunctionState::State m_Original;
  Queue* m_Queues[MaxQueues];
  uint32_t m_NotifyOffsets[MaxQueues];
  uint32_t m_NotifyMultiplier;
  uint64_t m_Features;
  uint16_t m_NumQueues;
  bool m_PciChanged;
  bool m_Initialised;
  bool m_Negotiated;
  bool m_DmaActive;
};

}  // namespace Virtio
#endif
