/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/utilities/utility.h"

#include "VirtualAddressSpace-internal.h"
#include "VirtualAddressSpace.h"

bool X64VirtualAddressSpace::tryMapUserPage(physical_uintptr_t physical, void* address,
                                            size_t flags, size_t* committedTablePages) {
  if (committedTablePages)
    *committedTablePages = 0;
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  if (!physical || ((physical | value) & 4095) || value < getUserStart() || value >= (1ULL << 47) ||
      (flags & (KernelMode | Swapped | Borrowed | Shared | CopyOnWrite)))
    return false;
  const size_t indexes[] = {PML4_INDEX(address), PAGE_DIRECTORY_POINTER_INDEX(address),
                            PAGE_DIRECTORY_INDEX(address), PAGE_TABLE_INDEX(address)};
  auto missing = [&]() -> size_t {
    physical_uintptr_t table = m_PhysicalPML4;
    for (size_t level = 0; level < 3; ++level) {
      const uint64_t entry = *TABLE_ENTRY(table, indexes[level]);
      if (!(entry & PAGE_PRESENT))
        return 3 - level;
      if (entry & PAGE_2MB)
        return 4;
      table = entry & 0x000ffffffffff000ULL;
    }
    return (*TABLE_ENTRY(table, indexes[3]) & (PAGE_PRESENT | PAGE_NO_ACCESS | PAGE_SWAPPED)) ? 4
                                                                                              : 0;
  };
  size_t count;
  {
    LockGuard<Spinlock> guard(m_Lock);
    count = missing();
  }
  if (count > 3)
    return false;
  auto& memory = PhysicalMemoryManager::instance();
  physical_uintptr_t pages[3] = {};
  size_t prepared = 0;
  for (; prepared < count; ++prepared) {
    pages[prepared] = memory.tryAllocatePage();
    if (!pages[prepared])
      break;
    ByteSet(reinterpret_cast<void*>(physicalAddress(pages[prepared])), 0, 4096);
  }
  size_t used = 0;
  bool mapped = false;
  if (prepared == count) {
    X64MappingMutationScope mutation;
    mutation.lock(m_Lock);
    if (missing() <= prepared) {
      physical_uintptr_t table = m_PhysicalPML4;
      for (size_t level = 0; level < 3; ++level) {
        uint64_t* entry = TABLE_ENTRY(table, indexes[level]);
        if (!(*entry & PAGE_PRESENT))
          *entry = pages[used++] | PAGE_PRESENT | PAGE_USER | PAGE_WRITE;
        table = PAGE_GET_PHYSICAL_ADDRESS(entry);
      }
      *TABLE_ENTRY(table, indexes[3]) = physical | toFlags(flags, true);
      if (!invalidateMapping(address, mutation))
        mutation.panicInvalidationFailure();
      mapped = true;
    }
  }
  for (size_t i = used; i < prepared; ++i)
    memory.freePage(pages[i]);
  if (mapped && committedTablePages)
    *committedTablePages = used;
  return mapped;
}

bool X64VirtualAddressSpace::tryDetachUserPage(void* address, physical_uintptr_t expected) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  if ((value & 4095) || value < getUserStart() || value >= (1ULL << 47))
    return false;
  {
    X64MappingMutationScope mutation;
    mutation.lock(m_Lock);
    uint64_t* entry = nullptr;
    if (!getPageTableEntry(address, entry) || PAGE_GET_PHYSICAL_ADDRESS(entry) != expected ||
        !(*entry & PAGE_NO_ACCESS) || (*entry & PAGE_SWAPPED))
      return false;
    *entry = 0;
    if (!invalidateMapping(address, mutation))
      mutation.panicInvalidationFailure();
  }
  return true;
}
