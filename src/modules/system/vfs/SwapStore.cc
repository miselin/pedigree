/* Copyright (c) 2026, Pedigree Developers. */
#include "SwapStore.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

struct SwapStore::State {
  struct Slot {
    size_t references = 0;
    uint32_t generation = 0;
  } slots[MaximumPages];
  PagingChannel channel;
  alignas(16) unsigned char buffer[PagingChannel::PageBytes];
  size_t pages = 0;
  size_t used = 0;
};

SwapStore& SwapStore::instance() {
  static SwapStore store;
  return store;
}
bool SwapStore::prepareAtBoot() {
#if (X64 || HOSTED) && THREADS
  if (!m_State)
    m_State = new State;
  return m_State != nullptr;
#else
  return false;
#endif
}
SwapStatus SwapStore::activate(uint32_t endpoint) {
#if !(X64 || HOSTED) || !THREADS
  return SwapStatus::Unsupported;
#endif
  if (!m_State)
    return SwapStatus::NoMemory;
  if (m_State->channel)
    return SwapStatus::Busy;
  switch (DiskEndpoints::prepare(endpoint, m_State->channel)) {
    case PagingStatus::Success:
      break;
    case PagingStatus::Busy:
      return SwapStatus::Busy;
    case PagingStatus::NoMemory:
      return SwapStatus::NoMemory;
    case PagingStatus::Unsupported:
      return SwapStatus::Unsupported;
    case PagingStatus::Invalid:
    case PagingStatus::Closed:
      return SwapStatus::Invalid;
    default:
      return SwapStatus::IoError;
  }
  SwapStatus result = SwapStatus::IoError;
  if (m_State->channel.readPage(0, m_State->buffer) == PagingStatus::Success)
    result = validateHeader(m_State->buffer, m_State->channel.size(), m_State->pages);
  if (result != SwapStatus::Success) {
    m_State->pages = 0;
    m_State->channel.reset();
  }
  return result;
}
SwapStatus SwapStore::finishDeactivate(uint32_t endpoint) {
  if (!m_State || !m_State->channel || endpoint != m_State->channel.endpointId())
    return SwapStatus::NotActive;
  if (m_State->used)
    return SwapStatus::Busy;
  if (m_State->channel.flush() != PagingStatus::Success)
    return SwapStatus::IoError;
  m_State->channel.reset();
  m_State->pages = 0;
  return SwapStatus::Success;
}
SwapStatus SwapStore::writePage(physical_uintptr_t physical, SwapReference& result) {
  if (result)
    return SwapStatus::Invalid;
  if (!m_State || !m_State->channel || !m_State->pages)
    return SwapStatus::NotActive;
  size_t index = 0;
  for (; index < m_State->pages; ++index)
    if (!m_State->slots[index].references && m_State->slots[index].generation != ~uint32_t(0))
      break;
  if (index == m_State->pages)
    return SwapStatus::NoMemory;
  if (!PhysicalMemoryManager::instance().copyPhysicalPageToBuffer(physical, m_State->buffer) ||
      m_State->channel.writePage((index + 1) * PagingChannel::PageBytes, m_State->buffer) !=
          PagingStatus::Success ||
      m_State->channel.flush() != PagingStatus::Success)
    return SwapStatus::IoError;
  auto& slot = m_State->slots[index];
  ++slot.generation;
  slot.references = 1;
  ++m_State->used;
  result.value = (uint64_t(slot.generation) << 32) | (index + 1);
  return SwapStatus::Success;
}
SwapStatus SwapStore::readPage(SwapReference reference, physical_uintptr_t physical) {
  const size_t index = uint32_t(reference.value) - size_t(1);
  if (!m_State || index >= m_State->pages || !m_State->slots[index].references ||
      m_State->slots[index].generation != uint32_t(reference.value >> 32))
    return SwapStatus::Invalid;
  if (m_State->channel.readPage((index + 1) * PagingChannel::PageBytes, m_State->buffer) !=
          PagingStatus::Success ||
      !PhysicalMemoryManager::instance().copyPhysicalPageFromBuffer(physical, m_State->buffer))
    return SwapStatus::IoError;
  return SwapStatus::Success;
}
bool SwapStore::retain(SwapReference reference) {
  const size_t index = uint32_t(reference.value) - size_t(1);
  if (!m_State || index >= m_State->pages || !m_State->slots[index].references ||
      m_State->slots[index].references == ~size_t(0) ||
      m_State->slots[index].generation != uint32_t(reference.value >> 32))
    return false;
  ++m_State->slots[index].references;
  return true;
}
void SwapStore::release(SwapReference& reference) {
  if (!reference)
    return;
  const size_t index = uint32_t(reference.value) - size_t(1);
  assert(m_State && index < m_State->pages && m_State->slots[index].references &&
         m_State->slots[index].generation == uint32_t(reference.value >> 32));
  if (!--m_State->slots[index].references)
    --m_State->used;
  reference.value = 0;
}
SwapSnapshot SwapStore::snapshot() const {
  if (!m_State || !m_State->channel)
    return {};
  return {m_State->pages, m_State->used, true};
}
uint32_t SwapStore::endpointId() const {
  return m_State ? m_State->channel.endpointId() : 0;
}

bool SwapStore::zeroPage(physical_uintptr_t physical) {
  if (!m_State)
    return false;
  ByteSet(m_State->buffer, 0, PagingChannel::PageBytes);
  return PhysicalMemoryManager::instance().copyPhysicalPageFromBuffer(physical, m_State->buffer);
}
