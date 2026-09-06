/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

physical_uintptr_t VirtualAddressSpace::m_ZeroPage = 0;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
VirtualAddressSpace::CopyOnWritePreCommitHook VirtualAddressSpace::m_CopyOnWritePreCommitHook =
    nullptr;

void VirtualAddressSpace::setCopyOnWritePreCommitHookForTest(CopyOnWritePreCommitHook hook) {
  __atomic_store_n(&m_CopyOnWritePreCommitHook, hook, __ATOMIC_RELEASE);
}

void VirtualAddressSpace::copyOnWritePreCommitForTest(void* virtualAddress) {
  CopyOnWritePreCommitHook hook = __atomic_load_n(&m_CopyOnWritePreCommitHook, __ATOMIC_ACQUIRE);
  if (hook) {
    hook(virtualAddress);
  }
}
#endif

bool VirtualAddressSpace::admitRawMemoryChange(const PreparedMemoryLock& plan, bool privileged,
                                               MemoryLockCharge& charge) const {
  MemoryLockAccount* account = memoryLockAccount();
  charge = account ? account->charge() : MemoryLockCharge{};
  if (!account)
    return true;
  if (plan.removedPages() > charge.rawPages)
    return false;
  const size_t retained = charge.rawPages - plan.removedPages();
  if (plan.addedPages() > ~size_t(0) - retained)
    return false;
  const size_t next = retained + plan.addedPages();
  if (charge.managedPages > ~size_t(0) - next)
    return false;
  if (next > charge.rawPages && !account->permitsTotalPages(charge.managedPages + next, privileged))
    return false;
  charge.rawPages = next;
  return true;
}

void VirtualAddressSpace::commitRawMemoryChange(PreparedMemoryLock& plan, MemoryLockCharge charge) {
  plan.commit();
  if (MemoryLockAccount* account = memoryLockAccount())
    account->publish(charge, account->futureMode());
}

bool VirtualAddressSpace::prepareZeroPage() {
  if (__atomic_load_n(&m_ZeroPage, __ATOMIC_ACQUIRE))
    return true;
  PhysicalMemoryManager& manager = PhysicalMemoryManager::instance();
  MemoryRegion candidate("User zero page");
  if (!manager.allocateRegion(candidate, 1, 0, KernelMode | Write))
    return false;
  ByteSet(candidate.virtualAddress(), 0, PhysicalMemoryManager::getPageSize());
  physical_uintptr_t physical = 0;
  size_t flags = 0;
  getKernelAddressSpace().getMapping(candidate.virtualAddress(), physical, flags);
  // The first pin represents the region; the second survives its destruction.
  manager.pin(physical);
  manager.pin(physical);
  physical_uintptr_t expected = 0;
  if (!__atomic_compare_exchange_n(&m_ZeroPage, &expected, physical, false, __ATOMIC_RELEASE,
                                   __ATOMIC_ACQUIRE))
    manager.freePage(physical);
  return true;
}

void* VirtualAddressSpace::expandHeap(ssize_t incr, size_t flags) {
  UserMemoryOperation operation(*this);
  const uintptr_t oldEnd = reinterpret_cast<uintptr_t>(m_HeapEnd);
  const uintptr_t start = reinterpret_cast<uintptr_t>(m_Heap);
  const size_t page = PhysicalMemoryManager::getPageSize();
  if (!incr)
    return m_HeapEnd;
  uintptr_t newEnd = oldEnd;
  if (incr > 0) {
    if (static_cast<uintptr_t>(incr) > ~uintptr_t(0) - oldEnd)
      return nullptr;
    newEnd += static_cast<uintptr_t>(incr);
  } else {
    const uintptr_t amount = static_cast<uintptr_t>(-(incr + 1)) + 1;
    if (amount > oldEnd - start)
      return nullptr;
    newEnd -= amount;
  }
  const uintptr_t limit = getDynamicStart() ? getDynamicStart() : getKernelStart();
  if (newEnd < start || newEnd >= limit || newEnd > ~uintptr_t(0) - (page - 1))
    return nullptr;
  const uintptr_t oldPagesEnd = (oldEnd + page - 1) & ~(page - 1);
  const uintptr_t newPagesEnd = (newEnd + page - 1) & ~(page - 1);
  if (oldPagesEnd == newPagesEnd) {
    m_HeapEnd = reinterpret_cast<void*>(newEnd);
    return reinterpret_cast<void*>(oldEnd);
  }
  if (!m_HeapRegionId)
    m_HeapRegionId = rawUserMemory().nextRegionId();
  if (!m_HeapRegionId)
    return nullptr;
  UserRegion previous{m_HeapRegionId, start, oldPagesEnd - start, UserRegion::Kind::Heap, true};
  UserRegion replacement{m_HeapRegionId, start, newPagesEnd - start, UserRegion::Kind::Heap, true};
  UniquePointer<PreparedMemoryLock> plan;
  if (rawUserMemory().prepareChange(&previous, &replacement, plan) != MemoryLockStatus::Success)
    return nullptr;
  MemoryLockCharge charge;
  if (!admitRawMemoryChange(*plan.get(), operation.privileged(), charge))
    return nullptr;
  if (newPagesEnd > oldPagesEnd) {
    if (!prepareZeroPage())
      return nullptr;
    uintptr_t address = oldPagesEnd;
    for (; address < newPagesEnd; address += page) {
      PhysicalMemoryManager::instance().pin(m_ZeroPage);
      if (!map(m_ZeroPage, reinterpret_cast<void*>(address), (flags & ~Write) | CopyOnWrite)) {
        PhysicalMemoryManager::instance().freePage(m_ZeroPage);
        rollbackHeapExpansion(reinterpret_cast<void*>(oldPagesEnd), (address - oldPagesEnd) / page);
        return nullptr;
      }
    }
    if (plan.get()->populate() != PopulationStatus::Success) {
      rollbackHeapExpansion(reinterpret_cast<void*>(oldPagesEnd),
                            (newPagesEnd - oldPagesEnd) / page);
      return nullptr;
    }
  }
  commitRawMemoryChange(*plan.get(), charge);
  m_HeapEnd = reinterpret_cast<void*>(newEnd);
  return reinterpret_cast<void*>(oldEnd);
}

void VirtualAddressSpace::rollbackHeapExpansion(void* virtualAddress, size_t pageCount) {
  for (size_t i = 0; i < pageCount; ++i) {
    size_t flags = 0;
    physical_uintptr_t physical = 0;
    if (detachMapping(virtualAddress, physical, flags))
      PhysicalMemoryManager::instance().freePage(physical);
    virtualAddress = adjust_pointer(virtualAddress, PhysicalMemoryManager::getPageSize());
  }
}

bool VirtualAddressSpace::mapHuge(physical_uintptr_t physAddress, void* virtualAddress,
                                  size_t count, size_t flags) {
  for (size_t i = 0; i < count; ++i) {
    size_t addend = PhysicalMemoryManager::getPageSize() * i;
    if (!map(physAddress + addend, adjust_pointer(virtualAddress, addend), flags)) {
      return false;
    }
  }

  return true;
}
