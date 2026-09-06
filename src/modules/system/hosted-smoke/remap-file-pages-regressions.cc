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
using Status = MemoryMapManager::FileRemapStatus;

bool check(bool condition, const char* detail) {
  if (!condition)
    ERROR("REMAP-FILE-PAGES-TEST: FAIL " << detail);
  return condition;
}

class TestAccount final : public MemoryLockAccount {
 public:
  bool permitsTotalPages(size_t, bool) const override {
    return true;
  }
};

class RemapProbeFile final : public File {
 public:
  static constexpr size_t Pages = 8;
  RemapProbeFile()
      : File(String("remap-file-pages-probe"), 0, 0, 0, 1, nullptr,
             Pages * PhysicalMemoryManager::getPageSize(), nullptr),
        m_Storage("Remap File Pages Probe") {
    enableFillCacheWriteback();
  }
  ~RemapProbeFile() override {
    shutdownFillCacheWriteback();
  }
  bool initialise() {
    if (!PhysicalMemoryManager::instance().allocateRegion(
            m_Storage, Pages, 0, VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write))
      return false;
    const size_t page = PhysicalMemoryManager::getPageSize();
    for (size_t n = 0; n < Pages; ++n)
      ByteSet(static_cast<uint8_t*>(m_Storage.virtualAddress()) + n * page, pattern(n), page);
    return true;
  }
  static uint8_t pattern(size_t page) {
    return 0x31 + page;
  }
  size_t getBlockSize() const override {
    return PhysicalMemoryManager::getPageSize() / 2;
  }
  bool allowMapping(bool, bool, bool&) override {
    return !rejectMapping;
  }
  physical_uintptr_t getPhysicalPage(size_t offset) override {
    const auto physical = File::getPhysicalPage(offset);
    const size_t index = offset / PhysicalMemoryManager::getPageSize();
    if (physical != ~physical_uintptr_t(0) && index < Pages)
      __atomic_add_fetch(&m_Loans[index], 1, __ATOMIC_RELEASE);
    return physical;
  }
  void returnPhysicalPage(size_t offset) override {
    const size_t index = offset / PhysicalMemoryManager::getPageSize();
    if (index < Pages)
      __atomic_sub_fetch(&m_Loans[index], 1, __ATOMIC_RELEASE);
    File::returnPhysicalPage(offset);
  }
  size_t loansAt(size_t page) const {
    return __atomic_load_n(&m_Loans[page], __ATOMIC_ACQUIRE);
  }
  size_t loans() {
    return __atomic_load_n(&physicalPageLoans(), __ATOMIC_ACQUIRE);
  }
  bool evict(size_t page) {
    return cacheState().fill.evict(page * PhysicalMemoryManager::getPageSize());
  }
  bool balanced() {
    if (loans())
      return false;
    for (size_t n = 0; n < Pages; ++n) {
      if (loansAt(n))
        return false;
    }
    return true;
  }
  bool rejectMapping = false;

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
  size_t m_Loans[Pages] = {};
};

bool mapProbe(MemoryMapManager& manager, RemapProbeFile& file, uintptr_t& address, size_t length,
              size_t offset = 0, MemoryLockMode mode = MemoryLockMode::None) {
  return manager.mapFile(&file, address, length, MemoryMappedObject::Read, offset, false,
                         MemoryMapManager::Placement::Hint, nullptr,
                         MemoryMappedObject::Read | MemoryMappedObject::Write,
                         SharedPointer<MappingAttachment>(), mode,
                         FileMappingOrigin{0x524650, true}) != nullptr;
}

bool borrowed(VirtualAddressSpace& space, uintptr_t address, physical_uintptr_t& physical) {
  if (!space.isMapped(reinterpret_cast<void*>(address)))
    return false;
  size_t flags = 0;
  space.getMapping(reinterpret_cast<void*>(address), physical, flags);
  return (flags & VirtualAddressSpace::Borrowed) &&
         !(flags & (VirtualAddressSpace::Write | VirtualAddressSpace::CopyOnWrite));
}

bool sameBorrowed(VirtualAddressSpace& space, uintptr_t address, physical_uintptr_t expected) {
  physical_uintptr_t current = 0;
  return borrowed(space, address, current) && current == expected;
}

bool hasPattern(uintptr_t address, size_t filePage) {
  const auto* bytes = reinterpret_cast<volatile uint8_t*>(address);
  const size_t page = PhysicalMemoryManager::getPageSize();
  return bytes[0] == RemapProbeFile::pattern(filePage) &&
         bytes[page - 1] == RemapProbeFile::pattern(filePage);
}

bool cleanup(MemoryMapManager& manager, TestAccount& account, RemapProbeFile& file) {
  MemoryMapManager::OperationGuard operation(manager);
  manager.unmapAll();
  account.publish(account.charge(), MemoryLockMode::None);
  return check(!account.charge().managedPages && !account.charge().rawPages && file.balanced(),
               "teardown retained charge or an offset-specific loan");
}

bool slicedRetirement(MemoryMapManager& manager, VirtualAddressSpace& space, TestAccount& account) {
  RemapProbeFile file;
  uintptr_t base = 0;
  const size_t page = PhysicalMemoryManager::getPageSize();
  bool passed = check(file.initialise(), "slice backing allocation");
  if (passed) {
    passed = [&]() {
      MemoryMapManager::OperationGuard operation(manager);
      physical_uintptr_t original[3] = {};
      if (!check(mapProbe(manager, file, base, 3 * page), "slice mapping setup"))
        return false;
      for (size_t n = 0; n < 3; ++n) {
        if (!check(manager.faultIn(base + n * page, false) &&
                       borrowed(space, base + n * page, original[n]) && file.loansAt(n) == 1,
                   "slice initial PTE and loan"))
          return false;
      }
      file.rejectMapping = true;
      if (!check(manager.remapFilePages(base + page, page, 4 * page, false) == Status::PolicyDenied,
                 "backing policy rejection"))
        return false;
      file.rejectMapping = false;
      for (size_t n = 0; n < 3; ++n) {
        if (!check(sameBorrowed(space, base + n * page, original[n]) && file.loansAt(n) == 1 &&
                       hasPattern(base + n * page, n),
                   "rejected replacement changed a victim"))
          return false;
      }
      if (!check(manager.remapFilePages(base + page, page, 4 * page, true) == Status::Success &&
                     !space.isMapped(reinterpret_cast<void*>(base + page)) && file.loans() == 2 &&
                     !file.loansAt(1) && !file.loansAt(4) && file.evict(1),
                 "NONBLOCK did not retire the old offset and leave a lazy replacement"))
        return false;
      if (!check(sameBorrowed(space, base, original[0]) &&
                     sameBorrowed(space, base + 2 * page, original[2]) && file.loansAt(0) == 1 &&
                     file.loansAt(2) == 1 && hasPattern(base, 0) && hasPattern(base + 2 * page, 2),
                 "slice replacement changed unaffected physical pages or loans"))
        return false;
      physical_uintptr_t replacement = 0;
      if (!check(manager.faultIn(base + page, false) && borrowed(space, base + page, replacement) &&
                     file.loansAt(4) == 1 && hasPattern(base + page, 4),
                 "lazy replacement fault used the old offset"))
        return false;
      if (!check(manager.remapFilePages(base + page, page, 5 * page, false) == Status::Success &&
                     borrowed(space, base + page, replacement) && !file.loansAt(4) &&
                     file.loansAt(5) == 1 && file.loans() == 3 && hasPattern(base + page, 5) &&
                     sameBorrowed(space, base, original[0]) &&
                     sameBorrowed(space, base + 2 * page, original[2]),
                 "default remap did not populate its new offset or preserve slices"))
        return false;
      return true;
    }();
  }
  passed = cleanup(manager, account, file) && passed;
  if (passed)
    NOTICE(
        "REMAP-FILE-PAGES-TEST: PASS slices policy=preserved old-loans=retired "
        "neighbors=retained nonblock=lazy default=populated");
  return passed;
}

bool stagingLimit(MemoryMapManager& manager, VirtualAddressSpace& space, TestAccount& account) {
  RemapProbeFile file;
  uintptr_t base = 0;
  const size_t page = PhysicalMemoryManager::getPageSize();
  bool passed = check(file.initialise(), "staging-limit backing allocation");
  if (passed) {
    passed = [&]() {
      MemoryMapManager::OperationGuard operation(manager);
      // Only two pages are resident; the virtual extent exercises the real staging bound.
      const size_t length = (VirtualAddressSpace::MaximumRemapPages + 1) * page;
      physical_uintptr_t first = 0, second = 0;
      if (!check(mapProbe(manager, file, base, length) && manager.faultIn(base, false) &&
                     manager.faultIn(base + page, false) && borrowed(space, base, first) &&
                     borrowed(space, base + page, second),
                 "staging-limit mapping setup"))
        return false;
      return check(manager.remapFilePages(base + page, page, 4 * page, true) == Status::NoMemory &&
                       sameBorrowed(space, base, first) &&
                       sameBorrowed(space, base + page, second) && file.loansAt(0) == 1 &&
                       file.loansAt(1) == 1 && file.loans() == 2 && !file.loansAt(4) &&
                       hasPattern(base, 0) && hasPattern(base + page, 1) &&
                       !account.charge().managedPages,
                   "failed staging changed PTEs, backing offsets, loans, or charge");
    }();
  }
  passed = cleanup(manager, account, file) && passed;
  if (passed)
    NOTICE("REMAP-FILE-PAGES-TEST: PASS staging-limit resident-pages=2 victims=preserved");
  return passed;
}

bool compactUntilAbsent(MemoryMapManager& manager, VirtualAddressSpace& space, uintptr_t address) {
  for (size_t attempt = 0; attempt < 256; ++attempt) {
    manager.compact();
    if (!space.isMapped(reinterpret_cast<void*>(address)))
      return true;
    Scheduler::instance().yield();
  }
  return false;
}

bool lockedReplacement(MemoryLockMode mode, MemoryMapManager& manager, VirtualAddressSpace& space,
                       TestAccount& account) {
  RemapProbeFile file;
  uintptr_t target = 0, control = 0;
  const size_t page = PhysicalMemoryManager::getPageSize();
  const bool onfault = mode == MemoryLockMode::OnFault;
  physical_uintptr_t replacement = 0;
  bool passed = check(file.initialise(), "locked backing allocation");
  if (passed) {
    passed = [&]() {
      {
        MemoryMapManager::OperationGuard operation(manager);
        if (!check(mapProbe(manager, file, target, page, 0, mode) &&
                       mapProbe(manager, file, control, page, 2 * page) &&
                       manager.faultIn(target, false) && manager.faultIn(control, false),
                   "locked mapping setup"))
          return false;
        if (onfault)
          account.publish(account.charge(), MemoryLockMode::OnFault);
        if (!check(manager.remapFilePages(target, page, 4 * page, !onfault) == Status::Success &&
                       !file.loansAt(0) && file.loansAt(2) == 1 &&
                       account.charge().managedPages == 1 &&
                       manager.hasLockedMemory(space, target, page),
                   "locked replacement changed charge or retained old offset loan"))
          return false;
        if (onfault) {
          if (!check(!space.isMapped(reinterpret_cast<void*>(target)) && !file.loansAt(4) &&
                         file.loans() == 1,
                     "FUTURE ONFAULT remap populated despite its lazy policy"))
            return false;
          if (!check(manager.faultIn(target, false), "ONFAULT replacement fault"))
            return false;
        }
        if (!check(borrowed(space, target, replacement) && file.loansAt(4) == 1 &&
                       hasPattern(target, 4),
                   "locked replacement did not populate the requested file page"))
          return false;
      }
      if (!check(compactUntilAbsent(manager, space, control),
                 "actual compactor did not evict the unlocked control"))
        return false;
      {
        MemoryMapManager::OperationGuard operation(manager);
        if (!check(sameBorrowed(space, target, replacement) && file.loansAt(4) == 1 &&
                       !file.loansAt(2) && file.loans() == 1 && !file.evict(4) &&
                       hasPattern(target, 4) && account.charge().managedPages == 1,
                   "actual compactor retired the locked replacement or its new-offset loan"))
          return false;
        if (!check(manager.lockMemory(space, target, page, MemoryLockMode::None, false) ==
                           MemoryLockStatus::Success &&
                       !account.charge().managedPages,
                   "replacement unlock retained charge"))
          return false;
      }
      if (!check(compactUntilAbsent(manager, space, target),
                 "unlocked replacement retained its PTE"))
        return false;
      MemoryMapManager::OperationGuard operation(manager);
      return check(!file.loansAt(4) && file.balanced() && file.evict(4),
                   "unlocked replacement retained its new-offset PTE or loan");
    }();
  }
  passed = cleanup(manager, account, file) && passed;
  if (passed)
    NOTICE("REMAP-FILE-PAGES-TEST: PASS "
           << (onfault ? "onfault" : "eager")
           << " replacement=retained control=evicted unlock=evicted");
  return passed;
}

int remapFilePagesWorker(void* parameter) {
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
  passed = slicedRetirement(manager, space, account) && stagingLimit(manager, space, account) &&
           lockedReplacement(MemoryLockMode::OnFault, manager, space, account) &&
           lockedReplacement(MemoryLockMode::Eager, manager, space, account);
  {
    MemoryMapManager::OperationGuard operation(manager);
    manager.unmapAll();
    space.rawUserMemory().clear();
    passed &= check(!account.charge().managedPages && !account.charge().rawPages,
                    "final account teardown");
    space.setMemoryLockAccount(nullptr);
    space.setUserMemoryPolicy(nullptr);
  }
  return 0;
}
}  // namespace

bool runRemapFilePagesRegressions() {
  NOTICE("REMAP-FILE-PAGES-TEST: BEGIN");
  Process* process = new Process(Scheduler::instance().getKernelProcess(), true);
  if (!check(process != nullptr, "test process allocation"))
    return false;
  bool passed = false;
  Thread* worker = new Thread(process, remapFilePagesWorker, &passed, nullptr, false, true, true);
  const bool started = worker && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (started && !joined)
    FATAL("REMAP-FILE-PAGES-TEST: worker could not be joined safely");
  if (!started)
    delete worker;
  delete process;
  passed = check(started && joined && passed, "isolated worker") && passed;
  if (passed)
    NOTICE("REMAP-FILE-PAGES-TEST: END PASS");
  return passed;
}
