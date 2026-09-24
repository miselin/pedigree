#include "PhysicalMemoryManager.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/MemoryRegion.h"

#include <string.h>

#include "AddressLayout.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

EXPORTED_PUBLIC size_t g_FreePages = 0;
EXPORTED_PUBLIC size_t g_AllocedPages = 0;

namespace {
constexpr uint64_t MaximumPhysicalAddress = 0x80000000ULL;
}  // namespace

Armv7PhysicalMemoryManager::Armv7PhysicalMemoryManager()
    : m_References(nullptr),
      m_FreeBitmap(nullptr),
      m_MaxPage(0),
      m_NextPage(0),
      m_TotalPages(0),
      m_FreePages(0),
      m_NextRegion(KERNEL_VIRTUAL_MEMORYREGION_ADDRESS) {}

Armv7PhysicalMemoryManager& Armv7PhysicalMemoryManager::instance() {
  static Armv7PhysicalMemoryManager manager;
  return manager;
}

PhysicalMemoryManager& PhysicalMemoryManager::instance() {
  return Armv7PhysicalMemoryManager::instance();
}

void Armv7PhysicalMemoryManager::initialise(const BootstrapStruct_t& info) {
  uint64_t highest = 0;
  for (void* entry = info.getMemoryMap(); entry; entry = info.nextMemoryMapEntry(entry)) {
    if (info.getMemoryMapEntryType(entry) != 1) {
      continue;
    }
    uint64_t start = info.getMemoryMapEntryAddress(entry);
    uint64_t length = info.getMemoryMapEntryLength(entry);
    if (start >= MaximumPhysicalAddress || length > MaximumPhysicalAddress - start) {
      length = start < MaximumPhysicalAddress ? MaximumPhysicalAddress - start : 0;
    }
    if (start + length > highest) {
      highest = start + length;
    }
  }
  m_MaxPage = highest / PAGE_SIZE;
  if (!m_MaxPage) {
    panic("ARMv7: no usable RAM");
  }

  const size_t referenceBytes = m_MaxPage * sizeof(uint16_t);
  const size_t bitmapBytes = (m_MaxPage + 7) / 8;
  const size_t metadataBytes = (referenceBytes + bitmapBytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  physical_uintptr_t metadata = 0;
  for (void* entry = info.getMemoryMap(); entry; entry = info.nextMemoryMapEntry(entry)) {
    if (info.getMemoryMapEntryType(entry) != 1) {
      continue;
    }
    uint64_t start = (info.getMemoryMapEntryAddress(entry) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t end = info.getMemoryMapEntryAddress(entry) + info.getMemoryMapEntryLength(entry);
    if (end > MaximumPhysicalAddress) {
      end = MaximumPhysicalAddress;
    }
    if (start < end && end - start >= metadataBytes) {
      metadata = start;
      break;
    }
  }
  if (!metadata) {
    panic("ARMv7: cannot reserve page metadata");
  }

  m_References = reinterpret_cast<uint16_t*>(ARMV7_DIRECT_MAP_BASE + metadata);
  m_FreeBitmap = reinterpret_cast<uint8_t*>(m_References + m_MaxPage);
  memset(m_References, 0, metadataBytes);

  for (void* entry = info.getMemoryMap(); entry; entry = info.nextMemoryMapEntry(entry)) {
    if (info.getMemoryMapEntryType(entry) != 1) {
      continue;
    }
    uint64_t start = info.getMemoryMapEntryAddress(entry);
    uint64_t end = start + info.getMemoryMapEntryLength(entry);
    if (end > MaximumPhysicalAddress) {
      end = MaximumPhysicalAddress;
    }
    start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    for (uint64_t address = start; address < end; address += PAGE_SIZE) {
      if (address >= metadata && address < metadata + metadataBytes) {
        continue;
      }
      markPage(address / PAGE_SIZE, true);
      ++m_TotalPages;
      ++m_FreePages;
    }
  }
  m_NextPage = metadata / PAGE_SIZE + metadataBytes / PAGE_SIZE;
  g_FreePages = m_FreePages;
  g_AllocedPages = 0;
}

bool Armv7PhysicalMemoryManager::pageAvailable(size_t page) const {
  return page < m_MaxPage && (m_FreeBitmap[page / 8] & (1U << (page % 8)));
}

void Armv7PhysicalMemoryManager::markPage(size_t page, bool available) {
  uint8_t& bits = m_FreeBitmap[page / 8];
  if (available) {
    bits |= 1U << (page % 8);
  } else {
    bits &= ~(1U << (page % 8));
  }
}

size_t Armv7PhysicalMemoryManager::pageLimit(size_t constraints) const {
  size_t limit = m_MaxPage;
  if (constraints & below1MB) {
    limit = limit < (1U << 8) ? limit : (1U << 8);
  } else if (constraints & below16MB) {
    limit = limit < (1U << 12) ? limit : (1U << 12);
  } else if (constraints & below4GB) {
    limit = limit < (1U << 20) ? limit : (1U << 20);
  }
  return limit;
}

physical_uintptr_t Armv7PhysicalMemoryManager::allocatePageUnlocked(size_t constraints) {
  const size_t limit = pageLimit(constraints);
  if (!limit) {
    return 0;
  }

  size_t first = m_NextPage < limit ? m_NextPage : 0;
  for (size_t pass = 0; pass < 2; ++pass) {
    const size_t end = pass ? first : limit;
    for (size_t page = pass ? 0 : first; page < end; ++page) {
      if (!pageAvailable(page)) {
        continue;
      }
      markPage(page, false);
      m_References[page] = 1;
      --m_FreePages;
      --g_FreePages;
      ++g_AllocedPages;
      m_NextPage = page + 1;
      return page * PAGE_SIZE;
    }
  }
  return 0;
}

physical_uintptr_t Armv7PhysicalMemoryManager::allocateContinuousPagesUnlocked(size_t pages,
                                                                               size_t constraints) {
  const size_t limit = pageLimit(constraints);
  if (!pages || pages > limit) {
    return 0;
  }
  for (size_t first = 1; first <= limit - pages; ++first) {
    size_t count = 0;
    while (count < pages && pageAvailable(first + count)) {
      ++count;
    }
    if (count != pages) {
      first += count;
      continue;
    }
    for (size_t page = first; page < first + pages; ++page) {
      markPage(page, false);
      m_References[page] = 1;
    }
    m_FreePages -= pages;
    g_FreePages -= pages;
    g_AllocedPages += pages;
    m_NextPage = first + pages;
    return first * PAGE_SIZE;
  }
  return 0;
}

physical_uintptr_t Armv7PhysicalMemoryManager::allocatePage(size_t constraints) {
  LockGuard<Spinlock> guard(m_Lock);
  physical_uintptr_t page = allocatePageUnlocked(constraints);
  if (!page) {
    panic("ARMv7: out of physical pages");
  }
  return page;
}

physical_uintptr_t Armv7PhysicalMemoryManager::tryAllocatePage() {
  LockGuard<Spinlock> guard(m_Lock);
  return allocatePageUnlocked(0);
}

physical_uintptr_t Armv7PhysicalMemoryManager::allocateAlignedPages(size_t pages) {
  if (!pages || pages > m_MaxPage) {
    return 0;
  }
  LockGuard<Spinlock> guard(m_Lock);
  const size_t startingPage = ((m_NextPage + pages - 1) / pages) * pages;
  for (size_t pass = 0; pass < 2; ++pass) {
    const size_t begin = pass ? pages : startingPage;
    const size_t end = pass ? startingPage : m_MaxPage - pages + 1;
    for (size_t first = begin; first < end; first += pages) {
      size_t count = 0;
      while (count < pages && pageAvailable(first + count)) {
        ++count;
      }
      if (count != pages) {
        continue;
      }
      for (size_t page = first; page < first + pages; ++page) {
        markPage(page, false);
        m_References[page] = 1;
      }
      m_FreePages -= pages;
      g_FreePages -= pages;
      g_AllocedPages += pages;
      m_NextPage = first + pages;
      return first * PAGE_SIZE;
    }
  }
  return 0;
}

void Armv7PhysicalMemoryManager::freePageUnlocked(physical_uintptr_t page) {
  const size_t index = page / PAGE_SIZE;
  if (!page || (page & (PAGE_SIZE - 1)) || index >= m_MaxPage || !m_References[index]) {
    panic("ARMv7: invalid physical page free");
  }
  if (--m_References[index]) {
    return;
  }
  markPage(index, true);
  ++m_FreePages;
  ++g_FreePages;
  --g_AllocedPages;
  if (index < m_NextPage) {
    m_NextPage = index;
  }
}

void Armv7PhysicalMemoryManager::freePage(physical_uintptr_t page) {
  LockGuard<Spinlock> guard(m_Lock);
  freePageUnlocked(page);
}

void Armv7PhysicalMemoryManager::pin(physical_uintptr_t page) {
  LockGuard<Spinlock> guard(m_Lock);
  const size_t index = page / PAGE_SIZE;
  if (index >= m_MaxPage || !m_References[index] || m_References[index] == 0xffff) {
    panic("ARMv7: invalid physical page pin");
  }
  ++m_References[index];
}

bool Armv7PhysicalMemoryManager::copyPhysicalPageToBuffer(physical_uintptr_t page, void* buffer) {
  if (!page || page >= MaximumPhysicalAddress || (page & (PAGE_SIZE - 1)) || !buffer) {
    return false;
  }
  memcpy(buffer, reinterpret_cast<const void*>(ARMV7_DIRECT_MAP_BASE + page), PAGE_SIZE);
  return true;
}

bool Armv7PhysicalMemoryManager::copyPhysicalPageFromBuffer(physical_uintptr_t page,
                                                            const void* buffer) {
  if (!page || page >= MaximumPhysicalAddress || (page & (PAGE_SIZE - 1)) || !buffer) {
    return false;
  }
  memcpy(reinterpret_cast<void*>(ARMV7_DIRECT_MAP_BASE + page), buffer, PAGE_SIZE);
  return true;
}

PhysicalMemoryManager::MemorySnapshot Armv7PhysicalMemoryManager::memorySnapshot() const {
  LockGuard<Spinlock> guard(m_Lock);
  return {m_TotalPages, m_FreePages, m_MaxPage != 0};
}

size_t Armv7PhysicalMemoryManager::freePageCount() const {
  LockGuard<Spinlock> guard(m_Lock);
  return m_FreePages;
}

bool Armv7PhysicalMemoryManager::allocateRegion(MemoryRegion& region, size_t pages,
                                                size_t constraints, size_t flags,
                                                physical_uintptr_t start) {
  if (!pages || pages > (~size_t(0) / PAGE_SIZE)) {
    return false;
  }
  LockGuard<Spinlock> guard(m_RegionLock);
  const size_t bytes = pages * PAGE_SIZE;
  const uintptr_t address = (m_NextRegion + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  if (address + bytes < address || address + bytes > KERNEL_VIRTUAL_MEMORYREGION_END) {
    return false;
  }
  m_NextRegion = address + bytes;

  const bool explicitPhysical = start != static_cast<physical_uintptr_t>(-1);
  const bool virtualOnlyRegion = constraints & virtualOnly;
  if (explicitPhysical && (start & (PAGE_SIZE - 1))) {
    return false;
  }
  physical_uintptr_t continuousBase = 0;
  if (!explicitPhysical && !virtualOnlyRegion && (constraints & continuous)) {
    LockGuard<Spinlock> pagesGuard(m_Lock);
    continuousBase = allocateContinuousPagesUnlocked(pages, constraints);
    if (!continuousBase) {
      return false;
    }
  }
  VirtualAddressSpace& space = VirtualAddressSpace::getKernelAddressSpace();
  for (size_t i = 0; i < pages && !virtualOnlyRegion; ++i) {
    physical_uintptr_t physical = 0;
    if (explicitPhysical) {
      physical = start + i * PAGE_SIZE;
    } else if (continuousBase) {
      physical = continuousBase + i * PAGE_SIZE;
    } else {
      LockGuard<Spinlock> pagesGuard(m_Lock);
      physical = allocatePageUnlocked(constraints);
    }
    if (!physical ||
        !space.map(physical, reinterpret_cast<void*>(address + i * PAGE_SIZE),
                   flags | VirtualAddressSpace::KernelMode |
                       ((constraints & nonRamMemory) ? VirtualAddressSpace::CacheDisable : 0))) {
      if (!explicitPhysical && !continuousBase && physical) {
        freePage(physical);
      }
      for (size_t mapped = 0; mapped < i; ++mapped) {
        void* virtualPage = reinterpret_cast<void*>(address + mapped * PAGE_SIZE);
        physical_uintptr_t old = 0;
        size_t oldFlags = 0;
        if (space.getMapping(virtualPage, old, oldFlags)) {
          space.unmap(virtualPage);
          if (!explicitPhysical && !continuousBase) {
            freePage(old);
          }
        }
      }
      if (continuousBase) {
        for (size_t page = 0; page < pages; ++page) {
          freePage(continuousBase + page * PAGE_SIZE);
        }
      }
      return false;
    }
  }

  region.m_VirtualAddress = reinterpret_cast<void*>(address);
  region.m_PhysicalAddress = explicitPhysical ? start : continuousBase;
  region.m_Size = bytes;
  region.m_bPageBacked = !explicitPhysical;
  region.m_bNonRamMemory = explicitPhysical;
  region.m_bForced = explicitPhysical;
  region.m_bAnonymous = constraints & anonymous;
  m_MemoryRegions.pushBack(&region);
  return true;
}

void Armv7PhysicalMemoryManager::unmapRegion(MemoryRegion* region) {
  if (!region || !region->m_Size) {
    return;
  }
  LockGuard<Spinlock> guard(m_RegionLock);
  VirtualAddressSpace& space = VirtualAddressSpace::getKernelAddressSpace();
  for (size_t i = 0; i < region->m_Size; i += PAGE_SIZE) {
    void* address =
        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(region->m_VirtualAddress) + i);
    physical_uintptr_t physical = 0;
    size_t flags = 0;
    if (!space.getMapping(address, physical, flags)) {
      continue;
    }
    space.unmap(address);
    if (region->m_bPageBacked) {
      freePage(physical);
    }
  }
  for (Vector<MemoryRegion*>::Iterator it = m_MemoryRegions.begin(); it != m_MemoryRegions.end();
       ++it) {
    if (*it == region) {
      m_MemoryRegions.erase(it);
      break;
    }
  }
  region->m_Size = 0;
}
