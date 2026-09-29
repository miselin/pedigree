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
  MappingList<MemoryMappedObject>* replacement = nullptr;
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
    // Only surviving tracked pages need staging; sparse virtual extent is
    // not a useful bound on the metadata needed to split or retire an owner.
    auto* object = owner->stageSlice(first, end - first, first, end - first);
    if (!object)
      return false;
    staged.pushBack(object);
    return replacement->tryPushBack(object);
  }
};

class DirectReservation {
 public:
  DirectReservation()
      : m_Process(nullptr),
        m_Region(Process::UserRegion::Normal),
        m_Base(0),
        m_Length(0),
        m_Committed(false) {}

  void arm(Process* process, Process::UserRegion region, uintptr_t base, size_t length) {
    m_Process = process;
    m_Region = region;
    m_Base = base;
    m_Length = length;
  }

  void commit() {
    m_Committed = true;
  }

  ~DirectReservation() {
    if (m_Process && !m_Committed)
      m_Process->freeUserRange(m_Region, m_Base, m_Length);
  }

 private:
  Process* m_Process;
  Process::UserRegion m_Region;
  uintptr_t m_Base;
  size_t m_Length;
  bool m_Committed;
};

bool allocateDirect(Process& process, VirtualAddressSpace& space, size_t length, size_t mask,
                    uintptr_t& address, Process::UserRegion& region) {
  const size_t allocationLength = length + mask;
  auto allocate = [&](Process::UserRegion candidate) {
    uintptr_t allocation = 0;
    if (!process.allocateUserRange(candidate, allocationLength, allocation))
      return false;
    if (allocation > ~uintptr_t(0) - mask) {
      process.freeUserRange(candidate, allocation, allocationLength);
      return false;
    }
    const uintptr_t aligned = (allocation + mask) & ~mask;
    if (aligned > ~uintptr_t(0) - length || allocationLength > ~uintptr_t(0) - allocation) {
      process.freeUserRange(candidate, allocation, allocationLength);
      return false;
    }
    const uintptr_t allocationEnd = allocation + allocationLength;
    const uintptr_t usedEnd = aligned + length;
    if (aligned != allocation)
      process.freeUserRange(candidate, allocation, aligned - allocation);
    if (usedEnd != allocationEnd)
      process.freeUserRange(candidate, usedEnd, allocationEnd - usedEnd);
    address = aligned;
    region = candidate;
    return true;
  };

  if (space.getDynamicStart() && allocate(Process::UserRegion::Dynamic))
    return true;
  return allocate(Process::UserRegion::Normal);
}
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
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  process->recordBenchmarkVmCounter(Process::VmPublishCalls);
#endif
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
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  process->recordBenchmarkVmCounter(Process::VmPublishObjectCount, objects->count());
#endif
  const uintptr_t requested = address;
  const bool directPlacement = requested == 0 && placement == Placement::Hint;
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    Snapshot snapshot;
    uintptr_t destination = requested;
    Process::UserRegion directRegion = Process::UserRegion::Normal;
    DirectReservation directReservation;
    bool direct = false;
    if (directPlacement) {
      direct = allocateDirect(*process, space, length, mask, destination, directRegion);
      if (!direct)
        return nullptr;
      directReservation.arm(process, directRegion, destination, length);
    } else {
      if (!process->snapshotUserReservations(snapshot))
        return nullptr;
      auto placementStatus = place(snapshot, space, destination, length, placement, mask);
      if (placementStatus != MapStatus::Success) {
        if (status)
          *status = placementStatus;
        return nullptr;
      }
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
    bool overlaps = false;
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    size_t objectVisits = 0;
#endif
    if (!direct)
      for (auto* object : *objects) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
        ++objectVisits;
#endif
        const uintptr_t end = (object->address() + object->length() + mask) & ~mask;
        if (destination < end && object->address() < destination + length) {
          overlaps = true;
          break;
        }
      }
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    process->recordBenchmarkVmCounter(Process::VmPublishOverlapProbeVisits, objectVisits);
    if (overlaps)
      process->recordBenchmarkVmCounter(Process::VmPublishOverlapHits);
#endif
    MappingPlan plan;
    if (!plan.staged.tryReserve(overlaps ? objects->count() * 2 + 1 : 1))
      return nullptr;
    size_t removedPages = 0;
    if (overlaps) {
      if (!plan.retired.tryReserve(objects->count()))
        return nullptr;
      plan.replacement = new MmObjectList;
      if (!plan.replacement)
        return nullptr;
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
        plan.retired.pushBack(object);
        if (!plan.appendSlice(object, object->address(), first) ||
            !plan.appendSlice(object, last, end))
          return nullptr;
      }
    }
    auto charge = account ? account->charge() : MemoryLockCharge{};
    if (account) {
      const size_t rawRemovedPages = raw ? raw.get()->removedPages() : 0;
      assert(removedPages <= charge.managedPages && rawRemovedPages <= charge.rawPages);
      charge.managedPages -= removedPages;
      charge.rawPages -= rawRemovedPages;
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
    plan.inserted->m_OwnerProcess = process->addressSpaceOwner();
    plan.inserted->m_OwnsMappings = false;
    plan.inserted->m_LockMode = mode;
    plan.staged.pushBack(plan.inserted);
    if (file && !static_cast<MemoryMappedFile*>(plan.inserted)->m_UseAdmitted) {
      if (status)
        *status = MapStatus::TextBusy;
      return nullptr;
    }
    auto* publication = overlaps ? plan.replacement : objects;
    if (publication->count() >= MaximumObjects || !publication->tryPushBack(plan.inserted))
      return nullptr;
    if (!direct && !process->commitUserReservations(snapshot.generation, snapshot)) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
      process->recordBenchmarkVmCounter(Process::VmPublishCommitRetries);
#endif
      // The operation gate excludes other executions; the allocation-free
      // reservation commit cannot reenter this registry. Undo the provisional
      // append before destroying the staged object.
      if (!overlaps) {
        [[maybe_unused]] auto* removed = objects->popBack();
        assert(removed == plan.inserted);
      }
      continue;
    }
    directReservation.commit();
    // Every recoverable preparation failure precedes retirement. Latest PTEs
    // are detached by each owner, preserving independent stale CoW cache loans.
    for (auto* object : plan.retired) {
      const uintptr_t end = (object->address() + object->length() + mask) & ~mask;
      const uintptr_t first = object->address() > destination ? object->address() : destination;
      const uintptr_t last = end < destination + length ? end : destination + length;
      object->discardRange(space, first, last - first);
      object->m_OwnsMappings = false;
    }
    if (raw)
      raw.get()->commit();
    for (auto* object : plan.staged)
      object->m_OwnsMappings = true;
    if (overlaps) {
      LockGuard<Spinlock> guard(m_Lock);
      m_MmObjectLists.insert(&space, plan.replacement);
    }
    plan.replacement = nullptr;
    plan.committed = true;
    if (overlaps)
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
  if (objects->count() > 4096)
    return 0;
  auto* process = Processor::information().getCurrentThread()->getParent();
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  process->recordBenchmarkVmCounter(Process::VmRemoveCalls);
  process->recordBenchmarkVmCounter(Process::VmRemoveObjectCount, objects->count());
  size_t objectVisits = 0;
#endif

  // Mmap users normally unmap the exact mapping they just created.
  // The registry is append-ordered, so find and retire that common case from
  // the tail without rebuilding a reservation or mapping snapshot.
  for (auto it = objects->rbegin(); it != objects->rend(); ++it) {
    auto* object = *it;
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    ++objectVisits;
#endif
    const uintptr_t objectEnd = (object->address() + object->length() + mask) & ~mask;
    if (object->address() != base || objectEnd != base + length)
      continue;

    const size_t removedPages =
        object->m_LockMode == MemoryLockMode::None ? 0 : (objectEnd - base) / pageSize;
    object->discardRange(space, base, length);
    object->m_OwnsMappings = false;
    if (releaseReservations)
      releaseReservation(process, space, base, length);
    objects->erase(it);
    delete object;
    retireLockedPages(space, removedPages);
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    process->recordBenchmarkVmCounter(Process::VmRemoveObjectVisits, objectVisits);
    process->recordBenchmarkVmCounter(Process::VmRemoveAffectedObjects, 1);
#endif
    if (status)
      *status = VmStatus::Success;
    return 1;
  }

#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  objectVisits = 0;
#endif
  bool needsSlices = false;
  for (auto* object : *objects) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    ++objectVisits;
#endif
    const uintptr_t end = (object->address() + object->length() + mask) & ~mask;
    if (object->address() < base + length && base < end &&
        (object->address() < base || end > base + length)) {
      needsSlices = true;
      break;
    }
  }
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  process->recordBenchmarkVmCounter(Process::VmRemoveObjectVisits, objectVisits);
#endif
  if (!needsSlices) {
    size_t affected = 0, removedPages = 0;
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    objectVisits = 0;
#endif
    for (auto it = objects->begin(); it != objects->end();) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
      ++objectVisits;
#endif
      auto* object = *it;
      const uintptr_t first = object->address();
      const uintptr_t end = (first + object->length() + mask) & ~mask;
      if (first >= base + length || end <= base) {
        ++it;
        continue;
      }
      if (object->m_LockMode != MemoryLockMode::None)
        removedPages += (end - first) / pageSize;
      object->discardRange(space, first, end - first);
      object->m_OwnsMappings = false;
      if (releaseReservations)
        releaseReservation(process, space, first, end - first);
      it = objects->erase(it);
      delete object;
      ++affected;
    }
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    process->recordBenchmarkVmCounter(Process::VmRemoveObjectVisits, objectVisits);
    process->recordBenchmarkVmCounter(Process::VmRemoveAffectedObjects, affected);
#endif
    retireLockedPages(space, removedPages);
    if (status)
      *status = VmStatus::Success;
    return affected;
  }
  MappingPlan plan;
  if (!plan.staged.tryReserve(objects->count() * 2) || !plan.retired.tryReserve(objects->count()))
    return 0;
  plan.replacement = new MmObjectList;
  if (!plan.replacement)
    return 0;
  size_t removedPages = 0;
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  objectVisits = 0;
#endif
  for (auto* object : *objects) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    ++objectVisits;
#endif
    const uintptr_t end = (object->address() + object->length() + mask) & ~mask;
    const uintptr_t first = object->address() > base ? object->address() : base;
    const uintptr_t last = end < base + length ? end : base + length;
    if (first >= last) {
      if (!plan.replacement->tryPushBack(object))
        return 0;
      continue;
    }
    if (object->m_LockMode != MemoryLockMode::None)
      removedPages += (last - first) / pageSize;
    plan.retired.pushBack(object);
    if (!plan.appendSlice(object, object->address(), first) || !plan.appendSlice(object, last, end))
      return 0;
  }
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  process->recordBenchmarkVmCounter(Process::VmRemoveObjectVisits, objectVisits);
  process->recordBenchmarkVmCounter(Process::VmRemoveSliceCalls);
  process->recordBenchmarkVmCounter(Process::VmRemoveAffectedObjects, plan.retired.count());
#endif
  if (plan.replacement->count() > 4096)
    return 0;
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
