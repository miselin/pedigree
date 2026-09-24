/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "Virtqueue.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

#include "VirtioPci.h"

using namespace Virtio;

Queue::Queue()
    : m_Descriptors("virtio descriptors"),
      m_Available("virtio available ring"),
      m_Used("virtio used ring"),
      m_Transport(nullptr),
      m_Cookies{},
      m_Next{},
      m_ChainLength{},
      m_FreeHead(Invalid),
      m_FreeCount(0),
      m_Depth(0),
      m_AvailableIndex(0),
      m_UsedIndex(0),
      m_Active{},
      m_Online(false),
      m_DmaArmed(false),
      m_Attached(false) {}

Queue::~Queue() {
  if (m_Attached || m_DmaArmed) {
    panic("virtio: queue freed before transport reset");
  }
}

bool Queue::initialise(uint16_t depth, PciTransport* transport) {
  if (m_Online || m_DmaArmed || !transport || depth < 2 || depth > MaxDepth ||
      (depth & (depth - 1)) || PhysicalMemoryManager::getPageSize() < sizeof(Descriptor) * depth) {
    return false;
  }
  auto& memory = PhysicalMemoryManager::instance();
  const size_t flags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;
  if (!memory.allocateRegion(m_Descriptors, 1, PhysicalMemoryManager::continuous, flags) ||
      !memory.allocateRegion(m_Available, 1, PhysicalMemoryManager::continuous, flags) ||
      !memory.allocateRegion(m_Used, 1, PhysicalMemoryManager::continuous, flags)) {
    return false;
  }
  const size_t page = PhysicalMemoryManager::getPageSize();
  ByteSet(m_Descriptors.virtualAddress(), 0, page);
  ByteSet(m_Available.virtualAddress(), 0, page);
  ByteSet(m_Used.virtualAddress(), 0, page);
  m_Depth = depth;
  m_FreeCount = depth;
  m_FreeHead = 0;
  for (uint16_t i = 0; i < depth; ++i)
    m_Next[i] = i + 1 == depth ? Invalid : i + 1;
  m_Transport = transport;
  m_Online = true;
  return true;
}

bool Queue::submit(const Buffer* buffers, size_t count, void* cookie) {
  if (!buffers || !count || count > MaxDepth) {
    return false;
  }
  LockGuard<Mutex> guard(m_Lock);
  if (!m_Online || count > m_FreeCount) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!buffers[i].address || !buffers[i].length ||
        buffers[i].address + buffers[i].length < buffers[i].address) {
      return false;
    }
  }
  auto* descriptors = static_cast<Descriptor*>(m_Descriptors.virtualAddress());
  const uint16_t head = m_FreeHead;
  uint16_t current = head;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t next = m_Next[current];
    descriptors[current].address = buffers[i].address;
    descriptors[current].length = buffers[i].length;
    descriptors[current].flags = (i + 1 < count ? 1U : 0U) | (buffers[i].deviceWrites ? 2U : 0U);
    descriptors[current].next = i + 1 < count ? next : 0;
    current = next;
  }
  m_FreeHead = current;
  m_FreeCount -= count;
  m_ChainLength[head] = count;
  m_Cookies[head] = cookie;
  m_Active[head] = true;
  auto* available = static_cast<volatile uint16_t*>(m_Available.virtualAddress());
  available[2 + (m_AvailableIndex % m_Depth)] = head;
  FENCE();
  available[1] = ++m_AvailableIndex;
  FENCE();
  return true;
}

bool Queue::pop(Completion& completion) {
  LockGuard<Mutex> guard(m_Lock);
  if (!m_Online) {
    return false;
  }
  auto* usedIndex = static_cast<volatile uint16_t*>(m_Used.virtualAddress());
  const uint16_t available = usedIndex[1];
  if (available == m_UsedIndex) {
    return false;
  }
  if (static_cast<uint16_t>(available - m_UsedIndex) > m_Depth) {
    ERROR("virtio: invalid used-ring index");
    m_Online = false;
    return false;
  }
  FENCE();
  auto* entries = static_cast<volatile uint32_t*>(m_Used.virtualAddress());
  const size_t slot = m_UsedIndex % m_Depth;
  const uint32_t id = entries[1 + slot * 2];
  const uint32_t length = entries[2 + slot * 2];
  if (id >= m_Depth || !m_Active[id] || !m_ChainLength[id]) {
    ERROR("virtio: invalid used-ring descriptor");
    m_Online = false;
    return false;
  }
  completion.cookie = m_Cookies[id];
  completion.length = length;
  m_Cookies[id] = nullptr;
  m_Active[id] = false;
  uint16_t current = id;
  for (uint16_t i = 0; i < m_ChainLength[id]; ++i) {
    const uint16_t next = m_Next[current];
    m_Next[current] = m_FreeHead;
    m_FreeHead = current;
    current = next;
  }
  m_FreeCount += m_ChainLength[id];
  m_ChainLength[id] = 0;
  ++m_UsedIndex;
  return true;
}

void Queue::stop() {
  LockGuard<Mutex> guard(m_Lock);
  m_Online = false;
}
