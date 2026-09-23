/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "VirtioPci.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/time/Time.h"

#include "Virtqueue.h"

using namespace Virtio;

namespace {
constexpr uint8_t VendorCapability = 9;
constexpr uint8_t CommonConfig = 1;
constexpr uint8_t NotifyConfig = 2;
constexpr uint8_t IsrConfig = 3;
constexpr uint8_t DeviceConfig = 4;
constexpr uint64_t Version1 = 1ULL << 32;
constexpr uint8_t Acknowledge = 1;
constexpr uint8_t Driver = 2;
constexpr uint8_t DriverOk = 4;
constexpr uint8_t FeaturesOk = 8;
constexpr uint8_t Failed = 128;
}  // namespace

PciTransport::PciTransport(Device* device)
    : m_Device(device),
      m_Common(),
      m_Notify(),
      m_Isr(),
      m_DeviceConfig(),
      m_Original(),
      m_Queues{},
      m_NotifyOffsets{},
      m_NotifyMultiplier(0),
      m_Features(0),
      m_NumQueues(0),
      m_PciChanged(false),
      m_Initialised(false),
      m_Negotiated(false),
      m_DmaActive(false) {}

PciTransport::~PciTransport() {
  if (m_Common.io && !reset()) {
    panic("virtio: cannot stop DMA during transport teardown");
  }
  if (m_PciChanged &&
      !PciBus::instance().updateCommand(m_Device, 0x407U, (m_Original.command & 3U) | 0x400U)) {
    panic("virtio: cannot leave PCI function quiesced");
  }
}

bool PciTransport::locateCapabilities() {
  auto& pci = PciBus::instance();
  uint16_t pciStatus = 0;
  uint8_t cap = 0;
  if (!pci.readConfig16(m_Device, 6, pciStatus) || !(pciStatus & 0x10U) ||
      !pci.readConfig8(m_Device, 0x34, cap)) {
    return false;
  }
  uint64_t visited = 0;
  for (unsigned count = 0; cap && count < 48; ++count) {
    if (cap < 0x40 || (cap & 3U) || (visited & (1ULL << (cap / 4)))) {
      return false;
    }
    visited |= 1ULL << (cap / 4);
    uint8_t id = 0, next = 0;
    if (!pci.readConfig8(m_Device, cap, id) || !pci.readConfig8(m_Device, cap + 1, next)) {
      return false;
    }
    if (id == VendorCapability) {
      uint8_t length = 0, type = 0;
      if (!pci.readConfig8(m_Device, cap + 2, length) || length < 16 || cap + length > 256 ||
          !pci.readConfig8(m_Device, cap + 3, type)) {
        return false;
      }
      if (type >= CommonConfig && type <= DeviceConfig) {
        uint8_t bar = 0;
        uint32_t offset = 0, bytes = 0;
        if (!pci.readConfig8(m_Device, cap + 4, bar) || bar >= 6 ||
            !pci.readConfig32(m_Device, cap + 8, offset) ||
            !pci.readConfig32(m_Device, cap + 12, bytes) || !bytes) {
          return false;
        }
        char name[] = "bar0";
        name[3] = '0' + bar;
        Device::Address* address = nullptr;
        for (auto* candidate : m_Device->addresses()) {
          if (candidate->m_Name == name && !candidate->m_IsIoSpace &&
              uint64_t(offset) + bytes <= candidate->m_Size) {
            address = candidate;
            break;
          }
        }
        if (!address) {
          return false;
        }
        address->map();
        if (!address->m_Io || address->m_Io->size() < uint64_t(offset) + bytes) {
          return false;
        }
        Capability* target = nullptr;
        switch (type) {
          case CommonConfig:
            target = &m_Common;
            if (bytes < 56 || (offset & 3U)) {
              return false;
            }
            break;
          case NotifyConfig:
            target = &m_Notify;
            if (length < 20 || bytes < 2 || (offset & 1U) ||
                !pci.readConfig32(m_Device, cap + 16, m_NotifyMultiplier)) {
              return false;
            }
            break;
          case IsrConfig:
            target = &m_Isr;
            break;
          case DeviceConfig:
            target = &m_DeviceConfig;
            break;
        }
        if (target->io) {
          return false;
        }
        *target = {address->m_Io, offset, bytes};
      }
    }
    cap = next;
  }
  return !cap && m_Common.io && m_Notify.io && m_Isr.io;
}

bool PciTransport::status(uint8_t bits) {
  if (!m_Common.io) {
    return false;
  }
  const uint8_t before = m_Common.io->read8(m_Common.offset + 20);
  if (before == 0xff || (before & Failed)) {
    return false;
  }
  const uint8_t desired = before | bits;
  m_Common.io->write8(desired, m_Common.offset + 20);
  return (m_Common.io->read8(m_Common.offset + 20) & bits) == bits;
}

bool PciTransport::initialise() {
  if (m_Initialised || !m_Device || m_Device->getPciVendorId() != 0x1af4 ||
      m_Device->getPciDeviceId() < 0x1000 || m_Device->getPciDeviceId() > 0x107f) {
    return false;
  }
  auto& pci = PciBus::instance();
  if (!pci.inspectFunction(m_Device, m_Original)) {
    ERROR("virtio-pci: invalid PCI function state");
    return false;
  }
  m_PciChanged = true;
  if (!pci.updateCommand(m_Device, 4U, 2U | 0x400U) ||
      !pci.disableMessageInterrupts(m_Device, m_Original) ||
      !pci.resourcesUnchanged(m_Device, m_Original)) {
    ERROR("virtio-pci: could not prepare PCI function");
    return false;
  }
  if (!locateCapabilities()) {
    ERROR("virtio-pci: invalid or missing modern PCI capabilities");
    return false;
  }
  if (!reset() || !status(Acknowledge) || !status(Driver)) {
    ERROR("virtio-pci: device did not acknowledge reset or driver status");
    return false;
  }
  m_NumQueues = m_Common.io->read16(m_Common.offset + 18);
  if (!m_NumQueues) {
    ERROR("virtio-pci: device exposes no queues");
    return false;
  }
  m_Initialised = true;
  return true;
}

bool PciTransport::negotiate(uint64_t supportedFeatures, uint64_t requiredFeatures) {
  if (!m_Initialised || m_Negotiated || (requiredFeatures & ~supportedFeatures)) {
    return false;
  }
  auto* io = m_Common.io;
  const uint32_t base = m_Common.offset;
  io->write32(0, base);
  const uint64_t offeredLow = io->read32(base + 4);
  io->write32(1, base);
  const uint64_t offered = offeredLow | (uint64_t(io->read32(base + 4)) << 32);
  if (!(offered & Version1) || (requiredFeatures & ~offered)) {
    ERROR("virtio-pci: required modern features unavailable");
    return false;
  }
  m_Features = offered & (supportedFeatures | Version1);
  io->write32(0, base + 8);
  io->write32(m_Features, base + 12);
  io->write32(1, base + 8);
  io->write32(m_Features >> 32, base + 12);
  io->write16(0xffff, base + 16);
  if (!status(FeaturesOk) || !(io->read8(base + 20) & FeaturesOk)) {
    ERROR("virtio-pci: feature negotiation rejected");
    return false;
  }
  m_Negotiated = true;
  return true;
}

bool PciTransport::setupQueue(uint16_t index, Queue& queue) {
  if (!m_Negotiated || m_DmaActive || index >= m_NumQueues || index >= MaxQueues ||
      m_Queues[index]) {
    return false;
  }
  auto* io = m_Common.io;
  const uint32_t base = m_Common.offset;
  io->write16(index, base + 22);
  uint16_t maximum = io->read16(base + 24);
  if (maximum < 2 || io->read16(base + 28)) {
    ERROR("virtio-pci: queue " << Dec << index << " unavailable or already enabled" << Hex);
    return false;
  }
  if (maximum > Queue::MaxDepth) {
    maximum = Queue::MaxDepth;
  }
  uint16_t depth = 1;
  while (depth <= maximum / 2)
    depth *= 2;
  const uint16_t notify = io->read16(base + 30);
  const uint64_t notifyOffset = uint64_t(notify) * m_NotifyMultiplier;
  if (notifyOffset + 2 > m_Notify.length || (notifyOffset & 1U) || !queue.initialise(depth, this)) {
    ERROR("virtio-pci: queue " << Dec << index << " notification or DMA setup failed" << Hex);
    return false;
  }
  io->write16(depth, base + 24);
  io->write16(0xffff, base + 26);
  io->write32(queue.descriptorAddress(), base + 32);
  io->write32(queue.descriptorAddress() >> 32, base + 36);
  io->write32(queue.availableAddress(), base + 40);
  io->write32(queue.availableAddress() >> 32, base + 44);
  io->write32(queue.usedAddress(), base + 48);
  io->write32(queue.usedAddress() >> 32, base + 52);
  FENCE();
  io->write16(1, base + 28);
  if (io->read16(base + 28) != 1) {
    ERROR("virtio-pci: queue " << Dec << index << " enable rejected" << Hex);
    return false;
  }
  m_NotifyOffsets[index] = notifyOffset;
  m_Queues[index] = &queue;
  queue.m_Attached = true;
  return true;
}

bool PciTransport::ready() {
  if (!m_Negotiated || m_DmaActive) {
    return false;
  }
  bool haveQueue = false;
  for (auto* queue : m_Queues)
    haveQueue |= queue != nullptr;
  if (!haveQueue) {
    return false;
  }
  auto& pci = PciBus::instance();
  if (!pci.updateCommand(m_Device, 0, 6U | 0x400U)) {
    return false;
  }
  for (auto* queue : m_Queues)
    if (queue) {
      queue->m_DmaArmed = true;
    }
  m_DmaActive = true;
  if (!status(DriverOk) || !pci.updateCommand(m_Device, 0x400U, 6U)) {
    if (!reset()) {
      panic("virtio: device could not be reset after failed activation");
    }
    return false;
  }
  return true;
}

bool PciTransport::reset() {
  if (!m_PciChanged) {
    return true;
  }
  auto& pci = PciBus::instance();
  const bool busStopped = pci.updateCommand(m_Device, 4U, 0x400U);
  if (!m_Common.io) {
    return busStopped;
  }
  m_Common.io->write8(0, m_Common.offset + 20);
  const auto deadline = Time::getTicks() + Time::Multiplier::Second;
  while (m_Common.io->read8(m_Common.offset + 20) != 0 && Time::getTicks() < deadline)
    Time::delay(Time::Multiplier::Millisecond);
  if (m_Common.io->read8(m_Common.offset + 20) != 0 || !busStopped) {
    return false;
  }
  m_DmaActive = false;
  m_Negotiated = false;
  m_Initialised = false;
  m_Features = 0;
  for (auto*& queue : m_Queues) {
    if (queue) {
      queue->stop();
      queue->m_DmaArmed = false;
      queue->m_Attached = false;
      queue = nullptr;
    }
  }
  return true;
}

uint8_t PciTransport::readIsr() {
  return m_Initialised && m_Isr.io ? m_Isr.io->read8(m_Isr.offset) : 0;
}

void PciTransport::notify(uint16_t index) {
  if (!m_Negotiated || index >= MaxQueues || !m_Queues[index]) {
    return;
  }
  FENCE();
  m_Notify.io->write16(index, m_Notify.offset + m_NotifyOffsets[index]);
}

bool PciTransport::writeDeviceConfig8(uint16_t offset, uint8_t value) {
  if (!m_Initialised || !m_DeviceConfig.io || offset >= m_DeviceConfig.length) {
    return false;
  }
  m_DeviceConfig.io->write8(value, m_DeviceConfig.offset + offset);
  return true;
}

bool PciTransport::readConfig(uint16_t offset, unsigned width, uint64_t& value) {
  if (!m_Initialised || !m_DeviceConfig.io || uint32_t(offset) + width > m_DeviceConfig.length) {
    return false;
  }
  const uint32_t common = m_Common.offset;
  const uint32_t device = m_DeviceConfig.offset + offset;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    const uint8_t generation = m_Common.io->read8(common + 21);
    switch (width) {
      case 1:
        value = m_DeviceConfig.io->read8(device);
        break;
      case 2:
        value = m_DeviceConfig.io->read16(device);
        break;
      case 4:
        value = m_DeviceConfig.io->read32(device);
        break;
      case 8:
        value = m_DeviceConfig.io->read32(device) |
                (uint64_t(m_DeviceConfig.io->read32(device + 4)) << 32);
        break;
      default:
        return false;
    }
    if (generation == m_Common.io->read8(common + 21)) {
      return true;
    }
  }
  return false;
}

bool PciTransport::readDeviceConfig8(uint16_t offset, uint8_t& value) {
  uint64_t result = 0;
  if (!readConfig(offset, 1, result)) {
    return false;
  }
  value = result;
  return true;
}
bool PciTransport::readDeviceConfig16(uint16_t offset, uint16_t& value) {
  uint64_t result = 0;
  if (!readConfig(offset, 2, result)) {
    return false;
  }
  value = result;
  return true;
}
bool PciTransport::readDeviceConfig32(uint16_t offset, uint32_t& value) {
  uint64_t result = 0;
  if (!readConfig(offset, 4, result)) {
    return false;
  }
  value = result;
  return true;
}
bool PciTransport::readDeviceConfig64(uint16_t offset, uint64_t& value) {
  return readConfig(offset, 8, value);
}
