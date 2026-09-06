/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"

namespace {
using Status = MemoryMapManager::UserPageCopyStatus;
using Resident = VirtualAddressSpace::ResidentCopyStatus;
constexpr auto ReadWrite = MemoryMappedObject::Read | MemoryMappedObject::Write;
bool check(bool condition, const char* detail) {
  if (!condition)
    ERROR("PROCESS-MEMORY-BACKEND: FAIL " << detail);
  return condition;
}
bool filled(const uint8_t* bytes, size_t count, uint8_t value) {
  for (size_t i = 0; i < count; ++i)
    if (bytes[i] != value)
      return false;
  return true;
}

class ProbeFile final : public File {
 public:
  using File::sync;
  ProbeFile()
      : File(String("process-memory-probe"), 0, 0, 0, 1, nullptr,
             2 * PhysicalMemoryManager::getPageSize() + 137, nullptr),
        storage("Process Memory Probe") {
    enableFillCacheWriteback();
  }
  ~ProbeFile() override {
    shutdownFillCacheWriteback();
  }
  bool initialise() {
    if (!PhysicalMemoryManager::instance().allocateRegion(
            storage, 3, 0, VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write))
      return false;
    ByteSet(storage.virtualAddress(), 0x49, 3 * PhysicalMemoryManager::getPageSize());
    return true;
  }
  size_t getBlockSize() const override {
    return PhysicalMemoryManager::getPageSize() / 2;
  }
  bool isDirectPhysicalMapping() const override {
    return direct;
  }
  bool prepareSharedMapping(size_t, size_t) override {
    if (noMemory) {
      Processor::information().getCurrentThread()->setErrno(Error::OutOfMemory);
      return false;
    }
    return true;
  }
  bool sync(size_t offset, bool async) override {
    bool present = false;
    return syncFillCache(offset, async, present);
  }
  size_t loans() {
    return __atomic_load_n(&physicalPageLoans(), __ATOMIC_ACQUIRE);
  }
  bool backingEquals(size_t offset, uint8_t value) const {
    return static_cast<const uint8_t*>(storage.virtualAddress())[offset] == value;
  }
  bool failRead = false, noMemory = false, direct = false;

 protected:
  uintptr_t readBlock(uint64_t offset) override {
    if (failRead || offset >= getSize()) {
      Processor::information().getCurrentThread()->setErrno(Error::IoError);
      return FILE_BAD_BLOCK;
    }
    return reinterpret_cast<uintptr_t>(storage.virtualAddress()) + offset;
  }
  bool pinBlock(uint64_t offset) override {
    return !failRead && offset < getSize();
  }
  void unpinBlock(uint64_t) override {}
  void writeBlocks(uint64_t offset, uintptr_t source, size_t length) override {
    if (offset >= getSize())
      return;
    const size_t remaining = getSize() - offset;
    MemoryCopy(static_cast<uint8_t*>(storage.virtualAddress()) + offset,
               reinterpret_cast<const void*>(source), length < remaining ? length : remaining);
  }

 private:
  MemoryRegion storage;
};

struct Context {
  Semaphore ready{0}, release{0};
  VirtualAddressSpace* space = nullptr;
  ProbeFile* file = nullptr;
  ProbeFile* failure = nullptr;
  uintptr_t anonymous = 0, privateFile = 0, sharedFile = 0, readonly = 0, execute = 0;
  uintptr_t inaccessible = 0, failedFile = 0, raw = 0;
  bool setup = false, cleaned = false;
};

int ownerWorker(void* parameter) {
  auto& context = *static_cast<Context*>(parameter);
  auto& manager = MemoryMapManager::instance();
  auto& space = Processor::information().getVirtualAddressSpace();
  const size_t page = PhysicalMemoryManager::getPageSize();
  ProbeFile file, failure;
  VirtualAddressSpace::Stack* raw = nullptr;
  context.space = &space;
  context.file = &file;
  context.failure = &failure;
  {
    MemoryMapManager::OperationGuard operation(manager);
    manager.bindMemoryLockPolicy(space);
    context.setup = file.initialise() && failure.initialise() &&
                    manager.mapAnon(context.anonymous, page, ReadWrite) &&
                    manager.mapAnon(context.readonly, page, MemoryMappedObject::Read) &&
                    manager.mapAnon(context.execute, page, MemoryMappedObject::Exec) &&
                    manager.mapAnon(context.inaccessible, page, MemoryMappedObject::None) &&
                    manager.mapFile(&file, context.privateFile, page, ReadWrite, 0, true) &&
                    manager.mapFile(&file, context.sharedFile, 4 * page, ReadWrite, 0, false) &&
                    manager.mapFile(&failure, context.failedFile, page, ReadWrite, 0, false);
    if (context.setup) {
      raw = space.allocateStack(3 * page);
      context.setup = raw && raw->regionId();
      if (raw)
        context.raw = reinterpret_cast<uintptr_t>(raw->getBase());
    }
  }
  context.ready.release();
  if (!context.release.acquire(1, 30))
    FATAL("PROCESS-MEMORY-BACKEND: owner release timed out");
  {
    MemoryMapManager::OperationGuard operation(manager);
    manager.unmapAll();
    if (raw)
      space.freeStack(raw);
    context.cleaned = check(!file.loans() && !failure.loans(), "final file loan retirement");
    space.rawUserMemory().clear();
    space.setUserMemoryPolicy(nullptr);
  }
  return 0;
}

bool managedCopies(Context& context) {
  auto& manager = MemoryMapManager::instance();
  auto& space = *context.space;
  const size_t page = PhysicalMemoryManager::getPageSize();
  uint8_t bytes[64];
  MemoryMapManager::OperationGuard operation(manager);
  auto copy = [&](uintptr_t address, bool write) {
    return manager.copyUserPage(space, address, bytes, sizeof(bytes), write);
  };
  if (!check(!space.isMapped(reinterpret_cast<void*>(context.anonymous)) &&
                 !space.isMapped(reinterpret_cast<void*>(context.privateFile)),
             "initial lazy mappings"))
    return false;
  ByteSet(bytes, 0x71, sizeof(bytes));
  if (!check(copy(context.anonymous + 19, false) == Status::Success && filled(bytes, 64, 0),
             "remote anonymous zero read"))
    return false;
  ByteSet(bytes, 0x71, sizeof(bytes));
  if (!check(copy(context.anonymous + 19, true) == Status::Success,
             "remote anonymous write preparation"))
    return false;
  physical_uintptr_t physical = 0;
  size_t flags = 0;
  space.getMapping(reinterpret_cast<void*>(context.anonymous), physical, flags);
  if (!check((flags & (VirtualAddressSpace::Accessed | VirtualAddressSpace::Dirty)) ==
                     (VirtualAddressSpace::Accessed | VirtualAddressSpace::Dirty) &&
                 !(flags & VirtualAddressSpace::Shared) &&
                 copy(context.anonymous + 19, false) == Status::Success && filled(bytes, 64, 0x71),
             "anonymous bytes and latest PTE dirty state"))
    return false;

  if (!check(copy(context.privateFile + 19, false) == Status::Success && filled(bytes, 64, 0x49),
             "lazy private file read"))
    return false;
  physical_uintptr_t borrowed = 0;
  space.getMapping(reinterpret_cast<void*>(context.privateFile), borrowed, flags);
  if (!check((flags & VirtualAddressSpace::Borrowed) && !(flags & VirtualAddressSpace::Write) &&
                 context.file->loans() == 1,
             "read prematurely resolved private ownership"))
    return false;
  ByteSet(bytes, 0x72, sizeof(bytes));
  if (!check(copy(context.privateFile + 19, true) == Status::Success,
             "private file write preparation"))
    return false;
  space.getMapping(reinterpret_cast<void*>(context.privateFile), physical, flags);
  if (!check(physical != borrowed && !(flags & VirtualAddressSpace::Borrowed) &&
                 (flags & VirtualAddressSpace::Dirty) && !context.file->loans() &&
                 context.file->backingEquals(19, 0x49),
             "private file write changed backing or retained its loan"))
    return false;

  ByteSet(bytes, 0x73, sizeof(bytes));
  if (!check(copy(context.sharedFile + 19, true) == Status::Success && context.file->loans() == 1 &&
                 context.file->sync(0, false) && context.file->backingEquals(19, 0x73),
             "shared file writeback did not use its original loan"))
    return false;
  ByteSet(bytes, 0x7a, sizeof(bytes));
  if (!check(copy(context.sharedFile + 2 * page + 200, false) == Status::Success &&
                 filled(bytes, 64, 0),
             "partial EOF page zero tail"))
    return false;
  ByteSet(bytes, 0x7a, sizeof(bytes));
  return check(
      copy(context.sharedFile + 3 * page, false) == Status::Inaccessible &&
          copy(context.readonly, true) == Status::Inaccessible &&
          copy(context.execute, false) == Status::Inaccessible &&
          copy(context.inaccessible, false) == Status::Inaccessible &&
          copy(context.anonymous + page - 32, false) == Status::Inaccessible &&
          manager.copyUserPage(space, context.anonymous, bytes, 0, false) == Status::Inaccessible &&
          filled(bytes, 64, 0x7a) && !space.isMapped(reinterpret_cast<void*>(context.readonly)) &&
          !space.isMapped(reinterpret_cast<void*>(context.execute)),
      "denied fragments changed bytes or populated a denied page");
}

bool failedPreparation(Context& context) {
  auto& manager = MemoryMapManager::instance();
  auto& space = *context.space;
  auto& file = *context.failure;
  uint8_t bytes[16];
  ByteSet(bytes, 0x7b, sizeof(bytes));
  MemoryMapManager::OperationGuard operation(manager);
  auto* thread = Processor::information().getCurrentThread();
  const size_t previous = thread->getErrno();
  thread->setErrno(Error::BadFileDescriptor);
  file.noMemory = true;
  bool passed =
      check(manager.copyUserPage(space, context.failedFile, bytes, 16, true) == Status::NoMemory &&
                thread->getErrno() == Error::BadFileDescriptor &&
                !space.isMapped(reinterpret_cast<void*>(context.failedFile)) && !file.loans() &&
                filled(bytes, 16, 0x7b),
            "backing allocation rejection changed page, bytes or caller errno");
  file.noMemory = false;
  file.failRead = true;
  passed &=
      check(manager.copyUserPage(space, context.failedFile, bytes, 16, false) == Status::IoError &&
                thread->getErrno() == Error::BadFileDescriptor &&
                !space.isMapped(reinterpret_cast<void*>(context.failedFile)) && !file.loans() &&
                filled(bytes, 16, 0x7b),
            "backing I/O failure changed page, bytes or caller errno");
  file.failRead = false;
  file.direct = true;
  passed &= check(
      manager.copyUserPage(space, context.failedFile, bytes, 16, false) == Status::Unsupported &&
          !file.loans() && filled(bytes, 16, 0x7b),
      "direct physical backing admitted");
  file.direct = false;
  passed &=
      check(manager.copyUserPage(space, context.failedFile, bytes, 16, false) == Status::Success &&
                filled(bytes, 16, 0x49),
            "backing did not recover after preparation rejection");
  thread->setErrno(previous);
  return passed;
}

bool rawCopies(Context& context) {
  auto& manager = MemoryMapManager::instance();
  auto& space = *context.space;
  auto& memory = PhysicalMemoryManager::instance();
  const size_t page = memory.getPageSize();
  const uintptr_t base = context.raw;
  uint8_t bytes[16];
  ByteSet(bytes, 0x51, sizeof(bytes));
  MemoryMapManager::OperationGuard operation(manager);
  auto& raw = space.rawUserMemory();
  if (!check(raw.covers(base + 1, 3 * page - 2) && !raw.covers(base - 1, 2) &&
                 !raw.covers(base, 0) && !raw.covers(~uintptr_t(0) - 1, 4) &&
                 manager.copyUserPage(space, base, bytes, 16, true) == Status::Success,
             "raw coverage and initial bytes"))
    return false;
  UniquePointer<PreparedMemoryLock> hole;
  if (!check(raw.prepareReplacement(base + 2 * page, page, hole) == MemoryLockStatus::Success,
             "raw hole preparation"))
    return false;
  hole.get()->commit();
  if (!check(raw.covers(base, 2 * page) && !raw.covers(base, 3 * page) &&
                 !raw.covers(base + 2 * page, 1) &&
                 manager.copyUserPage(space, base + 2 * page, bytes, 16, false) ==
                     Status::Inaccessible,
             "raw replacement hole was treated as continuous heap"))
    return false;

  physical_uintptr_t original = 0, retired = 0;
  size_t flags = 0, retiredFlags = 0;
  space.getMapping(reinterpret_cast<void*>(base), original, flags);
  if (!check(space.detachMapping(reinterpret_cast<void*>(base + page), retired, retiredFlags),
             "CoW alias destination retirement"))
    return false;
  memory.freePage(retired);
  // Fresh allocations have no tracked references: enroll the original leaf
  // before adding the alias so resolving either CoW leaf retains the other.
  memory.pin(original);
  memory.pin(original);
  if (!space.map(original, reinterpret_cast<void*>(base + page),
                 VirtualAddressSpace::CopyOnWrite)) {
    memory.freePage(original);
    return check(false, "CoW alias publication");
  }
  space.setFlags(reinterpret_cast<void*>(base),
                 (flags & ~VirtualAddressSpace::Write) | VirtualAddressSpace::CopyOnWrite);
  if (!check(space.copyResidentUserPage(base, bytes, 16, true) == Resident::Inaccessible,
             "resident primitive wrote an unresolved CoW page"))
    return false;
  ByteSet(bytes, 0x52, sizeof(bytes));
  if (!check(manager.copyUserPage(space, base, bytes, 16, true) == Status::Success,
             "raw CoW preparation"))
    return false;
  physical_uintptr_t replacement = 0;
  space.getMapping(reinterpret_cast<void*>(base), replacement, flags);
  if (!check(replacement != original && (flags & VirtualAddressSpace::Dirty) &&
                 !(flags & VirtualAddressSpace::CopyOnWrite) &&
                 manager.copyUserPage(space, base + page, bytes, 16, false) == Status::Success &&
                 filled(bytes, 16, 0x51),
             "CoW replacement changed the retained alias"))
    return false;
  space.setFlags(reinterpret_cast<void*>(base), flags | VirtualAddressSpace::WriteProtected);
  ByteSet(bytes, 0x53, sizeof(bytes));
  if (!check(space.copyResidentUserPage(base, bytes, 16, true) == Resident::Inaccessible &&
                 manager.copyUserPage(space, base, bytes, 16, true) == Status::Inaccessible &&
                 manager.copyUserPage(space, base, bytes, 16, false) == Status::Success &&
                 filled(bytes, 16, 0x52),
             "write protection changed resident bytes"))
    return false;

  const auto runtime = memory.allocatePage();
  if (!check(runtime != 0, "runtime page allocation"))
    return false;
  void* runtimeAddress = reinterpret_cast<void*>(base + 2 * page);
  if (!space.map(runtime, runtimeAddress,
                 VirtualAddressSpace::Write | VirtualAddressSpace::RuntimeMapping)) {
    memory.freePage(runtime);
    return check(false, "runtime page publication");
  }
  bool passed =
      check(manager.copyUserPage(space, base + 2 * page, bytes, 16, true) == Status::Success,
            "classified runtime user RAM rejected");
  space.setFlags(runtimeAddress, VirtualAddressSpace::Write);
  passed &= check(
      manager.copyUserPage(space, base + 2 * page, bytes, 16, false) == Status::Inaccessible &&
          space.copyResidentUserPage(base + 2 * page, bytes, 16, false) == Resident::Success,
      "unclassified resident user RAM was admitted by the manager");
  if (!space.detachMapping(runtimeAddress, retired, retiredFlags))
    FATAL("PROCESS-MEMORY-BACKEND: runtime page retirement failed");
  memory.freePage(retired);
  return passed;
}
}  // namespace

EXPORTED_PUBLIC bool processMemoryBackendRegression() {
  NOTICE("PROCESS-MEMORY-BACKEND: BEGIN");
  auto* callerSpace = &Processor::information().getVirtualAddressSpace();
  Process* process = new Process(Scheduler::instance().getKernelProcess(), true);
  if (!check(process != nullptr, "owner process allocation"))
    return false;
  Context context;
  Thread* worker = new Thread(process, ownerWorker, &context, nullptr, false, true, true);
  const bool started = worker && worker->start();
  const bool ready = started && context.ready.acquire(1, 10);
  bool passed = check(ready && context.setup && context.space != callerSpace, "owner setup");
  if (passed)
    passed = managedCopies(context) && failedPreparation(context) && rawCopies(context);
  passed &= check(&Processor::information().getVirtualAddressSpace() == callerSpace,
                  "remote operation switched the caller's address space");
  context.release.release();
  if (worker && !started)
    worker->setUnwindState(Thread::TerminateThread);
  const bool joined = worker && worker->joinForCompletion();
  if (worker && !joined)
    FATAL("PROCESS-MEMORY-BACKEND: owner could not be joined safely");
  delete process;
  passed = check(joined && context.cleaned, "owner teardown") && passed;
  if (passed)
    NOTICE("PROCESS-MEMORY-BACKEND: END PASS");
  return passed;
}
