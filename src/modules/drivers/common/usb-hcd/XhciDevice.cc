/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Xhci.h"
using namespace XhciHw;
uint32_t* Xhci::inputContext(size_t index) {
  return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m_Input.virtualAddress()) +
                                     index * m_ContextSize);
}
uint32_t* Xhci::outputContext(uint8_t slot, size_t index) {
  return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m_Slots[slot].output.virtualAddress()) +
                                     index * m_ContextSize);
}
uint8_t Xhci::findSlot(const UsbEndpoint& endpoint) const {
  if (endpoint.nAddress >= 128 || endpoint.nRootPort >= m_PortCount || endpoint.nHubAddress)
    return 0;
  const uint8_t slot =
      endpoint.nAddress ? m_Addresses[endpoint.nAddress] : m_PortSlots[endpoint.nRootPort];
  const uint32_t status = read(m_Op + 0x400 + endpoint.nRootPort * 16);
  if (!slot || slot > m_SlotCount || m_Slots[slot].port != endpoint.nRootPort ||
      m_Slots[slot].generation != endpoint.nRootPortGeneration ||
      currentRootPortGeneration(endpoint.nRootPort) != endpoint.nRootPortGeneration ||
      m_Ports[endpoint.nRootPort].changePending || !(status & 1U) || (status & (1U << 17)))
    return 0;
  return slot;
}
bool Xhci::prepareDevice(uint8_t address, const UsbEndpoint& endpoint) {
  OperationBarrier::Lease admission;
  if (!m_Submissions.tryAcquire(admission) || !address || address >= 128 ||
      endpoint.nRootPort >= m_PortCount || endpoint.nHubAddress)
    return false;
  LockGuard<Mutex> configuration(m_ConfigurationLock);
  const uint8_t port = endpoint.nRootPort;
  if (m_Addresses[address] || m_PortSlots[port])
    return false;
  uint8_t slotId = 0;
  if (!command({0, 0, (9U << 10) | (uint32_t{m_Ports[port].slotType} << 16)}, &slotId))
    return false;
  if (!slotId || slotId > m_SlotCount || m_Slots[slotId].logical)
    panic("xHCI: controller returned invalid slot ownership");
  auto& slot = m_Slots[slotId];
  slot.logical = address;
  slot.port = port;
  slot.generation = endpoint.nRootPortGeneration;
  slot.speedId = (read(m_Op + 0x400 + port * 16) >> 10) & 15U;
  m_Addresses[address] = slotId;
  m_PortSlots[port] = slotId;
  if (!m_Ports[port].validSpeed[slot.speedId] || !allocate(slot.output, 1))
    return false;
  auto* ep = new Endpoint;
  slot.endpoints[1] = ep;
  ep->description = endpoint;
  if (!ep->ring.initialise())
    return false;
  static_cast<uint64_t*>(m_Dcbaa.virtualAddress())[slotId] = slot.output.physicalAddress();
  ByteSet(m_Input.virtualAddress(), 0, PageBytes);
  inputContext(0)[1] = 3;
  uint32_t* device = inputContext(1);
  device[0] = (uint32_t{slot.speedId} << 20) | (1U << 27);
  device[1] = (uint32_t{port + 1U} << 16);
  uint32_t* control = inputContext(2);
  control[1] = (3U << 1) | (4U << 3) | (endpoint.nMaxPacketSize << 16);
  const uint64_t dequeue = ep->ring.enqueuePointer();
  control[2] = dequeue;
  control[3] = dequeue >> 32;
  control[4] = 8;
  FENCE();
  if (!command({m_Input.physicalAddress(), 0, (11U << 10) | (1U << 9) | (uint32_t{slotId} << 24)}))
    return false;
  NOTICE("xHCI: logical address " << Dec << address << " slot " << slotId << " root port "
                                  << port + 1U << " USB" << m_Ports[port].major << " speed ID "
                                  << slot.speedId << Hex);
  return true;
}
bool Xhci::addressDevice(uint8_t address, const UsbEndpoint& endpoint) {
  OperationBarrier::Lease admission;
  if (!m_Submissions.tryAcquire(admission))
    return false;
  LockGuard<Mutex> configuration(m_ConfigurationLock);
  const uint8_t slotId = findSlot(endpoint);
  if (!slotId || m_Slots[slotId].logical != address)
    return false;
  auto& slot = m_Slots[slotId];
  auto* ep = slot.endpoints[1];
  ByteSet(m_Input.virtualAddress(), 0, PageBytes);
  inputContext(0)[1] = 2;
  MemoryCopy(inputContext(2), outputContext(slotId, 1), m_ContextSize);
  inputContext(2)[0] &= ~7U;
  inputContext(2)[1] = (inputContext(2)[1] & 0xffffU) | (endpoint.nMaxPacketSize << 16);
  if (ep->description.nMaxPacketSize != endpoint.nMaxPacketSize &&
      !command({m_Input.physicalAddress(), 0, (13U << 10) | (uint32_t{slotId} << 24)}))
    return false;
  ep->description.nMaxPacketSize = endpoint.nMaxPacketSize;
  inputContext(0)[1] = 3;
  MemoryCopy(inputContext(1), outputContext(slotId, 0), m_ContextSize);
  inputContext(1)[3] = 0;
  const uint64_t dequeue = ep->ring.enqueuePointer();
  inputContext(2)[2] = dequeue;
  inputContext(2)[3] = dequeue >> 32;
  FENCE();
  return command({m_Input.physicalAddress(), 0, (11U << 10) | (uint32_t{slotId} << 24)});
}
Xhci::Endpoint* Xhci::ensureEndpoint(uint8_t slotId, const UsbEndpoint& description) {
  auto& slot = m_Slots[slotId];
  const uint8_t dci = description.nEndpoint ? description.nEndpoint * 2 + description.nIn : 1;
  if (dci >= 32 || description.nStreams || description.nMaxBurst > 15 ||
      description.nTransferType == 1 || description.nMaxPacketSize > 1024)
    return nullptr;
  if (slot.endpoints[dci]) {
    auto* endpoint = slot.endpoints[dci];
    if (endpoint->needsReset && !resetEndpointHardware(slotId, dci, false))
      return nullptr;
    return endpoint;
  }
  if (!description.nEndpoint || (description.nTransferType != 2 && description.nTransferType != 3))
    return nullptr;
  auto* endpoint = new Endpoint;
  endpoint->description = description;
  if (!endpoint->ring.initialise()) {
    delete endpoint;
    return nullptr;
  }
  uint32_t interval = 0;
  if (description.nTransferType == 3) {
    if (!description.nInterval) {
      delete endpoint;
      return nullptr;
    }
    if (description.speed == HighSpeed || description.speed == SuperSpeed) {
      if (description.nInterval > 16) {
        delete endpoint;
        return nullptr;
      }
      interval = description.nInterval - 1;
    } else {
      interval = 3;
      for (size_t frames = description.nInterval; frames >= 2; frames /= 2)
        ++interval;
    }
  }
  ByteSet(m_Input.virtualAddress(), 0, PageBytes);
  inputContext(0)[1] = 1U | (1U << dci);
  MemoryCopy(inputContext(1), outputContext(slotId, 0), m_ContextSize);
  const size_t entries = dci > slot.contextEntries ? dci : slot.contextEntries;
  inputContext(1)[0] = (inputContext(1)[0] & ~(31U << 27)) | (entries << 27);
  inputContext(1)[3] = 0;
  uint32_t* context = inputContext(dci + 1);
  size_t esit = description.nTransferType == 3
                    ? (description.nBytesPerInterval ? description.nBytesPerInterval
                                                     : description.nMaxPacketSize)
                    : 0;
  context[0] = (interval << 16) | ((esit >> 16) << 24);
  const uint32_t type = (description.nTransferType == 2 ? 2U : 3U) + (description.nIn ? 4U : 0U);
  context[1] = (3U << 1) | (type << 3) | (uint32_t{description.nMaxBurst} << 8) |
               (description.nMaxPacketSize << 16);
  const uint64_t dequeue = endpoint->ring.enqueuePointer();
  context[2] = dequeue;
  context[3] = dequeue >> 32;
  context[4] = description.nMaxPacketSize | ((esit & 0xffffU) << 16);
  FENCE();
  if (!command({m_Input.physicalAddress(), 0, (12U << 10) | (uint32_t{slotId} << 24)})) {
    delete endpoint;
    return nullptr;
  }
  slot.endpoints[dci] = endpoint;
  slot.contextEntries = entries;
  return endpoint;
}
bool Xhci::resetEndpointHardware(uint8_t slotId, uint8_t dci, bool stop) {
  auto* endpoint = m_Slots[slotId].endpoints[dci];
  if (!endpoint)
    return false;
  const uint32_t state = outputContext(slotId, dci)[0] & 7U;
  const uint32_t target = (uint32_t{slotId} << 24) | (uint32_t{dci} << 16);
  if (state == 2) {
    if (!command({0, 0, (14U << 10) | target}))
      return false;
  } else if (state == 1 && stop) {
    if (!command({0, 0, (15U << 10) | target}))
      return false;
  } else if (state != 3)
    return false;
  if (!command({endpoint->ring.enqueuePointer(), 0, (16U << 10) | target}))
    return false;
  endpoint->needsReset = false;
  return true;
}
bool Xhci::resetEndpoint(const UsbEndpoint& endpoint) {
  OperationBarrier::Lease admission;
  if (!m_Submissions.tryAcquire(admission))
    return false;
  LockGuard<Mutex> configuration(m_ConfigurationLock);
  const uint8_t slot = findSlot(endpoint);
  const uint8_t dci = endpoint.nEndpoint ? endpoint.nEndpoint * 2 + endpoint.nIn : 1;
  if (!slot || dci >= 32 || !m_Slots[slot].endpoints[dci])
    return false;
  return resetEndpointHardware(slot, dci, true);
}
void Xhci::releaseDeviceAddress(uint8_t address) {
  LockGuard<Mutex> configuration(m_ConfigurationLock);
  const uint8_t slotId = m_Addresses[address];
  if (!slotId)
    return;
  const bool retired = m_Online && command({0, 0, (10U << 10) | (uint32_t{slotId} << 24)});
  if (m_Online && !retired) {
    LockGuard<Mutex> lock(m_Lock);
    failLocked();
  }
  LockGuard<Mutex> lock(m_Lock);
  auto& slot = m_Slots[slotId];
  for (auto*& endpoint : slot.endpoints) {
    if (endpoint && endpoint->active)
      panic("xHCI: logical address retired with an owned transfer");
    delete endpoint;
    endpoint = nullptr;
  }
  if (m_Dcbaa)
    static_cast<uint64_t*>(m_Dcbaa.virtualAddress())[slotId] = 0;
  m_PortSlots[slot.port] = 0;
  m_Addresses[address] = 0;
  slot.output.free();
  slot.logical = 0;
  slot.contextEntries = 1;
#if PEDIGREE_USB_SMOKE_TESTS
  if (retired && !m_Failed)
    NOTICE("XHCI-SMOKE: slot-retired logical=" << Hex << address);
#endif
}
