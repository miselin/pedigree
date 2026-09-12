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
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/subsys/posix/file-syscalls.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include <sys/mman.h>

namespace {
bool check(bool condition, const char* detail) {
  if (!condition) {
    ERROR("VM-OWNERSHIP-TEST: FAIL " << detail);
  }
  return condition;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
bool protectedClone(bool inaccessible) {
  VirtualAddressSpace& originalSpace = Processor::information().getVirtualAddressSpace();
  PhysicalMemoryManager& memory = PhysicalMemoryManager::instance();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  Process* sourceProcess = new Process(Scheduler::instance().getKernelProcess(), true);
  VirtualAddressSpace* source = sourceProcess->getAddressSpace();
  void* address = reinterpret_cast<void*>(source->getDynamicStart() + 64 * pageSize);
  const physical_uintptr_t physical = memory.allocatePage();
  if (!physical || !source->map(physical, address, VirtualAddressSpace::Write)) {
    if (physical) {
      memory.freePage(physical);
    }
    delete sourceProcess;
    return check(false, "private mapping setup");
  }
  Processor::switchAddressSpace(*source);
  volatile uint8_t* bytes = reinterpret_cast<volatile uint8_t*>(address);
  bytes[0] = 0x31;
  bytes[pageSize - 1] = 0x7A;
  source->setFlags(address, inaccessible ? VirtualAddressSpace::NoAccess : 0);
  Process* childProcess = new Process(sourceProcess, true);
  VirtualAddressSpace* child = childProcess->getAddressSpace();

  physical_uintptr_t parentPhysical = 0;
  physical_uintptr_t childPhysical = 0;
  size_t parentFlags = 0;
  size_t childFlags = 0;
  source->getMapping(address, parentPhysical, parentFlags);
  child->getMapping(address, childPhysical, childFlags);
  bool passed =
      check(source->isMapped(address) && child->isMapped(address) && parentPhysical == physical &&
                childPhysical == physical && (parentFlags & VirtualAddressSpace::CopyOnWrite) &&
                (childFlags & VirtualAddressSpace::CopyOnWrite) &&
                (parentFlags & VirtualAddressSpace::WriteProtected) &&
                (childFlags & VirtualAddressSpace::WriteProtected) &&
                !(parentFlags & VirtualAddressSpace::KernelMode) &&
                bool(parentFlags & VirtualAddressSpace::NoAccess) == inaccessible,
            "protected clone changed ownership or permissions");
  passed &= check(!source->handleCopyOnWriteFault(address, true) &&
                      !source->handleCopyOnWriteFault(address, false) &&
                      !child->handleCopyOnWriteFault(address, true) &&
                      PhysicalMemoryManager::pageReferenceCountForTest(physical) == 2,
                  "copy-on-write bypassed read-only protection");

  source->setFlags(address, parentFlags & ~(VirtualAddressSpace::NoAccess |
                                            VirtualAddressSpace::WriteProtected));
  bool parentResolved = source->handleCopyOnWriteFault(address, true);
  passed &= check(parentResolved, "parent write upgrade could not resolve");
  if (parentResolved) {
    passed &= check(bytes[0] == 0x31 && bytes[pageSize - 1] == 0x7A,
                    "restoring parent access lost contents");
    bytes[0] = 0xA1;
  }
  source->getMapping(address, parentPhysical, parentFlags);

  Processor::switchAddressSpace(*child);
  child->setFlags(
      address, childFlags & ~(VirtualAddressSpace::NoAccess | VirtualAddressSpace::WriteProtected));
  bool childResolved = child->handleCopyOnWriteFault(address, true);
  passed &= check(childResolved, "child write upgrade could not resolve");
  if (childResolved) {
    passed &= check(bytes[0] == 0x31 && bytes[pageSize - 1] == 0x7A,
                    "parent write changed protected child");
    bytes[0] = 0xB1;
  }
  child->getMapping(address, childPhysical, childFlags);
  Processor::switchAddressSpace(*source);
  if (parentResolved) {
    passed &= check(bytes[0] == 0xA1, "child write changed parent");
  }
  Processor::switchAddressSpace(originalSpace);
  delete childProcess;
  delete sourceProcess;
  passed &= check(PhysicalMemoryManager::pageReferenceCountForTest(physical) == 0 &&
                      PhysicalMemoryManager::pageReferenceCountForTest(parentPhysical) == 0 &&
                      PhysicalMemoryManager::pageReferenceCountForTest(childPhysical) == 0,
                  "protected clones leaked a physical owner");
  return passed;
}

bool borrowedClones() {
  VirtualAddressSpace& originalSpace = Processor::information().getVirtualAddressSpace();
  PhysicalMemoryManager& memory = PhysicalMemoryManager::instance();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  Process* sourceProcess = new Process(Scheduler::instance().getKernelProcess(), true);
  VirtualAddressSpace* source = sourceProcess->getAddressSpace();
  void* address = reinterpret_cast<void*>(source->getDynamicStart() + 64 * pageSize);
  const physical_uintptr_t physical = memory.allocatePage();
  if (!physical || !source->map(physical, address,
                                VirtualAddressSpace::Shared | VirtualAddressSpace::Borrowed |
                                    VirtualAddressSpace::NoAccess)) {
    if (physical) {
      memory.freePage(physical);
    }
    delete sourceProcess;
    return check(false, "borrowed mapping setup");
  }
  const size_t references = PhysicalMemoryManager::pageReferenceCountForTest(physical);
  Processor::switchAddressSpace(*source);
  Process* first = new Process(sourceProcess, true);
  Process* second = new Process(sourceProcess, true);
  physical_uintptr_t clonePhysical = 0;
  size_t cloneFlags = 0;
  first->getAddressSpace()->getMapping(address, clonePhysical, cloneFlags);
  bool passed = check(clonePhysical == physical && (cloneFlags & VirtualAddressSpace::Borrowed) &&
                          (cloneFlags & VirtualAddressSpace::NoAccess) &&
                          !(cloneFlags & VirtualAddressSpace::CopyOnWrite) &&
                          PhysicalMemoryManager::pageReferenceCountForTest(physical) == references,
                      "borrowed clones acquired physical ownership");
  passed &= check(
      !source->detachMapping(address, clonePhysical, cloneFlags, VirtualAddressSpace::Write) &&
          clonePhysical == physical && (cloneFlags & VirtualAddressSpace::Borrowed) &&
          source->isMapped(address),
      "conditional detach changed a mismatched mapping");
  passed &= check(first->getAddressSpace()->detachMapping(address, clonePhysical, cloneFlags,
                                                          VirtualAddressSpace::Borrowed) &&
                      clonePhysical == physical && (cloneFlags & VirtualAddressSpace::NoAccess) &&
                      !first->getAddressSpace()->isMapped(address) && source->isMapped(address) &&
                      PhysicalMemoryManager::pageReferenceCountForTest(physical) == references,
                  "foreign protected detach changed backing ownership");
  Processor::switchAddressSpace(originalSpace);
  delete second;
  delete first;
  delete sourceProcess;
  passed &= check(PhysicalMemoryManager::pageReferenceCountForTest(physical) == references,
                  "borrowed teardown released backing ownership");
  memory.freePage(physical);
  passed &= check(PhysicalMemoryManager::pageReferenceCountForTest(physical) == 0,
                  "backing owner could not release borrowed page");
  return passed;
}
#endif

class ResizeProbeFile final : public File {
 public:
  explicit ResizeProbeFile(size_t pages = 2)
      : File(String("mapped-resize-probe"), 0, 0, 0, 1, nullptr,
             pages * PhysicalMemoryManager::getPageSize(), nullptr),
        storage("Mapped Resize Probe"),
        rejectResize(true),
        rejectWritableMapping(false),
        preparedLoans(0),
        committedLoans(0),
        shrinkCommits(0),
        rejectSync(false),
        syncCalls(0) {}

  bool initialise() {
    if (!PhysicalMemoryManager::instance().allocateRegion(
            storage, getSize() / PhysicalMemoryManager::getPageSize(), 0,
            VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write)) {
      return false;
    }
    ByteSet(storage.virtualAddress(), 0x49, getSize());
    return true;
  }

  using File::sync;
  bool sync(size_t, bool) override {
    ++syncCalls;
    return !rejectSync;
  }
  size_t loans() {
    return __atomic_load_n(&physicalPageLoans(), __ATOMIC_ACQUIRE);
  }
  bool prepareSharedMapping(size_t, size_t) override {
    if (rejectWritableMapping) {
      SYSCALL_ERROR(OutOfMemory);
      return false;
    }
    return true;
  }

  MemoryRegion storage;
  bool rejectResize;
  bool rejectWritableMapping;
  size_t preparedLoans;
  size_t committedLoans;
  size_t shrinkCommits;
  bool rejectSync;
  size_t syncCalls;

 protected:
  uintptr_t readBlock(uint64_t location) override {
    return reinterpret_cast<uintptr_t>(storage.virtualAddress()) + location;
  }
  bool pinBlock(uint64_t) override {
    return true;
  }
  void unpinBlock(uint64_t) override {}
  class ShrinkPlan final : public File::PreparedShrink {
   public:
    ShrinkPlan(ResizeProbeFile& file, size_t size) : file(file), size(size) {}
    void commit() override {
      file.committedLoans = file.loans();
      ++file.shrinkCommits;
      file.setSize(size);
    }
    ResizeProbeFile& file;
    size_t size;
  };
  bool prepareShrink(const ShrinkContext& context,
                     UniquePointer<PreparedShrink>& prepared) override {
    preparedLoans = loans();
    if (rejectResize) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    prepared = UniquePointer<PreparedShrink>::adopt(new ShrinkPlan(*this, context.newSize));
    if (!prepared) {
      SYSCALL_ERROR(OutOfMemory);
      return false;
    }
    return true;
  }
  bool resizeFile(size_t size) override {
    if (rejectResize) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    setSize(size);
    return true;
  }
};

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
int resizeFailureWorker(void* parameter) {
  bool& passed = *static_cast<bool*>(parameter);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  ResizeProbeFile file;
  if (!file.initialise()) {
    return 0;
  }
  MemoryMapManager& manager = MemoryMapManager::instance();
  uintptr_t shared = 0;
  uintptr_t privateAddress = 0;
  auto permissions = MemoryMappedObject::Read | MemoryMappedObject::Write;
  MemoryMappedObject* sharedObject =
      manager.mapFile(&file, shared, 2 * pageSize, MemoryMappedObject::Read, 0, false);
  MemoryMappedObject* privateObject =
      manager.mapFile(&file, privateAddress, 2 * pageSize, permissions);
  if (!sharedObject || !privateObject || !manager.faultIn(shared, false) ||
      !manager.faultIn(shared + pageSize, false) ||
      !manager.faultIn(privateAddress + pageSize, true)) {
    manager.removeAndRelease(shared, sharedObject ? 2 * pageSize : 0);
    manager.removeAndRelease(privateAddress, privateObject ? 2 * pageSize : 0);
    manager.unmapAll();
    return 0;
  }
  volatile uint8_t* privateByte = reinterpret_cast<volatile uint8_t*>(privateAddress + pageSize);
  *privateByte = 0xA5;
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  physical_uintptr_t owned = 0;
  size_t flags = 0;
  file.rejectWritableMapping = true;
  MemoryMapManager::ProtectStatus status;
  passed = check(!manager.setPermissions(shared, 2 * pageSize, permissions, &status) &&
                     status == MemoryMapManager::ProtectStatus::NoMemory,
                 "writable protection ignored failed backing preparation");
  space.getMapping(reinterpret_cast<void*>(shared + pageSize), owned, flags);
  passed &= check(!(flags & VirtualAddressSpace::Write),
                  "failed backing preparation published write permission");
  file.rejectWritableMapping = false;
  passed &= check(manager.setPermissions(shared, 2 * pageSize, permissions) != 0,
                  "writable protection could not retry backing preparation");
  space.getMapping(const_cast<uint8_t*>(privateByte), owned, flags);
  const size_t privateFlags = flags;
  physical_uintptr_t borrowed = 0;
  size_t borrowedFlags = 0;
  physical_uintptr_t prefix = 0;
  size_t prefixFlags = 0;
  space.getMapping(reinterpret_cast<void*>(shared + pageSize), borrowed, borrowedFlags);
  space.getMapping(reinterpret_cast<void*>(shared), prefix, prefixFlags);
  const size_t originalLoans = file.loans();
  const size_t privateReferences = PhysicalMemoryManager::pageReferenceCountForTest(owned);
  passed &= check(originalLoans >= 2 && (borrowedFlags & VirtualAddressSpace::Borrowed) &&
                      (prefixFlags & VirtualAddressSpace::Borrowed) && privateReferences == 1,
                  "shrink fixture did not establish borrowed and private ownership");
  const bool rejected = !file.resize(pageSize);
  physical_uintptr_t afterBorrowed = 0;
  size_t afterBorrowedFlags = 0;
  physical_uintptr_t afterPrivate = 0;
  size_t afterPrivateFlags = 0;
  const bool borrowedPresent = space.isMapped(reinterpret_cast<void*>(shared + pageSize));
  const bool privatePresent = space.isMapped(const_cast<uint8_t*>(privateByte));
  if (borrowedPresent)
    space.getMapping(reinterpret_cast<void*>(shared + pageSize), afterBorrowed, afterBorrowedFlags);
  if (privatePresent)
    space.getMapping(const_cast<uint8_t*>(privateByte), afterPrivate, afterPrivateFlags);
  passed &= check(
      rejected && !file.shrinkCommits && file.preparedLoans == originalLoans &&
          file.loans() == originalLoans && file.getSize() == 2 * pageSize && borrowedPresent &&
          afterBorrowed == borrowed && afterBorrowedFlags == borrowedFlags && privatePresent &&
          afterPrivate == owned && afterPrivateFlags == privateFlags && *privateByte == 0xA5 &&
          PhysicalMemoryManager::pageReferenceCountForTest(owned) == privateReferences,
      "failed backend preparation changed a suffix PTE, loan, or private page");
  file.rejectResize = false;
  const bool resized = file.resize(pageSize);
  physical_uintptr_t afterPrefix = 0;
  size_t afterPrefixFlags = 0;
  const bool prefixPresent = space.isMapped(reinterpret_cast<void*>(shared));
  if (prefixPresent)
    space.getMapping(reinterpret_cast<void*>(shared), afterPrefix, afterPrefixFlags);
  passed &=
      check(resized && file.shrinkCommits == 1 && file.preparedLoans == originalLoans &&
                file.committedLoans == 1 && file.loans() == 1 && prefixPresent &&
                afterPrefix == prefix && afterPrefixFlags == prefixFlags &&
                file.getSize() == pageSize && !space.isMapped(const_cast<uint8_t*>(privateByte)) &&
                !space.isMapped(reinterpret_cast<void*>(shared + pageSize)) &&
                !manager.faultIn(privateAddress + pageSize, false) &&
                !manager.faultIn(shared + pageSize, false) &&
                PhysicalMemoryManager::pageReferenceCountForTest(owned) == 0,
            "shrink commit lost the prefix or retained suffix ownership");
  manager.removeAndRelease(shared, 2 * pageSize);
  manager.removeAndRelease(privateAddress, 2 * pageSize);
  manager.unmapAll();
  return 0;
}

bool failedMappedResize() {
  Process* process = new Process(Scheduler::instance().getKernelProcess(), true);
  bool passed = false;
  Thread* worker = new Thread(process, resizeFailureWorker, &passed, nullptr, false, true, true);
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  delete process;
  return check(started && joined && passed, "mapped resize backend failure fixture");
}
#endif

int sparseSplitWorker(void* parameter) {
  bool& passed = *static_cast<bool*>(parameter);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  MemoryMapManager& manager = MemoryMapManager::instance();
  MemoryMapManager::OperationGuard operation(manager);
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  ResizeProbeFile file(3);
  if (!file.initialise()) {
    return 0;
  }
  passed = true;
  const unsigned masks[] = {4, 2, 5, 7};
  for (unsigned mask : masks) {
    NOTICE("VM-OWNERSHIP-TEST: BEGIN sparse-split mask=" << mask);
    uintptr_t address = 0;
    MemoryMappedObject* object =
        manager.mapFile(&file, address, 3 * pageSize, MemoryMappedObject::Read, 0, false);
    if (!check(object != nullptr, "sparse file setup")) {
      passed = false;
      break;
    }
    size_t expectedLoans = 0;
    for (size_t page = 3; page; --page) {
      if (mask & (1U << (page - 1))) {
        passed &= check(manager.faultIn(address + (page - 1) * pageSize, false),
                        "sparse resident page setup");
        ++expectedLoans;
      }
    }
    passed &= check(file.loans() == expectedLoans, "sparse backing loan count before split");
    passed &= check(
        manager.setPermissions(address + pageSize, 2 * pageSize, MemoryMappedObject::Read) != 0,
        "sparse protection split");
    passed &= check(file.loans() == expectedLoans, "sparse split changed backing loans");
    passed &= check(manager.removeAndRelease(address, 3 * pageSize) == 2,
                    "sparse removal did not visit both objects");
    passed &= check(!manager.contains(address, 3 * pageSize), "sparse objects survived removal");
    passed &= check(file.loans() == 0, "sparse removal retained backing loans");
    for (size_t page = 0; page < 3; ++page) {
      passed &= check(!space.isMapped(reinterpret_cast<void*>(address + page * pageSize)),
                      "sparse removal retained a PTE");
    }
    const uintptr_t requested = address;
    MemoryMapManager::MapStatus status;
    object = manager.mapAnon(address, 3 * pageSize, MemoryMappedObject::Read,
                             MemoryMapManager::Placement::FixedNoReplace, &status);
    passed &=
        check(object && address == requested && status == MemoryMapManager::MapStatus::Success,
              "sparse removal retained a reservation");
    if (object) {
      for (size_t page = 0; page < 3; ++page) {
        const uintptr_t at = address + page * pageSize;
        if (manager.faultIn(at, false)) {
          const volatile uint8_t* bytes = reinterpret_cast<const volatile uint8_t*>(at);
          for (size_t byte = 0; byte < pageSize; ++byte) {
            if (!check(bytes[byte] == 0, "sparse anonymous reuse retained file contents")) {
              ERROR("VM-OWNERSHIP-TEST: byte=" << page * pageSize + byte
                                               << " value=" << static_cast<unsigned>(bytes[byte]));
              passed = false;
              break;
            }
          }
        } else {
          passed &= check(false, "sparse anonymous reuse could not fault");
        }
      }
      manager.removeAndRelease(address, 3 * pageSize);
    }
    if (!passed) {
      break;
    }
    NOTICE("VM-OWNERSHIP-TEST: PASS sparse-split mask="
           << mask << " objects=0 ptes=0 loans=0 reservation=reused zero=verified");
  }
  manager.unmapAll();
  return 0;
}

bool sparseSplitOwnership() {
  Process* process = new Process(Scheduler::instance().getKernelProcess(), true);
  bool passed = false;
  Thread* worker = new Thread(process, sparseSplitWorker, &passed, nullptr, false, true, true);
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  delete process;
  return check(started && joined && passed, "sparse split ownership fixture");
}

int checkedSyncWorker(void* parameter) {
  bool& passed = *static_cast<bool*>(parameter);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  MemoryMapManager& manager = MemoryMapManager::instance();
  MemoryMapManager::OperationGuard operation(manager);
  ResizeProbeFile file(3);
  if (!file.initialise()) {
    return 0;
  }
  uintptr_t address = 0;
  MemoryMappedObject* object =
      manager.mapFile(&file, address, 3 * pageSize, MemoryMappedObject::Read, 0, false);
  if (!object || !manager.faultIn(address, false) || !manager.faultIn(address + pageSize, false)) {
    manager.unmapAll();
    return 0;
  }
  manager.removeAndRelease(address + 2 * pageSize, pageSize);
  Thread* thread = Processor::information().getCurrentThread();
  void* mapping = reinterpret_cast<void*>(address);
  file.rejectSync = true;
  thread->setErrno(0);
  passed = check(posix_msync(mapping, 3 * pageSize, MS_SYNC) == -1 &&
                     thread->getErrno() == Error::OutOfMemory && file.syncCalls == 0,
                 "msync range failure reached backing I/O or lost ENOMEM");
  thread->setErrno(0);
  passed &= check(posix_msync(mapping, 2 * pageSize, MS_SYNC) == -1 &&
                      thread->getErrno() == Error::IoError && file.syncCalls == 2,
                  "msync lost EIO or skipped a page after the first backend failure");
  file.rejectSync = false;
  thread->setErrno(0);
  passed &= check(posix_msync(mapping, 2 * pageSize, MS_SYNC) == 0 && file.syncCalls == 4,
                  "msync could not retry both failed pages");
  manager.removeAndRelease(address, 2 * pageSize);
  manager.unmapAll();
  if (passed) {
    NOTICE(
        "VM-OWNERSHIP-TEST: PASS checked-msync range=ENOMEM range-io=0 failure=EIO "
        "attempted=2 retry=2");
  }
  return 0;
}

bool checkedMappedSync() {
  Process* process = new Process(Scheduler::instance().getKernelProcess(), true);
  bool passed = false;
  Thread* worker = new Thread(process, checkedSyncWorker, &passed, nullptr, false, true, true);
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  delete process;
  return check(started && joined && passed, "checked mapped sync fixture");
}
}  // namespace

bool runVmMappedOwnershipRegressions() {
  NOTICE("VM-OWNERSHIP-TEST: BEGIN sparse-split-ownership");
  if (!sparseSplitOwnership()) {
    return false;
  }
  NOTICE("VM-OWNERSHIP-TEST: PASS sparse-split-ownership cases=4");
  NOTICE("VM-OWNERSHIP-TEST: BEGIN checked-msync");
  if (!checkedMappedSync()) {
    return false;
  }
  NOTICE("VM-OWNERSHIP-TEST: PASS mapped-ownership");
  return true;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
bool runHostedVmPermissionRegressions() {
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  bool passed = check(space.isAddressValid(reinterpret_cast<void*>(0x00007FFFFFFFFFFFULL)) &&
                          !space.isAddressValid(reinterpret_cast<void*>(0x0000800000000000ULL)) &&
                          !space.isAddressValid(reinterpret_cast<void*>(0xFFFF7FFFFFFFFFFFULL)) &&
                          space.isAddressValid(reinterpret_cast<void*>(0xFFFF800000000000ULL)),
                      "four-level canonical address boundaries");
  MemoryMapManager& manager = MemoryMapManager::instance();
  {
    MemoryMapManager::OperationGuard operation(manager);
    MemoryMapManager::OperationGuard nestedOperation(manager);
    MemoryMapManager::OperationGuard pressure(manager, true);
    passed &= check(operation && nestedOperation && !pressure,
                    "pressure recovery entered an active mapping operation");
  }
  {
    MemoryMapManager::OperationGuard pressure(manager, true);
    passed &= check(bool(pressure), "mapping operation did not release the pressure gate");
  }
  passed &= protectedClone(false);
  passed &= protectedClone(true);
  passed &= borrowedClones();
  passed &= failedMappedResize();
  passed &= runVmMappedOwnershipRegressions();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS vm-permission-ownership");
  }
  return passed;
}
#endif
