/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/MemoryRegion.h"

#include "modules/drivers/common/virtio/VirtioPci.h"
#include "modules/drivers/common/virtio/Virtqueue.h"

class VirtioInputDevice final : public Device, public IrqHandler {
 public:
  explicit VirtioInputDevice(Device* pci);
  ~VirtioInputDevice() override;

  bool initialise();
  IrqDisposition irq(irq_id_t number) override;

 private:
  static constexpr size_t MaxSlots = 64;
  static constexpr uint32_t AbsoluteMaximum = 0x7fff;

  enum DeviceType { Keyboard, Mouse, Tablet };

  struct Event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
  };
  static_assert(sizeof(Event) == 8, "unexpected virtio input event size");

  struct Slot {
    Event* data;
    physical_uintptr_t physical;
  };

  bool supportsEvent(uint8_t type, uint16_t code, bool& supported);
  bool readAxis(uint8_t axis, int32_t& minimum, int32_t& maximum);
  bool postReceive(Slot& slot);
  void handle(const Event& event);
  void reportPointer();
  uint32_t scaleAxis(int32_t value, unsigned axis) const;

  Device* m_Pci;
  Virtio::PciTransport m_Transport;
  Virtio::Queue m_EventQueue;
  Virtio::Queue m_StatusQueue;
  MemoryRegion m_Events;
  Mutex m_Lock;
  Slot m_Slots[MaxSlots];
  size_t m_SlotCount;
  irq_id_t m_Irq;
  DeviceType m_Type;
  int32_t m_AbsMinimum[2];
  int32_t m_AbsMaximum[2];
  uint32_t m_AbsPosition[2];
  ssize_t m_Relative[3];
  uint32_t m_Buttons;
  bool m_KeyDown[256];
  bool m_PointerChanged;
  bool m_Active;
  bool m_Stopping;

  VirtioInputDevice(const VirtioInputDevice&) = delete;
  VirtioInputDevice& operator=(const VirtioInputDevice&) = delete;
};

#endif
