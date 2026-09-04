/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"

#include <signal.h>
#include <stdint.h>

#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/signal-syscalls.h"

namespace {
constexpr size_t TestSignal = 10;
constexpr int PreservedErrno = 123;
constexpr uint64_t TestSignalBit = static_cast<uint64_t>(1) << (TestSignal - 1);
constexpr uint64_t PreservedSignalBit = static_cast<uint64_t>(1) << (12 - 1);
constexpr uint64_t UnblockableSignalBits =
    (static_cast<uint64_t>(1) << (SIGKILL - 1)) | (static_cast<uint64_t>(1) << (SIGSTOP - 1));
constexpr uint64_t OriginalSignalMask = TestSignalBit | PreservedSignalBit;
constexpr uint64_t RequestedSignalMask = PreservedSignalBit | UnblockableSignalBits;
constexpr uint64_t ActiveSignalMask = PreservedSignalBit;

Atomic<size_t> g_SigsuspendSignalCalls(0);
Atomic<size_t> g_SigsuspendNonSignalCalls(0);
Atomic<uint64_t> g_SigsuspendHandlerMask(0);
Atomic<size_t> g_SigsuspendPreEnrolmentCalls(0);
Atomic<size_t> g_SigsuspendPreEnrolmentQueued(0);
Atomic<size_t> g_SigsuspendUnexpectedWaits(0);
Atomic<size_t> g_SigsuspendDefaultStopCalls(0);
Atomic<size_t> g_SigsuspendDefaultContinueCalls(0);
Thread* g_SigsuspendPreEnrolmentTarget = nullptr;

void sigsuspendSignalHandler(size_t) {
  Thread* thread = Processor::information().getCurrentThread();
  g_SigsuspendSignalCalls += 1;
  g_SigsuspendHandlerMask = thread ? thread->getSignalMask() : 0;
}

void sigsuspendNonSignalHandler(size_t) {
  g_SigsuspendNonSignalCalls += 1;
}

void sigsuspendDefaultStopHandler(size_t) {
  Thread* thread = Processor::information().getCurrentThread();
  g_SigsuspendDefaultStopCalls += 1;
  if (thread) {
    thread->getParent()->suspend();
  }
}

void sigsuspendDefaultContinueHandler(size_t) {
  Thread* thread = Processor::information().getCurrentThread();
  g_SigsuspendDefaultContinueCalls += 1;
  if (thread) {
    thread->getParent()->resume();
  }
}

void sigsuspendPreEnrolmentHook(Thread* thread) {
  if (g_SigsuspendPreEnrolmentCalls) {
    return;
  }
  g_SigsuspendPreEnrolmentCalls += 1;

  SignalEvent signal(reinterpret_cast<uintptr_t>(&sigsuspendSignalHandler), TestSignal, ~0UL, 0,
                     true, false, Event::HandlerPrivilege::Kernel,
                     SignalEvent::DeliveryDisposition::CaughtHandler);
  if (thread->sendEvent(&signal)) {
    g_SigsuspendPreEnrolmentQueued += 1;
    thread->getScheduler()->checkEventState(0);
    if (thread->hasEvent(&signal)) {
      thread->cullEvent(&signal);
    }
  }
}

void sigsuspendUnexpectedWaitHook(WaitQueue*, Thread* thread, const WaitQueue::Channel&,
                                  size_t debugState) {
  if (thread == g_SigsuspendPreEnrolmentTarget && debugState == Thread::EventWait) {
    g_SigsuspendUnexpectedWaits += 1;
    thread->setUnwindState(Thread::TerminateThread);
  }
}

class SigsuspendNonSignalEvent : public Event {
 public:
  SigsuspendNonSignalEvent()
      : Event(reinterpret_cast<uintptr_t>(&sigsuspendNonSignalHandler), false) {}

  size_t serialize(uint8_t*) override {
    return 0;
  }

  size_t getNumber() override {
    return 0x53555350;
  }
};

bool waitForEventBlock(Thread* thread) {
  const Time::Timestamp deadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (thread->getWaitDebugInfo(info) && info.queue && info.queued &&
        thread->getDebugState(debugAddress) == Thread::EventWait) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool waitForCounter(const Atomic<size_t>& counter, size_t value) {
  const Time::Timestamp deadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (Time::getTicks() < deadline) {
    if (counter == value) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return counter == value;
}

bool waitForSuspensionState(Process* process, bool suspended) {
  const Time::Timestamp deadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (Time::getTicks() < deadline) {
    if (process->isSuspended() == suspended) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return process->isSuspended() == suspended;
}

struct SigsuspendValidationContext {
  SigsuspendValidationContext() : entered(0), returned(0), passed(false) {}

  Atomic<size_t> entered;
  Atomic<size_t> returned;
  bool passed;
};

int sigsuspendValidationWorker(void* parameter) {
  SigsuspendValidationContext* context = reinterpret_cast<SigsuspendValidationContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  context->entered += 1;
  thread->setSignalMask(OriginalSignalMask);
  thread->clearInterruption();
  g_SigsuspendSignalCalls = 0;
  g_SigsuspendHandlerMask = 0;

  SignalEvent pendingSignal(reinterpret_cast<uintptr_t>(&sigsuspendSignalHandler), TestSignal);
  const bool signalQueued = thread->sendEvent(&pendingSignal);
  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();

  thread->setErrno(0);
  const int wrongSize =
      posix_rt_sigsuspend(reinterpret_cast<const uint64_t*>(kernelStart), sizeof(uint64_t) - 1);
  const int wrongSizeError = thread->getErrno();
  const bool wrongSizePreserved = thread->getSignalMask() == OriginalSignalMask &&
                                  thread->hasEvent(&pendingSignal) && !g_SigsuspendSignalCalls;

  thread->setErrno(0);
  const int badAddress =
      posix_rt_sigsuspend(reinterpret_cast<const uint64_t*>(kernelStart), sizeof(uint64_t));
  const int badAddressError = thread->getErrno();
  const bool badAddressPreserved = thread->getSignalMask() == OriginalSignalMask &&
                                   thread->hasEvent(&pendingSignal) && !g_SigsuspendSignalCalls;

  thread->setErrno(0);
  const int nullAddress = posix_rt_sigsuspend(nullptr, sizeof(uint64_t));
  const int nullAddressError = thread->getErrno();
  const bool nullAddressPreserved = thread->getSignalMask() == OriginalSignalMask &&
                                    thread->hasEvent(&pendingSignal) && !g_SigsuspendSignalCalls;

  thread->setErrno(PreservedErrno);
  const int result = posix_rt_sigsuspend(&RequestedSignalMask, sizeof(RequestedSignalMask));
  const int error = thread->getErrno();
  const uint64_t restoredMask = thread->getSignalMask();
  const bool signalConsumed = !thread->hasEvent(&pendingSignal);
  const bool interruptionConsumed = thread->getInterruptionReason() == Thread::NotInterrupted;

  if (thread->hasEvent(&pendingSignal)) {
    thread->cullEvent(&pendingSignal);
  }
  thread->setSignalMask(0);
  thread->clearInterruption();

  context->passed =
      signalQueued && wrongSize == -1 && wrongSizeError == Error::InvalidArgument &&
      wrongSizePreserved && badAddress == -1 && badAddressError == Error::BadAddress &&
      badAddressPreserved && nullAddress == -1 && nullAddressError == Error::BadAddress &&
      nullAddressPreserved && result == -1 && error == Error::Interrupted &&
      restoredMask == OriginalSignalMask && signalConsumed && interruptionConsumed &&
      g_SigsuspendSignalCalls == 1 && g_SigsuspendHandlerMask == (ActiveSignalMask | TestSignalBit);
  context->returned += 1;
  return context->passed ? 0 : 1;
}

bool sigsuspendValidationAndPrequeued(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  SigsuspendValidationContext context;
  Thread* worker =
      new Thread(process, sigsuspendValidationWorker, &context, nullptr, false, true, true);
  worker->setName("hosted rt_sigsuspend validation worker");
  const bool started = worker->start();
  const bool entered = started && waitForCounter(context.entered, 1);
  const bool returned = entered && waitForCounter(context.returned, 1);
  if (started && !context.returned) {
    worker->setUnwindState(Thread::TerminateThread);
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool passed =
      started && entered && returned && joined && context.returned == 1 && context.passed;
  delete process;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL rt-sigsuspend-validation: "
        "Linux sigset validation, prequeued delivery, or restoration regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS rt-sigsuspend-validation");
  return true;
}

struct SigsuspendWaitContext {
  SigsuspendWaitContext() : entered(0), returned(0), result(-2), error(0), restoredMask(0) {}

  Atomic<size_t> entered;
  Atomic<size_t> returned;
  int result;
  int error;
  uint64_t restoredMask;
};

int sigsuspendWaitWorker(void* parameter) {
  SigsuspendWaitContext* context = reinterpret_cast<SigsuspendWaitContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  thread->setSignalMask(OriginalSignalMask);
  thread->clearInterruption();
  context->entered += 1;
  thread->setErrno(PreservedErrno);
  context->result = posix_rt_sigsuspend(&RequestedSignalMask, sizeof(RequestedSignalMask));
  context->error = thread->getErrno();
  context->restoredMask = thread->getSignalMask();
  thread->setSignalMask(0);
  thread->clearInterruption();
  context->returned += 1;
  return 0;
}

bool sigsuspendClosesPreEnrolmentWindow(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  g_SigsuspendSignalCalls = 0;
  g_SigsuspendHandlerMask = 0;
  g_SigsuspendPreEnrolmentCalls = 0;
  g_SigsuspendPreEnrolmentQueued = 0;
  g_SigsuspendUnexpectedWaits = 0;

  SigsuspendWaitContext context;
  Thread* worker = new Thread(process, sigsuspendWaitWorker, &context, nullptr, false, true, true);
  worker->setName("hosted rt_sigsuspend pre-enrolment worker");
  g_SigsuspendPreEnrolmentTarget = worker;
  Thread::setSignalWaitPreEnrolmentHookForHostedTest(worker, sigsuspendPreEnrolmentHook);
  WaitQueue::setBeforeBlockHook(sigsuspendUnexpectedWaitHook);

  const bool started = worker->start();
  const bool returned = started && waitForCounter(context.returned, 1);

  Thread::setSignalWaitPreEnrolmentHookForHostedTest(nullptr, nullptr);
  WaitQueue::setBeforeBlockHook(nullptr);
  g_SigsuspendPreEnrolmentTarget = nullptr;
  if (started && !context.returned) {
    worker->setUnwindState(Thread::TerminateThread);
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool passed = started && returned && joined && context.result == -1 &&
                      context.error == Error::Interrupted &&
                      context.restoredMask == OriginalSignalMask &&
                      g_SigsuspendPreEnrolmentCalls == 1 && g_SigsuspendPreEnrolmentQueued == 1 &&
                      g_SigsuspendUnexpectedWaits == 0 && g_SigsuspendSignalCalls == 1 &&
                      g_SigsuspendHandlerMask == (ActiveSignalMask | TestSignalBit);
  delete process;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL rt-sigsuspend-pre-enrolment: "
        "a caught signal was lost between predicate inspection and wait publication");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS rt-sigsuspend-pre-enrolment");
  return true;
}

void installDefaultSignalAction(PosixSubsystem* subsystem, size_t signal, uintptr_t handler) {
  PosixSubsystem::SignalHandler* disposition = new PosixSubsystem::SignalHandler;
  disposition->type = 1;
  disposition->pEvent =
      new SignalEvent(handler, signal, ~0UL, 0, true, false, Event::HandlerPrivilege::Kernel,
                      SignalEvent::DeliveryDisposition::DefaultAction);
  subsystem->setSignalHandler(signal, disposition);
}

bool sigsuspendContinuesAfterDefaultActions(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  installDefaultSignalAction(subsystem, SIGCHLD,
                             reinterpret_cast<uintptr_t>(&sigsuspendNonSignalHandler));
  installDefaultSignalAction(subsystem, SIGTSTP,
                             reinterpret_cast<uintptr_t>(&sigsuspendDefaultStopHandler));
  installDefaultSignalAction(subsystem, SIGCONT,
                             reinterpret_cast<uintptr_t>(&sigsuspendDefaultContinueHandler));

  g_SigsuspendSignalCalls = 0;
  g_SigsuspendHandlerMask = 0;
  g_SigsuspendNonSignalCalls = 0;
  g_SigsuspendDefaultStopCalls = 0;
  g_SigsuspendDefaultContinueCalls = 0;
  SigsuspendWaitContext context;
  Thread* worker = new Thread(process, sigsuspendWaitWorker, &context, nullptr, false, true, true);
  worker->setName("hosted rt_sigsuspend default-action worker");

  const bool started = worker->start();
  const bool entered = started && waitForCounter(context.entered, 1);
  const bool initiallyBlocked = entered && waitForEventBlock(worker);
  const PosixSubsystem::SignalDeliveryResult ignoredResult =
      initiallyBlocked ? subsystem->queueSignalDelivery(worker, SIGCHLD)
                       : PosixSubsystem::SignalDeliveryResult::Unavailable;
  const bool ignoredStayedBlocked =
      ignoredResult == PosixSubsystem::SignalDeliveryResult::Ignored && !context.returned &&
      waitForEventBlock(worker) && !g_SigsuspendNonSignalCalls;

  const PosixSubsystem::SignalDeliveryResult stopResult =
      ignoredStayedBlocked ? subsystem->queueSignalDelivery(worker, SIGTSTP)
                           : PosixSubsystem::SignalDeliveryResult::Unavailable;
  const bool stopped = stopResult == PosixSubsystem::SignalDeliveryResult::Queued &&
                       waitForSuspensionState(process, true) &&
                       waitForCounter(g_SigsuspendDefaultStopCalls, 1) && !context.returned;

  if (process->isSuspended()) {
    process->resume();
  }
  const PosixSubsystem::SignalDeliveryResult continueResult =
      stopped ? subsystem->queueSignalDelivery(worker, SIGCONT)
              : PosixSubsystem::SignalDeliveryResult::Unavailable;
  const bool continued = waitForSuspensionState(process, false);
  const bool defaultHandlersReturned =
      continueResult == PosixSubsystem::SignalDeliveryResult::Queued && continued &&
      waitForCounter(g_SigsuspendDefaultContinueCalls, 1);
  const bool blockedAgain =
      defaultHandlersReturned && !context.returned && waitForEventBlock(worker);

  SignalEvent caughtSignal(reinterpret_cast<uintptr_t>(&sigsuspendSignalHandler), TestSignal, ~0UL,
                           0, true, false, Event::HandlerPrivilege::Kernel,
                           SignalEvent::DeliveryDisposition::CaughtHandler);
  const bool caughtQueued = blockedAgain && worker->sendEvent(&caughtSignal);
  const bool returned = caughtQueued && waitForCounter(context.returned, 1);
  if (process->isSuspended()) {
    process->resume();
  }
  if (started && !context.returned) {
    worker->setUnwindState(Thread::TerminateThread);
    process->resume();
  }
  const bool joined = started && worker->joinForCompletion();
  process->resume();
  if (!started) {
    delete worker;
  }

  const bool passed = started && initiallyBlocked && ignoredStayedBlocked && stopped &&
                      defaultHandlersReturned && blockedAgain && caughtQueued && returned &&
                      joined && context.result == -1 && context.error == Error::Interrupted &&
                      context.restoredMask == OriginalSignalMask &&
                      g_SigsuspendDefaultStopCalls == 1 && g_SigsuspendDefaultContinueCalls == 1 &&
                      g_SigsuspendSignalCalls == 1;
  delete process;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL rt-sigsuspend-default-actions: "
        "an ignored, stop, or continue default action ended the signal wait");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS rt-sigsuspend-default-actions");
  return true;
}

bool sigsuspendIgnoresNonSignalWake(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  g_SigsuspendSignalCalls = 0;
  g_SigsuspendNonSignalCalls = 0;
  g_SigsuspendHandlerMask = 0;

  SigsuspendWaitContext context;
  Thread* worker = new Thread(process, sigsuspendWaitWorker, &context, nullptr, false, true, true);
  worker->setName("hosted rt_sigsuspend non-signal wake worker");
  SigsuspendNonSignalEvent nonSignal;
  SignalEvent signal(reinterpret_cast<uintptr_t>(&sigsuspendSignalHandler), TestSignal);

  const bool started = worker->start();
  const bool entered = started && waitForCounter(context.entered, 1);
  const bool initiallyBlocked = entered && waitForEventBlock(worker);
  const bool activeMaskObserved = initiallyBlocked && worker->getSignalMask() == ActiveSignalMask;
  const bool nonSignalQueued = initiallyBlocked && worker->sendEvent(&nonSignal);
  const bool nonSignalHandled = nonSignalQueued && waitForCounter(g_SigsuspendNonSignalCalls, 1);
  const bool stayedInCall = nonSignalHandled && !context.returned;
  const bool blockedAgain = stayedInCall && waitForEventBlock(worker);
  const bool signalQueued = blockedAgain && worker->sendEvent(&signal);

  const Time::Timestamp returnDeadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (started && !context.returned && Time::getTicks() < returnDeadline) {
    Scheduler::instance().yield();
  }
  if (started && !context.returned) {
    worker->setUnwindState(Thread::TerminateThread);
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool passed =
      started && initiallyBlocked && activeMaskObserved && nonSignalQueued && nonSignalHandled &&
      stayedInCall && blockedAgain && signalQueued && joined && context.returned == 1 &&
      context.result == -1 && context.error == Error::Interrupted &&
      context.restoredMask == OriginalSignalMask && g_SigsuspendNonSignalCalls == 1 &&
      g_SigsuspendSignalCalls == 1 && g_SigsuspendHandlerMask == (ActiveSignalMask | TestSignalBit);
  delete process;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL rt-sigsuspend-nonsignal: "
        "a non-signal wake returned early or signal restoration regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS rt-sigsuspend-nonsignal");
  return true;
}
}  // namespace

bool runHostedRtSigsuspendRegressions(Process* process) {
  return sigsuspendValidationAndPrequeued(process) && sigsuspendClosesPreEnrolmentWindow(process) &&
         sigsuspendContinuesAfterDefaultActions(process) && sigsuspendIgnoresNonSignalWake(process);
}
