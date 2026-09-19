/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/UserMemoryPolicy.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"

namespace {
constexpr size_t MaximumRawSegments = 4096;
size_t pageSize() {
  return PhysicalMemoryManager::getPageSize();
}
bool validRange(uintptr_t base, size_t length) {
  return !(base % pageSize()) && !(length % pageSize()) && length <= ~uintptr_t(0) - base;
}
}  // namespace

struct RawUserMemory::State {
  struct Entry {
    UserRegion region;
    MemoryLockMode mode;
  };
  Vector<Entry> entries;
  uint64_t nextId = 1;
  uint64_t epoch = 0;
  bool complete = false;
};

class RawUserMemory::Plan final : public PreparedMemoryLock {
 public:
  explicit Plan(RawUserMemory& owner) : owner(owner), epoch(owner.m_State.get()->epoch) {}
  size_t removedPages() const override {
    return removed;
  }
  size_t addedPages() const override {
    return added;
  }
  size_t coveredPages() const override {
    return covered;
  }
  size_t eligiblePages() const override {
    return eligible;
  }
  size_t removedRangeCount() const override {
    return retired.count();
  }
  const MemoryLockRange* removedRanges() const override {
    return retired.count() ? &retired[0] : nullptr;
  }
  void commit() override {
    assert(!committed && owner.m_State.get()->epoch == epoch);
    for (const MemoryLockRange& region : retired) {
      for (size_t offset = 0; offset < region.length; offset += pageSize()) {
        physical_uintptr_t physical = 0;
        size_t flags = 0;
        void* address = reinterpret_cast<void*>(region.base + offset);
        if (owner.m_Space.detachMapping(address, physical, flags) &&
            !(flags & (VirtualAddressSpace::Shared | VirtualAddressSpace::Borrowed |
                       VirtualAddressSpace::Swapped)))
          PhysicalMemoryManager::instance().freePage(physical);
      }
    }
    owner.m_State.get()->entries.swap(entries);
    ++owner.m_State.get()->epoch;
    committed = true;
  }
  PopulationStatus populate() override {
    for (const UserRegion& region : populateRegions) {
      for (size_t offset = 0; offset < region.length; offset += pageSize()) {
        void* address = reinterpret_cast<void*>(region.base + offset);
        if (!owner.m_Space.isMapped(address))
          return PopulationStatus::Inaccessible;
        physical_uintptr_t physical = 0;
        size_t flags = 0;
        owner.m_Space.getMapping(address, physical, flags);
        if (flags & (VirtualAddressSpace::NoAccess | VirtualAddressSpace::KernelMode))
          return PopulationStatus::Inaccessible;
        if (region.privateWritable && (flags & VirtualAddressSpace::CopyOnWrite)) {
          if (flags & VirtualAddressSpace::WriteProtected)
            return PopulationStatus::Inaccessible;
          if (!owner.m_Space.handleCopyOnWriteFault(address, true))
            return PopulationStatus::NoMemory;
        }
      }
    }
    return PopulationStatus::Success;
  }
  bool append(const State::Entry& entry) {
    if (!entry.region.length)
      return true;
    if (entries.count() == MaximumRawSegments || !entries.tryReserve(entries.count() + 1))
      return false;
    entries.pushBack(entry);
    return true;
  }
  bool appendPart(const State::Entry& entry, uintptr_t base, size_t length, MemoryLockMode mode) {
    State::Entry part = entry;
    part.region.base = base;
    part.region.length = length;
    part.mode = mode;
    return append(part);
  }
  bool retire(const State::Entry& entry, uintptr_t base, size_t length) {
    if (!length)
      return true;
    if (!retired.tryReserve(retired.count() + 1))
      return false;
    retired.pushBack({base, length});
    if (entry.mode != MemoryLockMode::None)
      removed += length / pageSize();
    return true;
  }
  bool populateLater(UserRegion region) {
    if (!region.length)
      return true;
    if (!populateRegions.tryReserve(populateRegions.count() + 1))
      return false;
    populateRegions.pushBack(region);
    return true;
  }
  bool addRegion(UserRegion region, MemoryLockMode mode) {
    if (!region.length)
      return true;
    if (UserMemoryPolicy* policy = owner.m_Space.userMemoryPolicy()) {
      if (policy->overlapsManagedMemory(owner.m_Space, region.base, region.length))
        return false;
    }
    for (const auto& entry : entries) {
      if (entry.region.base < region.base + region.length &&
          region.base < entry.region.base + entry.region.length)
        return false;
    }
    if (!append({region, mode}))
      return false;
    if (mode != MemoryLockMode::None)
      added += region.length / pageSize();
    return mode != MemoryLockMode::Eager || populateLater(region);
  }
  void sort() {
    for (size_t i = 1; i < entries.count(); ++i) {
      State::Entry value = entries[i];
      size_t j = i;
      while (j && entries[j - 1].region.base > value.region.base) {
        entries[j] = entries[j - 1];
        --j;
      }
      entries[j] = value;
    }
    size_t kept = 0;
    for (size_t i = 0; i < entries.count(); ++i) {
      const auto& current = entries[i];
      if (kept) {
        auto& previous = entries[kept - 1];
        if (previous.region.id == current.region.id && previous.mode == current.mode &&
            previous.region.base + previous.region.length == current.region.base &&
            previous.region.kind == current.region.kind &&
            previous.region.privateWritable == current.region.privateWritable) {
          previous.region.length += current.region.length;
          continue;
        }
      }
      entries[kept++] = current;
    }
    while (entries.count() > kept)
      entries.popBack();
  }
  static Plan* create(RawUserMemory& owner, UniquePointer<PreparedMemoryLock>& result) {
    result.reset();
    if (!owner.m_State)
      owner.m_State = UniquePointer<State>::adopt(new State);
    if (!owner.m_State)
      return nullptr;
    Plan* plan = new Plan(owner);
    result = UniquePointer<PreparedMemoryLock>::adopt(plan);
    return plan;
  }
  RawUserMemory& owner;
  uint64_t epoch;
  Vector<State::Entry> entries;
  Vector<MemoryLockRange> retired;
  Vector<UserRegion> populateRegions;
  size_t removed = 0, added = 0, covered = 0, eligible = 0;
  bool committed = false;
};

RawUserMemory::RawUserMemory(VirtualAddressSpace& space) : m_Space(space), m_State() {}
RawUserMemory::~RawUserMemory() = default;

uint64_t RawUserMemory::nextRegionId() {
  if (!m_State)
    m_State = UniquePointer<State>::adopt(new State);
  if (!m_State || !m_State.get()->nextId)
    return 0;
  return m_State.get()->nextId++;
}

bool RawUserMemory::completeInventory() const {
  return m_State && m_State.get()->complete;
}
void RawUserMemory::setCompleteInventory(bool complete) {
  if (!m_State)
    m_State = UniquePointer<State>::adopt(new State);
  if (m_State)
    m_State.get()->complete = complete;
}

bool RawUserMemory::covers(uintptr_t base, size_t length) const {
  if (!m_State || !length || length > ~uintptr_t(0) - base)
    return false;
  const uintptr_t end = base + length;
  for (const auto& entry : m_State.get()->entries) {
    const uintptr_t regionEnd = entry.region.base + entry.region.length;
    if (regionEnd <= base)
      continue;
    if (entry.region.base > base)
      return false;
    if (regionEnd >= end)
      return true;
    base = regionEnd;
  }
  return false;
}

bool RawUserMemory::hasLockedMemory(uintptr_t base, size_t length) const {
  if (!m_State || length > ~uintptr_t(0) - base)
    return false;
  for (const auto& entry : m_State.get()->entries) {
    if (entry.mode != MemoryLockMode::None && entry.region.base < base + length &&
        base < entry.region.base + entry.region.length)
      return true;
  }
  return false;
}

MemoryLockStatus RawUserMemory::prepareChange(const UserRegion* previous,
                                              const UserRegion* replacement,
                                              UniquePointer<PreparedMemoryLock>& result) {
  if ((!previous && !replacement) ||
      (previous && (!previous->id || !validRange(previous->base, previous->length))) ||
      (replacement && (!replacement->id || !validRange(replacement->base, replacement->length))) ||
      (previous && replacement && previous->id != replacement->id))
    return MemoryLockStatus::InvalidRange;
  UniquePointer<PreparedMemoryLock> prepared;
  Plan* plan = Plan::create(*this, prepared);
  if (!plan)
    return MemoryLockStatus::NoMemory;
  for (const auto& entry : m_State.get()->entries) {
    if (!previous || entry.region.id != previous->id) {
      if (!plan->append(entry))
        return MemoryLockStatus::NoMemory;
      continue;
    }
    const uintptr_t end = entry.region.base + entry.region.length;
    const uintptr_t keepStart = replacement && replacement->base > entry.region.base
                                    ? replacement->base
                                    : entry.region.base;
    const uintptr_t replacementEnd = replacement ? replacement->base + replacement->length : 0;
    const uintptr_t keepEnd = replacementEnd < end ? replacementEnd : end;
    if (!replacement || keepStart >= keepEnd) {
      if (!plan->retire(entry, entry.region.base, entry.region.length))
        return MemoryLockStatus::NoMemory;
    } else if (!plan->retire(entry, entry.region.base, keepStart - entry.region.base) ||
               !plan->appendPart(entry, keepStart, keepEnd - keepStart, entry.mode) ||
               !plan->retire(entry, keepEnd, end - keepEnd)) {
      return MemoryLockStatus::NoMemory;
    }
  }
  if (replacement) {
    MemoryLockAccount* account = m_Space.memoryLockAccount();
    const MemoryLockMode future = account ? account->futureMode() : MemoryLockMode::None;
    UserRegion added = *replacement;
    if (!previous) {
      if (!plan->addRegion(added, future))
        return MemoryLockStatus::NoMemory;
    } else {
      // A fixed mapping may have punched holes in an allocation. Preserve them
      // when its logical heap end grows; only admit the newly added tail.
      if (replacement->base < previous->base) {
        added.length = previous->base - replacement->base;
        if (added.length > replacement->length)
          added.length = replacement->length;
        if (!plan->addRegion(added, future))
          return MemoryLockStatus::NoMemory;
      }
      const uintptr_t oldEnd = previous->base + previous->length;
      const uintptr_t newEnd = replacement->base + replacement->length;
      if (newEnd > oldEnd) {
        added.base = oldEnd > replacement->base ? oldEnd : replacement->base;
        added.length = newEnd - added.base;
        if (!plan->addRegion(added, future))
          return MemoryLockStatus::NoMemory;
      }
    }
  }
  plan->sort();
  result = pedigree_std::move(prepared);
  return MemoryLockStatus::Success;
}

MemoryLockStatus RawUserMemory::prepareReplacement(uintptr_t base, size_t length,
                                                   UniquePointer<PreparedMemoryLock>& result) {
  if (!validRange(base, length))
    return MemoryLockStatus::InvalidRange;
  bool overlaps = false;
  if (m_State && length) {
    const uintptr_t end = base + length;
    for (const auto& entry : m_State.get()->entries) {
      if (entry.region.base >= end)
        break;
      if (base < entry.region.base + entry.region.length) {
        overlaps = true;
        break;
      }
    }
  }
  if (!overlaps) {
    result.reset();
    return MemoryLockStatus::Success;
  }
  UniquePointer<PreparedMemoryLock> prepared;
  Plan* plan = Plan::create(*this, prepared);
  if (!plan)
    return MemoryLockStatus::NoMemory;
  for (const auto& entry : m_State.get()->entries) {
    const uintptr_t end = entry.region.base + entry.region.length;
    const uintptr_t first = base > entry.region.base ? base : entry.region.base;
    const uintptr_t last = base + length < end ? base + length : end;
    if (first >= last) {
      if (!plan->append(entry))
        return MemoryLockStatus::NoMemory;
    } else {
      plan->covered += (last - first) / pageSize();
      if (!plan->appendPart(entry, entry.region.base, first - entry.region.base, entry.mode) ||
          !plan->retire(entry, first, last - first) ||
          !plan->appendPart(entry, last, end - last, entry.mode))
        return MemoryLockStatus::NoMemory;
    }
  }
  plan->sort();
  result = pedigree_std::move(prepared);
  return MemoryLockStatus::Success;
}

MemoryLockStatus RawUserMemory::prepareLocks(uintptr_t base, size_t length, MemoryLockMode mode,
                                             UniquePointer<PreparedMemoryLock>& result) {
  if (!validRange(base, length))
    return MemoryLockStatus::InvalidRange;
  UniquePointer<PreparedMemoryLock> prepared;
  Plan* plan = Plan::create(*this, prepared);
  if (!plan)
    return MemoryLockStatus::NoMemory;
  for (const auto& entry : m_State.get()->entries) {
    const uintptr_t end = entry.region.base + entry.region.length;
    const uintptr_t first = base > entry.region.base ? base : entry.region.base;
    const uintptr_t last = base + length < end ? base + length : end;
    if (first >= last) {
      if (!plan->append(entry))
        return MemoryLockStatus::NoMemory;
      continue;
    }
    const size_t pages = (last - first) / pageSize();
    plan->covered += pages;
    plan->eligible += pages;
    if (entry.mode != MemoryLockMode::None)
      plan->removed += pages;
    if (mode != MemoryLockMode::None)
      plan->added += pages;
    if (!plan->appendPart(entry, entry.region.base, first - entry.region.base, entry.mode) ||
        !plan->appendPart(entry, first, last - first, mode) ||
        !plan->appendPart(entry, last, end - last, entry.mode))
      return MemoryLockStatus::NoMemory;
    if (mode == MemoryLockMode::Eager) {
      UserRegion region = entry.region;
      region.base = first;
      region.length = last - first;
      if (!plan->populateLater(region))
        return MemoryLockStatus::NoMemory;
    }
  }
  // Skip absent page-table subtrees when the range spans large user holes.
  plan->covered += m_Space.runtimeMappingPages(base, length);
  plan->sort();
  result = pedigree_std::move(prepared);
  return MemoryLockStatus::Success;
}

MemoryLockStatus RawUserMemory::prepareAllLocks(MemoryLockMode mode,
                                                UniquePointer<PreparedMemoryLock>& result) {
  if (mode != MemoryLockMode::None && !completeInventory())
    return MemoryLockStatus::Unsupported;
  UniquePointer<PreparedMemoryLock> prepared;
  Plan* plan = Plan::create(*this, prepared);
  if (!plan)
    return MemoryLockStatus::NoMemory;
  for (const auto& entry : m_State.get()->entries) {
    const size_t pages = entry.region.length / pageSize();
    plan->covered += pages;
    plan->eligible += pages;
    if (entry.mode != MemoryLockMode::None)
      plan->removed += pages;
    if (mode != MemoryLockMode::None)
      plan->added += pages;
    if (!plan->appendPart(entry, entry.region.base, entry.region.length, mode) ||
        (mode == MemoryLockMode::Eager && !plan->populateLater(entry.region)))
      return MemoryLockStatus::NoMemory;
  }
  plan->sort();
  result = pedigree_std::move(prepared);
  return MemoryLockStatus::Success;
}

MemoryLockStatus RawUserMemory::cloneInto(RawUserMemory& target) {
  if (&target == this)
    return MemoryLockStatus::InvalidRange;
  auto copied = UniquePointer<State>::adopt(new State);
  if (!copied)
    return MemoryLockStatus::NoMemory;
  if (m_State) {
    copied.get()->complete = m_State.get()->complete;
    copied.get()->nextId = m_State.get()->nextId;
    if (!copied.get()->entries.tryReserve(m_State.get()->entries.count()))
      return MemoryLockStatus::NoMemory;
    for (auto entry : m_State.get()->entries) {
      entry.mode = MemoryLockMode::None;
      copied.get()->entries.pushBack(entry);
    }
  }
  target.m_State = pedigree_std::move(copied);
  return MemoryLockStatus::Success;
}

void RawUserMemory::clear() {
  if (m_State) {
    m_State.get()->entries.clear();
    m_State.get()->complete = false;
    ++m_State.get()->epoch;
  }
  if (MemoryLockAccount* account = m_Space.memoryLockAccount()) {
    auto charge = account->charge();
    charge.rawPages = 0;
    account->publish(charge, account->futureMode());
  }
}

size_t RawUserMemory::retireRegion(uint64_t id) {
  if (!m_State || !id)
    return 0;
  State& state = *m_State.get();
  size_t kept = 0, removed = 0;
  for (size_t i = 0; i < state.entries.count(); ++i) {
    const State::Entry entry = state.entries[i];
    if (entry.region.id != id) {
      state.entries[kept++] = entry;
      continue;
    }
    if (entry.mode != MemoryLockMode::None)
      removed += entry.region.length / pageSize();
    for (size_t offset = 0; offset < entry.region.length; offset += pageSize()) {
      physical_uintptr_t physical = 0;
      size_t flags = 0;
      if (m_Space.detachMapping(reinterpret_cast<void*>(entry.region.base + offset), physical,
                                flags) &&
          !(flags & (VirtualAddressSpace::Shared | VirtualAddressSpace::Borrowed |
                     VirtualAddressSpace::Swapped)))
        PhysicalMemoryManager::instance().freePage(physical);
    }
  }
  while (state.entries.count() > kept)
    state.entries.popBack();
  ++state.epoch;
  return removed;
}
