/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#include "File.h"
#include "MemoryMappedFile.h"

MemoryMapManager::FileRemapStatus MemoryMapManager::remapFilePages(uintptr_t base, size_t length,
                                                                   size_t byteOffset,
                                                                   bool nonblock) {
  TerminationDeferral lifetime;
  OperationGuard operation(*this);
  const size_t pageSize = PhysicalMemoryManager::getPageSize(), mask = pageSize - 1;
  constexpr uint64_t MaximumFileOffset = 0x7fffffffffffffffULL;
  if (!length || ((base | length | byteOffset) & mask) || length > ~uintptr_t(0) - base ||
      byteOffset > MaximumFileOffset || length > MaximumFileOffset - byteOffset)
    return FileRemapStatus::InvalidRange;
  const size_t pages = length / pageSize;
  if (pages > VirtualAddressSpace::MaximumRemapPages)
    return FileRemapStatus::NoMemory;

  auto& space = Processor::information().getVirtualAddressSpace();
  auto* objects = m_MmObjectLists.lookup(&space);
  if (objects && objects->count() > 4096)
    return FileRemapStatus::NoMemory;
  File* backing = nullptr;
  FileMappingOrigin origin;
  MemoryMappedObject::Permissions permissions = 0, maximum = 0;
  MemoryLockMode inherited = MemoryLockMode::None;
  const uintptr_t end = base + length;
  for (uintptr_t cursor = base; cursor < end;) {
    MemoryMappedObject* selected = nullptr;
    uintptr_t selectedEnd = 0;
    if (objects) {
      for (auto* object : *objects) {
        const uintptr_t objectEnd = (object->address() + object->length() + mask) & ~mask;
        if (object->address() <= cursor && cursor < objectEnd) {
          selected = object;
          selectedEnd = objectEnd;
          break;
        }
      }
    }
    if (!selected)
      return space.isMapped(reinterpret_cast<void*>(cursor)) ? FileRemapStatus::Unsupported
                                                             : FileRemapStatus::InvalidRange;
    auto* file = selected->backingFile();
    if (!file)
      return FileRemapStatus::InvalidRange;
    auto* mapping = static_cast<MemoryMappedFile*>(selected);
    if (mapping->m_Attachment || !mapping->m_Origin.openIdentity ||
        !file->supportsRegularFileOperations())
      return FileRemapStatus::Unsupported;
    if (mapping->m_bCopyOnWrite || !mapping->m_Origin.writableOpen)
      return FileRemapStatus::InvalidRange;
    if (!backing) {
      backing = file;
      origin = mapping->m_Origin;
      permissions = mapping->permissions();
      maximum = mapping->maximumPermissions();
      inherited = mapping->m_LockMode;
    } else if (backing != file || origin.openIdentity != mapping->m_Origin.openIdentity ||
               permissions != mapping->permissions() || maximum != mapping->maximumPermissions() ||
               inherited != mapping->m_LockMode) {
      return FileRemapStatus::InvalidRange;
    }
    cursor = selectedEnd < end ? selectedEnd : end;
  }

  auto* account = space.memoryLockAccount();
  const MemoryLockMode future = account ? account->futureMode() : MemoryLockMode::None;
  const MemoryLockMode mode = future == MemoryLockMode::OnFault ? MemoryLockMode::OnFault
                              : inherited != MemoryLockMode::None || future == MemoryLockMode::Eager
                                  ? MemoryLockMode::Eager
                                  : MemoryLockMode::None;
  if (account) {
    const bool privileged =
        Processor::information().getCurrentThread()->getParent()->getEffectiveUserId() == 0;
    if (inherited != MemoryLockMode::None && !account->permitsLocking(privileged))
      return FileRemapStatus::PermissionDenied;
    if (mode != MemoryLockMode::None) {
      // Fixed mmap admits the entire new locked range before retiring victims.
      // The publication path separately computes the final, net charge.
      const auto charge = account->charge();
      if (charge.rawPages > ~size_t(0) - charge.managedPages ||
          pages > ~size_t(0) - charge.managedPages - charge.rawPages ||
          !account->permitsTotalPages(charge.managedPages + charge.rawPages + pages, privileged))
        return FileRemapStatus::LockLimit;
    }
  }

  uintptr_t destination = base;
  MapStatus status;
  if (!mapFile(backing, destination, length, permissions, byteOffset, false,
               Placement::FixedReplace, &status, maximum, SharedPointer<MappingAttachment>(), mode,
               origin)) {
    switch (status) {
      case MapStatus::PolicyDenied:
        return FileRemapStatus::PolicyDenied;
      case MapStatus::LockLimit:
        return FileRemapStatus::LockLimit;
      case MapStatus::AddressInUse:
        return FileRemapStatus::InvalidRange;
      default:
        return FileRemapStatus::NoMemory;
    }
  }
  // Eager publication already populated, while ONFAULT must remain lazy even
  // when the caller did not request MAP_NONBLOCK. Population is best effort.
  if (mode == MemoryLockMode::None && !nonblock)
    populateMemory(space, base, length);
  return FileRemapStatus::Success;
}
