/* Copyright (c) 2026, Pedigree Developers. */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/hosted/FunctionProfile.h"
#include "pedigree/kernel/processor/hosted/smoke.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/utility.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/syscalls/translate.h"
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/VFS.h"
#include "system/kernel/core/processor/hosted/SyscallManager.h"
#include <sys/uio.h>

bool runHostedVasRegressions();
bool runHostedVmOperationGuardRegressions();
bool runHostedPostSyscallRegressions();
bool runHostedAccountingRegressions();

namespace {
constexpr size_t QueryCount = 1000000;
constexpr size_t IoCount = 1000;
constexpr size_t Repetitions = 3;
constexpr size_t PayloadSize = 4096;
constexpr uintptr_t ProfileUid = 123;

HostedSyscallState dispatch(uintptr_t number, uintptr_t p1 = 0, uintptr_t p2 = 0,
                            uintptr_t p3 = 0) {
  HostedSyscallState state = {};
  state.service = linuxCompat;
  state.number = number;
  state.p1 = p1;
  state.p2 = p2;
  state.p3 = p3;
  HostedSyscallManager::dispatchStateForTest(state);
  return state;
}

bool checkedCall(uintptr_t number, uintptr_t expected, uintptr_t p1 = 0, uintptr_t p2 = 0,
                 uintptr_t p3 = 0) {
  const HostedSyscallState state = dispatch(number, p1, p2, p3);
  if (state.result != expected || state.error) {
    ERROR("HOSTED-PROFILE: FAIL syscall=" << Dec << number
                                          << " result=" << static_cast<int64_t>(state.result)
                                          << " expected=" << expected << " errno=" << state.error);
    return false;
  }
  return true;
}

void reportPhase(const char* phase, size_t repetition, size_t count, Time::Timestamp elapsed) {
  NOTICE("HOSTED-PROFILE: phase=" << phase << " rep=" << Dec << repetition + 1 << " count=" << count
                                  << " elapsed_ns=" << elapsed);
}

NEVER_INLINE bool hostedProfileGetuid(size_t repetition, size_t count) {
  hostedFunctionProfileBegin("getuid", repetition, count);
  const Time::Timestamp start = Time::getTicks();
  for (size_t i = 0; i < count; ++i) {
    if (!checkedCall(PedigreeLinuxAmd64Syscall_getuid, ProfileUid)) {
      hostedFunctionProfileEnd();
      return false;
    }
  }
  const Time::Timestamp elapsed = Time::getTicks() - start;
  const bool captured = hostedFunctionProfileEnd();
  reportPhase("getuid", repetition, count, elapsed);
  return captured;
}

NEVER_INLINE bool hostedProfileLseek(size_t repetition, size_t count, int fd) {
  hostedFunctionProfileBegin("lseek", repetition, count);
  const Time::Timestamp start = Time::getTicks();
  for (size_t i = 0; i < count; ++i) {
    if (!checkedCall(PedigreeLinuxAmd64Syscall_lseek, 0, fd, 0, SEEK_SET)) {
      hostedFunctionProfileEnd();
      return false;
    }
  }
  const Time::Timestamp elapsed = Time::getTicks() - start;
  const bool captured = hostedFunctionProfileEnd();
  reportPhase("lseek", repetition, count, elapsed);
  return captured;
}

NEVER_INLINE bool hostedProfileWritev(size_t repetition, size_t count, int fd,
                                      const iovec* vectors) {
  hostedFunctionProfileBegin("lseek-writev", repetition, count);
  const Time::Timestamp start = Time::getTicks();
  for (size_t i = 0; i < count; ++i) {
    if (!checkedCall(PedigreeLinuxAmd64Syscall_lseek, 0, fd, 0, SEEK_SET) ||
        !checkedCall(PedigreeLinuxAmd64Syscall_writev, PayloadSize, fd,
                     reinterpret_cast<uintptr_t>(vectors), 2)) {
      hostedFunctionProfileEnd();
      return false;
    }
  }
  const Time::Timestamp elapsed = Time::getTicks() - start;
  const bool captured = hostedFunctionProfileEnd();
  reportPhase("lseek-writev", repetition, count, elapsed);
  return captured;
}

NEVER_INLINE bool hostedProfileReadv(size_t repetition, size_t count, int fd,
                                     const iovec* vectors) {
  hostedFunctionProfileBegin("lseek-readv", repetition, count);
  const Time::Timestamp start = Time::getTicks();
  for (size_t i = 0; i < count; ++i) {
    if (!checkedCall(PedigreeLinuxAmd64Syscall_lseek, 0, fd, 0, SEEK_SET) ||
        !checkedCall(PedigreeLinuxAmd64Syscall_readv, PayloadSize, fd,
                     reinterpret_cast<uintptr_t>(vectors), 2)) {
      hostedFunctionProfileEnd();
      return false;
    }
  }
  const Time::Timestamp elapsed = Time::getTicks() - start;
  const bool captured = hostedFunctionProfileEnd();
  reportPhase("lseek-readv", repetition, count, elapsed);
  return captured;
}

bool verifyPayload(int fd, const iovec* vectors, const uint8_t* expected, uint8_t* actual) {
  ByteSet(actual, 0, PayloadSize);
  if (!checkedCall(PedigreeLinuxAmd64Syscall_lseek, 0, fd, 0, SEEK_SET) ||
      !checkedCall(PedigreeLinuxAmd64Syscall_readv, PayloadSize, fd,
                   reinterpret_cast<uintptr_t>(vectors), 2)) {
    return false;
  }
  for (size_t i = 0; i < PayloadSize; ++i) {
    if (actual[i] != expected[i]) {
      ERROR("HOSTED-PROFILE: FAIL payload offset=" << Dec << i);
      return false;
    }
  }
  return true;
}

bool runMappedWork(uintptr_t address, size_t pageSize, size_t payloadSpan, size_t queryCount,
                   size_t ioCount) {
  auto* writeVectors = reinterpret_cast<iovec*>(address);
  auto* readVectors = writeVectors + 2;
  char* path = reinterpret_cast<char*>(readVectors + 2);
  constexpr char Path[] = "/syscall-profile";
  MemoryCopy(path, Path, sizeof(Path));
  auto* source = reinterpret_cast<uint8_t*>(address + pageSize);
  auto* destination = source + payloadSpan;
  writeVectors[0] = {source, PayloadSize / 2};
  writeVectors[1] = {source + PayloadSize / 2, PayloadSize / 2};
  readVectors[0] = {destination, PayloadSize / 2};
  readVectors[1] = {destination + PayloadSize / 2, PayloadSize / 2};

  const HostedSyscallState opened =
      dispatch(PedigreeLinuxAmd64Syscall_open, reinterpret_cast<uintptr_t>(path),
               O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (opened.error || static_cast<int64_t>(opened.result) < 0) {
    ERROR("HOSTED-PROFILE: FAIL open result=" << Dec << static_cast<int64_t>(opened.result)
                                              << " errno=" << opened.error);
    return false;
  }
  const int fd = static_cast<int>(opened.result);

  // Allocate the file's backing and fault in both copy directions before timing.
  bool passed = checkedCall(PedigreeLinuxAmd64Syscall_writev, PayloadSize, fd,
                            reinterpret_cast<uintptr_t>(writeVectors), 2) &&
                verifyPayload(fd, readVectors, source, destination);
  for (size_t repetition = 0; repetition < Repetitions && passed; ++repetition) {
    for (size_t i = 0; i < PayloadSize; ++i) {
      source[i] = static_cast<uint8_t>(i * 37 + repetition + 1);
    }
    passed = hostedProfileGetuid(repetition, queryCount) &&
             hostedProfileLseek(repetition, ioCount, fd) &&
             hostedProfileWritev(repetition, ioCount, fd, writeVectors) &&
             verifyPayload(fd, readVectors, source, destination);
    if (passed) {
      ByteSet(destination, 0, PayloadSize);
      passed = hostedProfileReadv(repetition, ioCount, fd, readVectors);
      for (size_t i = 0; i < PayloadSize && passed; ++i) {
        if (source[i] != destination[i]) {
          ERROR("HOSTED-PROFILE: FAIL readv payload offset=" << Dec << i);
          passed = false;
        }
      }
    }
  }
  const bool closed = checkedCall(PedigreeLinuxAmd64Syscall_close, 0, fd);
  return passed && closed;
}

struct ProfileContext {
  size_t queryCount;
  size_t ioCount;
  bool getuidOnly;
  bool passed = false;
  Atomic<size_t> returned{0};
};

int profileWorker(void* parameter) {
  auto* context = static_cast<ProfileContext*>(parameter);
  if (context->getuidOnly) {
    context->passed = true;
    for (size_t i = 0; i < 1000 && context->passed; ++i) {
      context->passed = checkedCall(PedigreeLinuxAmd64Syscall_getuid, ProfileUid);
    }
    for (size_t repetition = 0; repetition < Repetitions && context->passed; ++repetition) {
      context->passed = hostedProfileGetuid(repetition, context->queryCount);
    }
    context->returned += 1;
    return context->passed ? 0 : 1;
  }
  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread->getParent();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t payloadSpan = (PayloadSize + pageSize - 1) & ~(pageSize - 1);
  const size_t length = pageSize + payloadSpan * 2;
  uintptr_t address = 0;
  if (!process->allocateUserRange(Process::UserRegion::Normal, length, address)) {
    ERROR("HOSTED-PROFILE: FAIL user-range");
    context->returned += 1;
    return 1;
  }
  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping = MemoryMapManager::instance().mapAnon(
      mappedAddress, length, MemoryMappedObject::Read | MemoryMappedObject::Write);
  if (mapping && mappedAddress == address) {
    ByteSet(reinterpret_cast<void*>(address), 0, length);
    thread->setErrno(0);
    context->passed =
        runMappedWork(address, pageSize, payloadSpan, context->queryCount, context->ioCount);
  } else {
    ERROR("HOSTED-PROFILE: FAIL user-mapping");
  }
  MemoryMapManager::instance().remove(mappedAddress, length);
  process->freeUserRange(Process::UserRegion::Normal, address, length);
  context->returned += 1;
  return context->passed ? 0 : 1;
}
}  // namespace

bool hostedRunSyscallProfile() {
  const char* phase = getenv("PEDIGREE_HOSTED_PROFILE_PHASE");
  const bool getuidOnly = phase && !strcmp(phase, "getuid");
  if (phase && *phase && strcmp(phase, "all") && !getuidOnly) {
    ERROR("HOSTED-PROFILE: FAIL phase must be all or getuid");
    return false;
  }
  if (!runHostedVasRegressions()) {
    ERROR("HOSTED-PROFILE: FAIL address-space regressions");
    return false;
  }
  if (!runHostedVmOperationGuardRegressions()) {
    ERROR("HOSTED-PROFILE: FAIL operation-guard regressions");
    return false;
  }
  if (!runHostedPostSyscallRegressions()) {
    ERROR("HOSTED-PROFILE: FAIL post-syscall regressions");
    return false;
  }
  Thread* driver = Processor::information().getCurrentThread();
  if (!driver || !driver->runHostedStateCleanupRegression()) {
    ERROR("HOSTED-PROFILE: FAIL state-cleanup regressions");
    return false;
  }
  NOTICE("HOSTED-PROFILE: PASS state-cleanup regressions");
  if (!runHostedAccountingRegressions()) {
    ERROR("HOSTED-PROFILE: FAIL accounting regressions");
    return false;
  }
  const size_t divisor = hostedSyscallProfileDivisor();
  size_t queryCount = QueryCount / divisor;
  size_t ioCount = IoCount / divisor;
  if (!queryCount)
    queryCount = 1;
  if (!ioCount)
    ioCount = 1;
#if PEDIGREE_HOSTED_FUNCTION_PROFILE
  queryCount = hostedFunctionProfileCount(queryCount);
  ioCount = hostedFunctionProfileCount(ioCount);
#endif
  NOTICE("HOSTED-PROFILE: BEGIN scope=kernel-origin user-entry-return=excluded abi=linux");
  NOTICE("HOSTED-PROFILE: config uid=123 payload_bytes=4096 iov_count=2 repetitions=3 divisor="
         << Dec << divisor << " phase=" << (getuidOnly ? "getuid" : "all"));
  Process* kernelProcess = Scheduler::instance().getKernelProcess();
  if (!kernelProcess) {
    ERROR("HOSTED-PROFILE: FAIL kernel-process");
    return false;
  }

  RamFs filesystem;
  VFS::HostedRootViewScope fixture;
  if (!filesystem.initialise(nullptr) || !fixture.open(&filesystem)) {
    ERROR("HOSTED-PROFILE: FAIL ramfs-root");
    return false;
  }
  auto* process = new PosixProcess(kernelProcess, true, Process::FilesystemContextMode::Deferred);
  auto* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  subsystem->setAbi(PosixSubsystem::LinuxAbi);
  process->setUserId(ProfileUid);
  process->setEffectiveUserId(ProfileUid);
  const bool contextInstalled = fixture.installContext(*process);

  ProfileContext context{queryCount, ioCount, getuidOnly};
  auto* worker = new Thread(process, profileWorker, &context, nullptr, false, true, true);
  worker->setName("hosted syscall profile");
  process->publish();
  const bool started = contextInstalled && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  } else if (!joined) {
    FATAL("HOSTED-PROFILE: FAIL worker lifetime barrier");
  }
  const bool passed = started && joined && context.returned == 1 && context.passed;
  delete process;
  if (!fixture.close()) {
    FATAL("HOSTED-PROFILE: FAIL retained filesystem owners");
  }
  if (passed) {
    NOTICE("HOSTED-PROFILE: PASS all");
  } else {
    ERROR("HOSTED-PROFILE: FAIL fixture");
  }
  return passed;
}
