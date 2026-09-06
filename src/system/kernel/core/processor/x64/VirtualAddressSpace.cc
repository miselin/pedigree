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

#include "VirtualAddressSpace.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/utility.h"

#include "VirtualAddressSpace-internal.h"
#include "utils.h"

// Defined in boot-standalone.s
extern void* pml4;

X64VirtualAddressSpace X64VirtualAddressSpace::m_KernelSpace(
    KERNEL_VIRTUAL_HEAP,
    reinterpret_cast<uintptr_t>(&pml4) - reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_ADDRESS),
    KERNEL_VIRTUAL_STACK);

static void trackPages(ssize_t v, ssize_t p, ssize_t s) {
  // Track, if we can.
  Thread* pThread = Processor::information().getCurrentThread();
  if (pThread) {
    Process* pProcess = pThread->getParent();
    if (pProcess) {
      pProcess->trackPages(v, p, s);
    }
  }
}

VirtualAddressSpace& VirtualAddressSpace::getKernelAddressSpace() {
  return X64VirtualAddressSpace::m_KernelSpace;
}

VirtualAddressSpace* VirtualAddressSpace::create() {
  return new X64VirtualAddressSpace();
}

bool X64VirtualAddressSpace::memIsInKernelHeap(void* pMem) {
  if (pMem < KERNEL_VIRTUAL_HEAP) {
    return false;
  } else if (pMem >= adjust_pointer(KERNEL_VIRTUAL_HEAP, KERNEL_VIRTUAL_HEAP_SIZE)) {
    return false;
  }

  return true;
}

bool X64VirtualAddressSpace::memIsInHeap(void* pMem) {
  if (pMem < m_Heap) {
    WARNING("memIsInHeap: " << pMem << " is below the kernel heap.");
    return false;
  } else if (pMem >= getEndOfHeap()) {
    WARNING("memIsInHeap: " << pMem << " is beyond the end of the heap (" << getEndOfHeap()
                            << ").");
    return false;
  } else
    return true;
}
void* X64VirtualAddressSpace::getEndOfHeap() {
  if (m_Heap == KERNEL_VIRTUAL_HEAP) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_HEAP) +
                                   KERNEL_VIRTUAL_HEAP_SIZE);
  } else {
    return m_HeapEnd;
  }
}

bool X64VirtualAddressSpace::isAddressValid(void* virtualAddress) {
  if (reinterpret_cast<uint64_t>(virtualAddress) < 0x0000800000000000ULL ||
      reinterpret_cast<uint64_t>(virtualAddress) >= 0xFFFF800000000000ULL) {
    return true;
  }
  return false;
}
bool X64VirtualAddressSpace::isMapped(void* virtualAddress) {
  LockGuard<Spinlock> guard(m_Lock);

  size_t pml4Index = PML4_INDEX(virtualAddress);
  uint64_t* pml4Entry = TABLE_ENTRY(m_PhysicalPML4, pml4Index);

  // Is a page directory pointer table present?
  if ((*pml4Entry & PAGE_PRESENT) != PAGE_PRESENT)
    return false;

  size_t pageDirectoryPointerIndex = PAGE_DIRECTORY_POINTER_INDEX(virtualAddress);
  uint64_t* pageDirectoryPointerEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pml4Entry), pageDirectoryPointerIndex);

  // Is a page directory present?
  if ((*pageDirectoryPointerEntry & PAGE_PRESENT) != PAGE_PRESENT)
    return false;

  size_t pageDirectoryIndex = PAGE_DIRECTORY_INDEX(virtualAddress);
  uint64_t* pageDirectoryEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryPointerEntry), pageDirectoryIndex);

  // Is a page table or 2MB page present?
  if ((*pageDirectoryEntry & PAGE_PRESENT) != PAGE_PRESENT)
    return false;

  // Is it a 2MB page?
  if ((*pageDirectoryEntry & PAGE_2MB) == PAGE_2MB)
    return true;

  size_t pageTableIndex = PAGE_TABLE_INDEX(virtualAddress);
  uint64_t* pageTableEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryEntry), pageTableIndex);

  // Is a page present?
  return (*pageTableEntry & (PAGE_PRESENT | PAGE_NO_ACCESS)) != 0;
}

bool X64VirtualAddressSpace::map(physical_uintptr_t physAddress, void* virtualAddress,
                                 size_t flags) {
  X64MappingMutationScope mutation;
  mutation.lock(m_Lock);

  const bool mapped = mapUnlocked(physAddress, virtualAddress, flags, mutation, true);
  if (mutation.failed()) {
    mutation.panicInvalidationFailure();
  }
  return mapped;
}

bool X64VirtualAddressSpace::mapHuge(physical_uintptr_t physAddress, void* virtualAddress,
                                     size_t count, size_t flags) {
  const size_t smallPageSize = PhysicalMemoryManager::getPageSize();
  const size_t twoMiB = 1UL << 21UL;
  const size_t pagesPerTwoMiB = twoMiB / smallPageSize;
  const uintptr_t virtualValue = reinterpret_cast<uintptr_t>(virtualAddress);

  if (count < pagesPerTwoMiB || (physAddress % twoMiB) || (virtualValue % twoMiB)) {
    return VirtualAddressSpace::mapHuge(physAddress, virtualAddress, count, flags);
  }

  // The page-table walkers only recognize the page-size bit at the page
  // directory level. Keep this path at 2 MiB until every walker can safely
  // stop at a 1 GiB page-directory-pointer entry.
  const size_t numHugePages = count / pagesPerTwoMiB;
  const size_t mappedPages = numHugePages * pagesPerTwoMiB;
  {
    X64MappingMutationScope mutation;
    mutation.lock(m_Lock);

    // Clean up existing mappings before installing the huge-page entries.
    for (size_t i = 0; i < mappedPages; ++i) {
      unmapUnlocked(adjust_pointer(virtualAddress, i * smallPageSize), mutation, false);
      if (mutation.failed()) {
        mutation.panicInvalidationFailure();
      }
    }

    size_t Flags = toFlags(flags, true);
    for (size_t i = 0; i < numHugePages; ++i) {
      size_t pml4Index = PML4_INDEX(virtualAddress);
      uint64_t* pml4Entry = TABLE_ENTRY(m_PhysicalPML4, pml4Index);

      // Is a page directory pointer table present?
      if (conditionalTableEntryAllocation(pml4Entry, flags) == false) {
        return false;
      }

      size_t pageDirectoryPointerIndex = PAGE_DIRECTORY_POINTER_INDEX(virtualAddress);
      uint64_t* pageDirectoryPointerEntry =
          TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pml4Entry), pageDirectoryPointerIndex);

      // Is a page directory present?
      if (conditionalTableEntryAllocation(pageDirectoryPointerEntry, flags) == false) {
        return false;
      }

      size_t pageDirectoryIndex = PAGE_DIRECTORY_INDEX(virtualAddress);
      uint64_t* pageDirectoryEntry =
          TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryPointerEntry), pageDirectoryIndex);

      *pageDirectoryEntry = physAddress | PAGE_2MB | Flags;
      if (!invalidateMapping(virtualAddress, mutation)) {
        mutation.panicInvalidationFailure();
      }

      virtualAddress = adjust_pointer(virtualAddress, twoMiB);
      physAddress += twoMiB;
    }
  }

  if (mappedPages < count) {
    return mapHuge(physAddress, virtualAddress, count - mappedPages, flags);
  }

  return true;
}

bool X64VirtualAddressSpace::mapUnlocked(physical_uintptr_t physAddress, void* virtualAddress,
                                         size_t flags, X64MappingMutationScope& mutation,
                                         bool locked) {
  size_t Flags = toFlags(flags, true);
  size_t pml4Index = PML4_INDEX(virtualAddress);
  uint64_t* pml4Entry = TABLE_ENTRY(m_PhysicalPML4, pml4Index);

  // Check if a page directory pointer table was present *before* the
  // conditional allocation.
  const bool pml4WasPresent = (*pml4Entry & PAGE_PRESENT) == PAGE_PRESENT;

  // Is a page directory pointer table present?
  if (conditionalTableEntryAllocation(pml4Entry, flags) == false) {
    return false;
  }

  size_t pageDirectoryPointerIndex = PAGE_DIRECTORY_POINTER_INDEX(virtualAddress);
  uint64_t* pageDirectoryPointerEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pml4Entry), pageDirectoryPointerIndex);

  // Is a page directory present?
  if (conditionalTableEntryAllocation(pageDirectoryPointerEntry, flags) == false) {
    return false;
  }

  size_t pageDirectoryIndex = PAGE_DIRECTORY_INDEX(virtualAddress);
  uint64_t* pageDirectoryEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryPointerEntry), pageDirectoryIndex);

  // Is a page table present?
  if (conditionalTableEntryAllocation(pageDirectoryEntry, flags) == false) {
    return false;
  }

  size_t pageTableIndex = PAGE_TABLE_INDEX(virtualAddress);
  uint64_t* pageTableEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryEntry), pageTableIndex);

  // Is a page already present?
  if (*pageTableEntry & (PAGE_PRESENT | PAGE_NO_ACCESS)) {
    return false;
  }

  // Map the page
  *pageTableEntry = physAddress | Flags;

  trackPages(1, 0, 0);

  // We don't need the lock to propagate the PDPT.
  if (locked) {
    mutation.unlock(m_Lock);
  }

  // If there wasn't a PDPT already present, and the address is in the kernel
  // area of memory, we need to propagate this change across all address
  // spaces.
  if (!pml4WasPresent && Processor::m_Initialised == 2 && virtualAddress >= KERNEL_SPACE_START) {
    uint64_t thisPml4Entry = *pml4Entry;
    for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); i++) {
      Scheduler::ProcessLease process;
      if (!Scheduler::instance().acquireProcess(process, i)) {
        continue;
      }

      X64VirtualAddressSpace* x64VAS =
          reinterpret_cast<X64VirtualAddressSpace*>(process->getAddressSpace());
      uint64_t* otherPml4Entry = TABLE_ENTRY(x64VAS->m_PhysicalPML4, pml4Index);
      *otherPml4Entry = thisPml4Entry;
    }
  }

  // If we were locked before, take the lock to enforce that.
  if (locked) {
    mutation.relock(m_Lock);
  }

  // A previously non-present entry can be cached, and upper-half entries are
  // shared by every active address space. Invalidate only after any new PML4
  // entry has been propagated.
  if (!invalidateMapping(virtualAddress, mutation)) {
    return false;
  }

  return true;
}

void X64VirtualAddressSpace::getMapping(void* virtualAddress, physical_uintptr_t& physAddress,
                                        size_t& flags) {
  // Get a pointer to the page-table entry (Also checks whether the page is
  // actually present or marked swapped out)
  uint64_t* pageTableEntry = 0;
  if (getPageTableEntry(virtualAddress, pageTableEntry) == false) {
    panic("VirtualAddressSpace::getMapping(): function misused");
  }

  // Extract the physical address and the flags
  physAddress = PAGE_GET_PHYSICAL_ADDRESS(pageTableEntry);
  flags = fromFlags(PAGE_GET_FLAGS(pageTableEntry), true);
}

bool X64VirtualAddressSpace::handleCopyOnWriteFault(void* virtualAddress, bool userMode) {
  virtualAddress = page_align(virtualAddress);

  {
    LockGuard<Spinlock> guard(m_Lock);
    uint64_t* pageTableEntry = nullptr;
    if (!getPageTableEntry(virtualAddress, pageTableEntry)) {
      return false;
    }

    const uint64_t pageFlags = *pageTableEntry;
    if ((userMode && !(pageFlags & PAGE_USER)) ||
        (pageFlags & (PAGE_NO_ACCESS | PAGE_WRITE_PROTECTED))) {
      return false;
    }
    if ((pageFlags & PAGE_PRESENT) && (pageFlags & PAGE_WRITE) &&
        !(pageFlags & PAGE_COPY_ON_WRITE)) {
      return true;
    }
    if (!(pageFlags & PAGE_PRESENT) || !(pageFlags & PAGE_COPY_ON_WRITE) ||
        (pageFlags & PAGE_SWAPPED)) {
      return false;
    }
  }

  PhysicalMemoryManager& physicalMemory = PhysicalMemoryManager::instance();
  const physical_uintptr_t replacement = physicalMemory.allocatePage();
  if (!replacement) {
    return false;
  }

  bool retireReplacement = true;
  bool resolved = false;
  physical_uintptr_t oldPhysical = 0;
  {
    X64MappingMutationScope mutation;
    mutation.lock(m_Lock);

    uint64_t* pageTableEntry = nullptr;
    if (getPageTableEntry(virtualAddress, pageTableEntry)) {
      const uint64_t pageFlags = *pageTableEntry;
      if ((userMode && !(pageFlags & PAGE_USER)) ||
          (pageFlags & (PAGE_NO_ACCESS | PAGE_WRITE_PROTECTED))) {
        resolved = false;
      } else if ((pageFlags & PAGE_PRESENT) && (pageFlags & PAGE_WRITE) &&
                 !(pageFlags & PAGE_COPY_ON_WRITE)) {
        resolved = true;
      } else if ((pageFlags & PAGE_PRESENT) && (pageFlags & PAGE_COPY_ON_WRITE) &&
                 !(pageFlags & PAGE_SWAPPED)) {
        oldPhysical = PAGE_GET_PHYSICAL_ADDRESS(pageTableEntry);
        MemoryCopy(reinterpret_cast<void*>(physicalAddress(replacement)),
                   reinterpret_cast<void*>(physicalAddress(oldPhysical)),
                   PhysicalMemoryManager::getPageSize());

        uint64_t replacementFlags = PAGE_GET_FLAGS(pageTableEntry);
        replacementFlags |= PAGE_WRITE;
        replacementFlags &= ~(PAGE_COPY_ON_WRITE | PAGE_BORROWED | PAGE_SHARED);
        __atomic_store_n(pageTableEntry, replacement | replacementFlags, __ATOMIC_RELEASE);
        if (!invalidateMapping(virtualAddress, mutation)) {
          mutation.panicInvalidationFailure();
        }

        retireReplacement = false;
        resolved = true;
      }
    }
  }

  if (retireReplacement) {
    physicalMemory.freePage(replacement);
  }
  if (oldPhysical) {
    physicalMemory.freePage(oldPhysical);
  }
  return resolved;
}

bool X64VirtualAddressSpace::tryReadUser32(uintptr_t address, uint32_t& value) {
  uintptr_t word = 0;
  if (!tryAccessUserWord(address, sizeof(value), word, nullptr)) {
    return false;
  }
  value = static_cast<uint32_t>(word);
  return true;
}

bool X64VirtualAddressSpace::tryReadUserPointer(uintptr_t address, uintptr_t& value) {
  return tryAccessUserWord(address, sizeof(value), value, nullptr);
}

bool X64VirtualAddressSpace::tryCompareExchangeUser32(uintptr_t address, uint32_t& expected,
                                                      uint32_t desired, bool& exchanged) {
  const uint32_t original = expected;
  uintptr_t observed = expected;
  const uintptr_t replacement = desired;
  exchanged = false;
  if (!tryAccessUserWord(address, sizeof(expected), observed, &replacement)) {
    return false;
  }
  expected = static_cast<uint32_t>(observed);
  exchanged = expected == original;
  return true;
}

VirtualAddressSpace::ResidentCopyStatus X64VirtualAddressSpace::copyResidentUserPage(
    uintptr_t address, void* kernelBuffer, size_t bytes, bool write) {
  const uintptr_t userEnd = 0x0000800000000000ULL;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (!kernelBuffer || !bytes || bytes > pageSize || address < getUserStart() ||
      address >= userEnd || address >= getKernelStart() || bytes > userEnd - address ||
      bytes > getKernelStart() - address || (address & (pageSize - 1)) > pageSize - bytes)
    return ResidentCopyStatus::Inaccessible;

  LockGuard<Spinlock> guard(m_Lock);
  uint64_t table = m_PhysicalPML4;
  const size_t indices[] = {(address >> 39) & 0x1ff, (address >> 30) & 0x1ff,
                            (address >> 21) & 0x1ff};
  for (const size_t index : indices) {
    uint64_t* entry = TABLE_ENTRY(table, index);
    const uint64_t flags = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
    if (!(flags & PAGE_PRESENT) || !(flags & PAGE_USER) || (flags & PAGE_2MB) ||
        (write && !(flags & PAGE_WRITE)))
      return ResidentCopyStatus::Inaccessible;
    table = PAGE_GET_PHYSICAL_ADDRESS(entry);
  }
  uint64_t* entry = TABLE_ENTRY(table, (address >> 12) & 0x1ff);
  const uint64_t flags = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
  if (!(flags & PAGE_PRESENT) || !(flags & PAGE_USER) ||
      (flags &
       (PAGE_SWAPPED | PAGE_NO_ACCESS | PAGE_CACHE_DISABLE | PAGE_WRITE_COMBINE | PAGE_PAT)) ||
      (write && (!(flags & PAGE_WRITE) || (flags & (PAGE_COPY_ON_WRITE | PAGE_WRITE_PROTECTED)))))
    return ResidentCopyStatus::Inaccessible;

  // The owner cannot detach this latest leaf while its physical alias is in use.
  void* userBytes = reinterpret_cast<void*>(
      physicalAddress(PAGE_GET_PHYSICAL_ADDRESS(entry) + (address & (pageSize - 1))));
  if (write)
    MemoryCopy(userBytes, kernelBuffer, bytes);
  else
    MemoryCopy(kernelBuffer, userBytes, bytes);
  __atomic_fetch_or(entry, PAGE_ACCESSED | (write ? PAGE_DIRTY : 0), __ATOMIC_RELEASE);
  return ResidentCopyStatus::Success;
}

bool X64VirtualAddressSpace::tryAccessUserWord(uintptr_t address, size_t width, uintptr_t& value,
                                               const uintptr_t* replacement) {
  if (!address || (address % width) || address < getUserStart() ||
      address >= 0x0000800000000000ULL || address >= getKernelStart() ||
      address > getKernelStart() - width) {
    return false;
  }

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t pageOffset = address & (pageSize - 1);
  if (pageOffset > pageSize - width) {
    return false;
  }

  LockGuard<Spinlock> guard(m_Lock);
  uint64_t* pageTableEntry = nullptr;
  if (!getPageTableEntry(reinterpret_cast<void*>(address), pageTableEntry)) {
    return false;
  }

  const uint64_t pageFlags = *pageTableEntry;
  if (!(pageFlags & PAGE_PRESENT) || !(pageFlags & PAGE_USER) ||
      (pageFlags & (PAGE_SWAPPED | PAGE_NO_ACCESS)) ||
      (replacement &&
       (!(pageFlags & PAGE_WRITE) || (pageFlags & (PAGE_COPY_ON_WRITE | PAGE_WRITE_PROTECTED))))) {
    return false;
  }

  void* target = reinterpret_cast<void*>(
      physicalAddress(PAGE_GET_PHYSICAL_ADDRESS(pageTableEntry) + pageOffset));
  if (replacement) {
    uint32_t expected = static_cast<uint32_t>(value);
    __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(target), &expected,
                                static_cast<uint32_t>(*replacement), false, __ATOMIC_ACQ_REL,
                                __ATOMIC_ACQUIRE);
    value = expected;
  } else if (width == sizeof(uint32_t)) {
    value = __atomic_load_n(reinterpret_cast<uint32_t*>(target), __ATOMIC_ACQUIRE);
  } else {
    value = __atomic_load_n(reinterpret_cast<uintptr_t*>(target), __ATOMIC_ACQUIRE);
  }
  return true;
}

bool X64VirtualAddressSpace::tryWriteUser32(uintptr_t address, uint32_t value) {
  if (!address || (address % alignof(uint32_t)) || address < getUserStart() ||
      address >= 0x0000800000000000ULL || address >= getKernelStart() ||
      address > getKernelStart() - sizeof(value)) {
    return false;
  }

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t pageOffset = address & (pageSize - 1);
  if (pageOffset > pageSize - sizeof(value)) {
    return false;
  }

  LockGuard<Spinlock> guard(m_Lock);
  uint64_t* pageTableEntry = nullptr;
  if (!getPageTableEntry(reinterpret_cast<void*>(address), pageTableEntry)) {
    return false;
  }

  const uint64_t pageFlags = *pageTableEntry;
  if (!(pageFlags & PAGE_PRESENT) || !(pageFlags & PAGE_USER) || !(pageFlags & PAGE_WRITE) ||
      (pageFlags & (PAGE_COPY_ON_WRITE | PAGE_SWAPPED | PAGE_NO_ACCESS | PAGE_WRITE_PROTECTED))) {
    return false;
  }

  const physical_uintptr_t target = PAGE_GET_PHYSICAL_ADDRESS(pageTableEntry) + pageOffset;
  __atomic_store_n(reinterpret_cast<uint32_t*>(physicalAddress(target)), value, __ATOMIC_RELEASE);
  return true;
}

void X64VirtualAddressSpace::setFlags(void* virtualAddress, size_t newFlags) {
  X64MappingMutationScope mutation;
  mutation.lock(m_Lock);

  // Get a pointer to the page-table entry (Also checks whether the page is
  // actually present or marked swapped out)
  uint64_t* pageTableEntry = 0;
  if (getPageTableEntry(virtualAddress, pageTableEntry) == false) {
    mutation.panicWithoutRestoringInterrupts("VirtualAddressSpace::setFlags(): function misused");
  }

  // Set the flags
  PAGE_SET_FLAGS(pageTableEntry, toFlags(newFlags, true));

  // Flush TLB - modified the mapping for this address.
  if (!invalidateMapping(virtualAddress, mutation)) {
    mutation.panicInvalidationFailure();
  }
}

bool X64VirtualAddressSpace::trySetFlags(void* virtualAddress, size_t newFlags) {
  X64MappingMutationScope mutation;
  mutation.lock(m_Lock);

  // Get a pointer to the page-table entry (Also checks whether the page is
  // actually present or marked swapped out)
  uint64_t* pageTableEntry = 0;
  if (!getPageTableEntry(virtualAddress, pageTableEntry) || !(*pageTableEntry & PAGE_PRESENT) ||
      (*pageTableEntry & PAGE_SWAPPED)) {
    return false;
  }

  // Set the flags
  PAGE_SET_FLAGS(pageTableEntry, toFlags(newFlags, true));

  // Flush TLB - modified the mapping for this address.
  if (!invalidateMapping(virtualAddress, mutation)) {
    mutation.panicInvalidationFailure();
  }
  return true;
}

void X64VirtualAddressSpace::unmap(void* virtualAddress) {
  X64MappingMutationScope mutation;
  mutation.lock(m_Lock);

  if (!unmapUnlocked(virtualAddress, mutation)) {
    if (mutation.failed()) {
      mutation.panicInvalidationFailure();
    }
    mutation.panicWithoutRestoringInterrupts("VirtualAddressSpace::unmap(): function misused");
  }
}

bool X64VirtualAddressSpace::detachMapping(void* virtualAddress, physical_uintptr_t& physical,
                                           size_t& flags, size_t requiredFlags) {
  X64MappingMutationScope mutation;
  mutation.lock(m_Lock);
  physical = 0;
  flags = 0;
  uint64_t* entry = nullptr;
  if (!getPageTableEntry(virtualAddress, entry)) {
    return false;
  }
  physical = PAGE_GET_PHYSICAL_ADDRESS(entry);
  flags = fromFlags(PAGE_GET_FLAGS(entry), true);
  if ((flags & requiredFlags) != requiredFlags) {
    return false;
  }
  if (!unmapUnlocked(virtualAddress, mutation)) {
    mutation.panicInvalidationFailure();
  }
  return true;
}

bool X64VirtualAddressSpace::unmapUnlocked(void* virtualAddress, X64MappingMutationScope& mutation,
                                           bool requireMapped) {
  // Get a pointer to the page-table entry (Also checks whether the page is
  // actually present or marked swapped out)
  uint64_t* pageTableEntry = 0;
  if (getPageTableEntry(virtualAddress, pageTableEntry) == false) {
    // Not mapped! This is a panic for most cases, but private usage of
    // unmap within X64VirtualAddressSpace is allowed to do this.
    if (requireMapped) {
      return false;
    } else {
      return true;
    }
  }

  // Unmap the page
  *pageTableEntry = 0;

  trackPages(-1, 0, 0);

  // Detach empty paging structures before the invalidation, but retain their
  // storage until every processor has discarded both translations and
  // paging-structure-cache entries for this address.
  physical_uintptr_t detachedTables[3] = {};
  const size_t detachedCount = detachEmptyTables(virtualAddress, detachedTables);
  if (!invalidateMapping(virtualAddress, mutation)) {
    return false;
  }
  for (size_t i = 0; i < detachedCount; ++i) {
    PhysicalMemoryManager::instance().freePage(detachedTables[i]);
  }
  return true;
}

VirtualAddressSpace* X64VirtualAddressSpace::clone(bool copyOnWrite) {
  UserMemoryOperation operation(*this);
  /// \todo figure out how to handle page tracking here

  // Create a new virtual address space
  X64VirtualAddressSpace* pClone =
      static_cast<X64VirtualAddressSpace*>(VirtualAddressSpace::create());
  if (pClone == 0) {
    WARNING("X64VirtualAddressSpace: Clone() failed!");
    return 0;
  }

  if (rawUserMemory().cloneInto(pClone->rawUserMemory()) != MemoryLockStatus::Success) {
    delete pClone;
    return nullptr;
  }
  pClone->m_HeapRegionId = m_HeapRegionId;

  {
    // Lock both address spaces so we can clone their mappings safely.
    X64MappingMutationScope mutation;
    mutation.lock(pClone->m_Lock);
    mutation.lock(m_Lock);

    // The userspace area is only the bottom half of the address space - the top
    // 256 PML4 entries are for the kernel, and these should be mapped anyway.
    for (uint64_t i = 0; i < 256; i++) {
      uint64_t* pml4Entry = TABLE_ENTRY(m_PhysicalPML4, i);
      if ((*pml4Entry & PAGE_PRESENT) != PAGE_PRESENT)
        continue;

      for (uint64_t j = 0; j < 512; j++) {
        uint64_t* pdptEntry = TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pml4Entry), j);
        if ((*pdptEntry & PAGE_PRESENT) != PAGE_PRESENT)
          continue;

        for (uint64_t k = 0; k < 512; k++) {
          uint64_t* pdEntry = TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pdptEntry), k);
          if ((*pdEntry & PAGE_PRESENT) != PAGE_PRESENT)
            continue;

          /// \todo Deal with 2MB pages here.
          if ((*pdEntry & PAGE_2MB) == PAGE_2MB)
            continue;

          for (uint64_t l = 0; l < 512; l++) {
            uint64_t* ptEntry = TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pdEntry), l);
            if (!(*ptEntry & (PAGE_PRESENT | PAGE_NO_ACCESS)))
              continue;

            uint64_t flags = PAGE_GET_FLAGS(ptEntry);
            physical_uintptr_t physicalAddress = PAGE_GET_PHYSICAL_ADDRESS(ptEntry);

            void* virtualAddress =
                reinterpret_cast<void*>(((i & 0x100) ? (~0ULL << 48) : 0ULL) | /* Sign-extension. */
                                        (i << 39) | (j << 30) | (k << 21) | (l << 12));

            if (flags & PAGE_SHARED) {
              // The physical address is now referenced (shared) in
              // two address spaces, so make sure we hold another
              // reference on it. Otherwise, if one of the two
              // address spaces frees the page, the other may still
              // refer to the bad page (and eventually double-free).
              if (!(flags & PAGE_BORROWED)) {
                PhysicalMemoryManager::instance().pin(physicalAddress);
              }

              // Handle shared mappings - don't copy the original
              // page.
              pClone->mapUnlocked(physicalAddress, virtualAddress, fromFlags(flags, true),
                                  mutation);
              if (mutation.failed()) {
                mutation.panicInvalidationFailure();
              }
              continue;
            }

            // Map the new page in to the new address space for
            // copy-on-write. This implies read-only (so we #PF for copy
            // on write).
            bool bWasCopyOnWrite = (flags & PAGE_COPY_ON_WRITE);
            if (copyOnWrite) {
              if (!(flags & (PAGE_WRITE | PAGE_COPY_ON_WRITE))) {
                flags |= PAGE_WRITE_PROTECTED;
              }
              flags |= PAGE_COPY_ON_WRITE;
              flags &= ~PAGE_WRITE;
            }
            pClone->mapUnlocked(physicalAddress, virtualAddress, fromFlags(flags, true), mutation);
            if (mutation.failed()) {
              mutation.panicInvalidationFailure();
            }

            // We need to modify the entry in *this* address space as
            // well to also have the read-only and copy-on-write flag
            // set, as otherwise writes in the parent process will cause
            // the child process to see those changes immediately. Note:
            // changes only needed if we're setting copy-on-write as
            // otherwise the flags are unchanged in the parent space.
            if (copyOnWrite) {
              PAGE_SET_FLAGS(ptEntry, flags);
              if (!invalidateMapping(virtualAddress, mutation)) {
                mutation.panicInvalidationFailure();
              }
            }

            // Pin the page twice - once for each side of the clone.
            // But only pin for the parent if the parent page is not
            // already copy on write. If we pin the CoW page, it'll be
            // leaked when both parent and child terminate if the parent
            // clone()s again.
            if (!bWasCopyOnWrite)
              PhysicalMemoryManager::instance().pin(physicalAddress);
            PhysicalMemoryManager::instance().pin(physicalAddress);
          }
        }
      }
    }

    // Before returning the address space, bring across metadata.
    // Note though that if the parent of the clone (ie, this address space)
    // is the kernel address space, we mustn't copy metadata or else the
    // userspace defaults in the constructor get wiped out.

    if (m_Heap < KERNEL_SPACE_START) {
      pClone->m_Heap = m_Heap;
      pClone->m_HeapEnd = m_HeapEnd;
    }
  }

  // Now we pick up the stacks lock, so we can copy safely. However, we don't
  // have the VirtualAddressSpace lock, so we can still safely use the heap
  // without worrying about re-entering.
  LockGuard<Spinlock> cloneStacksGuard(pClone->m_StacksLock);
  m_StacksLock.acquire();

  if (m_pStackTop < KERNEL_SPACE_START) {
    pClone->m_pStackTop = m_pStackTop;
    for (Vector<Stack*>::Iterator it = m_freeStacks.begin(); it != m_freeStacks.end(); ++it) {
      Stack* pNewStack = new Stack(**it);
      pClone->m_freeStacks.pushBack(pNewStack);
    }
  }

  m_StacksLock.release();

  return pClone;
}

void X64VirtualAddressSpace::revertToKernelAddressSpace() {
  X64MappingMutationScope mutation;
  mutation.lock(m_Lock);

  // The userspace area is only the bottom half of the address space - the top
  // 256 PML4 entries are for the kernel, and these should be mapped anyway.
  for (uint64_t i = 0; i < 256; i++) {
    uint64_t* pml4Entry = TABLE_ENTRY(m_PhysicalPML4, i);
    if ((*pml4Entry & PAGE_PRESENT) != PAGE_PRESENT)
      continue;

    for (uint64_t j = 0; j < 512; j++) {
      uint64_t* pdptEntry = TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pml4Entry), j);
      if ((*pdptEntry & PAGE_PRESENT) != PAGE_PRESENT)
        continue;

      void* pdptVirtualAddress =
          reinterpret_cast<void*>(((i & 0x100) ? (~0ULL << 48) : 0ULL) | (i << 39) | (j << 30));

      for (uint64_t k = 0; k < 512; k++) {
        uint64_t* pdEntry = TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pdptEntry), k);
        if ((*pdEntry & PAGE_PRESENT) != PAGE_PRESENT)
          continue;

        // Address this region begins at.
        void* regionVirtualAddress =
            reinterpret_cast<void*>(((i & 0x100) ? (~0ULL << 48) : 0ULL) | /* Sign-extension. */
                                    (i << 39) | (j << 30) | (k << 21));

        if (regionVirtualAddress < USERSPACE_VIRTUAL_START)
          continue;
        if (regionVirtualAddress > KERNEL_SPACE_START)
          break;

        /// \todo Deal with 2MB pages here.
        if ((*pdEntry & PAGE_2MB) == PAGE_2MB)
          continue;

        for (uint64_t l = 0; l < 512; l++) {
          uint64_t* ptEntry = TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pdEntry), l);
          if (!(*ptEntry & (PAGE_PRESENT | PAGE_NO_ACCESS)))
            continue;

          void* virtualAddress = reinterpret_cast<void*>(
              reinterpret_cast<uintptr_t>(regionVirtualAddress) | (l << 12));

          size_t flags = PAGE_GET_FLAGS(ptEntry);
          physical_uintptr_t physicalAddress = PAGE_GET_PHYSICAL_ADDRESS(ptEntry);

          // Release the physical memory if it is not shared with
          // another process (eg, memory mapped file) Also avoid
          // stumbling over a swapped out page.
          /// \todo When swap system comes along, we want to remove
          /// this page
          ///       from swap!
          const bool releasePhysicalPage =
              (flags & (PAGE_SHARED | PAGE_SWAPPED | PAGE_BORROWED)) == 0;

          // Free the page.
          trackPages(-1, 0, 0);
          *ptEntry = 0;
          if (!invalidateMapping(virtualAddress, mutation)) {
            mutation.panicInvalidationFailure();
          }
          if (releasePhysicalPage) {
            PhysicalMemoryManager::instance().freePage(physicalAddress);
          }
        }

        // Remove the table.
        const physical_uintptr_t pageTable = PAGE_GET_PHYSICAL_ADDRESS(pdEntry);
        *pdEntry = 0;
        if (!invalidateMapping(regionVirtualAddress, mutation)) {
          mutation.panicInvalidationFailure();
        }
        PhysicalMemoryManager::instance().freePage(pageTable);
      }

      const physical_uintptr_t pageDirectory = PAGE_GET_PHYSICAL_ADDRESS(pdptEntry);
      *pdptEntry = 0;
      if (!invalidateMapping(pdptVirtualAddress, mutation)) {
        mutation.panicInvalidationFailure();
      }
      PhysicalMemoryManager::instance().freePage(pageDirectory);
    }

    const physical_uintptr_t pageDirectoryPointerTable = PAGE_GET_PHYSICAL_ADDRESS(pml4Entry);
    *pml4Entry = 0;
    void* pml4VirtualAddress =
        reinterpret_cast<void*>(((i & 0x100) ? (~0ULL << 48) : 0ULL) | (i << 39));
    if (!invalidateMapping(pml4VirtualAddress, mutation)) {
      mutation.panicInvalidationFailure();
    }
    PhysicalMemoryManager::instance().freePage(pageDirectoryPointerTable);
  }

  // Reset heap; it's been wiped out by this reversion.
  m_HeapEnd = m_Heap;
}

bool X64VirtualAddressSpace::mapPageStructures(physical_uintptr_t physAddress, void* virtualAddress,
                                               size_t flags) {
  // PageStack capacity is fully populated before AP startup. Keeping these
  // special no-shootdown mappings bootstrap-only makes that lifetime
  // invariant executable if a future caller tries to expand it at runtime.
  if (Processor::m_Initialised == 2) {
    panic("PageStack paging structures cannot expand after processor startup");
  }
  LockGuard<Spinlock> guard(m_Lock);

  size_t Flags = toFlags(flags);
  size_t pml4Index = PML4_INDEX(virtualAddress);
  uint64_t* pml4Entry = TABLE_ENTRY(m_PhysicalPML4, pml4Index);

  // Is a page directory pointer table present?
  if (conditionalTableEntryMapping(pml4Entry, physAddress, Flags) == true)
    return true;

  size_t pageDirectoryPointerIndex = PAGE_DIRECTORY_POINTER_INDEX(virtualAddress);
  uint64_t* pageDirectoryPointerEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pml4Entry), pageDirectoryPointerIndex);

  // Is a page directory present?
  if (conditionalTableEntryMapping(pageDirectoryPointerEntry, physAddress, Flags) == true)
    return true;

  size_t pageDirectoryIndex = PAGE_DIRECTORY_INDEX(virtualAddress);
  uint64_t* pageDirectoryEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryPointerEntry), pageDirectoryIndex);

  // Is a page table present?
  if (conditionalTableEntryMapping(pageDirectoryEntry, physAddress, Flags) == true)
    return true;

  size_t pageTableIndex = PAGE_TABLE_INDEX(virtualAddress);
  uint64_t* pageTableEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryEntry), pageTableIndex);

  // Is a page already present?
  if ((*pageTableEntry & PAGE_PRESENT) != PAGE_PRESENT) {
    *pageTableEntry = physAddress | flags;
    return true;
  }
  return false;
}

bool X64VirtualAddressSpace::mapPageStructuresAbove4GB(physical_uintptr_t physAddress,
                                                       void* virtualAddress, size_t flags) {
  if (Processor::m_Initialised == 2) {
    panic("PageStack paging structures cannot expand after processor startup");
  }
  LockGuard<Spinlock> guard(m_Lock);

  size_t Flags = toFlags(flags);
  size_t pml4Index = PML4_INDEX(virtualAddress);
  uint64_t* pml4Entry = TABLE_ENTRY(m_PhysicalPML4, pml4Index);

  // Is a page directory pointer table present?
  if (conditionalTableEntryAllocation(pml4Entry, Flags) == false)
    return true;

  size_t pageDirectoryPointerIndex = PAGE_DIRECTORY_POINTER_INDEX(virtualAddress);
  uint64_t* pageDirectoryPointerEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pml4Entry), pageDirectoryPointerIndex);

  // Is a page directory present?
  if (conditionalTableEntryAllocation(pageDirectoryPointerEntry, Flags) == false)
    return true;

  size_t pageDirectoryIndex = PAGE_DIRECTORY_INDEX(virtualAddress);
  uint64_t* pageDirectoryEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryPointerEntry), pageDirectoryIndex);

  // Is a page table present?
  if (conditionalTableEntryAllocation(pageDirectoryEntry, Flags) == false)
    return true;

  size_t pageTableIndex = PAGE_TABLE_INDEX(virtualAddress);
  uint64_t* pageTableEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryEntry), pageTableIndex);

  // Is a page already present?
  if ((*pageTableEntry & PAGE_PRESENT) != PAGE_PRESENT) {
    *pageTableEntry = physAddress | flags;
    return true;
  }
  return false;
}

size_t X64VirtualAddressSpace::runtimeMappingPages(uintptr_t base, size_t length) {
  if (length > ~uintptr_t(0) - base)
    return 0;
  LockGuard<Spinlock> guard(m_Lock);
  const uintptr_t end = base + length;
  size_t count = 0;
  for (uintptr_t address = base; address < end;) {
    uint64_t table = m_PhysicalPML4;
    size_t missingShift = 0;
    for (size_t shift = 39; shift > 12; shift -= 9) {
      const uint64_t entry = *TABLE_ENTRY(table, (address >> shift) & 511);
      if (!(entry & PAGE_PRESENT) || (entry & PAGE_2MB)) {
        missingShift = shift;
        break;
      }
      table = entry & ~0x8780000000000FFFULL;
    }
    if (missingShift) {
      const uintptr_t next = ((address >> missingShift) + 1) << missingShift;
      if (next <= address)
        break;
      address = next;
      continue;
    }
    const uint64_t leaf = *TABLE_ENTRY(table, (address >> 12) & 511);
    if ((leaf & PAGE_RUNTIME) && (leaf & (PAGE_PRESENT | PAGE_NO_ACCESS)))
      ++count;
    address += PhysicalMemoryManager::getPageSize();
  }
  return count;
}

VirtualAddressSpace::Stack* X64VirtualAddressSpace::allocateStack() {
  size_t sz = USERSPACE_VIRTUAL_STACK_SIZE;
  if (this == &m_KernelSpace)
    sz = KERNEL_STACK_SIZE;
  return doAllocateStack(sz);
}

VirtualAddressSpace::Stack* X64VirtualAddressSpace::allocateStack(size_t stackSz) {
  if (stackSz == 0)
    return allocateStack();
  return doAllocateStack(stackSz);
}

VirtualAddressSpace::Stack* X64VirtualAddressSpace::doAllocateStack(size_t sSize) {
  if (this != &m_KernelSpace && Processor::getInterrupts())
    return allocateTrackedUserStack(sSize);
  // Native generic events retain their existing IRQ-phase fallback. Such an
  // image cannot advertise complete CURRENT/FUTURE memory-lock coverage.
  if (this != &m_KernelSpace && rawUserMemory().completeInventory())
    FATAL("Unprepared user stack in a complete memory-lock inventory");
  size_t flags = 0;
  bool bMapAll = false;
  if (this == &m_KernelSpace) {
    // Don't demand map kernel mode stacks.
    flags = VirtualAddressSpace::KernelMode;
    bMapAll = true;
  }

  const size_t pageSz = PhysicalMemoryManager::getPageSize();

  // Grab a new stack pointer. Use the list of freed stacks if we can,
  // otherwise adjust the internal stack pointer. Using the list of freed
  // stacks helps avoid having the virtual address creep downwards.
  void* pStack = 0;
  m_StacksLock.acquire();
  if (m_freeStacks.count() != 0) {
    Stack* poppedStack = m_freeStacks.popBack();
    if (poppedStack->getSize() >= sSize) {
      pStack = poppedStack->getTop();
    }
    delete poppedStack;
  }
  m_StacksLock.release();

  if (!pStack) {
    // Need the main address space lock now so we can adjust the next stack
    // pointer without interference.
    m_Lock.acquire();
    pStack = m_pStackTop;

    // Always leave one page unmapped between each stack to catch overflow.
    m_pStackTop = adjust_pointer(m_pStackTop, -(sSize + pageSz));
    m_Lock.release();
  }

  // Map the top of the stack in proper.
  uintptr_t firstPage = reinterpret_cast<uintptr_t>(pStack) - pageSz;
  physical_uintptr_t phys = PhysicalMemoryManager::instance().allocatePage();
  if (!bMapAll)
    PhysicalMemoryManager::instance().pin(phys);
  if (!map(phys, reinterpret_cast<void*>(firstPage), flags | VirtualAddressSpace::Write))
    WARNING("map() failed in doAllocateStack");

  // Bring in the rest of the stack as CoW.
  uintptr_t stackBottom = reinterpret_cast<uintptr_t>(pStack) - sSize;
  for (uintptr_t addr = stackBottom; addr < firstPage; addr += pageSz) {
    size_t map_flags = 0;

    if (!bMapAll) {
      // Copy first stack page on write.
      PhysicalMemoryManager::instance().pin(phys);
      map_flags = VirtualAddressSpace::CopyOnWrite;
    } else {
      phys = PhysicalMemoryManager::instance().allocatePage();
      map_flags = VirtualAddressSpace::Write;
    }

    if (!map(phys, reinterpret_cast<void*>(addr), flags | map_flags))
      WARNING("CoW map() failed in doAllocateStack");
  }

  Stack* stackInfo = new Stack(pStack, sSize);
  return stackInfo;
}

VirtualAddressSpace::Stack* X64VirtualAddressSpace::allocateTrackedUserStack(size_t size) {
  const size_t page = PhysicalMemoryManager::getPageSize();
  if (!size || size > ~size_t(0) - (page - 1))
    return nullptr;
  size = (size + page - 1) & ~(page - 1);
  UserMemoryOperation operation(*this);
  const uint64_t id = rawUserMemory().nextRegionId();
  if (!id)
    return nullptr;
  // The operation gate serializes user stack allocation; kernel stacks use
  // their separate address space and retain the scheduler-safe path above.
  Stack* reusable = nullptr;
  {
    LockGuard<Spinlock> guard(m_StacksLock);
    if (m_freeStacks.count() && m_freeStacks[m_freeStacks.count() - 1]->getSize() == size)
      reusable = m_freeStacks.popBack();
  }
  if (reusable && userMemoryPolicy() &&
      userMemoryPolicy()->overlapsManagedMemory(
          *this, reinterpret_cast<uintptr_t>(reusable->getBase()), size)) {
    delete reusable;
    reusable = nullptr;
  }
  void* top = reusable ? reusable->getTop() : m_pStackTop;
  const uintptr_t topValue = reinterpret_cast<uintptr_t>(top);
  if (topValue < size + page || topValue - size < getUserStart()) {
    if (reusable) {
      LockGuard<Spinlock> guard(m_StacksLock);
      m_freeStacks.pushBack(reusable);
    }
    return nullptr;
  }
  Stack* stack = new Stack(top, size, id);
  if (!stack) {
    if (reusable) {
      LockGuard<Spinlock> guard(m_StacksLock);
      m_freeStacks.pushBack(reusable);
    }
    return nullptr;
  }
  const uintptr_t base = topValue - size;
  UserRegion region{id, base, size, UserRegion::Kind::Stack, true};
  UniquePointer<PreparedMemoryLock> plan;
  MemoryLockCharge charge;
  bool ready = rawUserMemory().prepareChange(nullptr, &region, plan) == MemoryLockStatus::Success;
  ready = ready && admitRawMemoryChange(*plan.get(), operation.privileged(), charge) &&
          prepareZeroPage();
  size_t mapped = 0;
  if (ready) {
    for (; mapped < size; mapped += page) {
      PhysicalMemoryManager::instance().pin(m_ZeroPage);
      if (!map(m_ZeroPage, reinterpret_cast<void*>(base + mapped), CopyOnWrite)) {
        PhysicalMemoryManager::instance().freePage(m_ZeroPage);
        ready = false;
        break;
      }
    }
    // Eager admission breaks private CoW before publishing the new region.
    if (ready)
      ready = plan.get()->populate() == PopulationStatus::Success;
  }
  if (!ready) {
    for (size_t offset = 0; offset < mapped; offset += page) {
      physical_uintptr_t physical = 0;
      size_t flags = 0;
      if (detachMapping(reinterpret_cast<void*>(base + offset), physical, flags))
        PhysicalMemoryManager::instance().freePage(physical);
    }
    delete stack;
    if (reusable) {
      LockGuard<Spinlock> guard(m_StacksLock);
      m_freeStacks.pushBack(reusable);
    }
    return nullptr;
  }
  commitRawMemoryChange(*plan.get(), charge);
  if (!reusable)
    m_pStackTop = reinterpret_cast<void*>(base - page);
  delete reusable;
  return stack;
}

void X64VirtualAddressSpace::freeStack(Stack* pStack) {
  if (!pStack)
    return;
  if (pStack->regionId()) {
    UserMemoryOperation operation(*this);
    const size_t removed = rawUserMemory().retireRegion(pStack->regionId());
    if (MemoryLockAccount* account = memoryLockAccount()) {
      MemoryLockCharge charge = account->charge();
      assert(removed <= charge.rawPages);
      charge.rawPages -= removed;
      account->publish(charge, account->futureMode());
    }
    LockGuard<Spinlock> guard(m_StacksLock);
    m_freeStacks.pushBack(pStack);
    return;
  }
  const size_t pageSz = PhysicalMemoryManager::getPageSize();

  // Clean up the stack
  uintptr_t stackTop = reinterpret_cast<uintptr_t>(pStack->getTop());
  for (size_t i = 0; i < pStack->getSize(); i += pageSz) {
    stackTop -= pageSz;
    void* v = reinterpret_cast<void*>(stackTop);
    if (!isMapped(v)) {
      continue;
    }

    size_t flags = 0;
    physical_uintptr_t phys = 0;
    getMapping(v, phys, flags);

    unmap(v);
    PhysicalMemoryManager::instance().freePage(phys);
  }

  // Add the stack to the list; using the stacks lock, not the main address
  // space lock, as pushing could require mapping pages via the heap.
  m_StacksLock.acquire();
  m_freeStacks.pushBack(pStack);
  m_StacksLock.release();
}

X64VirtualAddressSpace::~X64VirtualAddressSpace() {
  PhysicalMemoryManager& physicalMemoryManager = PhysicalMemoryManager::instance();

  /// \todo validate that we're cleaning up enough stuff here
  /// for example are we missing stuff like PDPTs etc?

  // Drop back to the kernel address space. This will blow away the child's
  // mappings, but maintains shared pages as needed.
  revertToKernelAddressSpace();

  // Free the PageMapLevel4
  physicalMemoryManager.freePage(m_PhysicalPML4);
}

X64VirtualAddressSpace::X64VirtualAddressSpace()
    : VirtualAddressSpace(USERSPACE_VIRTUAL_HEAP),
      m_PhysicalPML4(0),
      m_pStackTop(USERSPACE_VIRTUAL_STACK),
      m_freeStacks(),
      m_bKernelSpace(false),
      m_Lock(false, false),
      m_StacksLock(false) {
  // Allocate a new PageMapLevel4
  PhysicalMemoryManager& physicalMemoryManager = PhysicalMemoryManager::instance();
  m_PhysicalPML4 = physicalMemoryManager.allocatePage();

  // Initialise the page directory
  ByteSet(reinterpret_cast<void*>(physicalAddress(m_PhysicalPML4)), 0, 0x800);

  // Copy the kernel PageMapLevel4
  MemoryCopy(reinterpret_cast<void*>(physicalAddress(m_PhysicalPML4) + 0x800),
             reinterpret_cast<void*>(physicalAddress(m_KernelSpace.m_PhysicalPML4) + 0x800), 0x800);
}

X64VirtualAddressSpace::X64VirtualAddressSpace(void* Heap, physical_uintptr_t PhysicalPML4,
                                               void* VirtualStack)
    : VirtualAddressSpace(Heap),
      m_PhysicalPML4(PhysicalPML4),
      m_pStackTop(VirtualStack),
      m_freeStacks(),
      m_bKernelSpace(true),
      m_Lock(false, false),
      m_StacksLock(false) {}

bool X64VirtualAddressSpace::getPageTableEntry(void* virtualAddress,
                                               uint64_t*& pageTableEntry) const {
  size_t pml4Index = PML4_INDEX(virtualAddress);
  uint64_t* pml4Entry = TABLE_ENTRY(m_PhysicalPML4, pml4Index);

  // Is a page directory pointer table present?
  if ((*pml4Entry & PAGE_PRESENT) != PAGE_PRESENT)
    return false;

  size_t pageDirectoryPointerIndex = PAGE_DIRECTORY_POINTER_INDEX(virtualAddress);
  uint64_t* pageDirectoryPointerEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pml4Entry), pageDirectoryPointerIndex);

  // Is a page directory present?
  if ((*pageDirectoryPointerEntry & PAGE_PRESENT) != PAGE_PRESENT)
    return false;

  size_t pageDirectoryIndex = PAGE_DIRECTORY_INDEX(virtualAddress);
  uint64_t* pageDirectoryEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryPointerEntry), pageDirectoryIndex);

  // Is a page table or 2MB page present?
  if ((*pageDirectoryEntry & PAGE_PRESENT) != PAGE_PRESENT)
    return false;
  if ((*pageDirectoryEntry & PAGE_2MB) == PAGE_2MB)
    return false;

  size_t pageTableIndex = PAGE_TABLE_INDEX(virtualAddress);
  pageTableEntry = TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryEntry), pageTableIndex);

  // Is a page present?
  if ((*pageTableEntry & PAGE_PRESENT) != PAGE_PRESENT &&
      (*pageTableEntry & PAGE_SWAPPED) != PAGE_SWAPPED && !(*pageTableEntry & PAGE_NO_ACCESS))
    return false;

  return true;
}

bool X64VirtualAddressSpace::invalidateMapping(void* virtualAddress,
                                               X64MappingMutationScope& mutation) {
  // Upper-half mappings are shared by every address space. Lower-half
  // mappings can also be active on more than one processor, and no residency
  // mask currently identifies a narrower destination set. Use the same
  // address-specific barrier conservatively for both.
  return mutation.invalidate(virtualAddress);
}

size_t X64VirtualAddressSpace::detachEmptyTables(void* virtualAddress,
                                                 physical_uintptr_t* detachedTables) {
  const size_t pml4Index = PML4_INDEX(virtualAddress);
  uint64_t* pml4Entry = TABLE_ENTRY(m_PhysicalPML4, pml4Index);
  if ((*pml4Entry & PAGE_PRESENT) != PAGE_PRESENT) {
    return 0;
  }

  const size_t pageDirectoryPointerIndex = PAGE_DIRECTORY_POINTER_INDEX(virtualAddress);
  uint64_t* pageDirectoryPointerEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pml4Entry), pageDirectoryPointerIndex);
  if ((*pageDirectoryPointerEntry & PAGE_PRESENT) != PAGE_PRESENT) {
    return 0;
  }

  const size_t pageDirectoryIndex = PAGE_DIRECTORY_INDEX(virtualAddress);
  uint64_t* pageDirectoryEntry =
      TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryPointerEntry), pageDirectoryIndex);
  if ((*pageDirectoryEntry & PAGE_PRESENT) != PAGE_PRESENT ||
      (*pageDirectoryEntry & PAGE_2MB) == PAGE_2MB) {
    return 0;
  }

  for (size_t i = 0; i < 0x200; ++i) {
    uint64_t* entry = TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryEntry), i);
    if (*entry & (PAGE_PRESENT | PAGE_SWAPPED | PAGE_NO_ACCESS)) {
      return 0;
    }
  }

  size_t detachedCount = 0;
  detachedTables[detachedCount++] = PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryEntry);
  *pageDirectoryEntry = 0;

  for (size_t i = 0; i < 0x200; ++i) {
    uint64_t* entry = TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryPointerEntry), i);
    if ((*entry & PAGE_PRESENT) == PAGE_PRESENT) {
      return detachedCount;
    }
  }

  detachedTables[detachedCount++] = PAGE_GET_PHYSICAL_ADDRESS(pageDirectoryPointerEntry);
  *pageDirectoryPointerEntry = 0;

  // Every process PML4 contains its own copy of the upper-half entry. Retain
  // the shared PDPT root so clearing one PML4 cannot leave the others pointing
  // at freed storage.
  if (reinterpret_cast<uintptr_t>(virtualAddress) >=
      reinterpret_cast<uintptr_t>(KERNEL_SPACE_START)) {
    return detachedCount;
  }

  for (size_t i = 0; i < 0x200; ++i) {
    uint64_t* entry = TABLE_ENTRY(PAGE_GET_PHYSICAL_ADDRESS(pml4Entry), i);
    if ((*entry & PAGE_PRESENT) == PAGE_PRESENT) {
      return detachedCount;
    }
  }

  detachedTables[detachedCount++] = PAGE_GET_PHYSICAL_ADDRESS(pml4Entry);
  *pml4Entry = 0;
  return detachedCount;
}

uint64_t X64VirtualAddressSpace::toFlags(size_t flags, bool bFinal) const {
  uint64_t Flags = 0;
  if ((flags & KernelMode) == KernelMode)
    Flags |= PAGE_GLOBAL;
  else
    Flags |= PAGE_USER;
  if ((flags & Write) == Write)
    Flags |= PAGE_WRITE;
  if ((flags & WriteCombine) == WriteCombine)
    Flags |= PAGE_WRITE_COMBINE;
  if ((flags & CacheDisable) == CacheDisable)
    Flags |= PAGE_CACHE_DISABLE;
  if ((flags & Execute) != Execute)
    Flags |= PAGE_NX;
  if ((flags & Swapped) == Swapped)
    Flags |= PAGE_SWAPPED;
  else
    Flags |= PAGE_PRESENT;
  if (flags & Borrowed) {
    Flags |= PAGE_BORROWED;
  }
  if (flags & RuntimeMapping)
    Flags |= PAGE_RUNTIME;
  if (flags & NoAccess) {
    Flags &= ~PAGE_PRESENT;
    Flags |= PAGE_NO_ACCESS;
  }
  if (flags & WriteProtected) {
    Flags &= ~PAGE_WRITE;
    Flags |= PAGE_WRITE_PROTECTED;
  }
  if ((flags & CopyOnWrite) == CopyOnWrite)
    Flags |= PAGE_COPY_ON_WRITE;
  if ((flags & Shared) == Shared)
    Flags |= PAGE_SHARED;
  if (bFinal) {
    if ((flags & WriteThrough) == WriteThrough)
      Flags |= PAGE_WRITE_THROUGH;
    if ((flags & Accessed) == Accessed)
      Flags |= PAGE_ACCESSED;
    if ((flags & Dirty) == Dirty)
      Flags |= PAGE_DIRTY;
    if ((flags & ClearDirty) == ClearDirty)
      Flags &= ~PAGE_DIRTY;
  }
  return Flags;
}

size_t X64VirtualAddressSpace::fromFlags(uint64_t Flags, bool bFinal) const {
  size_t flags = 0;
  if ((Flags & PAGE_USER) != PAGE_USER)
    flags |= KernelMode;
  if ((Flags & PAGE_WRITE) == PAGE_WRITE)
    flags |= Write;
  if ((Flags & PAGE_WRITE_COMBINE) == PAGE_WRITE_COMBINE)
    flags |= WriteCombine;
  if ((Flags & PAGE_CACHE_DISABLE) == PAGE_CACHE_DISABLE)
    flags |= CacheDisable;
  if ((Flags & PAGE_NX) != PAGE_NX)
    flags |= Execute;
  if ((Flags & PAGE_SWAPPED) == PAGE_SWAPPED)
    flags |= Swapped;
  if (Flags & PAGE_BORROWED)
    flags |= Borrowed;
  if (Flags & PAGE_RUNTIME)
    flags |= RuntimeMapping;
  if (Flags & PAGE_NO_ACCESS)
    flags |= NoAccess;
  if (Flags & PAGE_WRITE_PROTECTED)
    flags |= WriteProtected;
  if ((Flags & PAGE_COPY_ON_WRITE) == PAGE_COPY_ON_WRITE)
    flags |= CopyOnWrite;
  if ((Flags & PAGE_SHARED) == PAGE_SHARED)
    flags |= Shared;
  if (bFinal) {
    if ((Flags & PAGE_WRITE_THROUGH) == PAGE_WRITE_THROUGH)
      flags |= WriteThrough;
    if ((Flags & PAGE_ACCESSED) == PAGE_ACCESSED)
      flags |= Accessed;
    if ((Flags & PAGE_DIRTY) == PAGE_DIRTY)
      flags |= Dirty;
  }
  return flags;
}

bool X64VirtualAddressSpace::conditionalTableEntryAllocation(uint64_t* tableEntry, uint64_t flags) {
  // Convert VirtualAddressSpace::* flags to X64 flags.
  flags = toFlags(flags);

  if ((*tableEntry & PAGE_PRESENT) != PAGE_PRESENT) {
    // Allocate a page
    PhysicalMemoryManager& PMemoryManager = PhysicalMemoryManager::instance();
    uint64_t page = PMemoryManager.allocatePage();
    if (page == 0) {
      ERROR(
          "OOM in "
          "X64VirtualAddressSpace::conditionalTableEntryAllocation!");
      return false;
    }

    // Add the WRITE and USER flags so that these can be controlled
    // on a page-granularity level.
    flags &= ~(PAGE_GLOBAL | PAGE_NX | PAGE_SWAPPED | PAGE_COPY_ON_WRITE | PAGE_NO_ACCESS |
               PAGE_WRITE_PROTECTED | PAGE_BORROWED);
    flags |= PAGE_WRITE | PAGE_USER | PAGE_PRESENT;

    // Map the page.
    *tableEntry = page | flags;

    // Zero the page directory pointer table.
    ByteSet(physicalAddress(reinterpret_cast<void*>(page)), 0,
            PhysicalMemoryManager::getPageSize());
  } else if (((*tableEntry & PAGE_USER) != PAGE_USER) && (flags & PAGE_USER)) {
    // Flags request user mapping, entry doesn't have that.
    *tableEntry |= PAGE_USER;
  }

  return true;
}

bool X64VirtualAddressSpace::conditionalTableEntryMapping(uint64_t* tableEntry,
                                                          uint64_t physAddress, uint64_t flags) {
  // Convert VirtualAddressSpace::* flags to X64 flags.
  flags = toFlags(flags, true);

  if ((*tableEntry & PAGE_PRESENT) != PAGE_PRESENT) {
    // Map the page. Add the WRITE and USER flags so that these can be
    // controlled on a page-granularity level.
    *tableEntry =
        physAddress | ((flags & ~(PAGE_GLOBAL | PAGE_NX | PAGE_SWAPPED | PAGE_COPY_ON_WRITE |
                                  PAGE_NO_ACCESS | PAGE_WRITE_PROTECTED | PAGE_BORROWED)) |
                       PAGE_WRITE | PAGE_USER | PAGE_PRESENT);

    // Zero the page directory pointer table
    ByteSet(physicalAddress(reinterpret_cast<void*>(physAddress)), 0,
            PhysicalMemoryManager::getPageSize());

    return true;
  } else if (((*tableEntry & PAGE_USER) != PAGE_USER) && (flags & PAGE_USER)) {
    // Flags request user mapping, entry doesn't have that.
    *tableEntry |= PAGE_USER;
  }

  return false;
}
