/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#include "File.h"
#include "MemoryMappedFile.h"

MemoryMapManager::UserPageCopyStatus MemoryMapManager::copyUserPage(VirtualAddressSpace& space,
                                                                    uintptr_t address,
                                                                    void* kernelBuffer,
                                                                    size_t bytes, bool write) {
#if !X64 || HOSTED
  (void)space;
  (void)address;
  (void)kernelBuffer;
  (void)bytes;
  (void)write;
  return UserPageCopyStatus::Unsupported;
#else
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  constexpr uintptr_t UserEnd = 0x0000800000000000ULL;
  if (!kernelBuffer || !bytes || bytes > pageSize || address < space.getUserStart() ||
      address >= UserEnd || address >= space.getKernelStart() || bytes > UserEnd - address ||
      bytes > space.getKernelStart() - address || (address & (pageSize - 1)) > pageSize - bytes)
    return UserPageCopyStatus::Inaccessible;

  OperationGuard operation(*this);
  const uintptr_t pageAddress = address & ~(pageSize - 1);
  void* page = reinterpret_cast<void*>(pageAddress);
  MemoryMappedObject* selected = nullptr;
  auto* objects = m_MmObjectLists.lookup(&space);
  if (objects) {
    for (auto* object : *objects) {
      if (object->matches(pageAddress)) {
        selected = object;
        break;
      }
    }
  }
  if (selected) {
    const auto required = write ? MemoryMappedObject::Write : MemoryMappedObject::Read;
    if (!(selected->permissions() & required) || selected->beyondBackingEnd(pageAddress))
      return UserPageCopyStatus::Inaccessible;
    if (selected->backingFile() && selected->backingFile()->isDirectPhysicalMapping())
      return UserPageCopyStatus::Unsupported;
  } else if (!space.rawUserMemory().covers(pageAddress, pageSize) &&
             space.runtimeMappingPages(pageAddress, pageSize) != 1) {
    return UserPageCopyStatus::Inaccessible;
  }

  bool prepare = !space.isMapped(page);
  if (!prepare) {
    physical_uintptr_t physical = 0;
    size_t flags = 0;
    space.getMapping(page, physical, flags);
    if (flags & (VirtualAddressSpace::KernelMode | VirtualAddressSpace::NoAccess |
                 VirtualAddressSpace::Swapped | VirtualAddressSpace::CacheDisable |
                 VirtualAddressSpace::WriteCombine | VirtualAddressSpace::WriteThrough))
      return UserPageCopyStatus::Inaccessible;
    if (write) {
      if (flags & VirtualAddressSpace::WriteProtected)
        return UserPageCopyStatus::Inaccessible;
      if (flags & VirtualAddressSpace::CopyOnWrite) {
        if (!space.handleCopyOnWriteFault(page, true))
          return UserPageCopyStatus::NoMemory;
      } else {
        prepare = !(flags & VirtualAddressSpace::Write);
      }
    }
  }

  if (prepare) {
    if (!selected)
      return UserPageCopyStatus::Inaccessible;
    // A read must retain lazy private CoW; eager-lock population intentionally does not.
    PopulationStatus status = PopulationStatus::NoMemory;
    auto* thread = Processor::information().getCurrentThread();
    const size_t previousError = thread ? thread->getErrno() : 0;
    if (thread)
      thread->setErrno(0);
    const bool prepared = selected->trap(space, pageAddress, write, &status);
    if (thread)
      thread->setErrno(previousError);
    if (!prepared) {
      switch (status) {
        case PopulationStatus::NoMemory:
          return UserPageCopyStatus::NoMemory;
        case PopulationStatus::Inaccessible:
          return UserPageCopyStatus::Inaccessible;
        default:
          return UserPageCopyStatus::IoError;
      }
    }
  }

  switch (space.copyResidentUserPage(address, kernelBuffer, bytes, write)) {
    case VirtualAddressSpace::ResidentCopyStatus::Success:
      return UserPageCopyStatus::Success;
    case VirtualAddressSpace::ResidentCopyStatus::Unsupported:
      return UserPageCopyStatus::Unsupported;
    default:
      return UserPageCopyStatus::Inaccessible;
  }
#endif
}
