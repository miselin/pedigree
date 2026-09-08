/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "XhciRing.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"
namespace XhciHw {
bool allocate(MemoryRegion& region, size_t pages, bool contiguous) {
  if (TargetInfo::getPageSize() != PageBytes)
    return false;
  if (!PhysicalMemoryManager::instance().allocateRegion(
          region, pages,
          PhysicalMemoryManager::below4GB | (contiguous ? PhysicalMemoryManager::continuous : 0),
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write))
    return false;
  ByteSet(region.virtualAddress(), 0, pages * PageBytes);
  return true;
}
Ring::Ring() : m_Memory("xHCI ring"), m_Tail(0), m_Used(0), m_Cycle(true), m_Wraps(0) {}
bool Ring::initialise() {
  return allocate(m_Memory, 1);
}
bool Ring::enqueue(const Trb* entries, size_t count, uint64_t* addresses) {
  if (!count || count > 32 || m_Used + count > RingEntries - 1)
    return false;
  struct Publication {
    size_t index;
    uint32_t control;
  } publications[34];
  size_t published = 0;
  auto* ring = static_cast<volatile Trb*>(m_Memory.virtualAddress());
  for (size_t i = 0; i < count; ++i) {
    if (m_Tail == RingEntries - 1) {
      const uint32_t flags = (6U << 10) | 2U | m_Cycle | (i ? entries[i - 1].control & Chain : 0);
      ring[m_Tail].parameter = address();
      ring[m_Tail].status = 0;
      ring[m_Tail].control = flags ^ Cycle;
      publications[published++] = {m_Tail, flags};
      m_Tail = 0;
      m_Cycle = !m_Cycle;
      ++m_Wraps;
    }
    const uint32_t flags = (entries[i].control & ~Cycle) | m_Cycle;
    ring[m_Tail].parameter = entries[i].parameter;
    ring[m_Tail].status = entries[i].status;
    ring[m_Tail].control = flags ^ Cycle;
    addresses[i] = address() + m_Tail * sizeof(Trb);
    publications[published++] = {m_Tail++, flags};
  }
  // Make the entire TD visible before publishing its first cycle bit.
  FENCE();
  while (published) {
    const auto& publication = publications[--published];
    ring[publication.index].control = publication.control;
  }
  FENCE();
  m_Used += count;
  return true;
}
void Ring::retire(size_t count) {
  assert(count <= m_Used);
  m_Used -= count;
}
}  // namespace XhciHw
