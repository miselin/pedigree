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
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/utility.h"

#include <dlfcn.h>
#include <errno.h>

#include "PhysicalMemoryManager.h"
#include <sys/mman.h>

VirtualAddressSpace* g_pCurrentlyCloning = 0;

HostedVirtualAddressSpace HostedVirtualAddressSpace::m_KernelSpace(KERNEL_VIRTUAL_HEAP,
                                                                   KERNEL_VIRTUAL_STACK);

typedef void* (*malloc_t)(size_t);
typedef void* (*realloc_t)(void*, size_t);
typedef void (*free_t)(void*);

#include <stdio.h>
void* __libc_malloc(size_t n) {
  static malloc_t local = (malloc_t)dlsym(RTLD_NEXT, "malloc");
  return local(n);
}

void* __libc_realloc(void* p, size_t n) {
  static realloc_t local = (realloc_t)dlsym(RTLD_NEXT, "realloc");
  return local(p, n);
}

void __libc_free(void* p) {
  static free_t local = (free_t)dlsym(RTLD_NEXT, "free");
  local(p);
}

VirtualAddressSpace& VirtualAddressSpace::getKernelAddressSpace() {
  return HostedVirtualAddressSpace::m_KernelSpace;
}

VirtualAddressSpace* VirtualAddressSpace::create() {
  return new HostedVirtualAddressSpace();
}

bool HostedVirtualAddressSpace::memIsInHeap(void* pMem) {
  if (pMem < m_Heap)
    return false;
  else if (pMem >= getEndOfHeap())
    return false;
  else
    return true;
}

bool HostedVirtualAddressSpace::memIsInKernelHeap(void* pMem) {
  if (pMem < KERNEL_VIRTUAL_HEAP)
    return false;
  else if (pMem >= adjust_pointer(KERNEL_VIRTUAL_HEAP, KERNEL_VIRTUAL_HEAP_SIZE))
    return false;
  else
    return true;
}

void* HostedVirtualAddressSpace::getEndOfHeap() {
  return adjust_pointer(KERNEL_VIRTUAL_HEAP, KERNEL_VIRTUAL_HEAP_SIZE);
}

bool HostedVirtualAddressSpace::isAddressValid(void* virtualAddress) {
  if (reinterpret_cast<uint64_t>(virtualAddress) < 0x0000800000000000ULL ||
      reinterpret_cast<uint64_t>(virtualAddress) >= 0xFFFF800000000000ULL)
    return true;
  return false;
}

bool HostedVirtualAddressSpace::isMapped(void* virtualAddress) {
  LockGuard<Spinlock> guard(m_Lock);

  virtualAddress = page_align(virtualAddress);

  int r = msync(virtualAddress, PhysicalMemoryManager::getPageSize(), MS_ASYNC);
  if (r < 0) {
    if (errno == ENOMEM) {
      return false;
    }
  }

  if (this != &getKernelAddressSpace()) {
    bool r = getKernelAddressSpace().isMapped(virtualAddress);
    if (r)
      return r;
  }

  return findMapping(virtualAddress) != nullptr;
}

HostedVirtualAddressSpace::mapping_t* HostedVirtualAddressSpace::findMapping(void* address) {
  const size_t index = m_MappingIndex.lookup(reinterpret_cast<uintptr_t>(address));
  return index == HostedMappingIndex::Missing ? nullptr : &m_pKnownMaps[index];
}

bool HostedVirtualAddressSpace::map(physical_uintptr_t physAddress, void* virtualAddress,
                                    size_t flags) {
  virtualAddress = page_align(virtualAddress);

  // If this should be a kernel mapping, use the kernel address space.
  if (this != &getKernelAddressSpace())
    if ((virtualAddress >= KERNEL_SPACE_START) || (flags & KernelMode))
      return getKernelAddressSpace().map(physAddress, virtualAddress, flags);

  // mmap() won't fail if the address is already mapped, but we need to.
  if (isMapped(virtualAddress)) {
    ERROR("HostedVirtualAddressSpace::map refused an existing mapping at "
          << Hex << reinterpret_cast<uintptr_t>(virtualAddress));
    return false;
  }

  LockGuard<Spinlock> guard(m_Lock);

  // An inactive address space can retain a record without a host mapping.
  // Reject that duplicate before MAP_FIXED could replace another space's page.
  if (findMapping(virtualAddress) || !m_MappingIndex.reserveForInsert())
    return false;

  // Extend list of known maps if we can't fit this one in.
  if (m_numKnownMaps == m_KnownMapsSize) {
    const size_t oldSize = m_KnownMapsSize;
    if (oldSize > (~size_t(0) / sizeof(mapping_t)) / 2)
      return false;
    const size_t count = oldSize ? oldSize * 2 : 2;
    auto* maps = static_cast<mapping_t*>(__libc_realloc(m_pKnownMaps, count * sizeof(mapping_t)));
    if (!maps)
      return false;
    m_pKnownMaps = maps;
    m_KnownMapsSize = count;

    // Mark all inactive.
    for (size_t i = oldSize; i < m_KnownMapsSize; ++i)
      m_pKnownMaps[i].active = false;
  }

  // Register in the list of known mappings.
  bool bRegistered = false;
  size_t idx = m_nLastUnmap;
  for (; idx < m_KnownMapsSize; ++idx) {
    if (m_pKnownMaps[idx].active)
      continue;

    bRegistered = true;
    break;
  }
  if (!bRegistered) {
    // Try again from the beginning.
    for (idx = 0; idx < m_nLastUnmap; ++idx) {
      if (m_pKnownMaps[idx].active)
        continue;

      bRegistered = true;
      break;
    }
  }

  if (!bRegistered)
    panic("Fatal algorithmic error in HostedVirtualAddressSpace::map");

  // Map, backed onto the "physical memory" of the system.
  int prot = toFlags(flags, true);
  void* r = mmap(virtualAddress, PhysicalMemoryManager::getPageSize(), prot, MAP_FIXED | MAP_SHARED,
                 HostedPhysicalMemoryManager::instance().getBackingFile(), physAddress);

  if (UNLIKELY(r == MAP_FAILED)) {
    ERROR("HostedVirtualAddressSpace::map failed at "
          << Hex << reinterpret_cast<uintptr_t>(virtualAddress) << " (errno " << Dec << errno
          << ")");
    return false;
  }

  assert(r == virtualAddress);

  m_pKnownMaps[idx].active = true;
  m_pKnownMaps[idx].vaddr = virtualAddress;
  m_pKnownMaps[idx].paddr = physAddress;
  m_pKnownMaps[idx].flags = flags;
  m_MappingIndex.insert(reinterpret_cast<uintptr_t>(virtualAddress), idx);

  ++m_numKnownMaps;

  return true;
}

void HostedVirtualAddressSpace::getMapping(void* virtualAddress, physical_uintptr_t& physAddress,
                                           size_t& flags) {
  LockGuard<Spinlock> guard(m_Lock);

  virtualAddress = page_align(virtualAddress);

  // Handle kernel mappings, if needed.
  if (this != &getKernelAddressSpace()) {
    if (getKernelAddressSpace().isMapped(virtualAddress)) {
      getKernelAddressSpace().getMapping(virtualAddress, physAddress, flags);
      return;
    }
  }

  size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t alignedVirtualAddress = reinterpret_cast<uintptr_t>(virtualAddress) & ~(pageSize - 1);
  virtualAddress = reinterpret_cast<void*>(alignedVirtualAddress);

  if (const mapping_t* mapping = findMapping(virtualAddress)) {
    physAddress = mapping->paddr;
    flags = fromFlags(mapping->flags, true);
    return;
  }

  ERROR("HostedVirtualAddressSpace::getMapping has no mapping for "
        << Hex << reinterpret_cast<uintptr_t>(virtualAddress) << " (caller "
        << reinterpret_cast<uintptr_t>(__builtin_return_address(0)) << ")");
  panic("HostedVirtualAddressSpace::getMapping - function misused");
}

bool HostedVirtualAddressSpace::handleCopyOnWriteFault(void* virtualAddress, bool userMode) {
  virtualAddress = page_align(virtualAddress);

  if (this != &getKernelAddressSpace() && getKernelAddressSpace().isMapped(virtualAddress)) {
    return getKernelAddressSpace().handleCopyOnWriteFault(virtualAddress, userMode);
  }

  {
    LockGuard<Spinlock> guard(m_Lock);
    mapping_t* mapping = findMapping(virtualAddress);

    if (!mapping) {
      return false;
    }
    if ((userMode && (mapping->flags & KernelMode)) ||
        (mapping->flags & (NoAccess | WriteProtected))) {
      return false;
    }
    if ((mapping->flags & Write) && !(mapping->flags & CopyOnWrite)) {
      return true;
    }
    if (!(mapping->flags & CopyOnWrite) || (mapping->flags & Swapped)) {
      return false;
    }
  }

  PhysicalMemoryManager& physicalMemory = PhysicalMemoryManager::instance();
  const physical_uintptr_t replacement = physicalMemory.allocatePage();
  if (!replacement) {
    return false;
  }

#if PEDIGREE_HOSTED_SMOKE_TESTS
  copyOnWritePreCommitForTest(virtualAddress);
#endif

  bool retireReplacement = true;
  bool resolved = false;
  bool publicationFailed = false;
  physical_uintptr_t oldPhysical = 0;
  {
    LockGuard<Spinlock> guard(m_Lock);
    mapping_t* mapping = findMapping(virtualAddress);

    if (mapping && ((userMode && (mapping->flags & KernelMode)) ||
                    (mapping->flags & (NoAccess | WriteProtected)))) {
      resolved = false;
    } else if (mapping && (mapping->flags & Write) && !(mapping->flags & CopyOnWrite)) {
      resolved = true;
    } else if (mapping && (mapping->flags & CopyOnWrite) && !(mapping->flags & Swapped)) {
      const size_t pageSize = PhysicalMemoryManager::getPageSize();
      void* sourceAlias =
          mmap(nullptr, pageSize, PROT_READ, MAP_SHARED,
               HostedPhysicalMemoryManager::instance().getBackingFile(), mapping->paddr);
      if (sourceAlias != MAP_FAILED) {
        void* replacementAlias =
            mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                 HostedPhysicalMemoryManager::instance().getBackingFile(), replacement);
        if (replacementAlias != MAP_FAILED) {
          MemoryCopy(replacementAlias, sourceAlias, pageSize);

          const size_t replacementFlags =
              (mapping->flags | VirtualAddressSpace::Write) &
              ~(VirtualAddressSpace::CopyOnWrite | VirtualAddressSpace::Borrowed |
                VirtualAddressSpace::Shared);
          void* published = mmap(
              virtualAddress, pageSize, toFlags(replacementFlags, true), MAP_FIXED | MAP_SHARED,
              HostedPhysicalMemoryManager::instance().getBackingFile(), replacement);
          if (published == virtualAddress) {
            oldPhysical = mapping->paddr;
            mapping->paddr = replacement;
            mapping->flags = replacementFlags;
            retireReplacement = false;
            resolved = true;
          } else {
            ERROR("HostedVirtualAddressSpace::handleCopyOnWriteFault failed to publish at "
                  << Hex << reinterpret_cast<uintptr_t>(virtualAddress) << " (errno " << Dec
                  << errno << ")");
            publicationFailed = true;
          }
          munmap(replacementAlias, pageSize);
        }
        munmap(sourceAlias, pageSize);
      }
    }
  }

  if (publicationFailed) {
    physicalMemory.freePage(replacement);
    panic("Hosted copy-on-write publication lost the original host mapping");
  }

  if (retireReplacement) {
    physicalMemory.freePage(replacement);
  }
  if (oldPhysical) {
    physicalMemory.freePage(oldPhysical);
  }
  return resolved;
}

bool HostedVirtualAddressSpace::tryReadUser32(uintptr_t address, uint32_t& value) {
  uintptr_t word = 0;
  if (!tryAccessUserWord(address, sizeof(value), word, nullptr)) {
    return false;
  }
  value = static_cast<uint32_t>(word);
  return true;
}

bool HostedVirtualAddressSpace::tryReadUserPointer(uintptr_t address, uintptr_t& value) {
  return tryAccessUserWord(address, sizeof(value), value, nullptr);
}

bool HostedVirtualAddressSpace::tryCompareExchangeUser32(uintptr_t address, uint32_t& expected,
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

bool HostedVirtualAddressSpace::tryAccessUserWord(uintptr_t address, size_t width, uintptr_t& value,
                                                  const uintptr_t* replacement) {
  if (!address || (address % width) || address < getUserStart() || address >= getKernelStart() ||
      address > getKernelStart() - width) {
    return false;
  }

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t pageOffset = address & (pageSize - 1);
  if (pageOffset > pageSize - width) {
    return false;
  }

  LockGuard<Spinlock> guard(m_Lock);
  const uintptr_t pageAddress = address - pageOffset;
  if (const mapping_t* record = findMapping(reinterpret_cast<void*>(pageAddress))) {
    const mapping_t& mapping = *record;
    if ((mapping.flags & (KernelMode | Swapped | NoAccess)) ||
        (replacement &&
         (!(mapping.flags & Write) || (mapping.flags & (CopyOnWrite | WriteProtected))))) {
      return false;
    }

    void* alias = mmap(nullptr, pageSize, PROT_READ | (replacement ? PROT_WRITE : 0), MAP_SHARED,
                       HostedPhysicalMemoryManager::instance().getBackingFile(), mapping.paddr);
    if (alias == MAP_FAILED) {
      return false;
    }
    void* target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(alias) + pageOffset);
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
    return munmap(alias, pageSize) == 0;
  }
  return false;
}

bool HostedVirtualAddressSpace::tryWriteUser32(uintptr_t address, uint32_t value) {
  if (!address || (address % alignof(uint32_t)) || address < getUserStart() ||
      address >= getKernelStart() || address > getKernelStart() - sizeof(value)) {
    return false;
  }

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const uintptr_t pageAddress = address & ~(pageSize - 1);
  const size_t pageOffset = address - pageAddress;
  if (pageOffset > pageSize - sizeof(value)) {
    return false;
  }

  LockGuard<Spinlock> guard(m_Lock);
  if (const mapping_t* record = findMapping(reinterpret_cast<void*>(pageAddress))) {
    const mapping_t& mapping = *record;

    if (!(mapping.flags & Write) ||
        (mapping.flags & (KernelMode | CopyOnWrite | Swapped | NoAccess | WriteProtected))) {
      return false;
    }

    void* alias = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                       HostedPhysicalMemoryManager::instance().getBackingFile(), mapping.paddr);
    if (alias == MAP_FAILED) {
      return false;
    }

    __atomic_store_n(reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(alias) + pageOffset),
                     value, __ATOMIC_RELEASE);
    return munmap(alias, pageSize) == 0;
  }

  return false;
}

void HostedVirtualAddressSpace::setFlags(void* virtualAddress, size_t newFlags) {
  LockGuard<Spinlock> guard(m_Lock);

  virtualAddress = page_align(virtualAddress);

  // Check for kernel mappings.
  if (this != &getKernelAddressSpace()) {
    if (getKernelAddressSpace().isMapped(virtualAddress)) {
      getKernelAddressSpace().setFlags(virtualAddress, newFlags);
      return;
    } else if (newFlags & KernelMode)
      WARNING(
          "setFlags called with KernelMode as a flag, page is not "
          "mapped in kernel.");
  }

  if (mapping_t* mapping = findMapping(virtualAddress)) {
    mapping->flags = newFlags;
  }

  size_t flags = toFlags(newFlags, true);
  if (mprotect(virtualAddress, PhysicalMemoryManager::getPageSize(), flags) != 0) {
    FATAL("HostedVirtualAddressSpace::setFlags failed with errno " << Dec << errno);
  }
}

bool HostedVirtualAddressSpace::tryMapUserPage(physical_uintptr_t physical, void* address,
                                               size_t flags, size_t* committedTablePages) {
  if (committedTablePages)
    *committedTablePages = 0;
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (!physical || ((physical | value) & (pageSize - 1)) || value < getUserStart() ||
      value >= getKernelStart() ||
      (flags & (KernelMode | Swapped | Borrowed | Shared | CopyOnWrite)))
    return false;
  LockGuard<Spinlock> guard(m_Lock);
  if (findMapping(address) || !m_MappingIndex.reserveForInsert())
    return false;
  size_t index = m_KnownMapsSize;
  for (size_t i = 0; i < m_KnownMapsSize; ++i) {
    if (!m_pKnownMaps[i].active) {
      index = i;
      break;
    }
  }
  if (index == m_KnownMapsSize) {
    if (m_KnownMapsSize > (~size_t(0) / sizeof(mapping_t)) / 2)
      return false;
    const size_t count = m_KnownMapsSize ? m_KnownMapsSize * 2 : 1;
    auto* maps = static_cast<mapping_t*>(__libc_realloc(m_pKnownMaps, count * sizeof(mapping_t)));
    if (!maps)
      return false;
    m_pKnownMaps = maps;
    for (size_t i = m_KnownMapsSize; i < count; ++i)
      maps[i].active = false;
    m_KnownMapsSize = count;
  }
  if (this == &Processor::information().getVirtualAddressSpace() &&
      mmap(address, pageSize, toFlags(flags, true), MAP_FIXED | MAP_SHARED,
           HostedPhysicalMemoryManager::instance().getBackingFile(), physical) == MAP_FAILED)
    return false;
  auto& mapping = m_pKnownMaps[index];
  mapping.active = true;
  mapping.vaddr = address;
  mapping.paddr = physical;
  mapping.flags = flags;
  m_MappingIndex.insert(value, index);
  ++m_numKnownMaps;
  return true;
}
bool HostedVirtualAddressSpace::tryDetachUserPage(void* address, physical_uintptr_t expected) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if ((value & (pageSize - 1)) || value < getUserStart() || value >= getKernelStart())
    return false;
  LockGuard<Spinlock> guard(m_Lock);
  const size_t i = m_MappingIndex.lookup(value);
  if (i != HostedMappingIndex::Missing) {
    auto& mapping = m_pKnownMaps[i];
    if (mapping.paddr != expected || !(mapping.flags & NoAccess) || (mapping.flags & Swapped))
      return false;
    if (this == &Processor::information().getVirtualAddressSpace() && munmap(address, pageSize))
      return false;
    m_MappingIndex.erase(reinterpret_cast<uintptr_t>(mapping.vaddr));
    mapping.active = false;
    m_nLastUnmap = i;
    // Legacy unmap leaves this count conservative; preserve that convention
    // rather than undercounting older inactive records.
    return true;
  }
  return false;
}

bool HostedVirtualAddressSpace::trySetFlags(void* virtualAddress, size_t newFlags) {
  virtualAddress = page_align(virtualAddress);
  if (this != &getKernelAddressSpace() && getKernelAddressSpace().isMapped(virtualAddress)) {
    return getKernelAddressSpace().trySetFlags(virtualAddress, newFlags);
  }

  LockGuard<Spinlock> guard(m_Lock);
  if (mapping_t* mapping = findMapping(virtualAddress)) {
    if (mprotect(virtualAddress, PhysicalMemoryManager::getPageSize(), toFlags(newFlags, true)) !=
        0) {
      return false;
    }
    mapping->flags = newFlags;
    return true;
  }
  return false;
}

void HostedVirtualAddressSpace::unmap(void* virtualAddress) {
  LockGuard<Spinlock> guard(m_Lock);

  virtualAddress = page_align(virtualAddress);

  // Check for kernel mappings.
  if (this != &getKernelAddressSpace()) {
    if (getKernelAddressSpace().isMapped(virtualAddress)) {
      getKernelAddressSpace().unmap(virtualAddress);
      return;
    }
  }

  const size_t i = m_MappingIndex.lookup(reinterpret_cast<uintptr_t>(virtualAddress));
  if (i != HostedMappingIndex::Missing) {
    m_MappingIndex.erase(reinterpret_cast<uintptr_t>(virtualAddress));
    m_pKnownMaps[i].active = false;
    m_nLastUnmap = i;
  }

  if (munmap(virtualAddress, PhysicalMemoryManager::getPageSize()) != 0) {
    FATAL("HostedVirtualAddressSpace::unmap failed with errno " << Dec << errno);
  }
}

bool HostedVirtualAddressSpace::detachMapping(void* virtualAddress, physical_uintptr_t& physical,
                                              size_t& flags, size_t requiredFlags) {
  LockGuard<Spinlock> guard(m_Lock);
  virtualAddress = page_align(virtualAddress);
  physical = 0;
  flags = 0;
  if (this != &getKernelAddressSpace() && getKernelAddressSpace().isMapped(virtualAddress)) {
    return getKernelAddressSpace().detachMapping(virtualAddress, physical, flags, requiredFlags);
  }
  const size_t i = m_MappingIndex.lookup(reinterpret_cast<uintptr_t>(virtualAddress));
  if (i != HostedMappingIndex::Missing) {
    mapping_t& mapping = m_pKnownMaps[i];
    physical = mapping.paddr;
    flags = mapping.flags;
    if ((flags & requiredFlags) != requiredFlags) {
      return false;
    }
    if ((this == &Processor::information().getVirtualAddressSpace() ||
         this == &getKernelAddressSpace()) &&
        munmap(virtualAddress, PhysicalMemoryManager::getPageSize()) != 0) {
      FATAL("HostedVirtualAddressSpace::detachMapping failed with errno " << Dec << errno);
    }
    m_MappingIndex.erase(reinterpret_cast<uintptr_t>(mapping.vaddr));
    mapping.active = false;
    m_nLastUnmap = i;
    return true;
  }
  return false;
}

VirtualAddressSpace* HostedVirtualAddressSpace::clone(bool copyOnWrite) {
  UserMemoryOperation operation(*this);
  HostedVirtualAddressSpace* pNew =
      static_cast<HostedVirtualAddressSpace*>(VirtualAddressSpace::create());
  if (!pNew)
    return nullptr;
  if (rawUserMemory().cloneInto(pNew->rawUserMemory()) != MemoryLockStatus::Success) {
    delete pNew;
    return nullptr;
  }
  pNew->m_HeapRegionId = m_HeapRegionId;

  {
    LockGuard<Spinlock> guard(m_Lock);

    // Copy over the known maps so the new address space can find them.
    if (m_KnownMapsSize) {
      pNew->m_pKnownMaps = (mapping_t*)__libc_malloc(m_KnownMapsSize * sizeof(mapping_t));
      if (!pNew->m_pKnownMaps) {
        guard.disown();
        m_Lock.release();
        delete pNew;
        return nullptr;
      }
      MemoryCopy(pNew->m_pKnownMaps, m_pKnownMaps, m_KnownMapsSize * sizeof(mapping_t));
    }
    pNew->m_KnownMapsSize = m_KnownMapsSize;
    pNew->m_numKnownMaps = m_numKnownMaps;
    pNew->m_nLastUnmap = m_nLastUnmap;

    // Complete fallible index allocation before changing page ownership or
    // protecting the source. Deletion must be outside the mapping lock.
    for (size_t i = 0; i < pNew->m_KnownMapsSize; ++i) {
      if (!pNew->m_pKnownMaps[i].active)
        continue;
      if (!pNew->m_MappingIndex.reserveForInsert()) {
        guard.disown();
        m_Lock.release();
        delete pNew;
        return nullptr;
      }
      pNew->m_MappingIndex.insert(reinterpret_cast<uintptr_t>(pNew->m_pKnownMaps[i].vaddr), i);
    }

    VirtualAddressSpace& kernelSpace = getKernelAddressSpace();
    const bool sourceIsCurrent = this == &Processor::information().getVirtualAddressSpace();

    // Readjust flags on both sides of a private userspace clone. Kernel and
    // explicitly shared mappings remain shared. clone(false) leaves existing
    // flags intact, so only an already-writable page becomes a writable alias.
    for (size_t i = 0; i < pNew->m_KnownMapsSize; ++i) {
      mapping_t* sourceMapping = &m_pKnownMaps[i];
      mapping_t* cloneMapping = &pNew->m_pKnownMaps[i];
      if (!cloneMapping->active)
        continue;

      // Kernel mappings are process-global in the hosted build and are
      // already found through getKernelAddressSpace(). Keeping duplicate
      // entries here would pin pages that revertToKernelAddressSpace
      // intentionally does not release.
      const bool kernelOwnedMapping = this == &kernelSpace || (cloneMapping->flags & KernelMode) ||
                                      kernelSpace.isMapped(cloneMapping->vaddr);
      if (kernelOwnedMapping) {
        pNew->m_MappingIndex.erase(reinterpret_cast<uintptr_t>(cloneMapping->vaddr));
        cloneMapping->active = false;
        --pNew->m_numKnownMaps;
        continue;
      }

      if (cloneMapping->flags & Borrowed) {
        continue;
      }
      PhysicalMemoryManager::instance().pin(cloneMapping->paddr);

      if (cloneMapping->flags & Shared) {
        continue;
      }

      if (!(cloneMapping->flags & CopyOnWrite))
        PhysicalMemoryManager::instance().pin(cloneMapping->paddr);

      const bool privateUserMapping = cloneMapping->vaddr < KERNEL_SPACE_START;
      if (!copyOnWrite || !privateUserMapping) {
        continue;
      }

      size_t cloneFlags = (cloneMapping->flags | CopyOnWrite) & ~Write;
      if (!(cloneMapping->flags & (Write | CopyOnWrite))) {
        cloneFlags |= WriteProtected;
      }
      if (sourceIsCurrent && mprotect(sourceMapping->vaddr, PhysicalMemoryManager::getPageSize(),
                                      toFlags(cloneFlags, true)) != 0) {
        FATAL(
            "HostedVirtualAddressSpace::clone failed to protect source "
            "mapping at "
            << Hex << reinterpret_cast<uintptr_t>(sourceMapping->vaddr) << " (errno " << Dec
            << errno << ")");
      }

      sourceMapping->flags = cloneFlags;
      cloneMapping->flags = cloneFlags;
    }
  }

  {
    // Stack metadata can allocate from the kernel heap, which may need to
    // re-enter the address-space mapping lock.
    LockGuard<Spinlock> stacksGuard(m_StacksLock);
    if (m_pStackTop < KERNEL_SPACE_START) {
      pNew->m_pStackTop = m_pStackTop;
      for (Vector<Stack*>::Iterator it = m_freeStacks.begin(); it != m_freeStacks.end(); ++it) {
        Stack* pNewStack = new Stack(**it);
        pNew->m_freeStacks.pushBack(pNewStack);
      }
    }
  }

  if (m_Heap < KERNEL_SPACE_START) {
    pNew->m_Heap = m_Heap;
    pNew->m_HeapEnd = m_HeapEnd;
    NOTICE("clone: heap=" << m_Heap << " end=" << m_HeapEnd);
  }

  return pNew;
}

void HostedVirtualAddressSpace::revertToKernelAddressSpace() {
  LockGuard<Spinlock> guard(m_Lock);

  for (size_t i = 0; i < m_KnownMapsSize; ++i) {
    if (m_pKnownMaps[i].active) {
      if (getKernelAddressSpace().isMapped(m_pKnownMaps[i].vaddr)) {
        m_MappingIndex.erase(reinterpret_cast<uintptr_t>(m_pKnownMaps[i].vaddr));
        m_pKnownMaps[i].active = false;
        m_nLastUnmap = i;
        continue;
      } else if (m_pKnownMaps[i].vaddr > KERNEL_SPACE_START)
        continue;

      if (munmap(m_pKnownMaps[i].vaddr, PhysicalMemoryManager::getPageSize()) != 0) {
        FATAL("HostedVirtualAddressSpace::revertToKernelAddressSpace failed with errno " << Dec
                                                                                         << errno);
      }

      // Clean up references to physical memory as needed.
      if ((m_pKnownMaps[i].flags & (Shared | Swapped | Borrowed)) == 0)
        PhysicalMemoryManager::instance().freePage(m_pKnownMaps[i].paddr);

      m_MappingIndex.erase(reinterpret_cast<uintptr_t>(m_pKnownMaps[i].vaddr));
      m_pKnownMaps[i].active = false;
    }
  }
}

VirtualAddressSpace::Stack* HostedVirtualAddressSpace::allocateStack() {
  size_t sz = USERSPACE_VIRTUAL_STACK_SIZE;
  if (this == &getKernelAddressSpace())
    sz = KERNEL_STACK_SIZE;
  return doAllocateStack(sz);
}

VirtualAddressSpace::Stack* HostedVirtualAddressSpace::allocateStack(size_t stackSz) {
  if (stackSz == 0)
    return allocateStack();
  return doAllocateStack(stackSz);
}

VirtualAddressSpace::Stack* HostedVirtualAddressSpace::doAllocateStack(size_t sSize) {
  size_t flags = 0;
  bool bMapAll = true;
  if (this == &m_KernelSpace) {
    // Don't demand map kernel mode stacks.
    flags = VirtualAddressSpace::KernelMode;
    bMapAll = true;
  }

  size_t pageSz = PhysicalMemoryManager::getPageSize();

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
    m_Lock.acquire();
    pStack = m_pStackTop;

    // Always leave one page unmapped between each stack to catch overflow.
    m_pStackTop = adjust_pointer(m_pStackTop, -static_cast<ssize_t>(sSize + pageSz));
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

void HostedVirtualAddressSpace::freeStack(Stack* pStack) {
  size_t pageSz = PhysicalMemoryManager::getPageSize();

  // Clean up the stack
  uintptr_t stackTop = reinterpret_cast<uintptr_t>(pStack->getTop());
  for (size_t i = 0; i < pStack->getSize(); i += pageSz) {
    stackTop -= pageSz;
    void* v = reinterpret_cast<void*>(stackTop);
    if (!isMapped(v))
      break;  // Hit end of stack.

    size_t flags = 0;
    physical_uintptr_t phys = 0;
    getMapping(v, phys, flags);

    unmap(v);
    PhysicalMemoryManager::instance().freePage(phys);
  }

  // Keep heap growth out of the address-space mapping lock: Slam may need
  // to map a new slab while the vector grows.
  m_StacksLock.acquire();
  m_freeStacks.pushBack(pStack);
  m_StacksLock.release();
}

HostedVirtualAddressSpace::~HostedVirtualAddressSpace() {
  __libc_free(m_pKnownMaps);
}

HostedVirtualAddressSpace::HostedVirtualAddressSpace()
    : VirtualAddressSpace(USERSPACE_VIRTUAL_HEAP),
      m_pStackTop(USERSPACE_VIRTUAL_STACK),
      m_freeStacks(),
      m_bKernelSpace(false),
      m_Lock(false, true),
      m_StacksLock(false, true),
      m_pKnownMaps(0),
      m_MappingIndex(),
      m_KnownMapsSize(0),
      m_numKnownMaps(0),
      m_nLastUnmap(0) {}

HostedVirtualAddressSpace::HostedVirtualAddressSpace(void* Heap, void* VirtualStack)
    : VirtualAddressSpace(Heap),
      m_pStackTop(VirtualStack),
      m_freeStacks(),
      m_bKernelSpace(true),
      m_Lock(false, true),
      m_StacksLock(false, true),
      m_pKnownMaps(0),
      m_MappingIndex(),
      m_KnownMapsSize(0),
      m_numKnownMaps(0),
      m_nLastUnmap(0) {}

uint64_t HostedVirtualAddressSpace::toFlags(size_t flags, bool bFinal) {
  if (flags & NoAccess) {
    return PROT_NONE;
  }
  uint64_t Flags = 0;
  if ((flags & Write) && !(flags & WriteProtected))
    Flags |= PROT_WRITE;
  if (flags & Swapped)
    Flags |= PROT_NONE;
  else
    Flags |= PROT_READ;
  if (flags & Execute)
    Flags |= PROT_EXEC;
  return Flags;
}

size_t HostedVirtualAddressSpace::fromFlags(uint64_t Flags, bool bFinal) {
  return Flags;
}

void HostedVirtualAddressSpace::switchAddressSpace(VirtualAddressSpace& a, VirtualAddressSpace& b) {
  HostedVirtualAddressSpace& oldSpace = static_cast<HostedVirtualAddressSpace&>(a);
  HostedVirtualAddressSpace& newSpace = static_cast<HostedVirtualAddressSpace&>(b);

  if (&oldSpace != &getKernelAddressSpace()) {
    for (size_t i = 0; i < oldSpace.m_KnownMapsSize; ++i) {
      if (oldSpace.m_pKnownMaps[i].active) {
        if (getKernelAddressSpace().isMapped(oldSpace.m_pKnownMaps[i].vaddr)) {
          continue;
        } else if (oldSpace.m_pKnownMaps[i].flags & KernelMode) {
          continue;
        }

        if (munmap(oldSpace.m_pKnownMaps[i].vaddr, PhysicalMemoryManager::getPageSize()) != 0) {
          FATAL("HostedVirtualAddressSpace::switchAddressSpace unmap failed with errno " << Dec
                                                                                         << errno);
        }
      }
    }
  }

  // Kernel mappings remain installed across every hosted address space. Looking
  // each one up in the kernel's own table is a quadratic no-op.
  if (&newSpace == &getKernelAddressSpace())
    return;

  for (size_t i = 0; i < newSpace.m_KnownMapsSize; ++i) {
    if (newSpace.m_pKnownMaps[i].active) {
      if (getKernelAddressSpace().isMapped(newSpace.m_pKnownMaps[i].vaddr)) {
        continue;
      }

      void* mapped = mmap(
          newSpace.m_pKnownMaps[i].vaddr, PhysicalMemoryManager::getPageSize(),
          newSpace.toFlags(newSpace.m_pKnownMaps[i].flags, true), MAP_FIXED | MAP_SHARED,
          HostedPhysicalMemoryManager::instance().getBackingFile(), newSpace.m_pKnownMaps[i].paddr);
      if (mapped == MAP_FAILED || mapped != newSpace.m_pKnownMaps[i].vaddr) {
        FATAL("HostedVirtualAddressSpace::switchAddressSpace map failed with errno " << Dec
                                                                                     << errno);
      }
    }
  }
}
