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
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

#include <limits.h>
#include <signal.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/signal-syscalls.h"

namespace {
constexpr size_t HostedAttempts = 10000;
constexpr size_t MuslTimerSignal = 32;
constexpr size_t MuslCancelSignal = 33;
constexpr size_t MuslSyncCallSignal = 34;
constexpr size_t FirstUnsupportedSignal = 35;

void threadSignalHandler(size_t) {}

void installDisposition(PosixSubsystem* subsystem, size_t signal, int type) {
  PosixSubsystem::SignalHandler* handler = new PosixSubsystem::SignalHandler;
  handler->type = type;
  handler->pEvent = new SignalEvent(reinterpret_cast<uintptr_t>(&threadSignalHandler), signal);
  subsystem->setSignalHandler(signal, handler);
}

PosixProcess* makeProcess(Process* parent, int64_t uid) {
  PosixProcess* process = new PosixProcess(parent);
  process->setSubsystem(new PosixSubsystem);
  process->setUserId(uid);
  process->setEffectiveUserId(uid);
  process->setSavedUserId(uid);
  return process;
}

int dormantThread(void*) {
  return 0;
}

struct ThreadSignalContext {
  ThreadSignalContext(PosixProcess* targetProcess, Thread* sameTarget, Thread* remoteTarget,
                      PosixProcess* deniedProcess, Thread* deniedTarget)
      : targetProcess(targetProcess),
        sameTarget(sameTarget),
        remoteTarget(remoteTarget),
        deniedProcess(deniedProcess),
        deniedTarget(deniedTarget),
        entered(0),
        passed(0) {}

  PosixProcess* targetProcess;
  Thread* sameTarget;
  Thread* remoteTarget;
  PosixProcess* deniedProcess;
  Thread* deniedTarget;
  Atomic<size_t> entered;
  Atomic<size_t> passed;
};

bool expectResult(Thread* current, int result, int expectedResult, size_t expectedError) {
  return result == expectedResult && current->getErrno() == expectedError;
}

int runThreadSignalCalls(void* parameter) {
  ThreadSignalContext* context = reinterpret_cast<ThreadSignalContext*>(parameter);
  Thread* current = Processor::information().getCurrentThread();
  PosixProcess* caller = static_cast<PosixProcess*>(current->getParent());
  bool passed = true;
  context->entered += 1;

  current->setErrno(0);
  passed &= expectResult(current, posix_tkill(context->sameTarget->getId(), 0), 0, 0);
  passed &= !context->sameTarget->hasEvent(SIGUSR1);

  current->setErrno(0);
  passed &= expectResult(current, posix_tkill(context->sameTarget->getId(), SIGUSR2), 0, 0);
  passed &= !context->sameTarget->hasEvent(SIGUSR2);

  const uint64_t originalMask = context->sameTarget->getSignalMask();
  context->sameTarget->setSignalMask(originalMask | (static_cast<uint64_t>(1) << (SIGUSR1 - 1)));
  current->setErrno(0);
  passed &= expectResult(current, posix_tkill(context->sameTarget->getId(), SIGUSR1), 0, 0);
  passed &= context->sameTarget->hasEvent(SIGUSR1) && !current->hasEvent(SIGUSR1);
  context->sameTarget->cullSignalEvent(SIGUSR1);
  context->sameTarget->setSignalMask(originalMask);

  context->sameTarget->setSignalMask(originalMask |
                                     (static_cast<uint64_t>(1) << (MuslTimerSignal - 1)));
  current->setErrno(0);
  passed &= expectResult(current, posix_tkill(context->sameTarget->getId(), MuslTimerSignal), 0, 0);
  passed &= context->sameTarget->hasEvent(MuslTimerSignal);
  context->sameTarget->cullSignalEvent(MuslTimerSignal);
  context->sameTarget->setSignalMask(originalMask);

  current->setErrno(0);
  passed &=
      expectResult(current, posix_tkill(context->sameTarget->getId(), MuslCancelSignal), 0, 0);
  passed &= context->sameTarget->hasEvent(MuslCancelSignal);
  context->sameTarget->cullSignalEvent(MuslCancelSignal);

  current->setErrno(0);
  passed &= expectResult(current, posix_tkill(0, 0), -1, Error::InvalidArgument);
  current->setErrno(0);
  passed &= expectResult(current, posix_tkill(-1, 0), -1, Error::InvalidArgument);
  current->setErrno(0);
  passed &=
      expectResult(current, posix_tkill(context->remoteTarget->getId(), FirstUnsupportedSignal), -1,
                   Error::NoSuchProcess);
  current->setErrno(0);
  passed &= expectResult(current, posix_tkill(context->sameTarget->getId(), FirstUnsupportedSignal),
                         -1, Error::InvalidArgument);

  current->setErrno(0);
  passed &=
      expectResult(current, posix_tgkill(caller->getId(), context->sameTarget->getId(), 0), 0, 0);
  current->setErrno(0);
  passed &= expectResult(current, posix_tgkill(0, context->sameTarget->getId(), 0), -1,
                         Error::InvalidArgument);
  current->setErrno(0);
  passed &= expectResult(current, posix_tgkill(caller->getId(), 0, 0), -1, Error::InvalidArgument);

  current->setErrno(0);
  passed &= expectResult(
      current,
      posix_tgkill(context->targetProcess->getId(), context->remoteTarget->getId(), SIGUSR1), 0, 0);
  passed &= context->remoteTarget->hasEvent(SIGUSR1);
  context->remoteTarget->cullSignalEvent(SIGUSR1);

  current->setErrno(0);
  passed &= expectResult(
      current,
      posix_tgkill(context->targetProcess->getId(), context->remoteTarget->getId(), SIGUSR2), 0, 0);
  passed &= !context->remoteTarget->hasEvent(SIGUSR2);

  current->setErrno(0);
  passed &= expectResult(current,
                         posix_tgkill(context->targetProcess->getId(),
                                      context->remoteTarget->getId(), MuslSyncCallSignal),
                         0, 0);
  passed &= context->remoteTarget->hasEvent(MuslSyncCallSignal);
  context->remoteTarget->cullSignalEvent(MuslSyncCallSignal);

  current->setErrno(0);
  passed &= expectResult(
      current, posix_tgkill(context->targetProcess->getId(), INT_MAX, FirstUnsupportedSignal), -1,
      Error::NoSuchProcess);
  current->setErrno(0);
  passed &= expectResult(
      current, posix_tgkill(INT_MAX, context->remoteTarget->getId(), FirstUnsupportedSignal), -1,
      Error::NoSuchProcess);
  current->setErrno(0);
  passed &= expectResult(current,
                         posix_tgkill(context->targetProcess->getId(),
                                      context->remoteTarget->getId(), FirstUnsupportedSignal),
                         -1, Error::InvalidArgument);

  current->setErrno(0);
  passed &= expectResult(current, posix_kill(caller->getId(), MuslCancelSignal), -1,
                         Error::InvalidArgument);

  current->setErrno(0);
  passed &= expectResult(
      current, posix_tgkill(context->deniedProcess->getId(), context->deniedTarget->getId(), 0), -1,
      Error::NotEnoughPermissions);

  if (passed) {
    context->passed += 1;
  }
  current->setErrno(0);
  return 0;
}

struct SuspendContext {
  explicit SuspendContext(Process* process) : process(process), entered(0), returned(0) {}

  Process* process;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
};

int suspendTarget(void* parameter) {
  SuspendContext* context = reinterpret_cast<SuspendContext*>(parameter);
  context->entered += 1;
  context->process->suspend();
  context->returned += 1;
  return 0;
}

bool waitForSuspension(Process* process) {
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    if (process->isSuspended()) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}
}  // namespace

bool runHostedThreadSignalSyscallRegressions(Process* kernelProcess) {
  PosixProcess* caller = makeProcess(kernelProcess, 1000);
  PosixSubsystem* callerSubsystem = static_cast<PosixSubsystem*>(caller->getSubsystem());
  installDisposition(callerSubsystem, SIGUSR1, 0);
  installDisposition(callerSubsystem, SIGUSR2, 2);
  installDisposition(callerSubsystem, MuslTimerSignal, 0);
  installDisposition(callerSubsystem, MuslCancelSignal, 0);
  Thread* sameTarget = new Thread(caller, dormantThread, nullptr, nullptr, false, true, true);

  PosixProcess* target = makeProcess(kernelProcess, 1000);
  PosixSubsystem* targetSubsystem = static_cast<PosixSubsystem*>(target->getSubsystem());
  installDisposition(targetSubsystem, SIGUSR1, 0);
  installDisposition(targetSubsystem, SIGUSR2, 2);
  installDisposition(targetSubsystem, MuslSyncCallSignal, 0);
  new Thread(target, dormantThread, nullptr, nullptr, false, true, true);
  new Thread(target, dormantThread, nullptr, nullptr, false, true, true);
  Thread* remoteTarget = new Thread(target, dormantThread, nullptr, nullptr, false, true, true);

  PosixProcess* denied = makeProcess(kernelProcess, 2000);
  PosixSubsystem* deniedSubsystem = static_cast<PosixSubsystem*>(denied->getSubsystem());
  installDisposition(deniedSubsystem, SIGUSR1, 0);
  Thread* deniedTarget = new Thread(denied, dormantThread, nullptr, nullptr, false, true, true);

  ThreadSignalContext context(target, sameTarget, remoteTarget, denied, deniedTarget);
  Thread* callerThread =
      new Thread(caller, runThreadSignalCalls, &context, nullptr, false, true, true);
  callerThread->setName("hosted thread-signal syscall caller");

  caller->publish();
  target->publish();
  denied->publish();

  SuspendContext suspendContext(target);
  Thread* suspender =
      new Thread(kernelProcess, suspendTarget, &suspendContext, nullptr, false, true, true);
  suspender->setName("hosted thread-signal suspended-target fixture");
  const bool suspenderStarted = suspender->start();
  const bool suspended = suspenderStarted && waitForSuspension(target);
  const bool callerStarted = suspended && callerThread->start();
  const bool callerJoined = callerStarted && callerThread->joinForCompletion();

  if (suspenderStarted && !suspended) {
    suspender->setUnwindState(Thread::TerminateThread);
  }
  if (target->isSuspended()) {
    target->resume();
  }
  const bool suspenderJoined = suspenderStarted && suspender->joinForCompletion();
  if (target->isSuspended()) {
    target->resume();
  }
  if (!callerStarted) {
    delete callerThread;
  }
  if (!suspenderStarted) {
    delete suspender;
  }

  const bool passed = suspended && callerJoined && suspenderJoined && suspendContext.entered == 1 &&
                      suspendContext.returned == 1 && context.entered == 1 && context.passed == 1;

  delete caller;
  delete target;
  delete denied;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL thread-signal-syscalls: "
        "tkill/tgkill target, mask, suspension, error, or permission semantics regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS thread-signal-syscalls");
  return true;
}
