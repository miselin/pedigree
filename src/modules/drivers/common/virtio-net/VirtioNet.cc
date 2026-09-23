/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "VirtioNet.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/network-stack/NetworkStack.h"

namespace {
constexpr uint64_t NetMac = 1ULL << 5;
constexpr uint64_t NetStatus = 1ULL << 16;
constexpr uint16_t LinkUp = 1;
constexpr size_t EthernetHeaderSize = 14;
constexpr size_t EthernetFrameMax = 1518;
constexpr size_t EthernetFrameMin = 60;

struct VirtioNetHeader {
  uint8_t flags;
  uint8_t gsoType;
  uint16_t headerLength;
  uint16_t gsoSize;
  uint16_t checksumStart;
  uint16_t checksumOffset;
  uint16_t numBuffers;
};
static_assert(sizeof(VirtioNetHeader) == 12, "unexpected virtio-net header size");

size_t pagesFor(size_t bytes) {
  return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}
}  // namespace

VirtioNet::VirtioNet(Device* pciDevice)
    : Network(),
      m_PciDevice(pciDevice),
      m_Transport(pciDevice),
      m_RxQueue(),
      m_TxQueue(),
      m_RxRegion("virtio-net-rx"),
      m_TxRegion("virtio-net-tx"),
      m_Lock(),
      m_RxSlots(),
      m_TxSlots(),
      m_RxCount(0),
      m_TxCount(0),
      m_NextTx(0),
      m_IrqId(0),
      m_HasStatus(false),
      m_NetworkRegistered(false),
      m_Initialised(false),
      m_Stopping(false) {
  setSpecificType(String("virtio-net-card"));
}

VirtioNet::~VirtioNet() {
  {
    LockGuard<Mutex> guard(m_Lock);
    m_Stopping = true;
    m_Initialised = false;
    if (!m_Transport.reset()) {
      panic("virtio-net could not stop DMA");
    }
  }

  if (m_IrqId && !Machine::instance().getIrqManager()->unregisterHandler(m_IrqId, this)) {
    panic("virtio-net could not synchronously unregister its IRQ");
  }
  if (m_NetworkRegistered) {
    NetworkStack::instance().deRegisterDevice(this);
  }
  m_RxQueue.stop();
  m_TxQueue.stop();
}

bool VirtioNet::initialise() {
  if (!m_Transport.initialise() || !m_Transport.negotiate(NetMac | NetStatus, NetMac) ||
      !m_Transport.setupQueue(0, m_RxQueue) || !m_Transport.setupQueue(1, m_TxQueue)) {
    ERROR("virtio-net: PCI transport or queues unavailable");
    return false;
  }

  m_RxCount = m_RxQueue.depth() < MaxRxSlots ? m_RxQueue.depth() : MaxRxSlots;
  m_TxCount = m_TxQueue.depth() < MaxTxSlots ? m_TxQueue.depth() : MaxTxSlots;
  if (!m_RxCount || !m_TxCount) {
    ERROR("virtio-net: no receive or transmit descriptors");
    return false;
  }

  auto& memory = PhysicalMemoryManager::instance();
  constexpr size_t dmaFlags = PhysicalMemoryManager::continuous;
  constexpr size_t mapFlags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;
  if (!memory.allocateRegion(m_RxRegion, pagesFor(m_RxCount * BufferSize), dmaFlags, mapFlags,
                             -1) ||
      !memory.allocateRegion(m_TxRegion, pagesFor(m_TxCount * BufferSize), dmaFlags, mapFlags,
                             -1)) {
    ERROR("virtio-net: could not allocate DMA buffers");
    return false;
  }

  for (size_t i = 0; i < m_RxCount; ++i) {
    m_RxSlots[i] = {static_cast<uint8_t*>(m_RxRegion.virtualAddress()) + i * BufferSize,
                    m_RxRegion.physicalAddress() + i * BufferSize, false};
  }
  for (size_t i = 0; i < m_TxCount; ++i) {
    m_TxSlots[i] = {static_cast<uint8_t*>(m_TxRegion.virtualAddress()) + i * BufferSize,
                    m_TxRegion.physicalAddress() + i * BufferSize, false};
  }

  for (size_t i = 0; i < 6; ++i) {
    uint8_t byte = 0;
    if (!m_Transport.readDeviceConfig8(i, byte)) {
      ERROR("virtio-net: could not read MAC address");
      return false;
    }
    m_StationInfo.mac.setMac(byte, i);
  }
  m_HasStatus = (m_Transport.features() & NetStatus) != 0;

  for (size_t i = 0; i < m_RxCount; ++i) {
    if (!postReceive(m_RxSlots[i])) {
      ERROR("virtio-net: could not populate receive queue");
      return false;
    }
  }

  m_IrqId = Machine::instance().getIrqManager()->registerPciIrqHandler(
      this, m_PciDevice, IrqPolicy::pciIntxThreaded());
  if (!m_IrqId) {
    ERROR("virtio-net: could not register PCI interrupt");
    return false;
  }

  NetworkStack::instance().registerDevice(this);
  m_NetworkRegistered = true;
  m_Initialised = true;
  if (!m_Transport.ready()) {
    m_Initialised = false;
    ERROR("virtio-net: device did not accept DRIVER_OK");
    return false;
  }
  m_Transport.notify(0);
  NOTICE("virtio-net: MAC " << m_StationInfo.mac[0] << ":" << m_StationInfo.mac[1] << ":"
                            << m_StationInfo.mac[2] << ":" << m_StationInfo.mac[3] << ":"
                            << m_StationInfo.mac[4] << ":" << m_StationInfo.mac[5]);
  return true;
}

void VirtioNet::getName(String& name) {
  name.assign("virtio-net", 10);
}

bool VirtioNet::postReceive(Slot& slot) {
  Virtio::Buffer buffer = {slot.physical, BufferSize, true};
  return m_RxQueue.submit(&buffer, 1, &slot);
}

void VirtioNet::reclaimTransmit() {
  Virtio::Completion completion;
  while (m_TxQueue.pop(completion)) {
    auto* slot = static_cast<Slot*>(completion.cookie);
    slot->busy = false;
  }
}

bool VirtioNet::send(size_t length, uintptr_t buffer) {
  if (!buffer || length < EthernetHeaderSize || length > EthernetFrameMax) {
    return false;
  }

  LockGuard<Mutex> guard(m_Lock);
  if (!m_Initialised || m_Stopping) {
    return false;
  }

  reclaimTransmit();
  for (size_t n = 0; n < m_TxCount; ++n) {
    const size_t index = (m_NextTx + n) % m_TxCount;
    Slot& slot = m_TxSlots[index];
    if (slot.busy) {
      continue;
    }

    const size_t frameLength = length < EthernetFrameMin ? EthernetFrameMin : length;
    ByteSet(slot.data, 0, sizeof(VirtioNetHeader));
    MemoryCopy(slot.data + sizeof(VirtioNetHeader), reinterpret_cast<void*>(buffer), length);
    if (frameLength > length) {
      ByteSet(slot.data + sizeof(VirtioNetHeader) + length, 0, frameLength - length);
    }

    Virtio::Buffer descriptor = {
        slot.physical, static_cast<uint32_t>(sizeof(VirtioNetHeader) + frameLength), false};
    slot.busy = true;
    if (!m_TxQueue.submit(&descriptor, 1, &slot)) {
      slot.busy = false;
      return false;
    }
    m_NextTx = (index + 1) % m_TxCount;
    m_Transport.notify(1);
    return true;
  }
  return false;
}

bool VirtioNet::setStationInfo(const StationInfo& info) {
  LockGuard<Mutex> guard(m_Lock);
  if (m_StationInfo.dnsServers != info.dnsServers) {
    delete[] m_StationInfo.dnsServers;
  }
  m_StationInfo.ipv4 = info.ipv4;
  m_StationInfo.ipv6 = info.ipv6;
  m_StationInfo.nIpv6Addresses = info.nIpv6Addresses;
  m_StationInfo.subnetMask = info.subnetMask;
  m_StationInfo.broadcast = info.broadcast;
  m_StationInfo.gateway = info.gateway;
  m_StationInfo.gatewayIpv6 = info.gatewayIpv6;
  m_StationInfo.dnsServers = info.dnsServers;
  m_StationInfo.nDnsServers = info.nDnsServers;
  return true;
}

const StationInfo& VirtioNet::getStationInfo() {
  return m_StationInfo;
}

bool VirtioNet::isConnected() {
  LockGuard<Mutex> guard(m_Lock);
  if (!m_Initialised || m_Stopping) {
    return false;
  }
  if (!m_HasStatus) {
    return true;
  }
  uint16_t status = 0;
  return m_Transport.readDeviceConfig16(6, status) && (status & LinkUp);
}

IrqDisposition VirtioNet::irq(irq_id_t number) {
  (void)number;
  Virtio::Completion received[MaxRxSlots];
  size_t receiveCount = 0;
  {
    LockGuard<Mutex> guard(m_Lock);
    if (m_Stopping || !m_Initialised) {
      return IrqDisposition::Quiesced;
    }
    if (!m_Transport.readIsr()) {
      return IrqDisposition::NotHandled;
    }
    reclaimTransmit();
    while (receiveCount < m_RxCount && m_RxQueue.pop(received[receiveCount])) {
      ++receiveCount;
    }
  }

  for (size_t i = 0; i < receiveCount; ++i) {
    const auto& completion = received[i];
    auto* slot = static_cast<Slot*>(completion.cookie);
    const auto* header = reinterpret_cast<const VirtioNetHeader*>(slot->data);
    const size_t frameLength = completion.length >= sizeof(VirtioNetHeader)
                                   ? completion.length - sizeof(VirtioNetHeader)
                                   : 0;
    // Some QEMU versions leave this field zero without mergeable RX buffers.
    const bool valid = completion.length >= sizeof(VirtioNetHeader) + EthernetHeaderSize &&
                       completion.length <= BufferSize && frameLength <= EthernetFrameMax &&
                       !header->flags && !header->gsoType &&
                       LITTLE_TO_HOST16(header->numBuffers) <= 1;
    if (valid) {
      gotPacket();
      NetworkStack::instance().receive(
          frameLength, reinterpret_cast<uintptr_t>(slot->data + sizeof(VirtioNetHeader)), this, 0);
    } else {
      badPacket();
    }

    LockGuard<Mutex> guard(m_Lock);
    if (!m_Stopping && m_Initialised) {
      if (postReceive(*slot)) {
        m_Transport.notify(0);
      } else {
        ERROR("virtio-net: could not replace receive buffer");
      }
    }
  }
  return IrqDisposition::Handled;
}
