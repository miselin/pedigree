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
#include <sys/resource.h>

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
        "query, fail-closed mutation, usercopy, or membarrier capability regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS resource-syscall-semantics");
  return true;
}
}  // namespace

bool runHostedResourceSyscallRegressions(Process* process) {
  return resourceSyscallSemantics(process);
}
