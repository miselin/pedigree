/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "File.h"
#include "MemoryMappedFile.h"

namespace {
using Snapshot = Process::UserReservationSnapshot;
using Status = MemoryMapManager::MapStatus;
using Placement = MemoryMapManager::Placement;
MemoryAllocator* allocatorFor(Snapshot& snapshot, VirtualAddressSpace& space, uintptr_t base,
                              size_t length) {
  if (space.getDynamicStart() && base >= space.getDynamicStart() &&
      base + length <= space.getDynamicEnd())
    return &snapshot.dynamic;
  if (base >= space.getUserStart() && base + length <= space.getUserReservedStart())
    return &snapshot.normal;
  return nullptr;
}
bool reserveFree(MemoryAllocator& allocator, uintptr_t base, size_t length) {
  while (true) {
    bool found = false;
    for (size_t i = 0; i < allocator.size(); ++i) {
      MemoryAllocator::Range range(0, 0);
      if (!allocator.getRange(i, range))
        continue;
      const uintptr_t first = range.address > base ? range.address : base;
      const uintptr_t rangeEnd = range.length > ~uintptr_t(0) - range.address
                                     ? ~uintptr_t(0)
                                     : range.address + range.length;
      const uintptr_t last = rangeEnd < base + length ? rangeEnd : base + length;
      if (first >= last)
        continue;
      if (!allocator.allocateSpecific(first, last - first))
        return false;
      found = true;
      break;
    }
    if (!found)
      return true;
  }
}
Status place(Snapshot& snapshot, VirtualAddressSpace& space, uintptr_t& address, size_t length,
             Placement placement, size_t mask) {
  if (address) {
    auto* allocator = allocatorFor(snapshot, space, address, length);
    if (allocator && allocator->allocateSpecific(address, length))
      return Status::Success;
    if (placement == Placement::FixedNoReplace)
      return Status::AddressInUse;
    if (placement == Placement::FixedReplace)
      return reserveFree(snapshot.dynamic, address, length) &&
                     reserveFree(snapshot.normal, address, length)
                 ? Status::Success
                 : Status::NoMemory;
  }
  MemoryAllocator* allocators[] = {&snapshot.dynamic, &snapshot.normal};
  for (auto* allocator : allocators) {
    uintptr_t allocation = 0;
    if (!allocator->allocate(length + mask, allocation))
      continue;
    address = (allocation + mask) & ~mask;
    if (address != allocation && !allocator->tryFree(allocation, address - allocation))
      return Status::NoMemory;
    const uintptr_t end = allocation + length + mask;
    if (address + length < end && !allocator->tryFree(address + length, end - address - length))
      return Status::NoMemory;
    return Status::Success;
  }
  return Status::NoMemory;
}
struct MappingPlan {
  List<MemoryMappedObject*>* replacement = nullptr;
  Vector<MemoryMappedObject*> staged, retired;
  MemoryMappedObject* inserted = nullptr;
  bool committed = false;
  ~MappingPlan() {
    for (auto* object : committed ? retired : staged)
      delete object;
    delete replacement;
  }
  bool appendSlice(MemoryMappedObject* owner, uintptr_t first, uintptr_t end) {
    if (first >= end)
      return true;
    auto* object = owner->stageSlice(first, end - first, first, end - first);
    if (!object)
      return false;
    staged.pushBack(object);
    return replacement->tryPushBack(object);
  }
};
}  // namespace

MemoryMappedObject* MemoryMapManager::mapFile(File* file, uintptr_t& address, size_t length,
                                              MemoryMappedObject::Permissions perms, size_t offset,
                                              bool copyOnWrite) {
  return mapFile(file, address, length, perms, offset, copyOnWrite, Placement::FixedReplace,
                 nullptr);
}
MemoryMappedObject* MemoryMapManager::mapFile(File* file, uintptr_t& address, size_t length,
                                              MemoryMappedObject::Permissions perms, size_t offset,
                                              bool copyOnWrite, Placement placement,
                                              MapStatus* status,
                                              MemoryMappedObject::Permissions maximumPerms,
                                              const SharedPointer<MappingAttachment>& attachment,
                                              MemoryLockMode requestedLock,
                                              const FileMappingOrigin& origin) {
  OperationGuard operation(*this);
  bool mayWrite = maximumPerms & MemoryMappedObject::Write;
  if (!file->allowMapping(!copyOnWrite, perms & MemoryMappedObject::Write, mayWrite)) {
    if (status)
      *status = MapStatus::PolicyDenied;
    return nullptr;
  }
  if (!mayWrite)
    maximumPerms &= ~MemoryMappedObject::Write;
  return publishMapping(file, address, length, perms, offset, copyOnWrite, placement, status,
                        maximumPerms, attachment, requestedLock, origin);
}
MemoryMappedObject* MemoryMapManager::mapAnon(uintptr_t& address, size_t length,
                                              MemoryMappedObject::Permissions perms) {
  return mapAnon(address, length, perms, Placement::FixedReplace, nullptr);
}
MemoryMappedObject* MemoryMapManager::mapAnon(uintptr_t& address, size_t length,
                                              MemoryMappedObject::Permissions perms,
                                              Placement placement, MapStatus* status,
                                              MemoryLockMode requestedLock) {
  OperationGuard operation(*this);
  return publishMapping(
      nullptr, address, length, perms, 0, true, placement, status,
      MemoryMappedObject::Read | MemoryMappedObject::Write | MemoryMappedObject::Exec,
      SharedPointer<MappingAttachment>(), requestedLock);
}

MemoryMappedObject* MemoryMapManager::publishMapping(
    File* file, uintptr_t& address, size_t length, MemoryMappedObject::Permissions perms,
    size_t offset, bool copyOnWrite, Placement placement, MapStatus* status,
    MemoryMappedObject::Permissions maximumPerms,
    const SharedPointer<MappingAttachment>& attachment, MemoryLockMode requestedLock,
    const FileMappingOrigin& origin) {
  if (status)
    *status = MapStatus::NoMemory;
  const size_t pageSize = PhysicalMemoryManager::getPageSize(), mask = pageSize - 1;
  const size_t actualLength = length;
  if (!length || length > ~size_t(0) - mask)
    return nullptr;
  length = (length + mask) & ~mask;
  if (length > ~size_t(0) - mask || length > ~uintptr_t(0) - address)
    return nullptr;
  auto& space = Processor::information().getVirtualAddressSpace();
  auto* process = Processor::information().getCurrentThread()->getParent();
  auto* account = space.memoryLockAccount();
  const MemoryLockMode mode = requestedLock != MemoryLockMode::None ? requestedLock
                              : account                             ? account->futureMode()
                                                                    : MemoryLockMode::None;
  auto* objects = m_MmObjectLists.lookup(&space);
  // Preallocate the empty registry entry before a reservation or PTE changes.
  if (!objects) {
    auto* empty = new MmObjectList;
    if (!empty)
      return nullptr;
    if (!m_MmObjectLists.tryInsert(&space, empty)) {
      delete empty;
      return nullptr;
    }
    objects = empty;
  }
  constexpr size_t MaximumObjects = 4096;
  if (objects->count() > MaximumObjects)
    return nullptr;
  const uintptr_t requested = address;
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    Snapshot snapshot;
    if (!process->snapshotUserReservations(snapshot))
      return nullptr;
    uintptr_t destination = requested;
    auto placementStatus = place(snapshot, space, destination, length, placement, mask);
    if (placementStatus != MapStatus::Success) {
      if (status)
        *status = placementStatus;
      return nullptr;
    }
    if (space.runtimeMappingPages(destination, length)) {
      if (status)
        *status = MapStatus::PolicyDenied;
      return nullptr;
    }
    UniquePointer<PreparedMemoryLock> raw;
    auto rawStatus = space.rawUserMemory().prepareReplacement(destination, length, raw);
    if (rawStatus != MemoryLockStatus::Success)
      return nullptr;
    MappingPlan plan;
    if (!plan.staged.tryReserve(objects->count() * 2 + 1) ||
        !plan.retired.tryReserve(objects->count()))
      return nullptr;
    plan.replacement = new MmObjectList;
    if (!plan.replacement)
      return nullptr;
    size_t removedPages = 0, stagedPages = 0;
    for (auto* object : *objects) {
      const uintptr_t end = (object->address() + object->length() + mask) & ~mask;
      if (destination >= end || object->address() >= destination + length) {
        if (!plan.replacement->tryPushBack(object))
          return nullptr;
        continue;
      }
      if (placement != Placement::FixedReplace)
        return nullptr;
      const uintptr_t first = object->address() > destination ? object->address() : destination;
      const uintptr_t last = end < destination + length ? end : destination + length;
      if (object->m_LockMode != MemoryLockMode::None)
        removedPages += (last - first) / pageSize;
      const size_t pages = (end - object->address()) / pageSize;
      if (pages > VirtualAddressSpace::MaximumRemapPages - stagedPages)
        return nullptr;
      stagedPages += pages;
      plan.retired.pushBack(object);
      if (!plan.appendSlice(object, object->address(), first) ||
          !plan.appendSlice(object, last, end))
        return nullptr;
    }
    auto charge = account ? account->charge() : MemoryLockCharge{};
    if (account) {
      assert(removedPages <= charge.managedPages && raw.get()->removedPages() <= charge.rawPages);
      charge.managedPages -= removedPages;
      charge.rawPages -= raw.get()->removedPages();
      if (mode != MemoryLockMode::None) {
        const size_t added = length / pageSize;
        if (added > ~size_t(0) - charge.managedPages)
          return nullptr;
        charge.managedPages += added;
        if (charge.rawPages > ~size_t(0) - charge.managedPages ||
            !account->permitsTotalPages(charge.managedPages + charge.rawPages,
                                        process->getEffectiveUserId() == 0)) {
          if (status)
            *status = MapStatus::LockLimit;
          return nullptr;
        }
      }
    }
    plan.inserted =
        file ? static_cast<MemoryMappedObject*>(
                   new MemoryMappedFile(destination, actualLength, offset, file, copyOnWrite, perms,
                                        maximumPerms, attachment, origin))
             : static_cast<MemoryMappedObject*>(new AnonymousMemoryMap(destination, length, perms));
    if (!plan.inserted)
      return nullptr;
    plan.inserted->m_OwnsMappings = false;
    plan.inserted->m_LockMode = mode;
    plan.staged.pushBack(plan.inserted);
    if (!plan.replacement->tryPushBack(plan.inserted) || plan.replacement->count() > MaximumObjects)
      return nullptr;
    if (!process->commitUserReservations(snapshot.generation, snapshot))
      continue;
    // Every recoverable preparation failure precedes retirement. Latest PTEs
    // are detached by each owner, preserving independent stale CoW cache loans.
    for (auto* object : plan.retired) {
      const uintptr_t end = (object->address() + object->length() + mask) & ~mask;
      const uintptr_t first = object->address() > destination ? object->address() : destination;
      const uintptr_t last = end < destination + length ? end : destination + length;
      object->discardRange(space, first, last - first);
      object->m_OwnsMappings = false;
    }
    raw.get()->commit();
    for (auto* object : plan.staged)
      object->m_OwnsMappings = true;
    {
      LockGuard<Spinlock> guard(m_Lock);
      m_MmObjectLists.insert(&space, plan.replacement);
    }
    plan.replacement = nullptr;
    plan.committed = true;
    delete objects;
    if (account)
      account->publish(charge, account->futureMode());
    address = destination;
    if (status)
      *status = MapStatus::Success;
    if (mode == MemoryLockMode::Eager)
      for (uintptr_t page = destination; page < destination + length; page += pageSize)
        plan.inserted->populatePage(space, page);
    return plan.inserted;
  }
  return nullptr;
}

size_t MemoryMapManager::removeInternal(uintptr_t base, size_t length, bool releaseReservations,
                                        VmStatus* status) {
  OperationGuard operation(*this);
  if (status)
    *status = VmStatus::InvalidRange;
  const size_t pageSize = PhysicalMemoryManager::getPageSize(), mask = pageSize - 1;
  if (!length || (base & mask) || length > ~size_t(0) - mask)
    return 0;
  length = (length + mask) & ~mask;
  if (length > ~uintptr_t(0) - base)
    return 0;
  auto& space = Processor::information().getVirtualAddressSpace();
  auto* objects = m_MmObjectLists.lookup(&space);
  if (!objects) {
    if (status)
      *status = VmStatus::Success;
    return 0;
  }
  if (status)
    *status = VmStatus::NoMemory;
  MappingPlan plan;
  if (objects->count() > 4096 || !plan.staged.tryReserve(objects->count() * 2) ||
      !plan.retired.tryReserve(objects->count()))
    return 0;
  plan.replacement = new MmObjectList;
  if (!plan.replacement)
    return 0;
  size_t removedPages = 0, stagedPages = 0;
  for (auto* object : *objects) {
    const uintptr_t end = (object->address() + object->length() + mask) & ~mask;
    const uintptr_t first = object->address() > base ? object->address() : base;
    const uintptr_t last = end < base + length ? end : base + length;
    if (first >= last) {
      if (!plan.replacement->tryPushBack(object))
        return 0;
      continue;
    }
    const size_t pages = (end - object->address()) / pageSize;
    if (pages > VirtualAddressSpace::MaximumRemapPages - stagedPages)
      return 0;
    stagedPages += pages;
    if (object->m_LockMode != MemoryLockMode::None)
      removedPages += (last - first) / pageSize;
    plan.retired.pushBack(object);
    if (!plan.appendSlice(object, object->address(), first) || !plan.appendSlice(object, last, end))
      return 0;
  }
  if (plan.replacement->count() > 4096)
    return 0;
  auto* process = Processor::information().getCurrentThread()->getParent();
  for (auto* object : plan.retired) {
    const uintptr_t end = (object->address() + object->length() + mask) & ~mask;
    const uintptr_t first = object->address() > base ? object->address() : base;
    const uintptr_t last = end < base + length ? end : base + length;
    object->discardRange(space, first, last - first);
    object->m_OwnsMappings = false;
    if (releaseReservations)
      releaseReservation(process, space, first, last - first);
  }
  for (auto* object : plan.staged)
    object->m_OwnsMappings = true;
  {
    LockGuard<Spinlock> guard(m_Lock);
    m_MmObjectLists.insert(&space, plan.replacement);
  }
  plan.replacement = nullptr;
  plan.committed = true;
  delete objects;
  retireLockedPages(space, removedPages);
  if (status)
    *status = VmStatus::Success;
  return plan.retired.count();
}
