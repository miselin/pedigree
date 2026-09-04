/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/linux-resource-abi.h"
#include "modules/subsys/posix/system-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/times.h>

namespace {
constexpr int PreservedErrno = 173;

struct ResourceSyscallContext {
  ResourceSyscallContext(Process* process, int foreignPid)
      : process(process), foreignPid(foreignPid), passed(false), returned(0) {}

  Process* process;
  int foreignPid;
  bool passed;
  Atomic<size_t> returned;
};

int resourceSyscallWorker(void* parameter) {
  ResourceSyscallContext* context = reinterpret_cast<ResourceSyscallContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t address = 0;
  if (!context->process->getSpaceAllocator().allocate(pageSize, address)) {
    context->returned += 1;
    return 1;
  }

  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping = MemoryMapManager::instance().mapAnon(
      mappedAddress, pageSize, MemoryMappedObject::Read | MemoryMappedObject::Write);
  if (!mapping || mappedAddress != address) {
    if (mapping) {
      MemoryMapManager::instance().remove(mappedAddress, pageSize);
    }
    context->process->getSpaceAllocator().free(address, pageSize);
    context->returned += 1;
    return 1;
  }

  struct rlimit* nativeLimit = reinterpret_cast<struct rlimit*>(address + 64);
  LinuxRlimit64* linuxLimit = reinterpret_cast<LinuxRlimit64*>(address + 128);
  struct tms* processTimes = reinterpret_cast<struct tms*>(address + 384);
  struct rusage* usage = reinterpret_cast<struct rusage*>(address + 512);
  LinuxRusage64* linuxUsage = reinterpret_cast<LinuxRusage64*>(address + 1024);
  uint8_t* linuxUsageCanary = reinterpret_cast<uint8_t*>(linuxUsage + 1);
  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();
  struct rlimit* badNative = reinterpret_cast<struct rlimit*>(kernelStart);
  LinuxRlimit64* badLinux = reinterpret_cast<LinuxRlimit64*>(kernelStart);
  bool passed = true;

  thread->setErrno(PreservedErrno);
  passed &= posix_getrlimit(RLIMIT_CPU, nativeLimit) == 0 &&
            nativeLimit->rlim_cur == RLIM_INFINITY && nativeLimit->rlim_max == RLIM_INFINITY &&
            thread->getErrno() == PreservedErrno;

  thread->setErrno(0);
  passed &= posix_getrlimit(-1, nativeLimit) == -1 && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &=
      posix_getrlimit(RLIMIT_NOFILE, badNative) == -1 && thread->getErrno() == Error::BadAddress;
  thread->setErrno(PreservedErrno);
  passed &= posix_getrlimit(RLIMIT_RTPRIO, nativeLimit) == 0 && nativeLimit->rlim_cur == 0 &&
            nativeLimit->rlim_max == 0 && thread->getErrno() == PreservedErrno;
#ifdef RLIMIT_RTTIME
  thread->setErrno(PreservedErrno);
  passed &= posix_getrlimit(RLIMIT_RTTIME, nativeLimit) == 0 &&
            nativeLimit->rlim_cur == RLIM_INFINITY && nativeLimit->rlim_max == RLIM_INFINITY &&
            thread->getErrno() == PreservedErrno;
#endif

  thread->setErrno(PreservedErrno);
  passed &= posix_prlimit64(0, RLIMIT_NOFILE, nullptr, linuxLimit) == 0 &&
            linuxLimit->current == 16384 && linuxLimit->maximum == 16384 &&
            thread->getErrno() == PreservedErrno;
  thread->setErrno(PreservedErrno);
  passed &= posix_prlimit64(static_cast<int>(context->process->getId()), RLIMIT_NOFILE, nullptr,
                            nullptr) == 0 &&
            thread->getErrno() == PreservedErrno;

  thread->setErrno(0);
  passed &= posix_prlimit64(0, RLIMIT_NOFILE, badLinux, nullptr) == -1 &&
            thread->getErrno() == Error::Unimplemented;
  thread->setErrno(0);
  passed &= posix_prlimit64(0, RLIMIT_NOFILE, linuxLimit, nullptr) == -1 &&
            thread->getErrno() == Error::Unimplemented;
  thread->setErrno(0);
  passed &= posix_prlimit64(0, RLIMIT_NOFILE, nullptr, badLinux) == -1 &&
            thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_prlimit64(-1, RLIMIT_NOFILE, nullptr, linuxLimit) == -1 &&
            thread->getErrno() == Error::NoSuchProcess;
  thread->setErrno(0);
  passed &= posix_prlimit64(context->foreignPid, RLIMIT_NOFILE, nullptr, linuxLimit) == -1 &&
            thread->getErrno() == Error::Unimplemented;
  thread->setErrno(0);
  passed &= posix_prlimit64(0, -1, nullptr, linuxLimit) == -1 &&
            thread->getErrno() == Error::InvalidArgument;

  thread->setErrno(0);
  passed &=
      posix_setrlimit(RLIMIT_NOFILE, badNative) == -1 && thread->getErrno() == Error::Unimplemented;
  thread->setErrno(0);
  passed &= posix_setrlimit(RLIMIT_NOFILE, nativeLimit) == -1 &&
            thread->getErrno() == Error::Unimplemented;

  thread->setErrno(PreservedErrno);
  passed &= posix_membarrier(0, 0, 0) == 0 && thread->getErrno() == PreservedErrno;
  thread->setErrno(0);
  passed &= posix_membarrier(1, 0, 0) == -1 && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &= posix_membarrier(0, 1, 0) == -1 && thread->getErrno() == Error::InvalidArgument;

  constexpr Time::Timestamp clockTick = Time::Multiplier::Second / 100;
  thread->publishTimeAccountingForHostedTest(73 * clockTick, 41 * clockTick);
  context->process->publishTimeAccountingForHostedTest(235 * clockTick, 127 * clockTick);
  Time::Timestamp expectedUser = context->process->getUserTime();
  Time::Timestamp expectedKernel = context->process->getKernelTime();
  const clock_t elapsedBefore = Time::getTicks() / clockTick;
  thread->setErrno(PreservedErrno);
  const clock_t elapsed = posix_times(processTimes);
  const clock_t elapsedAfter = Time::getTicks() / clockTick;
  passed &= elapsed >= elapsedBefore && elapsed <= elapsedAfter &&
            processTimes->tms_utime == static_cast<clock_t>(expectedUser / clockTick) &&
            processTimes->tms_stime == static_cast<clock_t>(expectedKernel / clockTick) &&
            !processTimes->tms_cutime && !processTimes->tms_cstime &&
            thread->getErrno() == PreservedErrno;
  thread->setErrno(PreservedErrno);
  passed &= posix_times(nullptr) >= 0 && thread->getErrno() == PreservedErrno;
  thread->setErrno(0);
  passed &= posix_times(reinterpret_cast<struct tms*>(kernelStart)) == -1 &&
            thread->getErrno() == Error::BadAddress;

  expectedUser = context->process->getUserTime();
  expectedKernel = context->process->getKernelTime();
  ByteSet(usage, 0xA5, sizeof(*usage));
  thread->setErrno(PreservedErrno);
  passed &=
      posix_getrusage(RUSAGE_SELF, usage) == 0 &&
      usage->ru_utime.tv_sec == static_cast<time_t>(expectedUser / Time::Multiplier::Second) &&
      usage->ru_utime.tv_usec ==
          static_cast<suseconds_t>((expectedUser % Time::Multiplier::Second) /
                                   Time::Multiplier::Microsecond) &&
      usage->ru_stime.tv_sec == static_cast<time_t>(expectedKernel / Time::Multiplier::Second) &&
      usage->ru_stime.tv_usec ==
          static_cast<suseconds_t>((expectedKernel % Time::Multiplier::Second) /
                                   Time::Multiplier::Microsecond) &&
      thread->getErrno() == PreservedErrno;
  for (size_t i = sizeof(LinuxRusage64); i < sizeof(*usage); ++i) {
    passed &= !reinterpret_cast<uint8_t*>(usage)[i];
  }
  ByteSet(usage, 0xA5, sizeof(*usage));
  const Time::Timestamp threadUserBeforeNative = thread->getUserTime();
  const Time::Timestamp threadKernelBeforeNative = thread->getKernelTime();
  thread->setErrno(PreservedErrno);
  const bool nativeThreadUsageSucceeded = posix_getrusage(RUSAGE_THREAD, usage) == 0;
  const Time::Timestamp threadUserAfterNative = thread->getUserTime();
  const Time::Timestamp threadKernelAfterNative = thread->getKernelTime();
  const Time::Timestamp nativeThreadUser =
      static_cast<Time::Timestamp>(usage->ru_utime.tv_sec) * Time::Multiplier::Second +
      static_cast<Time::Timestamp>(usage->ru_utime.tv_usec) * Time::Multiplier::Microsecond;
  const Time::Timestamp nativeThreadKernel =
      static_cast<Time::Timestamp>(usage->ru_stime.tv_sec) * Time::Multiplier::Second +
      static_cast<Time::Timestamp>(usage->ru_stime.tv_usec) * Time::Multiplier::Microsecond;
  passed &= nativeThreadUsageSucceeded &&
            nativeThreadUser >=
                threadUserBeforeNative - (threadUserBeforeNative % Time::Multiplier::Microsecond) &&
            nativeThreadUser <=
                threadUserAfterNative - (threadUserAfterNative % Time::Multiplier::Microsecond) &&
            nativeThreadKernel >= threadKernelBeforeNative -
                                      (threadKernelBeforeNative % Time::Multiplier::Microsecond) &&
            nativeThreadKernel <= threadKernelAfterNative -
                                      (threadKernelAfterNative % Time::Multiplier::Microsecond) &&
            thread->getErrno() == PreservedErrno;
  for (size_t i = offsetof(struct rusage, ru_maxrss); i < sizeof(*usage); ++i) {
    passed &= !reinterpret_cast<uint8_t*>(usage)[i];
  }
  struct rusage untouched = {};
  ByteSet(&untouched, 0xA5, sizeof(untouched));
  MemoryCopy(usage, &untouched, sizeof(untouched));
  thread->setErrno(PreservedErrno);
  passed &= posix_getrusage(RUSAGE_CHILDREN, usage) == 0 && !usage->ru_utime.tv_sec &&
            !usage->ru_utime.tv_usec && !usage->ru_stime.tv_sec && !usage->ru_stime.tv_usec &&
            thread->getErrno() == PreservedErrno;
  for (size_t i = offsetof(struct rusage, ru_maxrss); i < sizeof(*usage); ++i) {
    passed &= !reinterpret_cast<uint8_t*>(usage)[i];
  }
  thread->setErrno(0);
  passed &= posix_getrusage(RUSAGE_SELF, reinterpret_cast<struct rusage*>(kernelStart)) == -1 &&
            thread->getErrno() == Error::BadAddress;

  ByteSet(linuxUsage, 0xA5, sizeof(*linuxUsage) + 32);
  thread->setErrno(PreservedErrno);
  passed &=
      posix_linux_getrusage(RUSAGE_SELF, linuxUsage) == 0 &&
      linuxUsage->userSeconds == static_cast<int64_t>(expectedUser / Time::Multiplier::Second) &&
      linuxUsage->userMicroseconds ==
          static_cast<int64_t>((expectedUser % Time::Multiplier::Second) /
                               Time::Multiplier::Microsecond) &&
      linuxUsage->systemSeconds ==
          static_cast<int64_t>(expectedKernel / Time::Multiplier::Second) &&
      linuxUsage->systemMicroseconds ==
          static_cast<int64_t>((expectedKernel % Time::Multiplier::Second) /
                               Time::Multiplier::Microsecond) &&
      !linuxUsage->maximumResidentSetSize && !linuxUsage->involuntaryContextSwitches &&
      thread->getErrno() == PreservedErrno;
  for (size_t i = 0; i < 32; ++i) {
    passed &= linuxUsageCanary[i] == 0xA5;
  }
  ByteSet(linuxUsage, 0xA5, sizeof(*linuxUsage) + 32);
  const Time::Timestamp threadUserBeforeLinux = thread->getUserTime();
  const Time::Timestamp threadKernelBeforeLinux = thread->getKernelTime();
  thread->setErrno(PreservedErrno);
  const bool linuxThreadUsageSucceeded = posix_linux_getrusage(RUSAGE_THREAD, linuxUsage) == 0;
  const Time::Timestamp threadUserAfterLinux = thread->getUserTime();
  const Time::Timestamp threadKernelAfterLinux = thread->getKernelTime();
  const Time::Timestamp linuxThreadUser =
      static_cast<Time::Timestamp>(linuxUsage->userSeconds) * Time::Multiplier::Second +
      static_cast<Time::Timestamp>(linuxUsage->userMicroseconds) * Time::Multiplier::Microsecond;
  const Time::Timestamp linuxThreadKernel =
      static_cast<Time::Timestamp>(linuxUsage->systemSeconds) * Time::Multiplier::Second +
      static_cast<Time::Timestamp>(linuxUsage->systemMicroseconds) * Time::Multiplier::Microsecond;
  passed &= linuxThreadUsageSucceeded &&
            linuxThreadUser >=
                threadUserBeforeLinux - (threadUserBeforeLinux % Time::Multiplier::Microsecond) &&
            linuxThreadUser <=
                threadUserAfterLinux - (threadUserAfterLinux % Time::Multiplier::Microsecond) &&
            linuxThreadKernel >= threadKernelBeforeLinux -
                                     (threadKernelBeforeLinux % Time::Multiplier::Microsecond) &&
            linuxThreadKernel <=
                threadKernelAfterLinux - (threadKernelAfterLinux % Time::Multiplier::Microsecond) &&
            thread->getErrno() == PreservedErrno;
  for (size_t i = offsetof(LinuxRusage64, maximumResidentSetSize); i < sizeof(*linuxUsage); ++i) {
    passed &= !reinterpret_cast<uint8_t*>(linuxUsage)[i];
  }
  for (size_t i = 0; i < 32; ++i) {
    passed &= linuxUsageCanary[i] == 0xA5;
  }
  ByteSet(linuxUsage, 0xA5, sizeof(*linuxUsage) + 32);
  thread->setErrno(PreservedErrno);
  passed &= posix_linux_getrusage(RUSAGE_CHILDREN, linuxUsage) == 0 && !linuxUsage->userSeconds &&
            !linuxUsage->userMicroseconds && !linuxUsage->systemSeconds &&
            !linuxUsage->systemMicroseconds && thread->getErrno() == PreservedErrno;
  for (size_t i = 0; i < sizeof(*linuxUsage); ++i) {
    passed &= !reinterpret_cast<uint8_t*>(linuxUsage)[i];
  }
  for (size_t i = 0; i < 32; ++i) {
    passed &= linuxUsageCanary[i] == 0xA5;
  }
  thread->setErrno(0);
  passed &=
      posix_linux_getrusage(RUSAGE_SELF, reinterpret_cast<LinuxRusage64*>(kernelStart)) == -1 &&
      thread->getErrno() == Error::BadAddress;

  char* requestedName = reinterpret_cast<char*>(address + 256);
  char* returnedName = reinterpret_cast<char*>(address + 320);
  StringCopy(requestedName, "0123456789abcdef-long");
  ByteSet(returnedName, 0xA5, 16);
  const String originalName = thread->getName();
  thread->setErrno(PreservedErrno);
  passed &= posix_prctl(PR_SET_NAME, reinterpret_cast<uint64_t>(requestedName), 0, 0, 0) == 0 &&
            thread->getName().compare("0123456789abcde") && thread->getErrno() == PreservedErrno;
  thread->setErrno(PreservedErrno);
  passed &= posix_prctl(PR_GET_NAME, reinterpret_cast<uint64_t>(returnedName), 0, 0, 0) == 0 &&
            !MemoryCompare(returnedName, "0123456789abcde", 15) && !returnedName[15] &&
            thread->getErrno() == PreservedErrno;
  thread->setErrno(0);
  passed &= posix_prctl(-1, 0, 0, 0, 0) == -1 && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &= posix_prctl(PR_SET_NAME, kernelStart, 0, 0, 0) == -1 &&
            thread->getErrno() == Error::BadAddress && thread->getName().compare("0123456789abcde");
  thread->setErrno(0);
  passed &= posix_prctl(PR_GET_NAME, kernelStart, 0, 0, 0) == -1 &&
            thread->getErrno() == Error::BadAddress;
  thread->setName(originalName);

  MemoryMapManager::instance().remove(address, pageSize);
  context->process->getSpaceAllocator().free(address, pageSize);
  context->passed = passed;
  context->returned += 1;
  return passed ? 0 : 1;
}

bool resourceSyscallSemantics(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  PosixProcess* foreignProcess = new PosixProcess(kernelProcess);
  foreignProcess->setSubsystem(new PosixSubsystem);
  foreignProcess->publish();

  ResourceSyscallContext context(process, static_cast<int>(foreignProcess->getId()));
  Thread* worker = new Thread(process, resourceSyscallWorker, &context, nullptr, false, true, true);
  worker->setName("hosted resource syscall semantics");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool passed = started && joined && context.returned == 1 && context.passed;
  delete foreignProcess;
  delete process;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL resource-syscall-semantics: "
        "limits, accounting, fail-closed mutation, usercopy, membarrier, or prctl names regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS resource-syscall-semantics");
  return true;
}
}  // namespace

bool runHostedResourceSyscallRegressions(Process* process) {
  return resourceSyscallSemantics(process);
}
