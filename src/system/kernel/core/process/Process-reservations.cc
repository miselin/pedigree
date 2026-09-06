#include "pedigree/kernel/process/Process.h"

#if THREADS
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/Pointers.h"

namespace {
struct ReservationRange {
  uintptr_t address;
  size_t length;
};
}  // namespace

bool Process::snapshotUserReservations(UserReservationSnapshot& result) {
  UniqueArray<ReservationRange> ranges;
  size_t capacity = 0, normalCount = 0, dynamicCount = 0;
  uint64_t generation = 0;
  for (;;) {
    size_t needed = 0;
    {
      LockGuard<Spinlock> guard(m_UserReservationLock);
      normalCount = m_SpaceAllocator.size();
      dynamicCount = m_DynamicSpaceAllocator.size();
      if (normalCount > (~size_t{0} / sizeof(ReservationRange)) ||
          dynamicCount > (~size_t{0} / sizeof(ReservationRange)) - normalCount) {
        return false;
      }
      needed = normalCount + dynamicCount;
      if (needed <= capacity) {
        MemoryAllocator::Range range(0, 0);
        for (size_t i = 0; i < normalCount; ++i) {
          m_SpaceAllocator.getRange(i, range);
          ranges.get()[i] = {range.address, range.length};
        }
        for (size_t i = 0; i < dynamicCount; ++i) {
          m_DynamicSpaceAllocator.getRange(i, range);
          ranges.get()[normalCount + i] = {range.address, range.length};
        }
        generation = m_UserReservationGeneration;
        break;
      }
    }
    // Allocation can reclaim mappings; it must not hold the reservation lock.
    ranges = UniqueArray<ReservationRange>::allocate(needed);
    if (!ranges) {
      return false;
    }
    capacity = needed;
  }

  UserReservationSnapshot snapshot;
  for (size_t i = 0; i < normalCount; ++i) {
    const auto& range = ranges.get()[i];
    if (range.length && !snapshot.normal.tryFree(range.address, range.length, false)) {
      return false;
    }
  }
  for (size_t i = 0; i < dynamicCount; ++i) {
    const auto& range = ranges.get()[normalCount + i];
    if (range.length && !snapshot.dynamic.tryFree(range.address, range.length, false)) {
      return false;
    }
  }
  result.normal.swap(snapshot.normal);
  result.dynamic.swap(snapshot.dynamic);
  result.generation = generation;
  return true;
}

bool Process::commitUserReservations(uint64_t expectedGeneration,
                                     UserReservationSnapshot& replacement) {
  LockGuard<Spinlock> guard(m_UserReservationLock);
  if (m_UserReservationGeneration != expectedGeneration) {
    return false;
  }
  m_SpaceAllocator.swap(replacement.normal);
  m_DynamicSpaceAllocator.swap(replacement.dynamic);
  ++m_UserReservationGeneration;
  replacement.generation = m_UserReservationGeneration;
  return true;
}

bool Process::allocateUserRange(UserRegion region, size_t length, uintptr_t& address) {
  if (!length) {
    return false;
  }
  for (;;) {
    {
      LockGuard<Spinlock> guard(m_UserReservationLock);
      auto& allocator = region == UserRegion::Dynamic ? m_DynamicSpaceAllocator : m_SpaceAllocator;
      if (allocator.allocateWithoutAllocation(length, address)) {
        ++m_UserReservationGeneration;
        return true;
      }
    }
    UserReservationSnapshot snapshot;
    if (!snapshotUserReservations(snapshot)) {
      return false;
    }
    MemoryAllocator& allocator = region == UserRegion::Dynamic ? snapshot.dynamic : snapshot.normal;
    uintptr_t candidate = 0;
    if (!allocator.allocate(length, candidate)) {
      return false;
    }
    if (commitUserReservations(snapshot.generation, snapshot)) {
      address = candidate;
      return true;
    }
  }
}

bool Process::allocateSpecificUserRange(UserRegion region, uintptr_t address, size_t length) {
  if (!length || length > ~uintptr_t{0} - address) {
    return false;
  }
  for (;;) {
    {
      LockGuard<Spinlock> guard(m_UserReservationLock);
      auto& allocator = region == UserRegion::Dynamic ? m_DynamicSpaceAllocator : m_SpaceAllocator;
      if (allocator.allocateSpecificWithoutAllocation(address, length)) {
        ++m_UserReservationGeneration;
        return true;
      }
    }
    UserReservationSnapshot snapshot;
    if (!snapshotUserReservations(snapshot)) {
      return false;
    }
    MemoryAllocator& allocator = region == UserRegion::Dynamic ? snapshot.dynamic : snapshot.normal;
    if (!allocator.allocateSpecific(address, length)) {
      return false;
    }
    if (commitUserReservations(snapshot.generation, snapshot)) {
      return true;
    }
  }
}

void Process::freeUserRange(UserRegion region, uintptr_t address, size_t length) {
  if (!length) {
    return;
  }
  if (length > ~uintptr_t{0} - address) {
    FATAL("Invalid process reservation release");
  }
  for (;;) {
    {
      LockGuard<Spinlock> guard(m_UserReservationLock);
      auto& allocator = region == UserRegion::Dynamic ? m_DynamicSpaceAllocator : m_SpaceAllocator;
      if (allocator.freeWithoutAllocation(address, length)) {
        ++m_UserReservationGeneration;
        return;
      }
    }
    // Only creating a new free extent needs storage. Exhausted nodes and
    // adjacent extents above keep ordinary teardown allocation-free.
    UserReservationSnapshot snapshot;
    if (!snapshotUserReservations(snapshot)) {
      FATAL("Cannot grow process reservation metadata during release");
    }
    MemoryAllocator& allocator = region == UserRegion::Dynamic ? snapshot.dynamic : snapshot.normal;
    if (!allocator.tryFree(address, length)) {
      FATAL("Cannot grow process reservation metadata during release");
    }
    if (commitUserReservations(snapshot.generation, snapshot)) {
      return;
    }
  }
}

void Process::resetUserReservations() {
  for (;;) {
    UserReservationSnapshot snapshot;
    {
      LockGuard<Spinlock> guard(m_UserReservationLock);
      snapshot.generation = m_UserReservationGeneration;
    }
    snapshot.normal.free(
        getAddressSpace()->getUserStart(),
        getAddressSpace()->getUserReservedStart() - getAddressSpace()->getUserStart());
    if (getAddressSpace()->getDynamicStart()) {
      snapshot.dynamic.free(
          getAddressSpace()->getDynamicStart(),
          getAddressSpace()->getDynamicEnd() - getAddressSpace()->getDynamicStart());
    }
    if (commitUserReservations(snapshot.generation, snapshot)) {
      return;
    }
  }
}
#endif
