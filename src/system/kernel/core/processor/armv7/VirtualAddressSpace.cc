#include "VirtualAddressSpace.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/Processor.h"

#include <string.h>

#include "PhysicalMemoryManager.h"

extern "C" char armv7_boot_l1;

namespace {
constexpr uint32_t PageTable = 1;
constexpr uint32_t Section = 2;
constexpr uint32_t SmallPage = 2;
constexpr uint32_t AddressMask = 0xfffff000;
constexpr uint32_t TableMask = 0xfffffc00;

uint32_t* tableAt(physical_uintptr_t physical) {
  return reinterpret_cast<uint32_t*>(ARMV7_DIRECT_MAP_BASE + physical);
}

uint32_t* shadowAt(uint32_t* table) {
  return table + 256;
}

}  // namespace

Armv7VirtualAddressSpace Armv7VirtualAddressSpace::m_KernelSpace(true);

VirtualAddressSpace& VirtualAddressSpace::getKernelAddressSpace() {
  return Armv7VirtualAddressSpace::m_KernelSpace;
}

VirtualAddressSpace* VirtualAddressSpace::create() {
  auto* space = new Armv7VirtualAddressSpace(false);
  if (!space || !space->root()) {
    delete space;
    return nullptr;
  }
  return space;
}

Armv7VirtualAddressSpace::Armv7VirtualAddressSpace(bool kernel)
    : VirtualAddressSpace(
          reinterpret_cast<void*>(kernel ? KERNEL_VIRTUAL_HEAP : USERSPACE_VIRTUAL_HEAP)),
      m_Root(kernel ? reinterpret_cast<uintptr_t>(&armv7_boot_l1)
                    : Armv7PhysicalMemoryManager::instance().allocateAlignedPages(2)),
      m_StackTop(kernel ? KERNEL_VIRTUAL_STACK : USERSPACE_VIRTUAL_STACK) {
  if (!kernel && m_Root) {
    memset(tableAt(m_Root), 0, 2 * PAGE_SIZE);
  }
}

Armv7VirtualAddressSpace::~Armv7VirtualAddressSpace() {
  if (this != &m_KernelSpace && m_Root) {
    freeUserTables();
    Armv7PhysicalMemoryManager::instance().freePage(m_Root);
    Armv7PhysicalMemoryManager::instance().freePage(m_Root + PAGE_SIZE);
  }
}

bool Armv7VirtualAddressSpace::isAddressValid(void*) {
  return true;
}

uint32_t* Armv7VirtualAddressSpace::findExistingEntry(uintptr_t address, bool* isSection) const {
  const physical_uintptr_t root = address >= ARMV7_DIRECT_MAP_BASE ? m_KernelSpace.m_Root : m_Root;
  uint32_t* first = &tableAt(root)[address >> 20];
  if (isSection) {
    *isSection = (*first & 3) == Section;
  }
  if ((*first & 3) != PageTable) {
    return first;
  }
  return &tableAt(*first & TableMask)[(address >> 12) & 255];
}

uint32_t* Armv7VirtualAddressSpace::findEntry(uintptr_t address, bool create, size_t* newTables) {
  const physical_uintptr_t root = address >= ARMV7_DIRECT_MAP_BASE ? m_KernelSpace.m_Root : m_Root;
  uint32_t* first = &tableAt(root)[address >> 20];
  if (!*first && create) {
    physical_uintptr_t page = Armv7PhysicalMemoryManager::instance().tryAllocatePage();
    if (!page) {
      return nullptr;
    }
    memset(tableAt(page), 0, PAGE_SIZE);
    *first = page | PageTable;
    if (newTables) {
      ++*newTables;
    }
  }
  if ((*first & 3) != PageTable) {
    return nullptr;
  }
  return &tableAt(*first & TableMask)[(address >> 12) & 255];
}

uint32_t Armv7VirtualAddressSpace::pageDescriptor(physical_uintptr_t physical, size_t flags) {
  const bool kernel = flags & KernelMode;
  uint32_t entry = (physical & AddressMask) | SmallPage | (1U << 4);
  if (!kernel) {
    entry |= 1U << 5;
    entry |= 1U << 11;
  }
  if (!(flags & Write) || (flags & (CopyOnWrite | WriteProtected))) {
    entry |= 1U << 9;
  }
  if (!(flags & Execute)) {
    entry |= 1U;
  }
  if (!(flags & CacheDisable)) {
    entry |= 1U << 6;  // Normal, uncached memory while the bootstrap leaves caches disabled.
  }
  if (flags & (NoAccess | Swapped)) {
    entry &= ~3U;
  }
  return entry;
}

bool Armv7VirtualAddressSpace::isMapped(void* address) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  Armv7VirtualAddressSpace& owner = value >= ARMV7_DIRECT_MAP_BASE ? m_KernelSpace : *this;
  LockGuard<Spinlock> guard(owner.m_Lock);
  bool section = false;
  uint32_t* entry = owner.findExistingEntry(value, &section);
  if (!entry || !*entry) {
    return false;
  }
  if (section) {
    return true;
  }
  const unsigned int index = (value >> 12) & 255;
  return !(shadowAt(entry - index)[index] & Swapped);
}

bool Armv7VirtualAddressSpace::map(physical_uintptr_t physical, void* address, size_t flags) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  if ((value & (PAGE_SIZE - 1)) || (physical & (PAGE_SIZE - 1)) ||
      (value < ARMV7_DIRECT_MAP_BASE && (flags & KernelMode))) {
    return false;
  }
  Armv7VirtualAddressSpace& owner = value >= ARMV7_DIRECT_MAP_BASE ? m_KernelSpace : *this;
  LockGuard<Spinlock> guard(owner.m_Lock);
  uint32_t* entry = owner.findEntry(value, true);
  if (!entry || *entry) {
    return false;
  }
  shadowAt(entry - ((value >> 12) & 255))[(value >> 12) & 255] = flags;
  *entry = pageDescriptor(physical, flags);
  Processor::invalidate(address);
  return true;
}

bool Armv7VirtualAddressSpace::tryMapUserPage(physical_uintptr_t physical, void* address,
                                              size_t flags, size_t* committedTablePages) {
  if (committedTablePages) {
    *committedTablePages = 0;
  }
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  if ((flags & KernelMode) || value >= ARMV7_DIRECT_MAP_BASE || (value & (PAGE_SIZE - 1)) ||
      (physical & (PAGE_SIZE - 1))) {
    return false;
  }
  LockGuard<Spinlock> guard(m_Lock);
  size_t newTables = 0;
  uint32_t* entry = findEntry(value, true, &newTables);
  if (!entry || *entry) {
    return false;
  }
  const unsigned int index = (value >> 12) & 255;
  shadowAt(entry - index)[index] = flags;
  *entry = pageDescriptor(physical, flags);
  Processor::invalidate(address);
  if (committedTablePages) {
    *committedTablePages = newTables;
  }
  return true;
}

bool Armv7VirtualAddressSpace::getMapping(void* address, physical_uintptr_t& physical,
                                          size_t& flags) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  Armv7VirtualAddressSpace& owner = value >= ARMV7_DIRECT_MAP_BASE ? m_KernelSpace : *this;
  LockGuard<Spinlock> guard(owner.m_Lock);
  bool section = false;
  uint32_t* entry = owner.findExistingEntry(value, &section);
  if (!entry || !*entry) {
    return false;
  }
  if (section) {
    physical = (*entry & 0xfff00000) | (value & 0xfffff);
    flags = KernelMode | Write | ((*entry & (1U << 4)) ? 0 : Execute);
  } else {
    uint32_t* table = entry - ((value >> 12) & 255);
    physical = (*entry & AddressMask) | (value & (PAGE_SIZE - 1));
    flags = shadowAt(table)[(value >> 12) & 255];
  }
  return true;
}

bool Armv7VirtualAddressSpace::trySetFlags(void* address, size_t flags) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  Armv7VirtualAddressSpace& owner = value >= ARMV7_DIRECT_MAP_BASE ? m_KernelSpace : *this;
  LockGuard<Spinlock> guard(owner.m_Lock);
  uint32_t* entry = owner.findEntry(value, false);
  if (!entry || !*entry) {
    return false;
  }
  const unsigned int index = (value >> 12) & 255;
  shadowAt(entry - index)[index] = flags;
  *entry = pageDescriptor(*entry & AddressMask, flags);
  Processor::invalidate(address);
  return true;
}

void Armv7VirtualAddressSpace::setFlags(void* address, size_t flags) {
  if (!trySetFlags(address, flags)) {
    panic("ARMv7: setFlags on absent mapping");
  }
}

bool Armv7VirtualAddressSpace::detachMapping(void* address, physical_uintptr_t& physical,
                                             size_t& flags, size_t requiredFlags) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  Armv7VirtualAddressSpace& owner = value >= ARMV7_DIRECT_MAP_BASE ? m_KernelSpace : *this;
  LockGuard<Spinlock> guard(owner.m_Lock);
  uint32_t* entry = owner.findEntry(value, false);
  if (!entry || !*entry) {
    return false;
  }
  const unsigned int index = (value >> 12) & 255;
  uint32_t* shadow = shadowAt(entry - index);
  flags = shadow[index];
  if ((flags & requiredFlags) != requiredFlags) {
    return false;
  }
  physical = *entry & AddressMask;
  *entry = 0;
  shadow[index] = 0;
  Processor::invalidate(address);
  return true;
}

bool Armv7VirtualAddressSpace::tryDetachUserPage(void* address, physical_uintptr_t expected) {
  physical_uintptr_t physical = 0;
  size_t flags = 0;
  if (!getMapping(address, physical, flags) || physical != expected || (flags & KernelMode)) {
    return false;
  }
  return detachMapping(address, physical, flags);
}

void Armv7VirtualAddressSpace::unmap(void* address) {
  physical_uintptr_t physical = 0;
  size_t flags = 0;
  if (!detachMapping(address, physical, flags)) {
    panic("ARMv7: unmap on absent mapping");
  }
}

bool Armv7VirtualAddressSpace::handleCopyOnWriteFault(void* address, bool userMode) {
  physical_uintptr_t old = 0;
  size_t flags = 0;
  if (!getMapping(address, old, flags) || !(flags & CopyOnWrite) ||
      (flags & (NoAccess | WriteProtected | Swapped)) || (userMode && (flags & KernelMode))) {
    return false;
  }
  physical_uintptr_t fresh = Armv7PhysicalMemoryManager::instance().tryAllocatePage();
  if (!fresh) {
    return false;
  }
  memcpy(reinterpret_cast<void*>(ARMV7_DIRECT_MAP_BASE + fresh),
         reinterpret_cast<const void*>(ARMV7_DIRECT_MAP_BASE + old), PAGE_SIZE);
  physical_uintptr_t detached = 0;
  size_t detachedFlags = 0;
  if (!detachMapping(address, detached, detachedFlags) || detached != old ||
      !map(fresh, address, (flags | Write) & ~(CopyOnWrite | Borrowed | Shared))) {
    Armv7PhysicalMemoryManager::instance().freePage(fresh);
    return false;
  }
  Armv7PhysicalMemoryManager::instance().freePage(old);
  return true;
}

VirtualAddressSpace::Stack* Armv7VirtualAddressSpace::allocateStack() {
  return allocateStack(this == &m_KernelSpace ? KERNEL_STACK_SIZE : USERSPACE_VIRTUAL_STACK_SIZE);
}

VirtualAddressSpace::Stack* Armv7VirtualAddressSpace::allocateStack(size_t bytes) {
  if (!bytes) {
    return allocateStack();
  }
  if (bytes > SIZE_MAX - PAGE_SIZE + 1) {
    return nullptr;
  }
  bytes = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  uintptr_t top;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_StackTop < bytes + PAGE_SIZE) {
      return nullptr;
    }
    top = m_StackTop;
    m_StackTop -= bytes + PAGE_SIZE;
  }
  size_t flags = Write | (this == &m_KernelSpace ? KernelMode : 0);
  for (size_t offset = PAGE_SIZE; offset <= bytes; offset += PAGE_SIZE) {
    physical_uintptr_t physical = Armv7PhysicalMemoryManager::instance().tryAllocatePage();
    if (!physical || !map(physical, reinterpret_cast<void*>(top - offset), flags)) {
      if (physical) {
        Armv7PhysicalMemoryManager::instance().freePage(physical);
      }
      for (size_t mapped = PAGE_SIZE; mapped < offset; mapped += PAGE_SIZE) {
        void* page = reinterpret_cast<void*>(top - mapped);
        physical_uintptr_t old = 0;
        size_t oldFlags = 0;
        if (detachMapping(page, old, oldFlags)) {
          Armv7PhysicalMemoryManager::instance().freePage(old);
        }
      }
      return nullptr;
    }
  }
  return new Stack(reinterpret_cast<void*>(top), bytes);
}

void Armv7VirtualAddressSpace::freeStack(Stack* stack) {
  if (!stack) {
    return;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(stack->getBase());
  for (size_t offset = 0; offset < stack->getSize(); offset += PAGE_SIZE) {
    void* page = reinterpret_cast<void*>(base + offset);
    physical_uintptr_t physical = 0;
    size_t flags = 0;
    if (detachMapping(page, physical, flags)) {
      Armv7PhysicalMemoryManager::instance().freePage(physical);
    }
  }
  delete stack;
}

bool Armv7VirtualAddressSpace::cloneUserTables(Armv7VirtualAddressSpace& destination,
                                               bool copyOnWrite) {
  uint32_t* sourceRoot = tableAt(m_Root);
  uint32_t* targetRoot = tableAt(destination.m_Root);
  for (size_t first = 0; first < 2048; ++first) {
    const uint32_t entry = sourceRoot[first];
    if (!entry) {
      continue;
    }
    if ((entry & 3) != PageTable) {
      return false;
    }
    physical_uintptr_t child = Armv7PhysicalMemoryManager::instance().tryAllocatePage();
    if (!child) {
      return false;
    }
    uint32_t* source = tableAt(entry & TableMask);
    uint32_t* target = tableAt(child);
    memset(target, 0, PAGE_SIZE);
    targetRoot[first] = child | PageTable;
    for (size_t second = 0; second < 256; ++second) {
      if (!source[second]) {
        continue;
      }
      const physical_uintptr_t old = source[second] & AddressMask;
      const size_t flags = shadowAt(source)[second];
      if (copyOnWrite && !(flags & (Shared | Borrowed))) {
        physical_uintptr_t fresh = Armv7PhysicalMemoryManager::instance().tryAllocatePage();
        if (!fresh) {
          return false;
        }
        memcpy(reinterpret_cast<void*>(ARMV7_DIRECT_MAP_BASE + fresh),
               reinterpret_cast<const void*>(ARMV7_DIRECT_MAP_BASE + old), PAGE_SIZE);
        target[second] = pageDescriptor(fresh, flags);
        shadowAt(target)[second] = flags;
      } else {
        if (!(flags & Borrowed)) {
          Armv7PhysicalMemoryManager::instance().pin(old);
        }
        target[second] = source[second];
        shadowAt(target)[second] = flags;
      }
    }
  }
  return true;
}

void Armv7VirtualAddressSpace::freeUserTables() {
  uint32_t* root = tableAt(m_Root);
  for (size_t first = 0; first < 2048; ++first) {
    const uint32_t entry = root[first];
    if ((entry & 3) != PageTable) {
      continue;
    }
    uint32_t* table = tableAt(entry & TableMask);
    for (size_t second = 0; second < 256; ++second) {
      if (table[second] && !(shadowAt(table)[second] & Borrowed)) {
        Armv7PhysicalMemoryManager::instance().freePage(table[second] & AddressMask);
      }
    }
    root[first] = 0;
    Armv7PhysicalMemoryManager::instance().freePage(entry & TableMask);
  }
}

VirtualAddressSpace* Armv7VirtualAddressSpace::clone(bool copyOnWrite) {
  auto* clone = new Armv7VirtualAddressSpace(false);
  if (!clone || !clone->m_Root) {
    delete clone;
    return nullptr;
  }
  LockGuard<Spinlock> guard(m_Lock);
  // The bootstrap's low identity mappings are sections, not userspace pages.
  if (this != &m_KernelSpace && !cloneUserTables(*clone, copyOnWrite)) {
    delete clone;
    return nullptr;
  }
  clone->m_Heap = m_Heap;
  clone->m_HeapEnd = m_HeapEnd;
  clone->m_StackTop = m_StackTop;
  return clone;
}

void Armv7VirtualAddressSpace::revertToKernelAddressSpace() {
  if (this == &m_KernelSpace) {
    return;
  }
  LockGuard<Spinlock> guard(m_Lock);
  freeUserTables();
  m_Heap = reinterpret_cast<void*>(USERSPACE_VIRTUAL_HEAP);
  m_HeapEnd = m_Heap;
  m_StackTop = USERSPACE_VIRTUAL_STACK;
  Processor::invalidate(nullptr);
}

bool Armv7VirtualAddressSpace::memIsInKernelHeap(void* address) {
  uintptr_t value = reinterpret_cast<uintptr_t>(address);
  return value >= KERNEL_VIRTUAL_HEAP && value < KERNEL_VIRTUAL_HEAP_END;
}

bool Armv7VirtualAddressSpace::memIsInHeap(void* address) {
  uintptr_t value = reinterpret_cast<uintptr_t>(address);
  return value >= reinterpret_cast<uintptr_t>(m_Heap) &&
         value < reinterpret_cast<uintptr_t>(m_HeapEnd);
}

void* Armv7VirtualAddressSpace::getEndOfHeap() {
  return reinterpret_cast<void*>(this == &m_KernelSpace ? KERNEL_VIRTUAL_HEAP_END
                                                        : USERSPACE_DYNAMIC_END);
}
