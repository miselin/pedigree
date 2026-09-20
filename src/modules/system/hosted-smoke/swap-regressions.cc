/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/VFS.h"

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS && THREADS
namespace {
constexpr size_t PageSize = 4096, DataPages = 16;
bool check(bool value, const char* detail) {
  if (!value)
    ERROR("SWAP-VM-TEST: FAIL " << detail);
  return value;
}
class DeferredPagingProcess final : public Process {
 public:
  explicit DeferredPagingProcess(Process* parent) : Process(DeferredPublication(), parent) {}
  ~DeferredPagingProcess() override {
    prepareForDestruction();
  }
  void publishForTest() {
    publish();
  }
  void closeForTest() {
    prepareForDestruction();
  }
};
class RecordingSwapDisk final : public Disk, public PagingTransport {
 public:
  RecordingSwapDisk() : bytes(new unsigned char[(DataPages + 1) * PageSize]) {
    if (bytes) {
      ByteSet(bytes, 0, getSize());
      bytes[1024] = 1;
      bytes[1028] = DataPages;
      MemoryCopy(bytes + PageSize - 10, "SWAPSPACE2", 10);
      reserveEndpoint();
      publishEndpoint();
    }
  }
  ~RecordingSwapDisk() override {
    retireEndpoint();
    delete[] bytes;
  }
  size_t getSize() const override {
    return (DataPages + 1) * PageSize;
  }
  bool pin(uint64_t) override {
    return false;
  }
  void unpin(uint64_t) override {}
  PagingStatus preparePagingTransport(PagingTransport*& out) override {
    out = bytes ? this : nullptr;
    return bytes ? PagingStatus::Success : PagingStatus::NoMemory;
  }
  PagingStatus transfer(PagingOperation operation, uint64_t offset, void* data) override {
    if (operation == PagingOperation::Flush) {
      ++flushes;
      return failFlush ? PagingStatus::IoError : PagingStatus::Success;
    }
    if (offset > getSize() - PageSize || (offset & (PageSize - 1)))
      return PagingStatus::Invalid;
    if (operation == PagingOperation::Read) {
      if (readsBeforeFailure == 0)
        return PagingStatus::IoError;
      if (readsBeforeFailure > 0)
        --readsBeforeFailure;
      ++reads;
      MemoryCopy(data, bytes + offset, PageSize);
    } else {
      ++writes;
      if (failWrite)
        return PagingStatus::IoError;
      MemoryCopy(bytes + offset, data, PageSize);
    }
    return PagingStatus::Success;
  }
  void release() override {
    ++releases;
  }
  unsigned char* bytes;
  bool failWrite = false, failFlush = false;
  ssize_t readsBeforeFailure = -1;
  size_t reads = 0, writes = 0, flushes = 0, releases = 0;
};
int swapWorker(void* opaque) {
  bool& passed = *static_cast<bool*>(opaque);
  auto& manager = MemoryMapManager::instance();
  auto& space = Processor::information().getVirtualAddressSpace();
  auto& memory = PhysicalMemoryManager::instance();
  RecordingSwapDisk disk;
  File emptyFile(String("swap-empty-backing"), 0, 0, 0, 1, nullptr, 0, nullptr);
  uintptr_t base = 0;
  const uint32_t endpoint = disk.endpointId();
  passed = [&]() {
    if (!check(disk.bytes && endpoint && SwapStore::instance().prepareAtBoot(), "fixture setup"))
      return false;
    disk.bytes[PageSize - 10] = 0;
    if (!check(manager.activateSwap(endpoint) == SwapStatus::Invalid, "unformatted rejected"))
      return false;
    disk.bytes[PageSize - 10] = 'S';
    disk.bytes[1032] = 1;
    if (!check(manager.activateSwap(endpoint) == SwapStatus::Unsupported, "bad-page list rejected"))
      return false;
    disk.bytes[1032] = 0;
    DiskUse loan;
    if (!check(disk.acquireUse(loan) && manager.activateSwap(endpoint) == SwapStatus::Busy,
               "ordinary ownership excludes activation"))
      return false;
    loan.reset();
    if (!check(manager.activateSwap(endpoint) == SwapStatus::Success &&
                   manager.activateSwap(endpoint) == SwapStatus::Busy && !disk.acquireUse(loan),
               "activation exclusivity"))
      return false;
    if (!check(VFS::unloadAdmission(false) == Module::UnloadAdmission::Busy &&
                   VFS::unloadAdmission(true) == Module::UnloadAdmission::KeepMapped,
               "active paging retains VFS service"))
      return false;
    if (!check(manager.mapAnon(base, 3 * PageSize,
                               MemoryMappedObject::Read | MemoryMappedObject::Write) != nullptr,
               "map"))
      return false;
    for (size_t n = 0; n < 3; ++n) {
      if (!check(manager.faultIn(base + n * PageSize, true), "initial population"))
        return false;
      ByteSet(reinterpret_cast<void*>(base + n * PageSize), 0x40 + n, PageSize);
    }
    AnonymousMemoryMap unenrolled(base, PageSize,
                                  MemoryMappedObject::Read | MemoryMappedObject::Write);
    if (!check(!unenrolled.supportsPageOut(space, base), "unenrolled owner stays resident"))
      return false;
    const auto before = memory.memorySnapshot();
    disk.failWrite = true;
    if (!check(manager.pageOutRange(base, PageSize) == SwapStatus::IoError &&
                   space.isMapped(reinterpret_cast<void*>(base)) &&
                   *reinterpret_cast<unsigned char*>(base) == 0x40 &&
                   !manager.swapSnapshot().usedPages,
               "failed write retains RAM and releases slot"))
      return false;
    disk.failWrite = false;
    disk.failFlush = true;
    if (!check(manager.pageOutRange(base, PageSize) == SwapStatus::IoError &&
                   space.isMapped(reinterpret_cast<void*>(base)) &&
                   !manager.swapSnapshot().usedPages,
               "failed flush retains RAM"))
      return false;
    disk.failFlush = false;
    if (!check(manager.compact() && !space.isMapped(reinterpret_cast<void*>(base)) &&
                   manager.swapSnapshot().usedPages == 1 &&
                   memory.memorySnapshot().freePages > before.freePages,
               "real compactor releases RAM"))
      return false;
    disk.readsBeforeFailure = 0;
    if (!check(!manager.faultIn(base, false) && !space.isMapped(reinterpret_cast<void*>(base)) &&
                   manager.swapSnapshot().usedPages == 1,
               "failed read retains slot"))
      return false;
    disk.readsBeforeFailure = -1;
    if (!check(manager.faultIn(base, false) && *reinterpret_cast<unsigned char*>(base) == 0x40 &&
                   !manager.swapSnapshot().usedPages,
               "read retry restores contents"))
      return false;
    NOTICE("SWAP-VM-TEST: PASS write-flush-read-retry-physical-release");
    if (!check(manager.pageOutRange(base, 3 * PageSize) == SwapStatus::Success &&
                   manager.swapSnapshot().usedPages == 3,
               "drain preparation"))
      return false;
    disk.readsBeforeFailure = 1;
    if (!check(manager.deactivateSwap(endpoint) == SwapStatus::IoError &&
                   manager.swapSnapshot().active && manager.swapSnapshot().usedPages == 2 &&
                   space.isMapped(reinterpret_cast<void*>(base)) &&
                   *reinterpret_cast<unsigned char*>(base) == 0x40,
               "partial I/O drain remains active"))
      return false;
    disk.readsBeforeFailure = -1;
    PhysicalMemoryManager::setTryAllocationFailureForTest(0);
    const auto exhausted = manager.deactivateSwap(endpoint);
    PhysicalMemoryManager::setTryAllocationFailureForTest(-1);
    if (!check(exhausted == SwapStatus::NoMemory && manager.swapSnapshot().usedPages == 2 &&
                   manager.swapSnapshot().active,
               "OOM drain retains remaining references"))
      return false;
    disk.failFlush = true;
    if (!check(manager.deactivateSwap(endpoint) == SwapStatus::IoError &&
                   manager.swapSnapshot().active && !manager.swapSnapshot().usedPages,
               "final flush failure retains empty channel"))
      return false;
    disk.failFlush = false;
    if (!check(manager.deactivateSwap(endpoint) == SwapStatus::Success &&
                   !manager.swapSnapshot().active && disk.releases == 3 &&
                   *reinterpret_cast<unsigned char*>(base + 2 * PageSize) == 0x42,
               "drain retry releases channel"))
      return false;
    NOTICE("SWAP-VM-TEST: PASS partial-drain-io-oom-retry");
    if (!check(manager.activateSwap(endpoint) == SwapStatus::Success &&
                   manager.pageOutRange(base, 3 * PageSize) == SwapStatus::Success,
               "clone preparation"))
      return false;
    Process* child = nullptr;
    {
      MemoryMapManager::OperationGuard operation(manager);
      child = new Process(Processor::information().getCurrentThread()->getParent());
      if (!check(child != nullptr, "child address space"))
        return false;
      AnonymousMemoryMap::setCloneFailureForTest(1);
      const bool cloned = manager.clone(child);
      AnonymousMemoryMap::setCloneFailureForTest(-1);
      const bool rollback = !cloned && manager.swapSnapshot().usedPages == 3;
      Processor::switchAddressSpace(*child->getAddressSpace());
      manager.unmapAll();
      Processor::switchAddressSpace(space);
      delete child;
      child = nullptr;
      if (!check(rollback, "failed clone reference rollback"))
        return false;
    }
    if (!check(manager.deactivateSwap(endpoint) == SwapStatus::Success,
               "clone failure did not retain references"))
      return false;
    if (!check(manager.activateSwap(endpoint) == SwapStatus::Success &&
                   manager.pageOutRange(base, 3 * PageSize) == SwapStatus::Success,
               "foreign owner preparation"))
      return false;
    {
      MemoryMapManager::OperationGuard operation(manager);
      auto* parent = Processor::information().getCurrentThread()->getParent();
      auto* target = new DeferredPagingProcess(parent);
      if (!check(target != nullptr, "unpublished target"))
        return false;
      bool owned = manager.clone(target);
      owned = check(owned && manager.deactivateSwap(endpoint) == SwapStatus::Busy &&
                        manager.swapSnapshot().active,
                    "unpublished owner blocks drain") &&
              owned;
      target->publishForTest();
      if (owned)
        owned = check(manager.deactivateSwap(endpoint) == SwapStatus::Success,
                      "published foreign owner restores");
      Processor::switchAddressSpace(*target->getAddressSpace());
      if (owned)
        owned = check(*reinterpret_cast<unsigned char*>(base + 2 * PageSize) == 0x42,
                      "foreign restored contents");
      if (owned)
        owned = check(manager.activateSwap(endpoint) == SwapStatus::Success &&
                          manager.pageOutRange(base, PageSize) == SwapStatus::Success,
                      "foreign owner pageout");
      Processor::switchAddressSpace(space);
      target->closeForTest();
      if (owned)
        owned = check(manager.deactivateSwap(endpoint) == SwapStatus::Busy &&
                          manager.swapSnapshot().usedPages == 1,
                      "closed owner retains slot");
      Processor::switchAddressSpace(*target->getAddressSpace());
      manager.unmapAll();
      Processor::switchAddressSpace(space);
      delete target;
      if (manager.swapSnapshot().active)
        owned = check(manager.deactivateSwap(endpoint) == SwapStatus::Success,
                      "retired owner releases drain") &&
                owned;
      if (!owned)
        return false;
    }
    NOTICE("SWAP-VM-TEST: PASS foreign-owner-publication-retirement");
    if (!check(VFS::unloadAdmission(false) == Module::UnloadAdmission::Busy &&
                   VFS::unloadAdmission(true) == Module::UnloadAdmission::KeepMapped,
               "registered mapping service remains after drain"))
      return false;
    NOTICE("SWAP-VM-TEST: PASS clone-metadata-failure-retirement");
    NOTICE("SWAP-VM-TEST: PASS vfs-service-retention");
    using Resolution = MemoryMapManager::FaultResolution;
    if (!check(manager.activateSwap(endpoint) == SwapStatus::Success &&
                   manager.pageOutRange(base, PageSize) == SwapStatus::Success &&
                   manager.setPermissions(base, 3 * PageSize, MemoryMappedObject::Read),
               "ordinary fault resolver preparation"))
      return false;
    const size_t reads = disk.reads;
    if (!check(manager.resolveUserFault(base, true, false, false) == Resolution::Unhandled &&
                   manager.resolveUserFault(base, false, false, true) == Resolution::Unhandled &&
                   manager.setPermissions(base, 3 * PageSize, MemoryMappedObject::None) &&
                   manager.resolveUserFault(base, false, false, false) == Resolution::Unhandled &&
                   disk.reads == reads && manager.swapSnapshot().usedPages == 1 &&
                   !space.isMapped(reinterpret_cast<void*>(base)),
               "readonly NX and inaccessible faults retain slot without I/O"))
      return false;
    if (!check(manager.setPermissions(base, 3 * PageSize, MemoryMappedObject::Read),
               "resolver read permission restored"))
      return false;
    disk.readsBeforeFailure = 0;
    if (!check(manager.resolveUserFault(base, false, false, false) == Resolution::Unhandled &&
                   manager.swapSnapshot().usedPages == 1 &&
                   !space.isMapped(reinterpret_cast<void*>(base)),
               "resolver I/O failure retains backing"))
      return false;
    disk.readsBeforeFailure = -1;
    if (!check(manager.resolveUserFault(base, false, false, false) == Resolution::Resolved &&
                   *reinterpret_cast<unsigned char*>(base) == 0x40 &&
                   !manager.swapSnapshot().usedPages &&
                   manager.resolveUserFault(base, true, true, false) == Resolution::Unhandled,
               "resolver retry restores contents and preserves write protection"))
      return false;
    if (!check(manager.setPermissions(base, 3 * PageSize,
                                      MemoryMappedObject::Read | MemoryMappedObject::Write) &&
                   manager.pageOutRange(base, PageSize) == SwapStatus::Success &&
                   manager.setPermissions(base, 3 * PageSize, MemoryMappedObject::Exec) &&
                   manager.resolveUserFault(base, false, false, true) == Resolution::Resolved &&
                   !manager.swapSnapshot().usedPages,
               "permitted fetch restores execute-only swapped page"))
      return false;
    const bool splitMappingCreated =
        manager.setPermissions(base + PageSize, PageSize,
                               MemoryMappedObject::Read | MemoryMappedObject::Write) == 1;
    if (!check(splitMappingCreated &&
                   manager.pageOutRange(base + PageSize, PageSize) == SwapStatus::Success &&
                   manager.resolveUserFault(base + PageSize, false, false, false) ==
                       Resolution::Resolved &&
                   *reinterpret_cast<unsigned char*>(base + PageSize) == 0x41 &&
                   !manager.swapSnapshot().usedPages,
               "split mapping fault resolver reuses selected object"))
      return false;
    uintptr_t empty = 0;
    if (!check(
            manager.mapFile(&emptyFile, empty, PageSize, MemoryMappedObject::Read) &&
                manager.resolveUserFault(empty, false, false, false) == Resolution::BackingFault &&
                manager.resolveUserFault(empty, true, false, false) == Resolution::Unhandled &&
                !space.isMapped(reinterpret_cast<void*>(empty)) &&
                manager.deactivateSwap(endpoint) == SwapStatus::Success,
            "EOF classified for SIGBUS after logical permission checks"))
      return false;
    NOTICE("SWAP-VM-TEST: PASS ordinary-fault-retry-permissions-EOF");
    return true;
  }();
  PhysicalMemoryManager::setTryAllocationFailureForTest(-1);
  AnonymousMemoryMap::setCloneFailureForTest(-1);
  disk.failWrite = disk.failFlush = false;
  disk.readsBeforeFailure = -1;
  manager.unmapAll();
  if (manager.swapSnapshot().active)
    passed =
        check(manager.deactivateSwap(endpoint) == SwapStatus::Success, "fixture drain") && passed;
  return 0;
}
}  // namespace
bool runSwapRegressions() {
  NOTICE("SWAP-VM-TEST: BEGIN");
  auto* process = new Process(Scheduler::instance().getKernelProcess(), true);
  if (!check(process != nullptr, "isolated process"))
    return false;
  bool passed = false;
  auto* worker = new Thread(process, swapWorker, &passed, nullptr, false, true, true);
  const bool started = worker && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started)
    delete worker;
  if (!joined && started)
    return check(false, "worker completion");
  delete process;
  passed = check(started && joined && passed, "isolated worker result");
  if (passed)
    NOTICE("SWAP-VM-TEST: END PASS");
  return passed;
}
#endif
