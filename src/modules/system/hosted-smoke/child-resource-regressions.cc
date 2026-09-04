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
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/ZombieQueue.h"
#include "pedigree/kernel/utilities/lib.h"

#include <signal.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/linux-resource-abi.h"
#include "modules/subsys/posix/system-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/wait.h>

namespace {
constexpr size_t HostedAttempts = 10000;
constexpr int PreservedErrno = 173;

struct SeededExitContext {
  SeededExitContext(Time::Timestamp user, Time::Timestamp kernel, bool gated)
      : user(user), kernel(kernel), ready(0), release(gated ? 0 : 1) {}

  Time::Timestamp user;
  Time::Timestamp kernel;
  Atomic<size_t> ready;
  Atomic<size_t> release;
};

struct DescendantExitContext {
  DescendantExitContext(Time::Timestamp childUser, Time::Timestamp childKernel,
                        Time::Timestamp descendantUser, Time::Timestamp descendantKernel)
      : childUser(childUser),
        childKernel(childKernel),
        descendant(descendantUser, descendantKernel, false),
        ready(0),
        release(0),
        descendantReaped(false) {}

  Time::Timestamp childUser;
  Time::Timestamp childKernel;
  SeededExitContext descendant;
  Atomic<size_t> ready;
  Atomic<size_t> release;
  bool descendantReaped;
};

struct ConcurrentWaitContext {
  explicit ConcurrentWaitContext(int pid)
      : pid(pid), ready(0), go(0), returned(0), result(0), error(0) {}

  int pid;
  Atomic<size_t> ready;
  Atomic<size_t> go;
  Atomic<size_t> returned;
  int result;
  int error;
};

struct ChildResourceContext {
  explicit ChildResourceContext(PosixProcess* process)
      : process(process), passed(false), returned(0) {}

  PosixProcess* process;
  bool passed;
  Atomic<size_t> returned;
};

bool waitForValue(const Atomic<size_t>& value) {
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    if (value) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool waitForTermination(Process* process) {
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    if (process->isTerminationReapableForHostedTest()) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

int seededExit(void* parameter) {
  SeededExitContext* context = reinterpret_cast<SeededExitContext*>(parameter);
  context->ready += 1;
  while (!context->release) {
    Scheduler::instance().yield();
  }

  Thread* current = Processor::information().getCurrentThread();
  current->publishTimeAccountingForHostedTest(context->user, context->kernel);
  current->deferProcessExit(0);
  Thread::threadExited();
}

int reapDescendantAndExit(void* parameter) {
  DescendantExitContext* context = reinterpret_cast<DescendantExitContext*>(parameter);
  context->ready += 1;
  while (!context->release) {
    Scheduler::instance().yield();
  }

  Process* process = Processor::information().getCurrentThread()->getParent();
  PosixProcess* descendant = new PosixProcess(process);
  descendant->setSubsystem(new PosixSubsystem);
  Thread* descendantThread =
      new Thread(descendant, seededExit, &context->descendant, nullptr, false, true, true);
  descendantThread->setName("hosted resource descendant");
  descendant->publish();
  const int descendantPid = static_cast<int>(descendant->getId());
  if (descendantThread->start()) {
    context->descendantReaped = posix_waitpid(descendantPid, nullptr, 0, nullptr) == descendantPid;
  } else {
    delete descendantThread;
    delete descendant;
  }

  Thread* current = Processor::information().getCurrentThread();
  current->publishTimeAccountingForHostedTest(context->childUser, context->childKernel);
  current->deferProcessExit(context->descendantReaped ? 0 : 1);
  Thread::threadExited();
}

int concurrentWait(void* parameter) {
  ConcurrentWaitContext* context = reinterpret_cast<ConcurrentWaitContext*>(parameter);
  Thread* current = Processor::information().getCurrentThread();
  context->ready += 1;
  while (!context->go) {
    Scheduler::instance().yield();
  }

  current->setErrno(PreservedErrno);
  context->result = posix_waitpid(context->pid, nullptr, 0, nullptr);
  context->error = current->getErrno();
  context->returned += 1;
  return 0;
}

Time::Timestamp linuxUserTime(const LinuxRusage64& usage) {
  return static_cast<Time::Timestamp>(usage.userSeconds) * Time::Multiplier::Second +
         static_cast<Time::Timestamp>(usage.userMicroseconds) * Time::Multiplier::Microsecond;
}

Time::Timestamp linuxKernelTime(const LinuxRusage64& usage) {
  return static_cast<Time::Timestamp>(usage.systemSeconds) * Time::Multiplier::Second +
         static_cast<Time::Timestamp>(usage.systemMicroseconds) * Time::Multiplier::Microsecond;
}

bool transitiveChildAccounting(PosixProcess* parent, int* status, struct tms* processTimes,
                               struct rusage* nativeUsage, LinuxRusage64* linuxUsage,
                               uint8_t* linuxCanary) {
  constexpr Time::Timestamp clockTick = Time::Multiplier::Second / 100;
  constexpr Time::Timestamp childUser = 29 * clockTick;
  constexpr Time::Timestamp childKernel = 17 * clockTick;
  constexpr Time::Timestamp descendantUser = 43 * clockTick;
  constexpr Time::Timestamp descendantKernel = 23 * clockTick;

  const Time::Timestamp userBefore = parent->getReapedChildrenUserTime();
  const Time::Timestamp kernelBefore = parent->getReapedChildrenKernelTime();
  DescendantExitContext context(childUser, childKernel, descendantUser, descendantKernel);
  PosixProcess* child = new PosixProcess(parent);
  child->setSubsystem(new PosixSubsystem);
  Thread* childThread =
      new Thread(child, reapDescendantAndExit, &context, nullptr, false, true, true);
  childThread->setName("hosted transitive resource child");
  child->publish();
  const int childPid = static_cast<int>(child->getId());
  if (!childThread->start()) {
    delete childThread;
    delete child;
    return false;
  }

  bool passed = waitForValue(context.ready);
  uint8_t untouched[sizeof(LinuxRusage64) + 32];
  *status = 0x5A5A5A5A;
  ByteSet(linuxUsage, 0xA5, sizeof(LinuxRusage64) + 32);
  MemoryCopy(untouched, linuxUsage, sizeof(untouched));
  Thread* current = Processor::information().getCurrentThread();
  current->setErrno(PreservedErrno);
  passed &= posix_waitpid(childPid, status, WNOHANG, linuxUsage) == 0 && *status == 0x5A5A5A5A &&
            !MemoryCompare(linuxUsage, untouched, sizeof(untouched)) &&
            current->getErrno() == PreservedErrno;

  context.release += 1;
  ByteSet(linuxUsage, 0xA5, sizeof(LinuxRusage64) + 32);
  current->setErrno(PreservedErrno);
  const int waited = posix_waitpid(childPid, status, 0, linuxUsage);
  passed &= waited == childPid && WIFEXITED(*status) && !WEXITSTATUS(*status) &&
            context.descendantReaped && current->getErrno() == PreservedErrno;

  for (size_t i = offsetof(LinuxRusage64, maximumResidentSetSize); i < sizeof(*linuxUsage); ++i) {
    passed &= !reinterpret_cast<uint8_t*>(linuxUsage)[i];
  }
  for (size_t i = 0; i < 32; ++i) {
    passed &= linuxCanary[i] == 0xA5;
  }

  const Time::Timestamp userAfter = parent->getReapedChildrenUserTime();
  const Time::Timestamp kernelAfter = parent->getReapedChildrenKernelTime();
  const Time::Timestamp addedUser = userAfter - userBefore;
  const Time::Timestamp addedKernel = kernelAfter - kernelBefore;
  passed &=
      addedUser >= childUser + descendantUser && addedKernel >= childKernel + descendantKernel &&
      linuxUserTime(*linuxUsage) == addedUser - (addedUser % Time::Multiplier::Microsecond) &&
      linuxKernelTime(*linuxUsage) == addedKernel - (addedKernel % Time::Multiplier::Microsecond);

  ByteSet(nativeUsage, 0xA5, sizeof(*nativeUsage));
  passed &= posix_getrusage(RUSAGE_CHILDREN, nativeUsage) == 0;
  const Time::Timestamp nativeUser =
      static_cast<Time::Timestamp>(nativeUsage->ru_utime.tv_sec) * Time::Multiplier::Second +
      static_cast<Time::Timestamp>(nativeUsage->ru_utime.tv_usec) * Time::Multiplier::Microsecond;
  const Time::Timestamp nativeKernel =
      static_cast<Time::Timestamp>(nativeUsage->ru_stime.tv_sec) * Time::Multiplier::Second +
      static_cast<Time::Timestamp>(nativeUsage->ru_stime.tv_usec) * Time::Multiplier::Microsecond;
  passed &= nativeUser == userAfter - (userAfter % Time::Multiplier::Microsecond) &&
            nativeKernel == kernelAfter - (kernelAfter % Time::Multiplier::Microsecond);
  for (size_t i = offsetof(struct rusage, ru_maxrss); i < sizeof(*nativeUsage); ++i) {
    passed &= !reinterpret_cast<uint8_t*>(nativeUsage)[i];
  }

  ByteSet(linuxUsage, 0xA5, sizeof(LinuxRusage64) + 32);
  passed &=
      posix_linux_getrusage(RUSAGE_CHILDREN, linuxUsage) == 0 &&
      linuxUserTime(*linuxUsage) == userAfter - (userAfter % Time::Multiplier::Microsecond) &&
      linuxKernelTime(*linuxUsage) == kernelAfter - (kernelAfter % Time::Multiplier::Microsecond);
  for (size_t i = offsetof(LinuxRusage64, maximumResidentSetSize); i < sizeof(*linuxUsage); ++i) {
    passed &= !reinterpret_cast<uint8_t*>(linuxUsage)[i];
  }
  for (size_t i = 0; i < 32; ++i) {
    passed &= linuxCanary[i] == 0xA5;
  }

  passed &= posix_times(processTimes) >= 0 &&
            processTimes->tms_cutime == static_cast<clock_t>(userAfter / clockTick) &&
            processTimes->tms_cstime == static_cast<clock_t>(kernelAfter / clockTick);

  *status = 0x5A5A5A5A;
  ByteSet(linuxUsage, 0xA5, sizeof(LinuxRusage64) + 32);
  MemoryCopy(untouched, linuxUsage, sizeof(untouched));
  current->setErrno(0);
  passed &= posix_waitpid(childPid, status, WNOHANG, linuxUsage) == -1 &&
            current->getErrno() == Error::NoChildren && *status == 0x5A5A5A5A &&
            !MemoryCompare(linuxUsage, untouched, sizeof(untouched));

  passed &= ZombieQueue::instance().drain();
  return passed;
}

bool transitionAccounting(PosixProcess* parent, int* status, LinuxRusage64* linuxUsage) {
  constexpr Time::Timestamp clockTick = Time::Multiplier::Second / 100;
  SeededExitContext context(13 * clockTick, 7 * clockTick, true);
  PosixProcess* child = new PosixProcess(parent);
  child->setSubsystem(new PosixSubsystem);
  Thread* childThread = new Thread(child, seededExit, &context, nullptr, false, true, true);
  childThread->setName("hosted transition resource child");
  child->publish();
  const int childPid = static_cast<int>(child->getId());
  if (!childThread->start()) {
    delete childThread;
    delete child;
    return false;
  }

  bool passed = waitForValue(context.ready);
  const Time::Timestamp userBefore = parent->getReapedChildrenUserTime();
  const Time::Timestamp kernelBefore = parent->getReapedChildrenKernelTime();
  child->suspend(SIGSTOP);
  ByteSet(linuxUsage, 0xA5, sizeof(*linuxUsage));
  passed &= posix_waitpid(childPid, status, WUNTRACED, linuxUsage) == childPid &&
            WIFSTOPPED(*status) && WSTOPSIG(*status) == SIGSTOP &&
            parent->getReapedChildrenUserTime() == userBefore &&
            parent->getReapedChildrenKernelTime() == kernelBefore;
  for (size_t i = offsetof(LinuxRusage64, maximumResidentSetSize); i < sizeof(*linuxUsage); ++i) {
    passed &= !reinterpret_cast<uint8_t*>(linuxUsage)[i];
  }

  child->resume();
  ByteSet(linuxUsage, 0xA5, sizeof(*linuxUsage));
  passed &= posix_waitpid(childPid, status, WCONTINUED, linuxUsage) == childPid &&
            WIFCONTINUED(*status) && parent->getReapedChildrenUserTime() == userBefore &&
            parent->getReapedChildrenKernelTime() == kernelBefore;
  for (size_t i = offsetof(LinuxRusage64, maximumResidentSetSize); i < sizeof(*linuxUsage); ++i) {
    passed &= !reinterpret_cast<uint8_t*>(linuxUsage)[i];
  }

  context.release += 1;
  passed &= posix_waitpid(childPid, status, 0, nullptr) == childPid && WIFEXITED(*status) &&
            !WEXITSTATUS(*status) &&
            parent->getReapedChildrenUserTime() >= userBefore + context.user &&
            parent->getReapedChildrenKernelTime() >= kernelBefore + context.kernel;
  passed &= ZombieQueue::instance().drain();
  return passed;
}

bool concurrentReaperAccounting(PosixProcess* parent) {
  constexpr Time::Timestamp clockTick = Time::Multiplier::Second / 100;
  SeededExitContext exitContext(19 * clockTick, 11 * clockTick, true);
  PosixProcess* child = new PosixProcess(parent);
  child->setSubsystem(new PosixSubsystem);
  Thread* childThread = new Thread(child, seededExit, &exitContext, nullptr, false, true, true);
  childThread->setName("hosted concurrent resource child");
  child->publish();
  const int childPid = static_cast<int>(child->getId());

  Scheduler::ProcessLease childLease;
  const bool leased = Scheduler::instance().acquireProcess(childLease, child);
  const bool childStarted = leased && childThread->start();
  if (!childStarted) {
    delete childThread;
    childLease.reset();
    delete child;
    return false;
  }

  ConcurrentWaitContext firstContext(childPid);
  ConcurrentWaitContext secondContext(childPid);
  Thread* first = new Thread(parent, concurrentWait, &firstContext, nullptr, false, true, true);
  Thread* second = new Thread(parent, concurrentWait, &secondContext, nullptr, false, true, true);
  first->setName("hosted first child resource waiter");
  second->setName("hosted second child resource waiter");
  const bool firstStarted = first->start();
  const bool secondStarted = second->start();
  if (!firstStarted) {
    delete first;
  }
  if (!secondStarted) {
    delete second;
  }

  bool passed = firstStarted && secondStarted && waitForValue(firstContext.ready) &&
                waitForValue(secondContext.ready) && waitForValue(exitContext.ready);
  const Time::Timestamp userBefore = parent->getReapedChildrenUserTime();
  const Time::Timestamp kernelBefore = parent->getReapedChildrenKernelTime();
  firstContext.go += 1;
  secondContext.go += 1;
  exitContext.release += 1;

  const bool reapable = waitForTermination(child);
  const Time::Timestamp childUser = child->getUserTime();
  const Time::Timestamp childKernel = child->getKernelTime();
  const bool firstJoined = firstStarted && first->joinForCompletion();
  const bool secondJoined = secondStarted && second->joinForCompletion();
  const bool firstWon = firstContext.result == childPid;
  const bool secondWon = secondContext.result == childPid;

  Process::ReaperClaim rescueReaper;
  if (!firstWon && !secondWon && child->getState() == Process::Terminated) {
    auto guard = parent->acquireChildStateWait();
    if (child->getState() == Process::Terminated) {
      child->reap();
      rescueReaper = child->tryClaimReaper();
    }
  }
  if (rescueReaper) {
    Time::Timestamp ignoredUser = 0;
    Time::Timestamp ignoredKernel = 0;
    child->waitUntilTerminationReapable();
    parent->accountReapedChild(child, ignoredUser, ignoredKernel);
    rescueReaper.publish();
  }

  passed &= reapable && firstJoined && secondJoined && firstContext.returned == 1 &&
            secondContext.returned == 1 && firstWon != secondWon &&
            (firstWon ? firstContext.error == PreservedErrno
                      : firstContext.result == -1 && firstContext.error == Error::NoChildren) &&
            (secondWon ? secondContext.error == PreservedErrno
                       : secondContext.result == -1 && secondContext.error == Error::NoChildren) &&
            parent->getReapedChildrenUserTime() == userBefore + childUser &&
            parent->getReapedChildrenKernelTime() == kernelBefore + childKernel &&
            child->getState() == Process::Reaped;

  Process* childIdentity = child;
  childLease.reset();
  passed &= ZombieQueue::instance().drain();
  Scheduler::instance().waitUntilProcessRemoved(childIdentity);
  return passed;
}

int childResourceWorker(void* parameter) {
  ChildResourceContext* context = reinterpret_cast<ChildResourceContext*>(parameter);
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

  int* status = reinterpret_cast<int*>(address + 64);
  struct tms* processTimes = reinterpret_cast<struct tms*>(address + 128);
  struct rusage* nativeUsage = reinterpret_cast<struct rusage*>(address + 256);
  LinuxRusage64* linuxUsage = reinterpret_cast<LinuxRusage64*>(address + 1024);
  uint8_t* linuxCanary = reinterpret_cast<uint8_t*>(linuxUsage + 1);
  const bool passed = transitiveChildAccounting(context->process, status, processTimes, nativeUsage,
                                                linuxUsage, linuxCanary) &&
                      transitionAccounting(context->process, status, linuxUsage) &&
                      concurrentReaperAccounting(context->process);

  MemoryMapManager::instance().remove(address, pageSize);
  context->process->getSpaceAllocator().free(address, pageSize);
  context->passed = passed;
  context->returned += 1;
  return passed ? 0 : 1;
}
}  // namespace

bool runHostedChildResourceRegressions(Process* kernelProcess) {
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  ChildResourceContext context(process);
  Thread* worker = new Thread(process, childResourceWorker, &context, nullptr, false, true, true);
  worker->setName("hosted child resource semantics");
  process->publish();

  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  const bool passed = started && joined && context.returned == 1 && context.passed;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL child-resource-accounting: "
        "transitive, transition, concurrent reaper, or wait4 ABI semantics regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS child-resource-accounting");
  return true;
}
