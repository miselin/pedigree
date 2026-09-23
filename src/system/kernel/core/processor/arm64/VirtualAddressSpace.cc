#include "VirtualAddressSpace.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/Processor.h"

#include <string.h>

#include "PhysicalMemoryManager.h"

extern "C" char arm64_boot_high_l0, arm64_boot_low_l0;

namespace {
constexpr uint64_t Valid = 1ULL;
constexpr uint64_t Table = 2ULL;
constexpr uint64_t AddressMask = 0x0000fffffffff000ULL;
constexpr uint64_t Shareable = 3ULL << 8;
constexpr uint64_t AccessFlag = 1ULL << 10;
constexpr uint64_t NotGlobal = 1ULL << 11;
constexpr uint64_t PrivilegedExecuteNever = 1ULL << 53;
constexpr uint64_t UserExecuteNever = 1ULL << 54;
constexpr uint64_t SoftwareNoAccess = 1ULL << 3;
constexpr uint64_t SoftwareSwapped = 1ULL << 4;
constexpr uint64_t SoftwareCopyOnWrite = 1ULL << 55;
constexpr uint64_t SoftwareShared = 1ULL << 56;
constexpr uint64_t SoftwareBorrowed = 1ULL << 57;
constexpr uint64_t SoftwareWriteProtected = 1ULL << 58;

uint64_t* tableAt(physical_uintptr_t physical) {
  return reinterpret_cast<uint64_t*>(ARM64_DIRECT_MAP_BASE + physical);
}

size_t tableIndex(uintptr_t address, size_t level) {
  return (address >> (39 - level * 9)) & 511;
}
}  // namespace

Arm64VirtualAddressSpace Arm64VirtualAddressSpace::m_KernelSpace(true);

VirtualAddressSpace& VirtualAddressSpace::getKernelAddressSpace() {
  return Arm64VirtualAddressSpace::m_KernelSpace;
}

VirtualAddressSpace* VirtualAddressSpace::create() {
  return new Arm64VirtualAddressSpace(false);
}

Arm64VirtualAddressSpace::Arm64VirtualAddressSpace(bool kernel)
    : VirtualAddressSpace(
          reinterpret_cast<void*>(kernel ? KERNEL_VIRTUAL_HEAP : USERSPACE_VIRTUAL_HEAP)),
      m_Root(kernel ? reinterpret_cast<uintptr_t>(&arm64_boot_high_l0)
                    : Arm64PhysicalMemoryManager::instance().allocatePage()),
      m_StackTop(kernel ? KERNEL_VIRTUAL_STACK : USERSPACE_VIRTUAL_STACK) {
  if (!kernel) {
    memset(tableAt(m_Root), 0, PAGE_SIZE);
  }
}

Arm64VirtualAddressSpace::~Arm64VirtualAddressSpace() {
  if (this != &m_KernelSpace && m_Root) {
    freeTable(tableAt(m_Root), 0, true);
    Arm64PhysicalMemoryManager::instance().freePage(m_Root);
  }
}

bool Arm64VirtualAddressSpace::isAddressValid(void* address) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  return value < 0x0000800000000000ULL || value >= ARM64_DIRECT_MAP_BASE;
}

uint64_t* Arm64VirtualAddressSpace::findExistingEntry(uintptr_t address, size_t* depth) const {
  physical_uintptr_t root = m_Root;
  if (address >= ARM64_DIRECT_MAP_BASE) {
    root = m_KernelSpace.m_Root;
  } else if (this == &m_KernelSpace) {
    root = reinterpret_cast<uintptr_t>(&arm64_boot_low_l0);
  }
  uint64_t* table = tableAt(root);
  for (size_t level = 0; level < 4; ++level) {
    uint64_t* entry = &table[tableIndex(address, level)];
    if (level == 3 || !(*entry & Valid) || !(*entry & Table)) {
      if (depth) {
        *depth = level;
      }
      return entry;
    }
    table = tableAt(*entry & AddressMask);
  }
  return nullptr;
}

uint64_t* Arm64VirtualAddressSpace::findEntry(uintptr_t address, bool create, size_t* newTables) {
  physical_uintptr_t root = m_Root;
  if (address >= ARM64_DIRECT_MAP_BASE) {
    root = m_KernelSpace.m_Root;
  } else if (this == &m_KernelSpace) {
    root = reinterpret_cast<uintptr_t>(&arm64_boot_low_l0);
  }
  uint64_t* table = tableAt(root);
  for (size_t level = 0; level < 3; ++level) {
    uint64_t* entry = &table[tableIndex(address, level)];
    if (!(*entry & Valid) || !(*entry & Table)) {
      if (!create) {
        return nullptr;
      }
      physical_uintptr_t child = Arm64PhysicalMemoryManager::instance().tryAllocatePage();
      if (!child) {
        return nullptr;
      }
      uint64_t* childTable = tableAt(child);
      if (*entry & Valid) {
        // Split a bootstrap identity block before inserting a 4 KiB mapping.
        const size_t shift = 39 - level * 9;
        const uint64_t subSize = 1ULL << (shift - 9);
        const uint64_t base = *entry & ~(subSize * 512 - 1);
        const uint64_t attributes = *entry & ~AddressMask;
        for (size_t i = 0; i < 512; ++i) {
          childTable[i] = (base + i * subSize) | attributes | Valid;
        }
      } else {
        memset(childTable, 0, PAGE_SIZE);
      }
      *entry = child | Valid | Table;
      if (newTables) {
        ++*newTables;
      }
    }
    table = tableAt(*entry & AddressMask);
  }
  return &table[tableIndex(address, 3)];
}

uint64_t Arm64VirtualAddressSpace::pageDescriptor(physical_uintptr_t physical, size_t flags) {
  const bool kernel = flags & KernelMode;
  uint64_t entry = (physical & AddressMask) | Valid | Table | Shareable | AccessFlag;
  if (!(flags & CacheDisable)) {
    entry |= 1ULL << 2;
  }
  if (!kernel) {
    entry |= 1ULL << 6;
    // TTBR0 changes flush all entries, including runtime pages kept global.
    if (!(flags & RuntimeMapping)) {
      entry |= NotGlobal;
    }
  }
  if (!(flags & Write) || (flags & (CopyOnWrite | WriteProtected))) {
    entry |= 1ULL << 7;
  }
  if (kernel || !(flags & Execute)) {
    entry |= UserExecuteNever;
  }
  if (!kernel || !(flags & Execute)) {
    entry |= PrivilegedExecuteNever;
  }
  if (flags & NoAccess) {
    entry = (entry & ~Valid) | SoftwareNoAccess;
  }
  if (flags & CopyOnWrite) {
    entry |= SoftwareCopyOnWrite;
  }
  if (flags & Shared) {
    entry |= SoftwareShared;
  }
  if (flags & Borrowed) {
    entry |= SoftwareBorrowed;
  }
  if (flags & WriteProtected) {
    entry |= SoftwareWriteProtected;
  }
  if (flags & Swapped) {
    entry = (entry & ~Valid) | SoftwareSwapped;
  }
  return entry;
}

size_t Arm64VirtualAddressSpace::descriptorFlags(uint64_t entry) {
  size_t flags = 0;
  if (!(entry & (1ULL << 6))) {
    flags |= KernelMode;
  }
  if (!(entry & (1ULL << 7))) {
    flags |= Write;
  }
  if ((entry & (1ULL << 6)) ? !(entry & UserExecuteNever) : !(entry & PrivilegedExecuteNever)) {
    flags |= Execute;
  }
  if (!(entry & (1ULL << 2))) {
    flags |= CacheDisable;
  }
  if (entry & SoftwareNoAccess) {
    flags |= NoAccess;
  }
  if (entry & SoftwareCopyOnWrite) {
    flags |= CopyOnWrite;
  }
  if (entry & SoftwareShared) {
    flags |= Shared;
  }
  if (entry & SoftwareBorrowed) {
    flags |= Borrowed;
  }
  if (entry & SoftwareWriteProtected) {
    flags |= WriteProtected;
  }
  if (entry & SoftwareSwapped) {
    flags |= Swapped;
  }
  if ((entry & (1ULL << 6)) && !(entry & NotGlobal)) {
    flags |= RuntimeMapping;
  }
  return flags;
}

bool Arm64VirtualAddressSpace::isMapped(void* address) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  Arm64VirtualAddressSpace& owner = value >= ARM64_DIRECT_MAP_BASE ? m_KernelSpace : *this;
  LockGuard<Spinlock> guard(owner.m_Lock);
  uint64_t* entry = owner.findExistingEntry(value);
  return entry && *entry;
}

bool Arm64VirtualAddressSpace::map(physical_uintptr_t physical, void* address, size_t flags) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  if (!isAddressValid(address) || (value & (PAGE_SIZE - 1)) || (physical & (PAGE_SIZE - 1))) {
    return false;
  }
  Arm64VirtualAddressSpace& owner = value >= ARM64_DIRECT_MAP_BASE ? m_KernelSpace : *this;
  LockGuard<Spinlock> guard(owner.m_Lock);
  uint64_t* old = owner.findExistingEntry(value);
  if (old && *old && (*old & Valid) && value >= ARM64_DIRECT_MAP_BASE) {
    return false;
  }
  uint64_t* entry = owner.findEntry(value, true);
  if (!entry || (*entry && !(descriptorFlags(*entry) & KernelMode))) {
    return false;
  }
  *entry = pageDescriptor(physical, flags);
  Processor::invalidate(address);
  return true;
}

bool Arm64VirtualAddressSpace::tryMapUserPage(physical_uintptr_t physical, void* address,
                                              size_t flags, size_t* committedTablePages) {
  if (flags & KernelMode || reinterpret_cast<uintptr_t>(address) >= ARM64_DIRECT_MAP_BASE) {
    return false;
  }
  if (committedTablePages) {
    *committedTablePages = 0;
  }
  return map(physical, address, flags);
}

bool Arm64VirtualAddressSpace::getMapping(void* address, physical_uintptr_t& physical,
                                          size_t& flags) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  Arm64VirtualAddressSpace& owner = value >= ARM64_DIRECT_MAP_BASE ? m_KernelSpace : *this;
  LockGuard<Spinlock> guard(owner.m_Lock);
  size_t level = 0;
  uint64_t* entry = owner.findExistingEntry(value, &level);
  if (!entry || !*entry || (level != 3 && !(*entry & Valid))) {
    return false;
  }
  const uint64_t mask = (1ULL << (39 - level * 9)) - 1;
  physical = (*entry & AddressMask & ~mask) | (reinterpret_cast<uintptr_t>(address) & mask);
  flags = descriptorFlags(*entry);
  return true;
}

bool Arm64VirtualAddressSpace::trySetFlags(void* address, size_t flags) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  Arm64VirtualAddressSpace& owner = value >= ARM64_DIRECT_MAP_BASE ? m_KernelSpace : *this;
  LockGuard<Spinlock> guard(owner.m_Lock);
  uint64_t* entry = owner.findEntry(value, false);
  if (!entry || !*entry) {
    return false;
  }
  *entry = pageDescriptor(*entry & AddressMask, flags);
  Processor::invalidate(address);
  return true;
}

void Arm64VirtualAddressSpace::setFlags(void* address, size_t flags) {
  if (!trySetFlags(address, flags)) {
    panic("ARM64: setFlags on absent mapping");
  }
}

bool Arm64VirtualAddressSpace::detachMapping(void* address, physical_uintptr_t& physical,
                                             size_t& flags, size_t requiredFlags) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  Arm64VirtualAddressSpace& owner = value >= ARM64_DIRECT_MAP_BASE ? m_KernelSpace : *this;
  LockGuard<Spinlock> guard(owner.m_Lock);
  uint64_t* entry = owner.findEntry(value, false);
  if (!entry || !*entry) {
    return false;
  }
  flags = descriptorFlags(*entry);
  if ((flags & requiredFlags) != requiredFlags) {
    return false;
  }
  physical = *entry & AddressMask;
  *entry = 0;
  Processor::invalidate(address);
  return true;
}

bool Arm64VirtualAddressSpace::tryDetachUserPage(void* address, physical_uintptr_t expected) {
  physical_uintptr_t physical = 0;
  size_t flags = 0;
  if (!getMapping(address, physical, flags) || physical != expected || (flags & KernelMode)) {
    return false;
  }
  return detachMapping(address, physical, flags);
}

void Arm64VirtualAddressSpace::unmap(void* address) {
  physical_uintptr_t physical = 0;
  size_t flags = 0;
  if (!detachMapping(address, physical, flags)) {
    panic("ARM64: unmap on absent mapping");
  }
}

bool Arm64VirtualAddressSpace::handleCopyOnWriteFault(void* address, bool userMode) {
  physical_uintptr_t old = 0;
  size_t flags = 0;
  if (!getMapping(address, old, flags) || !(flags & CopyOnWrite) ||
      (flags & (NoAccess | WriteProtected | Swapped)) || (userMode && (flags & KernelMode))) {
    return false;
  }
  physical_uintptr_t fresh = Arm64PhysicalMemoryManager::instance().tryAllocatePage();
  if (!fresh) {
    return false;
  }
  memcpy(reinterpret_cast<void*>(ARM64_DIRECT_MAP_BASE + fresh),
         reinterpret_cast<const void*>(ARM64_DIRECT_MAP_BASE + old), PAGE_SIZE);
  physical_uintptr_t detached = 0;
  size_t detachedFlags = 0;
  if (!detachMapping(address, detached, detachedFlags) || detached != old ||
      !map(fresh, address, (flags | Write) & ~(CopyOnWrite | Borrowed | Shared))) {
    Arm64PhysicalMemoryManager::instance().freePage(fresh);
    return false;
  }
  Arm64PhysicalMemoryManager::instance().freePage(old);
  return true;
}

VirtualAddressSpace::Stack* Arm64VirtualAddressSpace::allocateStack() {
  return allocateStack(this == &m_KernelSpace ? KERNEL_STACK_SIZE : USERSPACE_VIRTUAL_STACK_SIZE);
}

VirtualAddressSpace::Stack* Arm64VirtualAddressSpace::allocateStack(size_t bytes) {
  if (!bytes) {
    return allocateStack();
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
    physical_uintptr_t physical = Arm64PhysicalMemoryManager::instance().tryAllocatePage();
    if (!physical || !map(physical, reinterpret_cast<void*>(top - offset), flags)) {
      if (physical) {
        Arm64PhysicalMemoryManager::instance().freePage(physical);
      }
      for (size_t mapped = PAGE_SIZE; mapped < offset; mapped += PAGE_SIZE) {
        void* page = reinterpret_cast<void*>(top - mapped);
        physical_uintptr_t old = 0;
        size_t oldFlags = 0;
        if (detachMapping(page, old, oldFlags)) {
          Arm64PhysicalMemoryManager::instance().freePage(old);
        }
      }
      return nullptr;
    }
  }
  return new Stack(reinterpret_cast<void*>(top), bytes);
}

void Arm64VirtualAddressSpace::freeStack(Stack* stack) {
  if (!stack) {
    return;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(stack->getBase());
  for (size_t offset = 0; offset < stack->getSize(); offset += PAGE_SIZE) {
    void* page = reinterpret_cast<void*>(base + offset);
    physical_uintptr_t physical = 0;
    size_t flags = 0;
    if (detachMapping(page, physical, flags)) {
      Arm64PhysicalMemoryManager::instance().freePage(physical);
    }
  }
  delete stack;
}

bool Arm64VirtualAddressSpace::cloneTable(uint64_t* destination, const uint64_t* source,
                                          size_t level, bool copyOnWrite) {
  for (size_t i = 0; i < 512; ++i) {
    const uint64_t entry = source[i];
    if (!entry) {
      continue;
    }
    if (level < 3 && (entry & Valid) && (entry & Table)) {
      physical_uintptr_t child = Arm64PhysicalMemoryManager::instance().tryAllocatePage();
      if (!child) {
        return false;
      }
      memset(tableAt(child), 0, PAGE_SIZE);
      destination[i] = child | Valid | Table;
      if (!cloneTable(tableAt(child), tableAt(entry & AddressMask), level + 1, copyOnWrite)) {
        return false;
      }
    } else if (level == 3 && (entry & (1ULL << 6)) && (entry & Valid)) {
      const physical_uintptr_t old = entry & AddressMask;
      if (copyOnWrite && !(entry & SoftwareShared)) {
        physical_uintptr_t fresh = Arm64PhysicalMemoryManager::instance().tryAllocatePage();
        if (!fresh) {
          return false;
        }
        memcpy(reinterpret_cast<void*>(ARM64_DIRECT_MAP_BASE + fresh),
               reinterpret_cast<const void*>(ARM64_DIRECT_MAP_BASE + old), PAGE_SIZE);
        destination[i] = ((entry & ~AddressMask) & ~SoftwareBorrowed) | fresh;
      } else {
        if (!(entry & SoftwareBorrowed)) {
          Arm64PhysicalMemoryManager::instance().pin(old);
        }
        destination[i] = entry;
      }
    } else {
      destination[i] = entry;
    }
  }
  return true;
}

void Arm64VirtualAddressSpace::freeTable(uint64_t* table, size_t level, bool freeLeaves) {
  for (size_t i = 0; i < 512; ++i) {
    uint64_t entry = table[i];
    if (!entry) {
      continue;
    }
    if (level < 3 && (entry & Valid) && (entry & Table)) {
      freeTable(tableAt(entry & AddressMask), level + 1, freeLeaves);
      Arm64PhysicalMemoryManager::instance().freePage(entry & AddressMask);
    } else if (level == 3 && freeLeaves && (entry & (1ULL << 6)) && (entry & Valid) &&
               !(entry & (SoftwareBorrowed | SoftwareShared))) {
      Arm64PhysicalMemoryManager::instance().freePage(entry & AddressMask);
    }
    table[i] = 0;
  }
}

VirtualAddressSpace* Arm64VirtualAddressSpace::clone(bool copyOnWrite) {
  auto* clone = new Arm64VirtualAddressSpace(false);
  if (!clone) {
    return nullptr;
  }
  LockGuard<Spinlock> guard(m_Lock);
  if (!cloneTable(tableAt(clone->m_Root), tableAt(m_Root), 0, copyOnWrite)) {
    delete clone;
    return nullptr;
  }
  clone->m_Heap = m_Heap;
  clone->m_HeapEnd = m_HeapEnd;
  clone->m_StackTop = m_StackTop;
  return clone;
}

void Arm64VirtualAddressSpace::revertToKernelAddressSpace() {
  if (this == &m_KernelSpace) {
    return;
  }
  LockGuard<Spinlock> guard(m_Lock);
  uint64_t* root = tableAt(m_Root);
  freeTable(root, 0, true);
  m_Heap = reinterpret_cast<void*>(USERSPACE_VIRTUAL_HEAP);
  m_HeapEnd = m_Heap;
  m_StackTop = USERSPACE_VIRTUAL_STACK;
  Processor::invalidate(nullptr);
}

bool Arm64VirtualAddressSpace::memIsInKernelHeap(void* address) {
  uintptr_t value = reinterpret_cast<uintptr_t>(address);
  return value >= KERNEL_VIRTUAL_HEAP && value < KERNEL_VIRTUAL_HEAP_END;
}

bool Arm64VirtualAddressSpace::memIsInHeap(void* address) {
  uintptr_t value = reinterpret_cast<uintptr_t>(address);
  return value >= reinterpret_cast<uintptr_t>(m_Heap) &&
         value < reinterpret_cast<uintptr_t>(m_HeapEnd);
}

void* Arm64VirtualAddressSpace::getEndOfHeap() {
  return reinterpret_cast<void*>(this == &m_KernelSpace ? KERNEL_VIRTUAL_HEAP_END
                                                        : USERSPACE_DYNAMIC_END);
}
