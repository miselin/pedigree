/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "VirtioInput.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/HidInputManager.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

namespace {
constexpr uint8_t ConfigEventBits = 0x11;
constexpr uint8_t ConfigAbsInfo = 0x12;
constexpr uint8_t EvSyn = 0;
constexpr uint8_t EvKey = 1;
constexpr uint8_t EvRel = 2;
constexpr uint8_t EvAbs = 3;
constexpr uint8_t RelX = 0;
constexpr uint8_t RelY = 1;
constexpr uint8_t RelWheel = 8;
constexpr uint8_t AbsX = 0;
constexpr uint8_t AbsY = 1;
constexpr uint16_t BtnLeft = 0x110;
constexpr uint16_t BtnRight = 0x111;
constexpr uint16_t BtnMiddle = 0x112;
constexpr uint16_t BtnSide = 0x113;
constexpr uint16_t BtnExtra = 0x114;

uint8_t hidFromLinux(uint16_t code) {
  static constexpr uint8_t letters[] = {
      30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
      49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
  };
  for (size_t i = 0; i < sizeof(letters); ++i) {
    if (letters[i] == code) {
      return i + 4;
    }
  }
  if (code >= 2 && code <= 10) {
    return code + 28;
  }
  if (code >= 59 && code <= 68) {
    return code - 1;
  }

  switch (code) {
    case 11:
      return 39;
    case 28:
      return 40;
    case 1:
      return 41;
    case 14:
      return 42;
    case 15:
      return 43;
    case 57:
      return 44;
    case 12:
      return 45;
    case 13:
      return 46;
    case 26:
      return 47;
    case 27:
      return 48;
    case 43:
      return 49;
    case 86:
      return 50;
    case 39:
      return 51;
    case 40:
      return 52;
    case 41:
      return 53;
    case 51:
      return 54;
    case 52:
      return 55;
    case 53:
      return 56;
    case 58:
      return 57;
    case 87:
      return 68;
    case 88:
      return 69;
    case 99:
      return 70;
    case 70:
      return 71;
    case 119:
      return 72;
    case 110:
      return 73;
    case 102:
      return 74;
    case 104:
      return 75;
    case 111:
      return 76;
    case 107:
      return 77;
    case 109:
      return 78;
    case 106:
      return 79;
    case 105:
      return 80;
    case 108:
      return 81;
    case 103:
      return 82;
    case 69:
      return 83;
    case 29:
      return 224;
    case 42:
      return 225;
    case 56:
      return 226;
    case 125:
      return 227;
    case 97:
      return 228;
    case 54:
      return 229;
    case 100:
      return 230;
    case 126:
      return 231;
    default:
      return 0;
  }
}

uint32_t buttonMask(uint16_t code) {
  switch (code) {
    case BtnLeft:
      return 1U << 0;
    case BtnMiddle:
      return 1U << 1;
    case BtnRight:
      return 1U << 2;
    case BtnSide:
      return 1U << 3;
    case BtnExtra:
      return 1U << 4;
    default:
      return 0;
  }
}
}  // namespace

VirtioInputDevice::VirtioInputDevice(Device* pci)
    : m_Pci(pci),
      m_Transport(pci),
      m_EventQueue(),
      m_StatusQueue(),
      m_Events("virtio-input-events"),
      m_Lock(),
      m_Slots{},
      m_SlotCount(0),
      m_Irq(0),
      m_Type(Keyboard),
      m_AbsMinimum{},
      m_AbsMaximum{},
      m_AbsPosition{},
      m_Relative{},
      m_Buttons(0),
      m_KeyDown{},
      m_PointerChanged(false),
      m_Active(false),
      m_Stopping(false) {
  setSpecificType(String("virtio-input-device"));
}

VirtioInputDevice::~VirtioInputDevice() {
  {
    LockGuard<Mutex> guard(m_Lock);
    m_Stopping = true;
    m_Active = false;
    if (!m_Transport.reset()) {
      panic("virtio-input could not stop DMA");
    }
  }
  if (m_Irq && !Machine::instance().getIrqManager()->unregisterHandler(m_Irq, this)) {
    panic("virtio-input could not synchronously unregister its IRQ");
  }
  m_EventQueue.stop();
  m_StatusQueue.stop();

  if (m_Type == Keyboard) {
    for (size_t key = 0; key < sizeof(m_KeyDown); ++key) {
      if (m_KeyDown[key]) {
        HidInputManager::instance().keyUp(key);
      }
    }
  } else if (m_Buttons) {
    if (m_Type == Mouse) {
      InputManager::instance().mouseUpdate(0, 0, 0, 0);
    } else {
      InputManager::instance().absoluteMouseUpdate(m_AbsPosition[0], m_AbsPosition[1], 0, 0);
    }
  }
}

bool VirtioInputDevice::supportsEvent(uint8_t type, uint16_t code, bool& supported) {
  if (!m_Transport.writeDeviceConfig8(0, ConfigEventBits) ||
      !m_Transport.writeDeviceConfig8(1, type)) {
    return false;
  }
  uint8_t size = 0;
  if (!m_Transport.readDeviceConfig8(2, size) || size > 128) {
    return false;
  }
  supported = false;
  if (code / 8 < size) {
    uint8_t bits = 0;
    if (!m_Transport.readDeviceConfig8(8 + code / 8, bits)) {
      return false;
    }
    supported = bits & (1U << (code % 8));
  }
  return true;
}

bool VirtioInputDevice::readAxis(uint8_t axis, int32_t& minimum, int32_t& maximum) {
  if (!m_Transport.writeDeviceConfig8(0, ConfigAbsInfo) ||
      !m_Transport.writeDeviceConfig8(1, axis)) {
    return false;
  }
  uint8_t size = 0;
  uint32_t minValue = 0;
  uint32_t maxValue = 0;
  if (!m_Transport.readDeviceConfig8(2, size) || size < 20 ||
      !m_Transport.readDeviceConfig32(8, minValue) ||
      !m_Transport.readDeviceConfig32(12, maxValue)) {
    return false;
  }
  minimum = static_cast<int32_t>(LITTLE_TO_HOST32(minValue));
  maximum = static_cast<int32_t>(LITTLE_TO_HOST32(maxValue));
  return maximum > minimum;
}

bool VirtioInputDevice::postReceive(Slot& slot) {
  Virtio::Buffer buffer = {slot.physical, sizeof(Event), true};
  return m_EventQueue.submit(&buffer, 1, &slot);
}

bool VirtioInputDevice::initialise() {
  if (!m_Pci || m_Pci->getPciVendorId() != 0x1af4 || m_Pci->getPciDeviceId() != 0x1052 ||
      !m_Transport.initialise() || !m_Transport.negotiate(0) ||
      !m_Transport.setupQueue(0, m_EventQueue) || !m_Transport.setupQueue(1, m_StatusQueue)) {
    ERROR("virtio-input: PCI transport or queues unavailable");
    return false;
  }

  bool absoluteX = false, absoluteY = false, relativeX = false, relativeY = false;
  bool keyA = false;
  if (!supportsEvent(EvAbs, AbsX, absoluteX) || !supportsEvent(EvAbs, AbsY, absoluteY) ||
      !supportsEvent(EvRel, RelX, relativeX) || !supportsEvent(EvRel, RelY, relativeY) ||
      !supportsEvent(EvKey, 30, keyA)) {
    ERROR("virtio-input: could not query supported input events");
    return false;
  }
  if (absoluteX && absoluteY) {
    m_Type = Tablet;
    if (!readAxis(AbsX, m_AbsMinimum[0], m_AbsMaximum[0]) ||
        !readAxis(AbsY, m_AbsMinimum[1], m_AbsMaximum[1])) {
      ERROR("virtio-input: invalid tablet axis ranges");
      return false;
    }
  } else if (relativeX && relativeY) {
    m_Type = Mouse;
  } else if (keyA) {
    m_Type = Keyboard;
  } else {
    ERROR("virtio-input: unsupported event set");
    return false;
  }

  m_SlotCount = m_EventQueue.depth() < MaxSlots ? m_EventQueue.depth() : MaxSlots;
  const size_t page = TargetInfo::getPageSize();
  const size_t pages = (m_SlotCount * sizeof(Event) + page - 1) / page;
  if (!m_SlotCount || !PhysicalMemoryManager::instance().allocateRegion(
                          m_Events, pages, PhysicalMemoryManager::continuous,
                          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write)) {
    ERROR("virtio-input: could not allocate event buffers");
    return false;
  }

  for (size_t i = 0; i < m_SlotCount; ++i) {
    m_Slots[i] = {static_cast<Event*>(m_Events.virtualAddress()) + i,
                  m_Events.physicalAddress() + i * sizeof(Event)};
    if (!postReceive(m_Slots[i])) {
      ERROR("virtio-input: could not populate event queue");
      return false;
    }
  }

  m_Irq = Machine::instance().getIrqManager()->registerPciIrqHandler(this, m_Pci,
                                                                     IrqPolicy::pciIntxThreaded());
  if (!m_Irq) {
    ERROR("virtio-input: could not register PCI interrupt");
    return false;
  }
  m_Active = true;
  if (!m_Transport.ready()) {
    m_Active = false;
    ERROR("virtio-input: device did not accept DRIVER_OK");
    return false;
  }
  m_Transport.notify(0);
  NOTICE(
      "virtio-input: " << (m_Type == Keyboard ? "keyboard" : (m_Type == Mouse ? "mouse" : "tablet"))
                       << " ready");
  return true;
}

uint32_t VirtioInputDevice::scaleAxis(int32_t value, unsigned axis) const {
  const int64_t minimum = m_AbsMinimum[axis];
  const int64_t maximum = m_AbsMaximum[axis];
  if (value <= minimum) {
    return 0;
  }
  if (value >= maximum) {
    return AbsoluteMaximum;
  }
  return uint64_t(int64_t(value) - minimum) * AbsoluteMaximum / (maximum - minimum);
}

void VirtioInputDevice::reportPointer() {
  if (!m_PointerChanged) {
    return;
  }
  if (m_Type == Mouse) {
    InputManager::instance().mouseUpdate(m_Relative[0], -m_Relative[1], m_Relative[2], m_Buttons);
  } else {
    InputManager::instance().absoluteMouseUpdate(m_AbsPosition[0], m_AbsPosition[1], m_Relative[2],
                                                 m_Buttons);
  }
  m_Relative[0] = m_Relative[1] = m_Relative[2] = 0;
  m_PointerChanged = false;
}

void VirtioInputDevice::handle(const Event& event) {
  const uint16_t type = LITTLE_TO_HOST16(event.type);
  const uint16_t code = LITTLE_TO_HOST16(event.code);
  const int32_t value = static_cast<int32_t>(LITTLE_TO_HOST32(event.value));

  if (m_Type == Keyboard) {
    if (type == EvKey && (value == 0 || value == 1)) {
      const uint8_t hid = hidFromLinux(code);
      if (hid && m_KeyDown[hid] != (value == 1)) {
        m_KeyDown[hid] = value == 1;
        if (value) {
          HidInputManager::instance().keyDown(hid);
        } else {
          HidInputManager::instance().keyUp(hid);
        }
      }
    }
    return;
  }

  if (type == EvSyn && code == 0) {
    reportPointer();
  } else if (type == EvKey && (value == 0 || value == 1)) {
    const uint32_t mask = buttonMask(code);
    if (mask) {
      const uint32_t before = m_Buttons;
      if (value) {
        m_Buttons |= mask;
      } else {
        m_Buttons &= ~mask;
      }
      m_PointerChanged |= before != m_Buttons;
    }
  } else if (type == EvRel) {
    if (code == RelWheel) {
      m_Relative[2] += value;
      m_PointerChanged = true;
    } else if (m_Type == Mouse && code <= RelY) {
      m_Relative[code] += value;
      m_PointerChanged = true;
    }
  } else if (type == EvAbs && m_Type == Tablet && code <= AbsY) {
    const uint32_t position = scaleAxis(value, code);
    m_PointerChanged |= m_AbsPosition[code] != position;
    m_AbsPosition[code] = position;
  }
}

IrqDisposition VirtioInputDevice::irq(irq_id_t number) {
  (void)number;
  LockGuard<Mutex> guard(m_Lock);
  if (m_Stopping || !m_Active) {
    return IrqDisposition::Quiesced;
  }
  if (!m_Transport.readIsr()) {
    return IrqDisposition::NotHandled;
  }

  bool replenished = false;
  Virtio::Completion completion;
  while (m_EventQueue.pop(completion)) {
    auto* slot = static_cast<Slot*>(completion.cookie);
    if (slot && completion.length == sizeof(Event)) {
      handle(*slot->data);
    }
    if (slot && postReceive(*slot)) {
      replenished = true;
    } else {
      ERROR("virtio-input: could not replenish event queue");
    }
  }
  if (replenished) {
    m_Transport.notify(0);
  }
  return IrqDisposition::Handled;
}
