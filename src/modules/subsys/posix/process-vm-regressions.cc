/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/utility.h"

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "process-vm-syscalls.h"
#include <sys/uio.h>

namespace {
constexpr auto ReadWrite = MemoryMappedObject::Read | MemoryMappedObject::Write;
bool check(bool condition, const char* detail) {
  if (!condition)
    ERROR("PROCESS-VM-REGRESSION: FAIL " << detail);
  return condition;
}
bool fillPage(uintptr_t address, uint8_t* scratch, size_t page, uint8_t value) {
  ByteSet(scratch, value, page);
  return PosixSubsystem::copyToUser(reinterpret_cast<void*>(address), scratch, page);
}
bool pageEquals(uintptr_t address, uint8_t* scratch, size_t page, uint8_t value) {
  if (!PosixSubsystem::copyFromUser(scratch, reinterpret_cast<void*>(address), page)) {
    ERROR("PROCESS-VM-REGRESSION: page read failed address=" << Hex << address);
    return false;
  }
  for (size_t n = 0; n < page; ++n) {
    if (scratch[n] != value) {
      ERROR("PROCESS-VM-REGRESSION: page mismatch address="
            << Hex << address << " byte=" << Dec << n << " expected="
            << static_cast<unsigned>(value) << " actual=" << static_cast<unsigned>(scratch[n]));
      return false;
    }
  }
  return true;
}
struct UserVectors {
  iovec local, remote[2];
};
struct Cutoff {
  PosixSubsystem& subsystem;
  VirtualAddressSpace& space;
  uintptr_t replacement;
  uint8_t* scratch;
  size_t page, calls = 0;
  bool changed = false;
};
void replaceAfterFragment(size_t copied, void* parameter) {
  auto& cutoff = *static_cast<Cutoff*>(parameter);
  ++cutoff.calls;
  setProcessVmAfterFragmentHookForTest(nullptr, nullptr, nullptr);
  if (!check(copied == cutoff.page, "hook did not follow exactly one committed page"))
    return;
  auto& manager = MemoryMapManager::instance();
  MemoryMapManager::OperationGuard operation(manager);
  cutoff.subsystem.invalidateUserImage();
  uintptr_t replacement = cutoff.replacement;
  if (!check(manager.mapAnon(replacement, cutoff.page, ReadWrite,
                             MemoryMapManager::Placement::FixedReplace, nullptr) &&
                 replacement == cutoff.replacement &&
                 fillPage(replacement, cutoff.scratch, cutoff.page, 0x52),
             "replacement page publication"))
    return;
  cutoff.changed =
      check(cutoff.subsystem.publishUserImage(cutoff.space), "replacement image publication");
}

bool generationCutoff(bool write) {
  auto* thread = Processor::information().getCurrentThread();
  auto* process = static_cast<PosixProcess*>(thread->getParent());
  auto& subsystem = *static_cast<PosixSubsystem*>(process->getSubsystem());
  auto& space = *process->getAddressSpace();
  auto& manager = MemoryMapManager::instance();
  const size_t page = PhysicalMemoryManager::getPageSize();
  auto scratch = UniqueArray<uint8_t>::allocate(page);
  if (!check(static_cast<bool>(scratch), "scratch allocation"))
    return false;
  uintptr_t local = 0, remote[2] = {}, metadata = 0;
  bool passed = false;
  {
    MemoryMapManager::OperationGuard operation(manager);
    const bool mapped = manager.mapAnon(local, 2 * page, ReadWrite) &&
                        manager.mapAnon(remote[0], page, ReadWrite) &&
                        manager.mapAnon(remote[1], page, ReadWrite) &&
                        manager.mapAnon(metadata, page, ReadWrite);
    if (!check(mapped, "owned user vector and independent payload mappings")) {
      manager.unmapAll();
      return false;
    }
  }
  auto* vectors = reinterpret_cast<UserVectors*>(metadata);
  const uint8_t localValue = write ? 0x61 : 0x70;
  const UserVectors initial{
      {reinterpret_cast<void*>(local), 2 * page},
      {{reinterpret_cast<void*>(remote[0]), page}, {reinterpret_cast<void*>(remote[1]), page}}};
  passed = [&]() {
    if (!check(fillPage(local, scratch.get(), page, localValue) &&
                   fillPage(local + page, scratch.get(), page, localValue) &&
                   fillPage(remote[0], scratch.get(), page, 0x41) &&
                   fillPage(remote[1], scratch.get(), page, 0x41) &&
                   PosixSubsystem::copyToUser(vectors, &initial, sizeof(initial)) &&
                   subsystem.publishUserImage(space),
               "owned image setup"))
      return false;

    Cutoff cutoff{subsystem, space, remote[1], scratch.get(), page};
    setProcessVmAfterFragmentHookForTest(thread, replaceAfterFragment, &cutoff);
    thread->setErrno(Error::IoError);
    const ssize_t result = write
                               ? posix_process_vm_writev(process->getUserspaceId(), &vectors->local,
                                                         1, vectors->remote, 2, 0)
                               : posix_process_vm_readv(process->getUserspaceId(), &vectors->local,
                                                        1, vectors->remote, 2, 0);
    const size_t error = thread->getErrno();
    setProcessVmAfterFragmentHookForTest(nullptr, nullptr, nullptr);
    if (!check(result == static_cast<ssize_t>(page) && !error && cutoff.calls == 1 &&
                   cutoff.changed && &Processor::information().getVirtualAddressSpace() == &space,
               "generation change did not stop actual transfer at its committed prefix"))
      return false;
    if (!check(pageEquals(local, scratch.get(), page, write ? 0x61 : 0x41) &&
                   pageEquals(local + page, scratch.get(), page, localValue) &&
                   pageEquals(remote[0], scratch.get(), page, write ? 0x61 : 0x41) &&
                   pageEquals(remote[1], scratch.get(), page, 0x52),
               "old transfer touched replacement or uncommitted local suffix"))
      return false;

    const UserVectors fresh{{reinterpret_cast<void*>(local + page), page},
                            {{reinterpret_cast<void*>(remote[1]), page}, {nullptr, 0}}};
    if (!check(PosixSubsystem::copyToUser(vectors, &fresh, sizeof(fresh)),
               "fresh user vector import setup"))
      return false;
    const ssize_t freshResult =
        write ? posix_process_vm_writev(process->getUserspaceId(), &vectors->local, 1,
                                        vectors->remote, 1, 0)
              : posix_process_vm_readv(process->getUserspaceId(), &vectors->local, 1,
                                       vectors->remote, 1, 0);
    const size_t freshError = thread->getErrno();
    const bool sameSpace = &Processor::information().getVirtualAddressSpace() == &space;
    const bool content =
        pageEquals(write ? remote[1] : local + page, scratch.get(), page, write ? 0x61 : 0x52);
    const bool freshPassed =
        freshResult == static_cast<ssize_t>(page) && !freshError && content && sameSpace;
    if (!freshPassed)
      ERROR("PROCESS-VM-REGRESSION: fresh-" << (write ? "write" : "read") << " result=" << Dec
                                            << freshResult << " errno=" << freshError
                                            << " expected=" << page << " same-space=" << sameSpace);
    return check(freshPassed, "fresh syscall could not transfer through the replacement image");
  }();
  setProcessVmAfterFragmentHookForTest(nullptr, nullptr, nullptr);
  {
    MemoryMapManager::OperationGuard operation(manager);
    subsystem.invalidateUserImage();
    manager.unmapAll();
  }
  if (passed)
    NOTICE("PROCESS-VM-REGRESSION: PASS generation-" << (write ? "write" : "read"));
  return passed;
}
int regressionWorker(void* parameter) {
  bool& passed = *static_cast<bool*>(parameter);
  passed = generationCutoff(false) && generationCutoff(true);
  return 0;
}
}  // namespace

EXPORTED_PUBLIC bool processVmRegression() {
  NOTICE("PROCESS-VM-REGRESSION: BEGIN");
  auto* process = new PosixProcess(Scheduler::instance().getKernelProcess());
  if (!process)
    return check(false, "isolated process allocation");
  auto* subsystem = new PosixSubsystem;
  if (!subsystem) {
    delete process;
    return check(false, "isolated subsystem allocation");
  }
  process->setSubsystem(subsystem);
  subsystem->setAbi(PosixSubsystem::LinuxAbi);
  bool passed = false;
  Thread* worker = new Thread(process, regressionWorker, &passed, nullptr, false, true, true);
  if (!worker) {
    delete process;
    return check(false, "isolated worker allocation");
  }
  process->publish();
  const bool started = worker->start();
  if (!started)
    worker->setUnwindState(Thread::TerminateThread);
  const bool joined = worker->joinForCompletion();
  if (!joined)
    FATAL("PROCESS-VM-REGRESSION: worker could not be joined safely");
  delete process;
  passed = check(started && joined && passed, "isolated transfer worker") && passed;
  if (passed)
    NOTICE("PROCESS-VM-REGRESSION: END PASS");
  return passed;
}
