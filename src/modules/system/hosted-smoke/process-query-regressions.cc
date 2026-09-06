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
#include "pedigree/kernel/time/Time.h"

#include <limits.h>
#include <signal.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/system-syscalls.h"
#include <sys/resource.h>
#include <sys/wait.h>

namespace {
constexpr int PreservedErrno = 173;
constexpr int TestUid = 913;
constexpr size_t TestSignal = 10;

struct PriorityContext {
  PosixProcess* process;
  int foreignPid;
  bool passed = false;
  Atomic<size_t> returned{0};
};

int priorityWorker(void* parameter) {
  PriorityContext* context = reinterpret_cast<PriorityContext*>(parameter);
  Thread* current = Processor::information().getCurrentThread();
  const int processId = static_cast<int>(context->process->getId());
  bool passed = true;

  constexpr bool Abis[] = {false, true};
  constexpr int Selectors[] = {PRIO_PROCESS, PRIO_PGRP, PRIO_USER};
  for (bool linuxAbi : Abis) {
    const int expected = linuxAbi ? 20 : 0;
    for (int selector : Selectors) {
      const int explicitTarget = selector == PRIO_USER ? TestUid : processId;
      current->setErrno(PreservedErrno);
      passed &= posix_getpriority(selector, 0, linuxAbi) == expected &&
                current->getErrno() == PreservedErrno;
      current->setErrno(PreservedErrno);
      passed &= posix_getpriority(selector, explicitTarget, linuxAbi) == expected &&
                current->getErrno() == PreservedErrno;
      current->setErrno(0);
      passed &= posix_getpriority(selector, INT_MAX, linuxAbi) == -1 &&
                current->getErrno() ==
                    (selector == PRIO_PROCESS ? Error::NoSuchProcess : Error::Unimplemented);
      current->setErrno(0);
      passed &= posix_getpriority(selector, -1, linuxAbi) == -1 &&
                current->getErrno() == Error::NoSuchProcess;
    }

    current->setErrno(PreservedErrno);
    passed &= posix_getpriority(PRIO_PROCESS, context->foreignPid, linuxAbi) == expected &&
              current->getErrno() == PreservedErrno;
    current->setErrno(0);
    passed &= posix_getpriority(PRIO_PGRP, context->foreignPid, linuxAbi) == -1 &&
              current->getErrno() == Error::Unimplemented;
    current->setErrno(0);
    passed &= posix_getpriority(PRIO_USER, TestUid + 1, linuxAbi) == -1 &&
              current->getErrno() == Error::Unimplemented;
    current->setErrno(0);
    passed &=
        posix_getpriority(-1, 0, linuxAbi) == -1 && current->getErrno() == Error::InvalidArgument;
    current->setErrno(0);
    passed &=
        posix_getpriority(3, 0, linuxAbi) == -1 && current->getErrno() == Error::InvalidArgument;
  }

  context->passed = passed;
  context->returned += 1;
  return passed ? 0 : 1;
}

bool priorityQueries(Process* kernelProcess) {
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  process->setUserId(TestUid);
  process->publish();

  PosixProcess* foreign = new PosixProcess(kernelProcess);
  foreign->setSubsystem(new PosixSubsystem);
  foreign->setUserId(TestUid + 1);
  foreign->publish();
  PriorityContext context{process, static_cast<int>(foreign->getId())};
  Thread* worker = new Thread(process, priorityWorker, &context, nullptr, false, true, true);
  worker->setName("hosted getpriority query contracts");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  const bool passed = started && joined && context.returned == 1 && context.passed;
  delete foreign;
  delete process;
  if (passed) {
    NOTICE("HOSTED-SYSCALL-TEST: PASS getpriority-query-contracts");
  } else {
    ERROR("HOSTED-SYSCALL-TEST: FAIL getpriority-query-contracts");
  }
  return passed;
}

void unusedSignalHandler(size_t) {}

class DeferredWaitSignal : public SignalEvent {
 public:
  DeferredWaitSignal()
      : SignalEvent(reinterpret_cast<uintptr_t>(&unusedSignalHandler), TestSignal, ~0UL, 0, true,
                    false, Event::HandlerPrivilege::User) {}

  bool requiresExactUserReturnState() const override {
    return true;
  }
};

struct WaitContext {
  PosixProcess* child;
  Event* signal;
  bool passed = false;
  bool collisionQueued = false;
  Atomic<size_t> returned{0};
};

WaitContext* g_WaitCollision = nullptr;

void childStatusSignalCollision(WaitQueue*, Thread* thread, const WaitQueue::Channel&,
                                size_t debugState) {
  WaitContext* context = g_WaitCollision;
  if (!context || debugState != Thread::ProcessWait ||
      thread->getParent() != context->child->getParent()) {
    return;
  }
  WaitQueue::setBeforeBlockHook(nullptr);
  context->collisionQueued = thread->sendEvent(context->signal);
  context->child->suspend(SIGSTOP);
}

int interruptedWaitWorker(void* parameter) {
  WaitContext* context = reinterpret_cast<WaitContext*>(parameter);
  Thread* current = Processor::information().getCurrentThread();
  const int childPid = static_cast<int>(context->child->getId());
  current->setInterruptionReason(Thread::InterruptedBySignal);
  current->setErrno(PreservedErrno);
  const int result = posix_waitpid(childPid, nullptr, WUNTRACED, nullptr);
  bool passed = result == -1 && current->getErrno() == Error::Interrupted &&
                current->getInterruptionReason() == Thread::NotInterrupted &&
                current->hasEvent(context->signal);
  current->cullEvent(context->signal);
  current->clearInterruption();

  if (passed) {
    current->setErrno(PreservedErrno);
    passed &= posix_waitpid(childPid, nullptr, WNOHANG | WUNTRACED, nullptr) == 0 &&
              current->getErrno() == PreservedErrno;

    g_WaitCollision = context;
    WaitQueue::setBeforeBlockHook(&childStatusSignalCollision);
    current->setErrno(PreservedErrno);
    const int childStatus = posix_waitpid(childPid, nullptr, WUNTRACED, nullptr);
    WaitQueue::setBeforeBlockHook(nullptr);
    g_WaitCollision = nullptr;
    passed &= childStatus == childPid && current->getErrno() == PreservedErrno &&
              context->collisionQueued && current->hasEvent(context->signal) &&
              current->getInterruptionReason() == Thread::NotInterrupted;
    current->cullEvent(context->signal);
    passed &= posix_waitpid(childPid, nullptr, WNOHANG | WUNTRACED, nullptr) == 0 &&
              current->getInterruptionReason() == Thread::NotInterrupted;
  }

  context->passed = passed;
  context->returned += 1;
  return passed ? 0 : 1;
}

bool waitUntilBlocked(Thread* thread) {
  const Time::Timestamp deadline = Time::getTicks() + 2 * Time::Multiplier::Second;
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo wait = {};
    uintptr_t debugAddress = 0;
    if (thread->getWaitDebugInfo(wait) && wait.queue && wait.queued &&
        thread->getDebugState(debugAddress) == Thread::ProcessWait) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool waitForReturn(const Atomic<size_t>& returned) {
  const Time::Timestamp deadline = Time::getTicks() + 2 * Time::Multiplier::Second;
  while (!returned && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  return returned == 1;
}

bool caughtSignalWait(Process* kernelProcess) {
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  process->publish();
  PosixProcess* child = new PosixProcess(process);
  child->setSubsystem(new PosixSubsystem);
  child->publish();

  DeferredWaitSignal signal;
  WaitContext context{child, &signal};
  Thread* worker = new Thread(process, interruptedWaitWorker, &context, nullptr, false, true, true);
  worker->setName("hosted caught-signal waitpid interruption");
  const bool started = worker->start();
  const bool queued = started && waitUntilBlocked(worker) && worker->sendEvent(&signal);
  const bool returnedBeforeChildStatus = queued && waitForReturn(context.returned);
  if (started && !returnedBeforeChildStatus) {
    // Let the old retry behavior return a status so a regression fails cleanly.
    child->suspend(SIGSTOP);
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool passed = queued && returnedBeforeChildStatus && joined && context.passed;
  delete child;
  delete process;
  if (passed) {
    NOTICE("HOSTED-SYSCALL-TEST: PASS waitpid-caught-signal-interruption");
  } else {
    ERROR("HOSTED-SYSCALL-TEST: FAIL waitpid-caught-signal-interruption");
  }
  return passed;
}
}  // namespace

bool runHostedProcessQueryRegressions(Process* kernelProcess) {
  const bool priorityPassed = priorityQueries(kernelProcess);
  const bool waitPassed = caughtSignalWait(kernelProcess);
  return priorityPassed && waitPassed;
}
