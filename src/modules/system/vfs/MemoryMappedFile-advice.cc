/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#include "File.h"
#include "MemoryMappedFile.h"

bool MemoryMappedObject::resident(VirtualAddressSpace& space, uintptr_t address,
                                  FileResidencyAccess, void*) {
  return space.isMapped(reinterpret_cast<void*>(address));
}

bool MemoryMappedFile::resident(VirtualAddressSpace& space, uintptr_t address,
                                FileResidencyAccess access, void* credentials) {
  // Permission masking precedes both the PTE and backing-cache observation.
  if (!access || !access(m_pBacking, credentials) || m_pBacking->isDirectPhysicalMapping())
    return true;
  if (space.isMapped(reinterpret_cast<void*>(address)))
    return true;
  const size_t displacement = address - m_Address;
  if (displacement > ~size_t(0) - m_Offset)
    return false;
  const size_t offset = m_Offset + displacement;
  const physical_uintptr_t physical = m_pBacking->getPhysicalPage(offset);
  if (physical == ~physical_uintptr_t(0))
    return false;
  m_pBacking->returnPhysicalPage(offset);
  return true;
}

void MemoryMappedFile::releaseDetachedPageUnlocked(uintptr_t oldAddress,
                                                   const VirtualAddressSpace::DetachedPage& page) {
  const bool loan = m_Mappings.contains(oldAddress) && getMapping(oldAddress) == ~0UL;
  const size_t offset = m_Offset + (oldAddress - m_Address);
  const bool borrowed = page.mapped && (page.flags & VirtualAddressSpace::Borrowed);
  if (borrowed && !m_bCopyOnWrite)
    m_pBacking->sync(offset, false);
  // Generic CoW can replace the PTE while the original cache loan remains.
  // Both references must be retired, independently, using the final PTE.
  if (loan || borrowed)
    m_pBacking->returnPhysicalPage(offset);
  if (page.mapped && !borrowed)
    PhysicalMemoryManager::instance().freePage(page.physical);
  untrackMapping(oldAddress);
}

void MemoryMappedFile::releaseDetachedPage(uintptr_t oldAddress,
                                           const VirtualAddressSpace::DetachedPage& page) {
  LockGuard<Mutex> guard(m_Lock);
  releaseDetachedPageUnlocked(oldAddress, page);
}

void MemoryMappedFile::discardRange(VirtualAddressSpace& space, uintptr_t base, size_t length) {
  LockGuard<Mutex> guard(m_Lock);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  for (uintptr_t address = base; address < base + length; address += pageSize) {
    VirtualAddressSpace::DetachedPage page{address, 0, 0, false};
    page.mapped = space.detachMapping(reinterpret_cast<void*>(address), page.physical, page.flags);
    releaseDetachedPageUnlocked(address, page);
  }
}

MemoryMapManager::VmStatus MemoryMapManager::residency(uintptr_t base, size_t length,
                                                       unsigned char* kernelVector,
                                                       FileResidencyAccess access,
                                                       void* credentials) {
  OperationGuard operation(*this);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if ((base & (pageSize - 1)) || (length & (pageSize - 1)) || length > ~uintptr_t(0) - base)
    return VmStatus::InvalidRange;
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  MmObjectList* objects = m_MmObjectLists.lookup(&space);
  for (uintptr_t address = base; address < base + length; address += pageSize) {
    MemoryMappedObject* selected = nullptr;
    if (objects) {
      for (auto* object : *objects) {
        if (object->matches(address)) {
          selected = object;
          break;
        }
      }
    }
    bool present = false;
    if (selected)
      present = selected->resident(space, address, access, credentials);
    else if (!(present = space.isMapped(reinterpret_cast<void*>(address))))
      return VmStatus::Unmapped;
    kernelVector[(address - base) / pageSize] = present ? 1 : 0;
  }
  return VmStatus::Success;
}

MemoryMapManager::VmStatus MemoryMapManager::discard(uintptr_t base, size_t length) {
  OperationGuard operation(*this);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t pageMask = pageSize - 1;
  if ((base & pageMask) || (length & pageMask) || length > ~uintptr_t(0) - base)
    return VmStatus::InvalidRange;
  if (!length)
    return VmStatus::Success;
#if !(X64 || HOSTED)
  return VmStatus::Unsupported;
#endif
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  if (hasLockedMemory(space, base, length))
    return VmStatus::InvalidRange;
  MmObjectList* objects = m_MmObjectLists.lookup(&space);
  if (!objects)
    return VmStatus::Unmapped;
  const uintptr_t end = base + length;
  uintptr_t cursor = base;
  while (cursor < end) {
    MemoryMappedObject* selected = nullptr;
    uintptr_t selectedEnd = 0;
    for (auto* object : *objects) {
      const uintptr_t objectEnd = (object->address() + object->length() + pageMask) & ~pageMask;
      if (object->address() <= cursor && cursor < objectEnd) {
        selected = object;
        selectedEnd = objectEnd;
        break;
      }
    }
    if (!selected)
      return VmStatus::Unmapped;
    if (selected->backingFile() && selected->backingFile()->isDirectPhysicalMapping())
      return VmStatus::Unsupported;
    cursor = selectedEnd < end ? selectedEnd : end;
  }
  for (auto* object : *objects) {
    const uintptr_t objectEnd = (object->address() + object->length() + pageMask) & ~pageMask;
    const uintptr_t first = object->address() > base ? object->address() : base;
    const uintptr_t last = objectEnd < end ? objectEnd : end;
    if (first < last)
      object->discardRange(space, first, last - first);
  }
  return VmStatus::Success;
}
