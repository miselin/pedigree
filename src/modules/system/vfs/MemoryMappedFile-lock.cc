/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "MemoryMappedFile.h"

class VfsUserMemoryPolicy final : public UserMemoryPolicy {
 public:
  bool callerHasMemoryLockPrivilege() const override {
    auto* thread = Processor::information().getCurrentThread();
    return thread && thread->getParent()->getEffectiveUserId() == 0;
  }
  bool overlapsManagedMemory(VirtualAddressSpace& space, uintptr_t base,
                             size_t length) const override {
    if (length > ~uintptr_t(0) - base)
      return true;
    auto* objects = MemoryMapManager::instance().m_MmObjectLists.lookup(&space);
    const size_t mask = PhysicalMemoryManager::getPageSize() - 1;
    if (objects)
      for (auto* object : *objects) {
        const uintptr_t end = (object->address() + object->length() + mask) & ~mask;
        if (base < end && object->address() < base + length)
          return true;
      }
    return false;
  }
  void enterOperation() override {
    MemoryMapManager::instance().enterOperation();
  }
  void leaveOperation() override {
    MemoryMapManager::instance().leaveOperation();
  }
};

namespace {
VfsUserMemoryPolicy policy;
constexpr size_t MaximumLockObjects = 4096;
constexpr size_t MaximumLockPages = 65536;
uintptr_t objectEnd(MemoryMappedObject* object) {
  const size_t mask = PhysicalMemoryManager::getPageSize() - 1;
  return (object->address() + object->length() + mask) & ~mask;
}
MemoryLockStatus populationStatus(PopulationStatus status) {
  switch (status) {
    case PopulationStatus::Success:
      return MemoryLockStatus::Success;
    case PopulationStatus::NoMemory:
      return MemoryLockStatus::PopulationNoMemory;
    case PopulationStatus::Inaccessible:
      return MemoryLockStatus::PopulationInaccessible;
    case PopulationStatus::IoError:
      return MemoryLockStatus::PopulationIoError;
  }
  return MemoryLockStatus::PopulationIoError;
}
bool addCharge(size_t previous, size_t removed, size_t added, size_t& result) {
  if (removed > previous || added > ~size_t(0) - (previous - removed))
    return false;
  result = previous - removed + added;
  return true;
}
}  // namespace

void MemoryMapManager::bindMemoryLockPolicy(VirtualAddressSpace& space) {
  space.setUserMemoryPolicy(&policy);
}

PopulationStatus MemoryMappedObject::populatePage(VirtualAddressSpace& space, uintptr_t address) {
  if (m_Permissions == None || beyondBackingEnd(address))
    return PopulationStatus::Inaccessible;
  const bool write = m_bCopyOnWrite && (m_Permissions & Write);
  void* page = reinterpret_cast<void*>(address);
  if (space.isMapped(page)) {
    physical_uintptr_t physical;
    size_t flags;
    space.getMapping(page, physical, flags);
    if (flags & (VirtualAddressSpace::NoAccess | VirtualAddressSpace::KernelMode |
                 VirtualAddressSpace::Swapped))
      return PopulationStatus::Inaccessible;
    if (!write || (flags & VirtualAddressSpace::Write))
      return PopulationStatus::Success;
    if (flags & VirtualAddressSpace::CopyOnWrite)
      return space.handleCopyOnWriteFault(reinterpret_cast<void*>(address), true)
                 ? PopulationStatus::Success
                 : PopulationStatus::NoMemory;
  }
  PopulationStatus status = PopulationStatus::NoMemory;
  auto* thread = Processor::information().getCurrentThread();
  const size_t previousError = thread ? thread->getErrno() : 0;
  if (thread)
    thread->setErrno(0);
  const bool populated = trap(space, address, write, &status);
  if (thread)
    thread->setErrno(previousError);
  return populated ? PopulationStatus::Success : status;
}

class MemoryMapManager::LockPlan final : public PreparedMemoryLock {
 public:
  LockPlan(MemoryMapManager& manager, VirtualAddressSpace& space, MemoryLockMode mode)
      : m_Manager(manager), m_Space(space), m_Mode(mode) {}
  ~LockPlan() override {
    for (auto* object : m_Committed ? m_Retired : m_Staged)
      delete object;
    delete m_Replacement;
  }
  size_t removedPages() const override {
    return m_Removed;
  }
  size_t addedPages() const override {
    return m_Added;
  }
  size_t coveredPages() const override {
    return m_Covered;
  }
  size_t eligiblePages() const override {
    return m_Eligible;
  }
  void commit() override {
    if (!m_Replacement) {
      m_Committed = true;
      return;
    }
    for (auto* object : m_Retired)
      object->m_OwnsMappings = false;
    for (auto* object : m_Staged)
      object->m_OwnsMappings = true;
    auto* previous = m_Manager.m_MmObjectLists.lookup(&m_Space);
    {
      LockGuard<Spinlock> guard(m_Manager.m_Lock);
      m_Manager.m_MmObjectLists.insert(&m_Space, m_Replacement);
    }
    m_Replacement = nullptr;
    delete previous;
    m_Committed = true;
  }
  PopulationStatus populate() override {
    if (m_Mode != MemoryLockMode::Eager)
      return PopulationStatus::Success;
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    PopulationStatus result = PopulationStatus::Success;
    for (auto* object : m_Populate) {
      for (uintptr_t page = object->address(); page < objectEnd(object); page += pageSize) {
        const auto status = object->populatePage(m_Space, page);
        if (status != PopulationStatus::Success && result == PopulationStatus::Success)
          result = status;
      }
    }
    return result;
  }
  bool append(MemoryMappedObject* owner, uintptr_t first, uintptr_t last, MemoryLockMode mode,
              bool populate) {
    if (first == last)
      return true;
    auto* slice = owner->stageSlice(first, last - first, first, last - first);
    if (!slice)
      return false;
    slice->m_LockMode = mode;
    m_Staged.pushBack(slice);
    if (populate)
      m_Populate.pushBack(slice);
    return m_Replacement->tryPushBack(slice);
  }
  MemoryMapManager& m_Manager;
  VirtualAddressSpace& m_Space;
  MemoryLockMode m_Mode;
  MmObjectList* m_Replacement = nullptr;
  Vector<MemoryMappedObject*> m_Retired, m_Staged, m_Populate;
  size_t m_Removed = 0, m_Added = 0, m_Covered = 0, m_Eligible = 0;
  bool m_Committed = false;
};

MemoryLockStatus MemoryMapManager::prepareManagedLocks(VirtualAddressSpace& space, uintptr_t base,
                                                       size_t length, MemoryLockMode mode, bool all,
                                                       UniquePointer<PreparedMemoryLock>& result) {
  auto* plan = new LockPlan(*this, space, mode);
  auto owned = UniquePointer<PreparedMemoryLock>::adopt(plan);
  if (!plan)
    return MemoryLockStatus::NoMemory;
  auto* objects = m_MmObjectLists.lookup(&space);
  if (!objects || !objects->count()) {
    result = pedigree_std::move(owned);
    return MemoryLockStatus::Success;
  }
  if (objects->count() > MaximumLockObjects || !plan->m_Retired.tryReserve(objects->count()) ||
      !plan->m_Staged.tryReserve(objects->count() * 3) ||
      !plan->m_Populate.tryReserve(objects->count()))
    return MemoryLockStatus::NoMemory;
  plan->m_Replacement = new MmObjectList;
  if (!plan->m_Replacement)
    return MemoryLockStatus::NoMemory;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  size_t stagedPages = 0;
  for (auto* object : *objects) {
    const uintptr_t end = objectEnd(object);
    plan->m_Eligible += (end - object->address()) / pageSize;
    const uintptr_t first = all || base < object->address() ? object->address() : base;
    const uintptr_t last = all || base + length > end ? end : base + length;
    if (first >= last) {
      if (!plan->m_Replacement->tryPushBack(object))
        return MemoryLockStatus::NoMemory;
      continue;
    }
    const size_t pages = (last - first) / pageSize;
    plan->m_Covered += pages;
    if (object->m_LockMode != MemoryLockMode::None)
      plan->m_Removed += pages;
    if (mode != MemoryLockMode::None)
      plan->m_Added += pages;
    const size_t objectPages = (end - object->address()) / pageSize;
    if (objectPages > MaximumLockPages - stagedPages)
      return MemoryLockStatus::NoMemory;
    stagedPages += objectPages;
    plan->m_Retired.pushBack(object);
    if (!plan->append(object, object->address(), first, object->m_LockMode, false) ||
        !plan->append(object, first, last, mode, true) ||
        !plan->append(object, last, end, object->m_LockMode, false))
      return MemoryLockStatus::NoMemory;
  }
  if (plan->m_Replacement->count() > MaximumLockObjects)
    return MemoryLockStatus::NoMemory;
  result = pedigree_std::move(owned);
  return MemoryLockStatus::Success;
}

MemoryLockStatus MemoryMapManager::lockMemory(VirtualAddressSpace& space, uintptr_t base,
                                              size_t length, MemoryLockMode mode, bool privileged) {
  OperationGuard operation(*this);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if ((base | length) & (pageSize - 1) || length > ~uintptr_t(0) - base)
    return MemoryLockStatus::InvalidRange;
  if (!length)
    return MemoryLockStatus::Success;
  auto* account = space.memoryLockAccount();
  if (!account)
    return MemoryLockStatus::Unsupported;
  if (length / pageSize > MaximumLockPages)
    return MemoryLockStatus::NoMemory;
  UniquePointer<PreparedMemoryLock> managed, raw;
  auto status = prepareManagedLocks(space, base, length, mode, false, managed);
  if (status != MemoryLockStatus::Success)
    return status;
  status = space.rawUserMemory().prepareLocks(base, length, mode, raw);
  if (status != MemoryLockStatus::Success)
    return status;
  size_t covered = managed.get()->coveredPages() + raw.get()->coveredPages();
  if (covered != length / pageSize)
    return MemoryLockStatus::Unmapped;
  auto charge = account->charge();
  if (!addCharge(charge.managedPages, managed.get()->removedPages(), managed.get()->addedPages(),
                 charge.managedPages) ||
      !addCharge(charge.rawPages, raw.get()->removedPages(), raw.get()->addedPages(),
                 charge.rawPages) ||
      charge.rawPages > ~size_t(0) - charge.managedPages)
    return MemoryLockStatus::NoMemory;
  if (mode != MemoryLockMode::None &&
      !account->permitsTotalPages(charge.managedPages + charge.rawPages, privileged))
    return MemoryLockStatus::LockLimit;
  managed.get()->commit();
  raw.get()->commit();
  account->publish(charge, account->futureMode());
  const auto managedPopulation = managed.get()->populate();
  const auto rawPopulation = raw.get()->populate();
  return populationStatus(managedPopulation != PopulationStatus::Success ? managedPopulation
                                                                         : rawPopulation);
}

MemoryLockStatus MemoryMapManager::lockAllMemory(VirtualAddressSpace& space, bool current,
                                                 MemoryLockMode currentMode,
                                                 MemoryLockMode futureMode, bool privileged) {
  OperationGuard operation(*this);
  auto* account = space.memoryLockAccount();
  if (!account || ((currentMode != MemoryLockMode::None || futureMode != MemoryLockMode::None) &&
                   !space.rawUserMemory().completeInventory()))
    return MemoryLockStatus::Unsupported;
  if (!current) {
    account->publish(account->charge(), futureMode);
    return MemoryLockStatus::Success;
  }
  UniquePointer<PreparedMemoryLock> managed, raw;
  auto status = prepareManagedLocks(space, 0, 0, currentMode, true, managed);
  if (status != MemoryLockStatus::Success)
    return status;
  status = space.rawUserMemory().prepareAllLocks(currentMode, raw);
  if (status != MemoryLockStatus::Success)
    return status;
  const size_t eligible = managed.get()->eligiblePages();
  if (raw.get()->eligiblePages() > ~size_t(0) - eligible)
    return MemoryLockStatus::NoMemory;
  if (currentMode != MemoryLockMode::None &&
      !account->permitsTotalPages(eligible + raw.get()->eligiblePages(), privileged))
    return MemoryLockStatus::LockLimit;
  auto charge = account->charge();
  if (!addCharge(charge.managedPages, managed.get()->removedPages(), managed.get()->addedPages(),
                 charge.managedPages) ||
      !addCharge(charge.rawPages, raw.get()->removedPages(), raw.get()->addedPages(),
                 charge.rawPages))
    return MemoryLockStatus::NoMemory;
  managed.get()->commit();
  raw.get()->commit();
  account->publish(charge, futureMode);
  // CURRENT publishes all lock state even if some pages cannot be populated.
  managed.get()->populate();
  raw.get()->populate();
  return MemoryLockStatus::Success;
}

bool MemoryMapManager::hasLockedMemory(VirtualAddressSpace& space, uintptr_t base, size_t length) {
  OperationGuard operation(*this);
  if (!length)
    return false;
  if (length > ~uintptr_t(0) - base)
    return true;
  auto* objects = m_MmObjectLists.lookup(&space);
  if (objects)
    for (auto* object : *objects)
      if (object->m_LockMode != MemoryLockMode::None && base < objectEnd(object) &&
          object->address() < base + length)
        return true;
  return space.rawUserMemory().hasLockedMemory(base, length);
}

void MemoryMapManager::retireLockedPages(VirtualAddressSpace& space, size_t pages) {
  auto* account = space.memoryLockAccount();
  if (!account || !pages)
    return;
  auto charge = account->charge();
  assert(pages <= charge.managedPages);
  charge.managedPages -= pages;
  account->publish(charge, account->futureMode());
}

PopulationStatus MemoryMapManager::populateMemory(VirtualAddressSpace& space, uintptr_t base,
                                                  size_t length) {
  OperationGuard operation(*this);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if ((base | length) & (pageSize - 1) || length > ~uintptr_t(0) - base)
    return PopulationStatus::Inaccessible;
  auto* objects = m_MmObjectLists.lookup(&space);
  if (!objects)
    return length ? PopulationStatus::Inaccessible : PopulationStatus::Success;
  PopulationStatus result = PopulationStatus::Success;
  for (uintptr_t page = base; page < base + length; page += pageSize) {
    MemoryMappedObject* selected = nullptr;
    for (auto* object : *objects)
      if (page >= object->address() && page < objectEnd(object)) {
        selected = object;
        break;
      }
    const auto status =
        selected ? selected->populatePage(space, page) : PopulationStatus::Inaccessible;
    if (result == PopulationStatus::Success && status != result)
      result = status;
  }
  return result;
}
