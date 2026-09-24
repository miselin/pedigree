/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/UserMemoryPolicy.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"

namespace {
bool check(bool condition, const char* detail) {
  if (!condition)
    ERROR("MEMORY-LOCK-TEST: FAIL " << detail);
  return condition;
}

class TestAccount final : public MemoryLockAccount {
 public:
  bool permitsTotalPages(size_t total, bool) const override {
    return total <= maximumPages;
  }
  size_t maximumPages = ~size_t(0);
};

class LockProbeFile final : public File {
 public:
  LockProbeFile()
      : File(String("memory-lock-probe"), 0, 0, 0, 1, nullptr,
             2 * PhysicalMemoryManager::getPageSize(), nullptr),
        m_Storage("Memory Lock Probe") {
    enableFillCacheWriteback();
  }
  ~LockProbeFile() override {
    shutdownFillCacheWriteback();
  }

  bool initialise() {
    if (!PhysicalMemoryManager::instance().allocateRegion(
            m_Storage, 2, 0, VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write))
      return false;
    ByteSet(m_Storage.virtualAddress(), 0x49, getSize());
    return true;
  }
  size_t getBlockSize() const override {
    return PhysicalMemoryManager::getPageSize() / 2;
  }
  bool evict(size_t offset) {
    return cacheState().fill.evict(offset);
  }
  size_t loans() {
    return __atomic_load_n(&physicalPageLoans(), __ATOMIC_ACQUIRE);
  }
  bool backingUnchanged() {
    auto* bytes = static_cast<const uint8_t*>(m_Storage.virtualAddress());
    for (size_t n = 0; n < getSize(); ++n) {
      if (bytes[n] != 0x49)
        return false;
    }
    return true;
  }

 protected:
  uintptr_t readBlock(uint64_t offset) override {
    if (offset >= getSize())
      return FILE_BAD_BLOCK;
    return reinterpret_cast<uintptr_t>(m_Storage.virtualAddress()) + offset;
  }
  bool pinBlock(uint64_t offset) override {
    return offset < getSize();
  }
  void unpinBlock(uint64_t) override {}

 private:
  MemoryRegion m_Storage;
};

bool compactUntilAbsent(MemoryMapManager& manager, VirtualAddressSpace& space, uintptr_t address) {
  for (size_t attempt = 0; attempt < 256; ++attempt) {
    // The pressure path refuses recursive entry; each call needs a fresh operation.
    manager.compact();
    if (!space.isMapped(reinterpret_cast<void*>(address)))
      return true;
    Scheduler::instance().yield();
  }
  return false;
}

bool cleanupMappings(MemoryMapManager& manager, TestAccount& account, LockProbeFile& file) {
  MemoryMapManager::OperationGuard operation(manager);
  manager.unmapAll();
  const auto charge = account.charge();
  return check(!charge.managedPages && !charge.rawPages && !file.loans(),
               "mapping teardown retained a lock charge or file loan");
}

bool compactorCase(MemoryLockMode mode, MemoryMapManager& manager, VirtualAddressSpace& space,
                   TestAccount& account) {
  LockProbeFile file;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t locked = 0;
  uintptr_t control = 0;
  physical_uintptr_t originalPhysical = 0;
  bool passed = check(file.initialise(), "compactor backing allocation");
  if (passed) {
    passed = [&]() {
      {
        MemoryMapManager::OperationGuard operation(manager);
        if (!check(manager.mapFile(&file, locked, pageSize, MemoryMappedObject::Read, 0, false) &&
                       manager.mapFile(&file, control, pageSize, MemoryMappedObject::Read, pageSize,
                                       false) &&
                       manager.faultIn(locked, false) && manager.faultIn(control, false),
                   "compactor mapping setup"))
          return false;
        size_t flags = 0;
        space.getMapping(reinterpret_cast<void*>(locked), originalPhysical, flags);
        if (!check((flags & VirtualAddressSpace::Borrowed) && file.loans() == 2 && !file.evict(0) &&
                       !file.evict(pageSize),
                   "resident shared mappings did not retain both cache loans"))
          return false;
        if (!check(manager.lockMemory(space, locked, pageSize, mode, false) ==
                           MemoryLockStatus::Success &&
                       manager.hasLockedMemory(space, locked, pageSize) &&
                       !manager.hasLockedMemory(space, control, pageSize) &&
                       account.charge().managedPages == 1,
                   "single-page lock policy publication"))
          return false;
      }

      if (!check(compactUntilAbsent(manager, space, control),
                 "actual compactor did not release the unlocked control"))
        return false;
      {
        MemoryMapManager::OperationGuard operation(manager);
        if (!check(space.isMapped(reinterpret_cast<void*>(locked)) && file.loans() == 1,
                   "compactor removed the locked mapping or its backing loan"))
          return false;
        physical_uintptr_t physical = 0;
        size_t flags = 0;
        space.getMapping(reinterpret_cast<void*>(locked), physical, flags);
        auto* bytes = reinterpret_cast<volatile uint8_t*>(locked);
        if (!check(physical == originalPhysical && (flags & VirtualAddressSpace::Borrowed) &&
                       !(flags & VirtualAddressSpace::Write) && bytes[0] == 0x49 &&
                       bytes[pageSize - 1] == 0x49 && !file.evict(0) && file.evict(pageSize),
                   "locked identity/data or cache eviction inhibition changed"))
          return false;
        if (!check(manager.lockMemory(space, locked, pageSize, MemoryLockMode::None, false) ==
                           MemoryLockStatus::Success &&
                       !manager.hasLockedMemory(space, locked, pageSize) &&
                       !account.charge().managedPages,
                   "unlock retained policy or virtual-page charge"))
          return false;
      }
      if (!check(compactUntilAbsent(manager, space, locked),
                 "unlock did not permit actual compaction"))
        return false;
      return check(!file.loans() && file.evict(0) && file.backingUnchanged(),
                   "unlock did not release the cache loan or changed backing bytes");
    }();
  }
  passed = cleanupMappings(manager, account, file) && passed;
  if (passed)
    NOTICE("MEMORY-LOCK-TEST: PASS compactor-"
           << (mode == MemoryLockMode::Eager ? "eager" : "onfault")
           << " locked-physical=retained control=evicted unlock=evicted");
  return passed;
}

bool privateEagerCopy(MemoryMapManager& manager, VirtualAddressSpace& space, TestAccount& account) {
  LockProbeFile file;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t address = 0;
  physical_uintptr_t ownedPhysical = 0;
  bool passed = check(file.initialise(), "private backing allocation");
  if (passed) {
    passed = [&]() {
      MemoryMapManager::OperationGuard operation(manager);
      if (!check(manager.mapFile(&file, address, pageSize,
                                 MemoryMappedObject::Read | MemoryMappedObject::Write, 0, true) &&
                     manager.faultIn(address, false),
                 "private borrowed mapping setup"))
        return false;
      physical_uintptr_t borrowedPhysical = 0;
      size_t flags = 0;
      space.getMapping(reinterpret_cast<void*>(address), borrowedPhysical, flags);
      if (!check((flags & VirtualAddressSpace::Borrowed) && file.loans() == 1,
                 "private read did not borrow its file page"))
        return false;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      const size_t borrowedReferences =
          PhysicalMemoryManager::pageReferenceCountForTest(borrowedPhysical);
#endif
      if (!check(manager.lockMemory(space, address, pageSize, MemoryLockMode::OnFault, false) ==
                     MemoryLockStatus::Success,
                 "private ONFAULT lock"))
        return false;
      physical_uintptr_t currentPhysical = 0;
      space.getMapping(reinterpret_cast<void*>(address), currentPhysical, flags);
      if (!check(currentPhysical == borrowedPhysical && (flags & VirtualAddressSpace::Borrowed) &&
                     !(flags & VirtualAddressSpace::Write) && file.loans() == 1,
                 "ONFAULT eagerly copied an already-resident private page"))
        return false;
      if (!check(manager.lockMemory(space, address, pageSize, MemoryLockMode::Eager, false) ==
                     MemoryLockStatus::Success,
                 "private eager lock"))
        return false;
      space.getMapping(reinterpret_cast<void*>(address), ownedPhysical, flags);
      if (!check(
              ownedPhysical != borrowedPhysical && (flags & VirtualAddressSpace::Write) &&
                  !(flags & (VirtualAddressSpace::Borrowed | VirtualAddressSpace::CopyOnWrite)) &&
                  !file.loans() && account.charge().managedPages == 1,
              "eager lock did not acquire exactly one writable private page"))
        return false;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      if (!check(PhysicalMemoryManager::pageReferenceCountForTest(ownedPhysical) == 1 &&
                     PhysicalMemoryManager::pageReferenceCountForTest(borrowedPhysical) ==
                         borrowedReferences,
                 "eager copy changed backing ownership or leaked a private reference"))
        return false;
#endif
      auto* bytes = reinterpret_cast<volatile uint8_t*>(address);
      for (size_t n = 0; n < pageSize; ++n) {
        if (!check(bytes[n] == 0x49, "eager private copy lost source bytes"))
          return false;
      }
      bytes[0] = 0x72;
      return check(file.backingUnchanged() && file.evict(0) && bytes[0] == 0x72 &&
                       bytes[pageSize - 1] == 0x49,
                   "private write or backing eviction changed the other owner");
    }();
  }
  passed = cleanupMappings(manager, account, file) && passed;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (ownedPhysical)
    passed &= check(!PhysicalMemoryManager::pageReferenceCountForTest(ownedPhysical),
                    "private eager page survived mapping teardown");
#endif
  if (passed)
    NOTICE(
        "MEMORY-LOCK-TEST: PASS private-eager-copy onfault=borrowed eager=owned backing=unchanged");
  return passed;
}

bool rawState(MemoryMapManager& manager, VirtualAddressSpace& space, TestAccount& account,
              size_t expectedPages, const char* detail) {
  MemoryMapManager::OperationGuard operation(manager);
  UniquePointer<PreparedMemoryLock> plan;
  const auto status = space.rawUserMemory().prepareAllLocks(MemoryLockMode::None, plan);
  return check(status == MemoryLockStatus::Success && plan &&
                   plan.get()->eligiblePages() == expectedPages &&
                   account.charge().rawPages == expectedPages && !account.charge().managedPages,
               detail);
}

void quota(MemoryMapManager& manager, TestAccount& account, size_t pages) {
  MemoryMapManager::OperationGuard operation(manager);
  account.maximumPages = pages;
}

bool rawOwnerFlows(MemoryMapManager& manager, VirtualAddressSpace& space, TestAccount& account) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const uintptr_t original = reinterpret_cast<uintptr_t>(space.getEndOfHeap());
  bool passed = [&]() {
    if (!check(original && !(original & (pageSize - 1)), "isolated heap alignment") ||
        !rawState(manager, space, account, 0, "isolated raw inventory is not empty"))
      return false;
    {
      MemoryMapManager::OperationGuard operation(manager);
      // This isolated kernel worker has no other user mappings or raw owners.
      space.rawUserMemory().setCompleteInventory(true);
      account.maximumPages = 0;
      if (!check(manager.lockAllMemory(space, false, MemoryLockMode::None, MemoryLockMode::Eager,
                                       false) == MemoryLockStatus::Success,
                 "raw FUTURE setup"))
        return false;
    }
    if (!check(!space.expandHeap(1, VirtualAddressSpace::Write) &&
                   reinterpret_cast<uintptr_t>(space.getEndOfHeap()) == original,
               "zero-quota heap admission changed the break") ||
        !rawState(manager, space, account, 0, "failed heap admission published a raw owner"))
      return false;
    quota(manager, account, 2);
    if (!check(reinterpret_cast<uintptr_t>(
                   space.expandHeap(pageSize + 1, VirtualAddressSpace::Write)) == original,
               "two-page heap admission") ||
        !rawState(manager, space, account, 2, "heap admission did not charge two pages"))
      return false;
    for (size_t offset = 0; offset < 2 * pageSize; offset += pageSize) {
      void* page = reinterpret_cast<void*>(original + offset);
      if (!check(space.isMapped(page), "eager heap page was not populated"))
        return false;
      physical_uintptr_t physical = 0;
      size_t flags = 0;
      space.getMapping(page, physical, flags);
      if (!check(
              (flags & VirtualAddressSpace::Write) && !(flags & VirtualAddressSpace::CopyOnWrite),
              "eager heap admission retained a write-faulting CoW page"))
        return false;
      auto* bytes = reinterpret_cast<volatile uint8_t*>(page);
      if (!check(bytes[0] == 0 && bytes[pageSize - 1] == 0, "eager heap page was not zeroed"))
        return false;
    }
    if (!check(reinterpret_cast<uintptr_t>(space.expandHeap(
                   pageSize - 1, VirtualAddressSpace::Write)) == original + pageSize + 1,
               "same-page heap growth") ||
        !rawState(manager, space, account, 2, "same-page growth charged an extra page"))
      return false;
    if (!check(reinterpret_cast<uintptr_t>(space.expandHeap(-static_cast<ssize_t>(2 * pageSize),
                                                            VirtualAddressSpace::Write)) ==
                       original + 2 * pageSize &&
                   reinterpret_cast<uintptr_t>(space.getEndOfHeap()) == original &&
                   !space.isMapped(reinterpret_cast<void*>(original)) &&
                   !space.isMapped(reinterpret_cast<void*>(original + pageSize)),
               "heap shrink did not retire its pages and break") ||
        !rawState(manager, space, account, 0, "heap shrink retained raw charge or inventory"))
      return false;

    return true;
  }();

  {
    MemoryMapManager::OperationGuard operation(manager);
    account.maximumPages = ~size_t(0);
    account.publish(account.charge(), MemoryLockMode::None);
    const uintptr_t current = reinterpret_cast<uintptr_t>(space.getEndOfHeap());
    if (current > original && current - original <= 2 * pageSize)
      space.expandHeap(-static_cast<ssize_t>(current - original), VirtualAddressSpace::Write);
    passed &= check(reinterpret_cast<uintptr_t>(space.getEndOfHeap()) == original,
                    "raw fixture cleanup could not restore the heap");
  }
  passed = rawState(manager, space, account, 0, "raw fixture final cleanup") && passed;
  if (passed)
    NOTICE("MEMORY-LOCK-TEST: PASS raw-owners heap=quota-byte-growth-shrink");
  return passed;
}

int memoryLockWorker(void* parameter) {
  bool& passed = *static_cast<bool*>(parameter);
  MemoryMapManager& manager = MemoryMapManager::instance();
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  TestAccount account;
  {
    MemoryMapManager::OperationGuard operation(manager);
    if (!check(!space.memoryLockAccount(), "test address space already has a lock account"))
      return 0;
    space.setMemoryLockAccount(&account);
    manager.bindMemoryLockPolicy(space);
  }
  passed = compactorCase(MemoryLockMode::Eager, manager, space, account) &&
           compactorCase(MemoryLockMode::OnFault, manager, space, account) &&
           privateEagerCopy(manager, space, account) && rawOwnerFlows(manager, space, account);
  {
    MemoryMapManager::OperationGuard operation(manager);
    manager.unmapAll();
    space.rawUserMemory().clear();
    const auto charge = account.charge();
    passed &= check(!charge.managedPages && !charge.rawPages,
                    "test account retained charge at final teardown");
    space.setMemoryLockAccount(nullptr);
    space.setUserMemoryPolicy(nullptr);
  }
  return 0;
}
}  // namespace

bool runMemoryLockRegressions() {
  NOTICE("MEMORY-LOCK-TEST: BEGIN");
  Process* process = new Process(Scheduler::instance().getKernelProcess(), true);
  if (!check(process != nullptr, "test process allocation"))
    return false;
  bool passed = false;
  Thread* worker = new Thread(process, memoryLockWorker, &passed, nullptr, false, true, true);
  const bool started = worker && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (started && !joined)
    FATAL("MEMORY-LOCK-TEST: worker could not be joined safely");
  if (!started)
    delete worker;
  delete process;
  passed = check(started && joined && passed, "isolated worker") && passed;
  if (passed)
    NOTICE("MEMORY-LOCK-TEST: END PASS");
  return passed;
}
