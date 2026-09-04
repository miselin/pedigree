/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/RingBuffer.h"

#if !defined(PEDIGREE_HOSTED_CORE_SMOKE)
#include <signal.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/signal-syscalls.h"
#endif

namespace {
constexpr size_t HostedSignalNumber = 10;

Atomic<size_t> g_SignalHandlerCalls(0);
Atomic<size_t> g_MonitorEventDestructions(0);
Atomic<size_t> g_NestedWaitHandlerCalls(0);
Atomic<size_t> g_NestedWaitHandlerLevel(0);
Atomic<size_t> g_NestedWaitReturned(0);
Atomic<size_t> g_NestedSignalHandlerCalls(0);
Atomic<size_t> g_NestedSignalHandlerLevel(0);
Atomic<size_t> g_DefaultActionHandlerCalls(0);

void hostedSignalHandler(size_t) {
  g_SignalHandlerCalls += 1;
}

void hostedNestedSignalHandler(size_t) {
  Thread* thread = Processor::information().getCurrentThread();
  g_NestedSignalHandlerCalls += 1;
  g_NestedSignalHandlerLevel = thread ? thread->getStateLevel() : 0;
}

void hostedNestedWaitHandler(size_t) {
  Thread* thread = Processor::information().getCurrentThread();
  if (!thread) {
    return;
  }

  const size_t stateLevel = thread->getStateLevel();
  g_NestedWaitHandlerCalls += 1;
  g_NestedWaitHandlerLevel = stateLevel;
  thread->waitForEvent();
  if (thread->getStateLevel() == stateLevel) {
    g_NestedWaitReturned += 1;
  }
}

void hostedDefaultActionHandler(size_t) {
  g_DefaultActionHandlerCalls += 1;
}

class HostedNestedWaitEvent : public Event {
 public:
  HostedNestedWaitEvent() : Event(reinterpret_cast<uintptr_t>(&hostedNestedWaitHandler), false) {}

  size_t serialize(uint8_t*) override {
    return 0;
  }

  size_t getNumber() override {
    return 0x4e535457;
  }
};

class HostedMonitorEvent : public Event {
 public:
  HostedMonitorEvent() : Event(reinterpret_cast<uintptr_t>(&hostedSignalHandler), false) {}

  ~HostedMonitorEvent() override {
    g_MonitorEventDestructions += 1;
  }

  size_t serialize(uint8_t*) override {
    return 0;
  }

  size_t getNumber() override {
    return 0x4d4f4e49;
  }
};

class SignalNumberCollisionEvent : public Event {
 public:
  SignalNumberCollisionEvent()
      : Event(reinterpret_cast<uintptr_t>(&hostedSignalHandler), false, MAX_NESTED_EVENTS) {}

  size_t serialize(uint8_t*) override {
    return 0;
  }

  size_t getNumber() override {
    return HostedSignalNumber;
  }
};

bool check(bool condition, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL signal-interruption: " << detail);
  return false;
}

bool waitUntilQueued(Thread* thread, size_t debugState) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (thread->getWaitDebugInfo(info) && info.queue && info.queued &&
        thread->getDebugState(debugAddress) == debugState) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool eventHandlerPrivilege() {
  constexpr const char* Test = "event-handler-privilege";
  SignalEvent kernelEvent(reinterpret_cast<uintptr_t>(&hostedSignalHandler), HostedSignalNumber);
  SignalEvent userEvent(reinterpret_cast<uintptr_t>(&hostedSignalHandler), HostedSignalNumber, ~0UL,
                        0, true, false, Event::HandlerPrivilege::User);
  Event* delivery = userEvent.cloneForDelivery();

  const bool passed =
      check(kernelEvent.getHandlerPrivilege() == Event::HandlerPrivilege::Kernel,
            "the compatible Event constructor did not default to kernel privilege") &&
      check(kernelEvent.isValidHandlerMapping(VirtualAddressSpace::KernelMode),
            "a kernel event rejected a kernel mapping") &&
      check(!kernelEvent.isValidHandlerMapping(VirtualAddressSpace::Execute),
            "a kernel event accepted a userspace mapping") &&
      check(userEvent.getHandlerPrivilege() == Event::HandlerPrivilege::User,
            "a user event lost its explicit privilege") &&
      check(userEvent.isValidHandlerMapping(VirtualAddressSpace::Execute),
            "a user event rejected an executable userspace mapping") &&
      check(!userEvent.isValidHandlerMapping(0),
            "a user event accepted a non-executable mapping") &&
      check(!userEvent.isValidHandlerMapping(VirtualAddressSpace::KernelMode |
                                             VirtualAddressSpace::Execute),
            "a user event accepted a kernel mapping") &&
      check(delivery && delivery->getHandlerPrivilege() == Event::HandlerPrivilege::User,
            "a signal delivery snapshot lost its user privilege");

  delete delivery;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

bool signalCullPreservesNumberCollision(Thread* thread) {
  constexpr const char* Test = "signal-cull-number-collision";
  constexpr uint64_t SignalBit = static_cast<uint64_t>(1) << (HostedSignalNumber - 1);
  const uint64_t originalMask = thread->getSignalMask();
  thread->setSignalMask(originalMask | SignalBit);

  SignalNumberCollisionEvent collision;
  SignalEvent signal(reinterpret_cast<uintptr_t>(&hostedSignalHandler), HostedSignalNumber);
  const bool collisionQueued = thread->sendEvent(&collision);
  const bool signalQueued = collisionQueued && thread->sendEvent(&signal);
  thread->cullSignalEvent(HostedSignalNumber);
  const bool preservedCollision = thread->hasEvent(&collision) && !thread->hasEvent(&signal);
  thread->cullEvent(&collision);
  thread->setSignalMask(originalMask);

  const bool passed =
      check(collisionQueued && signalQueued && preservedCollision,
            "signal culling removed a non-signal event with the same numeric identifier");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

#if !defined(PEDIGREE_HOSTED_CORE_SMOKE)
Atomic<size_t> g_ContinueHandlerCalls(0);
Atomic<size_t> g_ContinueHandlerObservedActive(0);

void hostedContinueHandler(size_t) {
  Thread* current = Processor::information().getCurrentThread();
  if (current && current->getParent()->getState() == Process::Active) {
    g_ContinueHandlerObservedActive += 1;
  }
  g_ContinueHandlerCalls += 1;
}

void installSignalDisposition(PosixSubsystem& subsystem, size_t signal, int type,
                              void (*handlerAddress)(size_t) = &hostedSignalHandler) {
  PosixSubsystem::SignalHandler* handler = new PosixSubsystem::SignalHandler;
  handler->type = type;
  handler->pEvent = new SignalEvent(reinterpret_cast<uintptr_t>(handlerAddress), signal);
  subsystem.setSignalHandler(signal, handler);
}

int dormantSignalThread(void*) {
  return 0;
}

struct ExecSignalResetContext {
  ExecSignalResetContext() : entered(0), returned(0) {}

  Atomic<size_t> entered;
  Atomic<size_t> returned;
};

int deliverExecResetSignal(void* parameter) {
  ExecSignalResetContext* context = reinterpret_cast<ExecSignalResetContext*>(parameter);
  Thread* current = Processor::information().getCurrentThread();
  context->entered += 1;
  current->setSignalMask(0);
  current->getScheduler()->checkEventState(0);
  context->returned += 1;
  return 0;
}

struct IgnoredContinueContext {
  IgnoredContinueContext(Process* process, bool blockSignal)
      : process(process), blockSignal(blockSignal), entered(0), returned(0) {}

  Process* process;
  bool blockSignal;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
};

struct IgnoredSignalWaitContext {
  explicit IgnoredSignalWaitContext(size_t blockedSignal = 0)
      : gate(0),
        blockedSignal(blockedSignal),
        entered(0),
        returned(0),
        acquired(0),
        error(Semaphore::NoError),
        interruption(Thread::NotInterrupted) {}

  Semaphore gate;
  size_t blockedSignal;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<size_t> acquired;
  Atomic<size_t> error;
  Atomic<size_t> interruption;
};

int waitThroughIgnoredSignal(void* parameter) {
  IgnoredSignalWaitContext* context = reinterpret_cast<IgnoredSignalWaitContext*>(parameter);
  Thread* current = Processor::information().getCurrentThread();
  const uint64_t originalMask = current->getSignalMask();
  if (context->blockedSignal) {
    current->setSignalMask(originalMask |
                           (static_cast<uint64_t>(1) << (context->blockedSignal - 1)));
  }
  context->entered += 1;
  Semaphore::SemaphoreError error = Semaphore::NoError;
  context->acquired = context->gate.acquireWithError(1, 0, 0, error) ? 1 : 0;
  context->error = static_cast<size_t>(error);
  if (context->blockedSignal) {
    current->setSignalMask(originalMask);
    current->getScheduler()->checkEventState(0);
  }
  context->interruption = static_cast<size_t>(current->getInterruptionReason());
  current->clearInterruption();
  context->returned += 1;
  return 0;
}

bool ignoredSignalDoesNotWakeWait(PosixProcess* process, PosixSubsystem* subsystem, size_t signal,
                                  int type) {
  installSignalDisposition(*subsystem, signal, type);
  PosixSubsystem::SignalDisposition disposition;
  const bool queryPreserved =
      subsystem->getSignalDisposition(signal, disposition) && disposition.type == type;
  IgnoredSignalWaitContext context;
  Thread* target =
      new Thread(process, waitThroughIgnoredSignal, &context, nullptr, false, true, true);
  target->setName("hosted ignored signal waiter");
  const bool started = target->start();
  const bool queued = started && waitUntilQueued(target, Thread::SemWait);

  if (queued) {
    subsystem->sendSignal(target, static_cast<int>(signal), false);
  }
  for (size_t attempt = 0; attempt < 32 && !context.returned; ++attempt) {
    Scheduler::instance().yield();
  }
  Thread::WaitDebugInfo wait = {};
  const bool stayedQueued = queued && !context.returned && target->getWaitDebugInfo(wait) &&
                            wait.queued && !target->hasEvents();

  context.gate.release();
  const bool joined = started && target->joinForCompletion();
  if (!started) {
    delete target;
  }
  return queryPreserved && started && queued && stayedQueued && joined && context.entered == 1 &&
         context.returned == 1 && context.acquired == 1 && context.error == Semaphore::NoError &&
         context.interruption == Thread::NotInterrupted;
}

bool ignoredSignalDoesNotInterruptWait(Process* kernelProcess) {
  constexpr const char* Test = "ignored-signal-does-not-interrupt";
  PosixProcess* process = new PosixProcess(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  process->publish();

  g_SignalHandlerCalls = 0;
  const bool explicitIgnore = ignoredSignalDoesNotWakeWait(process, subsystem, SIGUSR1, 2);
  const bool defaultIgnore = ignoredSignalDoesNotWakeWait(process, subsystem, SIGCHLD, 1);
  const bool passed = check(explicitIgnore && defaultIgnore && g_SignalHandlerCalls == 0,
                            "an explicit or default ignored signal woke an interruptible wait");
  delete process;

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

bool pendingSignalDiscarded(PosixProcess* process, PosixSubsystem* subsystem, size_t signal,
                            int ignoredType) {
  installSignalDisposition(*subsystem, signal, 0);
  IgnoredSignalWaitContext context(signal);
  Thread* target =
      new Thread(process, waitThroughIgnoredSignal, &context, nullptr, false, true, true);
  target->setName("hosted pending signal discard target");
  const bool started = target->start();
  const bool waiting = started && waitUntilQueued(target, Thread::SemWait);

  const PosixSubsystem::SignalDeliveryResult queued =
      waiting ? subsystem->queueSignalDelivery(target, signal)
              : PosixSubsystem::SignalDeliveryResult::Rejected;
  const bool observedPending =
      queued == PosixSubsystem::SignalDeliveryResult::Queued && target->hasEvent(signal);
  installSignalDisposition(*subsystem, signal, ignoredType);
  const bool discarded = observedPending && !target->hasEvent(signal);

  context.gate.release();
  const bool joined = started && target->joinForCompletion();
  if (!started) {
    delete target;
  }
  return waiting && observedPending && discarded && joined && context.entered == 1 &&
         context.returned == 1 && context.acquired == 1 && context.error == Semaphore::NoError &&
         context.interruption == Thread::NotInterrupted;
}

bool ignoredDispositionDiscardsPendingSignals(Process* kernelProcess) {
  constexpr const char* Test = "ignored-disposition-discards-pending";
  PosixProcess* process = new PosixProcess(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  process->publish();

  g_SignalHandlerCalls = 0;
  const bool explicitIgnore = pendingSignalDiscarded(process, subsystem, SIGUSR1, 2);
  const bool defaultIgnore = pendingSignalDiscarded(process, subsystem, SIGCHLD, 1);
  const bool passed =
      check(explicitIgnore && defaultIgnore && g_SignalHandlerCalls == 0,
            "a pending caught signal survived transition to an ignored disposition");
  delete process;

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

bool execSignalResetRebindsPending(Process* kernelProcess) {
  constexpr const char* Test = "exec-signal-reset-rebinds-pending";
  PosixProcess* process = new PosixProcess(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  process->publish();

  PosixSubsystem::SignalHandler* caught = new PosixSubsystem::SignalHandler;
  caught->sigMask = static_cast<uint64_t>(1) << (SIGUSR1 - 1);
  caught->flags = SA_RESTART;
  caught->restorer = 0x12345678;
  caught->type = 0;
  caught->pEvent = new SignalEvent(reinterpret_cast<uintptr_t>(&hostedSignalHandler), SIGCHLD);
  subsystem->setSignalHandler(SIGCHLD, caught);

  PosixSubsystem::SignalHandler* ignored = new PosixSubsystem::SignalHandler;
  ignored->sigMask = static_cast<uint64_t>(1) << (SIGCHLD - 1);
  ignored->flags = SA_RESTART;
  ignored->restorer = 0x87654321;
  ignored->type = 2;
  ignored->pEvent = new SignalEvent(reinterpret_cast<uintptr_t>(&hostedSignalHandler), SIGUSR2);
  subsystem->setSignalHandler(SIGUSR2, ignored);

  ExecSignalResetContext context;
  Thread* target =
      new Thread(process, deliverExecResetSignal, &context, nullptr, false, true, true);
  target->setName("hosted exec signal reset target");
  target->setSignalMask(static_cast<uint64_t>(1) << (SIGCHLD - 1));
  Thread::AlternateSignalStack& alternate = target->getAlternateSignalStack();
  alternate.base = 0x100000;
  alternate.size = 0x4000;
  alternate.enabled = true;
  alternate.inUse = true;

  g_SignalHandlerCalls = 0;
  const PosixSubsystem::SignalDeliveryResult queued =
      subsystem->queueSignalDelivery(target, SIGCHLD);
  const bool initiallyPending =
      queued == PosixSubsystem::SignalDeliveryResult::Queued && target->hasSignalEvent(SIGCHLD);

  pedigree_reset_signals_for_exec(target);

  PosixSubsystem::SignalDisposition resetCaught;
  PosixSubsystem::SignalDisposition resetIgnored;
  const bool caughtReset = subsystem->getSignalDisposition(SIGCHLD, resetCaught) &&
                           resetCaught.type == 1 && !resetCaught.signalMask && !resetCaught.flags &&
                           !resetCaught.restorer &&
                           resetCaught.handler != reinterpret_cast<uintptr_t>(&hostedSignalHandler);
  const bool ignoreRetained =
      subsystem->getSignalDisposition(SIGUSR2, resetIgnored) && resetIgnored.type == 2 &&
      !resetIgnored.signalMask && !resetIgnored.flags && !resetIgnored.restorer &&
      resetIgnored.handler != reinterpret_cast<uintptr_t>(&hostedSignalHandler);
  const bool pendingRebound = target->hasSignalEvent(SIGCHLD);
  const bool alternateReset =
      !alternate.base && !alternate.size && !alternate.enabled && !alternate.inUse;

  const bool started = target->start();
  const bool joined = started && target->joinForCompletion();
  if (!started) {
    target->cullSignalEvent(SIGCHLD);
    delete target;
  }

  const bool passed = check(initiallyPending && caughtReset && ignoreRetained && pendingRebound &&
                                alternateReset && started && joined && context.entered == 1 &&
                                context.returned == 1 && g_SignalHandlerCalls == 0,
                            "exec discarded a pending signal, retained old handler metadata, or "
                            "left the alternate stack enabled");
  delete process;

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

int suspendForIgnoredContinue(void* parameter) {
  IgnoredContinueContext* context = reinterpret_cast<IgnoredContinueContext*>(parameter);
  if (context->blockSignal) {
    Processor::information().getCurrentThread()->setSignalMask(static_cast<uint64_t>(1)
                                                               << (SIGCONT - 1));
  }
  context->entered += 1;
  context->process->suspend();
  context->returned += 1;
  return 0;
}

bool signalContinueResumes(PosixProcess* process, PosixSubsystem* subsystem, int type,
                           bool blockSignal) {
  installSignalDisposition(*subsystem, SIGCONT, type);
  IgnoredContinueContext context(process, blockSignal);
  Thread* target =
      new Thread(process, suspendForIgnoredContinue, &context, nullptr, false, true, true);
  target->setName("hosted SIGCONT target");
  const bool started = target->start();

  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (started && !process->isSuspended() && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  const bool suspended = process->isSuspended();

  g_SignalHandlerCalls = 0;
  if (suspended) {
    subsystem->sendSignal(target, SIGCONT, false);
  }
  const bool continuedBySignal = process->getState() == Process::Active;
  if (!continuedBySignal) {
    process->resume();
  }

  const bool joined = started && target->joinForCompletion();
  if (!started) {
    delete target;
  }
  Process::ChildTransition transition;
  bool continuedReported = false;
  bool reportedExactlyOnce = false;
  Process* parent = process->getParent();
  if (parent) {
    auto guard = parent->acquireChildStateWait();
    continuedReported = process->takePendingChildTransition(true, true, transition) &&
                        transition.kind == Process::ChildTransitionKind::Continued &&
                        !transition.stopSignal;
    reportedExactlyOnce = !process->takePendingChildTransition(true, true, transition);
  }
  return started && context.entered == 1 && suspended && continuedBySignal && joined &&
         context.returned == 1 && continuedReported && reportedExactlyOnce;
}

bool signalContinueStillResumes(Process* kernelProcess) {
  constexpr const char* Test = "sigcont-resumes-before-disposition";
  PosixProcess* process = new PosixProcess(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  process->publish();

  g_SignalHandlerCalls = 0;
  const bool ignored = signalContinueResumes(process, subsystem, 2, false);
  const bool caughtAndBlocked = signalContinueResumes(process, subsystem, 0, true);
  const bool passed =
      check(ignored && caughtAndBlocked && g_SignalHandlerCalls == 0,
            "an ignored or blocked SIGCONT required handler delivery to resume its target");
  delete process;

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

bool opposingJobControlSignalsCancelAcrossThreads(Process* kernelProcess) {
  constexpr const char* Test = "opposing-job-control-signals-cancel";
  PosixProcess* process = new PosixProcess(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  process->publish();

  constexpr size_t StopSignals[] = {SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU};
  constexpr uint64_t JobControlMask =
      (static_cast<uint64_t>(1) << (SIGCONT - 1)) | (static_cast<uint64_t>(1) << (SIGSTOP - 1)) |
      (static_cast<uint64_t>(1) << (SIGTSTP - 1)) | (static_cast<uint64_t>(1) << (SIGTTIN - 1)) |
      (static_cast<uint64_t>(1) << (SIGTTOU - 1));
  installSignalDisposition(*subsystem, SIGCONT, 0);
  for (size_t stopSignal : StopSignals) {
    installSignalDisposition(*subsystem, stopSignal, 0);
  }

  Thread* first = new Thread(process, dormantSignalThread, nullptr, nullptr, false, true, true);
  Thread* second = new Thread(process, dormantSignalThread, nullptr, nullptr, false, true, true);
  first->setSignalMask(JobControlMask);
  second->setSignalMask(JobControlMask);

  const bool continueQueued = subsystem->queueSignalDelivery(first, SIGCONT) ==
                                  PosixSubsystem::SignalDeliveryResult::Queued &&
                              first->hasEvent(SIGCONT);
  const bool stopQueued = continueQueued && subsystem->queueSignalDelivery(second, SIGTSTP) ==
                                                PosixSubsystem::SignalDeliveryResult::Queued;
  const bool stopCancelledContinue =
      stopQueued && !first->hasEvent(SIGCONT) && second->hasEvent(SIGTSTP);

  bool everyStopQueued = stopCancelledContinue;
  for (size_t i = 0; i < sizeof(StopSignals) / sizeof(StopSignals[0]); ++i) {
    Thread* target = i % 2 ? second : first;
    everyStopQueued &= subsystem->queueSignalDelivery(target, StopSignals[i]) ==
                       PosixSubsystem::SignalDeliveryResult::Queued;
  }
  const bool blockedContinueQueued =
      everyStopQueued && subsystem->queueSignalDelivery(first, SIGCONT) ==
                             PosixSubsystem::SignalDeliveryResult::Queued;
  bool continueCancelledEveryStop = blockedContinueQueued && first->hasEvent(SIGCONT);
  for (size_t stopSignal : StopSignals) {
    continueCancelledEveryStop &= !first->hasEvent(stopSignal) && !second->hasEvent(stopSignal);
  }

  installSignalDisposition(*subsystem, SIGCONT, 2);
  bool ignoredSetupQueued = continueCancelledEveryStop;
  for (size_t i = 0; i < sizeof(StopSignals) / sizeof(StopSignals[0]); ++i) {
    Thread* target = i % 2 ? first : second;
    ignoredSetupQueued &= subsystem->queueSignalDelivery(target, StopSignals[i]) ==
                          PosixSubsystem::SignalDeliveryResult::Queued;
  }
  const bool ignoredContinue =
      ignoredSetupQueued && subsystem->queueSignalDelivery(second, SIGCONT) ==
                                PosixSubsystem::SignalDeliveryResult::Ignored;
  bool ignoredContinueCancelledEveryStop =
      ignoredContinue && !first->hasEvent(SIGCONT) && !second->hasEvent(SIGCONT);
  for (size_t stopSignal : StopSignals) {
    ignoredContinueCancelledEveryStop &=
        !first->hasEvent(stopSignal) && !second->hasEvent(stopSignal);
  }

  Process::ChildTransition transition;
  bool redundantResumeWasSilent = false;
  {
    auto guard = kernelProcess->acquireChildStateWait();
    redundantResumeWasSilent = !process->takePendingChildTransition(true, true, transition);
  }

  const bool firstStarted = first->start();
  const bool secondStarted = second->start();
  const bool firstJoined = firstStarted && first->joinForCompletion();
  const bool secondJoined = secondStarted && second->joinForCompletion();
  if (!firstStarted) {
    delete first;
  }
  if (!secondStarted) {
    delete second;
  }

  const bool passed = check(
      stopCancelledContinue && continueCancelledEveryStop && ignoredContinueCancelledEveryStop &&
          redundantResumeWasSilent && firstStarted && secondStarted && firstJoined && secondJoined,
      "a process retained mutually exclusive pending stop and continue signals");
  delete process;

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

struct CaughtContinueContext {
  explicit CaughtContinueContext(Process* process)
      : process(process), finish(0), entered(0), resumed(0) {}

  Process* process;
  Semaphore finish;
  Atomic<size_t> entered;
  Atomic<size_t> resumed;
};

int suspendForCaughtContinue(void* parameter) {
  CaughtContinueContext* context = reinterpret_cast<CaughtContinueContext*>(parameter);
  context->entered += 1;
  context->process->suspend();
  context->resumed += 1;
  (void)context->finish.acquire();
  return 0;
}

bool stoppedProcessDefersSignalsUntilContinue(Process* kernelProcess) {
  constexpr const char* Test = "stopped-process-defers-signals";
  PosixProcess* process = new PosixProcess(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  process->publish();
  installSignalDisposition(*subsystem, SIGUSR1, 0);
  installSignalDisposition(*subsystem, SIGCONT, 0, &hostedContinueHandler);

  IgnoredSignalWaitContext ordinaryContext;
  Thread* ordinary =
      new Thread(process, waitThroughIgnoredSignal, &ordinaryContext, nullptr, false, true, true);
  ordinary->setName("hosted stopped ordinary-signal target");
  const bool ordinaryStarted = ordinary->start();
  const bool ordinaryWaiting = ordinaryStarted && waitUntilQueued(ordinary, Thread::SemWait);

  CaughtContinueContext continueContext(process);
  Thread* continuer =
      new Thread(process, suspendForCaughtContinue, &continueContext, nullptr, false, true, true);
  continuer->setName("hosted caught SIGCONT target");
  const bool continuerStarted = ordinaryWaiting && continuer->start();
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (continuerStarted && !process->isSuspended() && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  const bool suspended = process->isSuspended();

  g_SignalHandlerCalls = 0;
  g_ContinueHandlerCalls = 0;
  g_ContinueHandlerObservedActive = 0;
  const PosixSubsystem::SignalDeliveryResult ordinaryResult =
      suspended ? subsystem->queueSignalDelivery(ordinary, SIGUSR1)
                : PosixSubsystem::SignalDeliveryResult::Unavailable;
  for (size_t attempt = 0; attempt < 32 && !ordinaryContext.returned; ++attempt) {
    Scheduler::instance().yield();
  }
  Thread::WaitDebugInfo wait = {};
  const bool ordinaryStayedPending =
      ordinaryResult == PosixSubsystem::SignalDeliveryResult::Queued &&
      ordinary->hasEvent(SIGUSR1) && !ordinaryContext.returned && !g_SignalHandlerCalls &&
      ordinary->getWaitDebugInfo(wait) && wait.queued && process->isSuspended();

  const PosixSubsystem::SignalDeliveryResult continueResult =
      ordinaryStayedPending ? subsystem->queueSignalDelivery(continuer, SIGCONT)
                            : PosixSubsystem::SignalDeliveryResult::Unavailable;
  const bool continuedBySignal = process->getState() == Process::Active;
  const Time::Timestamp deliveryDeadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while ((!ordinaryContext.returned || !g_ContinueHandlerCalls) &&
         Time::getTicks() < deliveryDeadline) {
    Scheduler::instance().yield();
  }
  const bool deliveredByContinue = ordinaryContext.returned && g_ContinueHandlerCalls;

  if (continuerStarted && !suspended) {
    continuer->setUnwindState(Thread::TerminateThread);
  }
  if (!ordinaryContext.returned) {
    ordinaryContext.gate.release();
  }
  continueContext.finish.release();
  process->resume();
  const bool ordinaryJoined = ordinaryStarted && ordinary->joinForCompletion();
  const bool continuerJoined = continuerStarted && continuer->joinForCompletion();
  // A timeout can race just ahead of the worker's Active -> Suspended CAS.
  // Termination makes that late wait return; this second resume cleans the
  // process state after the worker is guaranteed off-stack.
  process->resume();
  if (!ordinaryStarted) {
    delete ordinary;
  }
  if (!continuerStarted) {
    delete continuer;
  }

  const bool passed = check(
      ordinaryStarted && ordinaryWaiting && continuerStarted && suspended &&
          ordinaryStayedPending && continueResult == PosixSubsystem::SignalDeliveryResult::Queued &&
          continuedBySignal && deliveredByContinue && ordinaryJoined && continuerJoined &&
          ordinaryContext.entered == 1 && ordinaryContext.returned == 1 &&
          ordinaryContext.acquired == 0 && ordinaryContext.error == Semaphore::Interrupted &&
          ordinaryContext.interruption == Thread::InterruptedBySignal &&
          continueContext.entered == 1 && continueContext.resumed == 1 &&
          g_SignalHandlerCalls == 1 && g_ContinueHandlerCalls == 1 &&
          g_ContinueHandlerObservedActive == 1,
      "a stopped signal escaped early, remained stranded, or ran SIGCONT before Active");
  delete process;

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}
#endif

bool execPreservesNestedSignalMask(Thread* thread) {
  constexpr const char* Test = "exec-preserves-nested-signal-mask";
  constexpr uint64_t TemporaryMask = static_cast<uint64_t>(1) << (HostedSignalNumber + 1);
  constexpr uint64_t NestedTemporaryMask = static_cast<uint64_t>(1) << (HostedSignalNumber + 2);
  constexpr uint64_t EffectiveMask = (static_cast<uint64_t>(1) << (HostedSignalNumber - 1)) |
                                     (static_cast<uint64_t>(1) << (HostedSignalNumber + 3));
  const uint64_t originalMask = thread->getSignalMask();
  const Thread::InterruptionReason originalInterruption = thread->getInterruptionReason();
  const Thread::AlternateSignalStack originalAlternate = thread->getAlternateSignalStack();
  const size_t originalLevel = thread->getStateLevel();

  bool baseTemporaryMaskActive = false;
  bool nestedTemporaryMaskActive = false;
  bool firstPushed = false;
  bool secondPushed = false;
  bool execScopePreserved = false;
  bool execScopeReleased = false;
  if (!originalLevel) {
    Thread::TemporarySignalMask baseTemporaryMask(*thread, TemporaryMask);
    baseTemporaryMaskActive = thread->hasActiveTemporarySignalMask();
    firstPushed = thread->pushState() != nullptr;
    if (firstPushed) {
      {
        Thread::TemporarySignalMask nestedTemporaryMask(*thread, NestedTemporaryMask);
        nestedTemporaryMaskActive = thread->hasActiveTemporarySignalMask();
        secondPushed = thread->pushState() != nullptr;
        if (secondPushed) {
          thread->setSignalMask(EffectiveMask);
          Thread::AlternateSignalStack& alternate = thread->getAlternateSignalStack();
          alternate.base = 0x200000;
          alternate.size = 0x8000;
          alternate.enabled = true;
          alternate.inUse = true;
          {
            Uninterruptible execScope;
            thread->prepareSignalStateForExec();
            execScopePreserved = thread->eventsDeferred();
          }
          execScopeReleased = !thread->eventsDeferred();
          thread->abandonCurrentState(false);
        }
      }
      if (thread->getStateLevel() > originalLevel) {
        thread->abandonCurrentState(false);
      }
    }
  }

  const Thread::AlternateSignalStack resetAlternate = thread->getAlternateSignalStack();
  const bool passed = check(
      baseTemporaryMaskActive && nestedTemporaryMaskActive && firstPushed && secondPushed &&
          execScopePreserved && execScopeReleased && thread->getStateLevel() == originalLevel &&
          thread->getSignalMask() == EffectiveMask && !resetAlternate.base &&
          !resetAlternate.size && !resetAlternate.enabled && !resetAlternate.inUse &&
          !thread->hasActiveTemporarySignalMask() &&
          thread->getInterruptionReason() == Thread::NotInterrupted,
      "exec lost the active handler mask or retained an outer temporary-mask scope");

  thread->setSignalMask(originalMask);
  thread->setInterruptionReason(originalInterruption);
  thread->getAlternateSignalStack() = originalAlternate;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

bool invalidUserHandlerDeliveryFailsClosed(Thread* thread) {
  constexpr const char* Test = "invalid-user-handler-delivery";
  g_SignalHandlerCalls = 0;

  SignalEvent* event =
      new SignalEvent(reinterpret_cast<uintptr_t>(&hostedSignalHandler), HostedSignalNumber, ~0UL,
                      0, true, true, Event::HandlerPrivilege::User);
  const bool queued = thread->sendEvent(event);
  if (!queued) {
    delete event;
  } else {
    thread->getScheduler()->checkEventState(0);
  }

  const bool passed = check(queued && !g_SignalHandlerCalls && !thread->getStateLevel(),
                            "an unmapped user handler executed or retained scheduler state");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

struct SignalContext {
  SignalContext(Thread* target, size_t debugState, Semaphore* releaseAfterDelivery = nullptr)
      : target(target),
        debugState(debugState),
        releaseAfterDelivery(releaseAfterDelivery),
        published(0),
        sent(0),
        released(0) {}

  Thread* target;
  size_t debugState;
  Semaphore* releaseAfterDelivery;
  Atomic<size_t> published;
  Atomic<size_t> sent;
  Atomic<size_t> released;
};

struct DefaultActionSemaphoreContext {
  DefaultActionSemaphoreContext(Thread* target, Semaphore* gate)
      : target(target), gate(gate), published(0), sent(0), released(0) {}

  Thread* target;
  Semaphore* gate;
  Atomic<size_t> published;
  Atomic<size_t> sent;
  Atomic<size_t> released;
};

struct TemporaryMaskMutexContext {
  TemporaryMaskMutexContext()
      : mutex(), holderReady(0), releaseHolder(0), holderAcquired(0), holderReturned(0) {}

  Mutex mutex;
  Semaphore holderReady;
  Semaphore releaseHolder;
  Atomic<size_t> holderAcquired;
  Atomic<size_t> holderReturned;
};

struct SemaphoreWakeCollisionContext {
  SemaphoreWakeCollisionContext()
      : semaphore(0),
        waiter(nullptr),
        entered(0),
        returned(0),
        acquired(0),
        error(Semaphore::NoError),
        interruption(Thread::NotInterrupted),
        rescueWaits(0) {}

  Semaphore semaphore;
  Thread* waiter;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<size_t> acquired;
  Atomic<size_t> error;
  Atomic<size_t> interruption;
  Atomic<size_t> rescueWaits;
};

SemaphoreWakeCollisionContext* g_SemaphoreWakeCollision = nullptr;

struct ConditionWakeCollisionContext {
  ConditionWakeCollisionContext()
      : waiter(nullptr),
        predicate(0),
        entered(0),
        returned(0),
        waits(0),
        lastResult(0),
        error(ConditionVariable::NoError),
        rescueWaits(0) {}

  Mutex mutex;
  ConditionVariable condition;
  Thread* waiter;
  Atomic<size_t> predicate;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<size_t> waits;
  Atomic<size_t> lastResult;
  Atomic<size_t> error;
  Atomic<size_t> rescueWaits;
};

ConditionWakeCollisionContext* g_ConditionWakeCollision = nullptr;

struct EventDrainContext {
  explicit EventDrainContext(Event* event) : event(event), entered(0), completed(0) {}

  Event* event;
  Atomic<size_t> entered;
  Atomic<size_t> completed;
};

int drainEventRegistrations(void* parameter) {
  EventDrainContext* context = reinterpret_cast<EventDrainContext*>(parameter);
  context->entered += 1;
  context->event->waitForDeliveries();
  context->completed += 1;
  return 0;
}

int interruptPublishedWait(void* parameter) {
  SignalContext* context = reinterpret_cast<SignalContext*>(parameter);
  if (waitUntilQueued(context->target, context->debugState)) {
    context->published += 1;
  }

  SignalEvent* event = new SignalEvent(reinterpret_cast<uintptr_t>(&hostedSignalHandler),
                                       HostedSignalNumber, ~0UL, 0, true, true);
  if (context->target->sendEvent(event)) {
    context->sent += 1;
  }

  if (context->releaseAfterDelivery) {
    const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (!g_SignalHandlerCalls && Time::getTicks() < deadline) {
      Scheduler::instance().yield();
    }
    context->releaseAfterDelivery->release();
    context->released += 1;
  }
  return 0;
}

int holdTemporaryMaskMutex(void* parameter) {
  TemporaryMaskMutexContext* context = reinterpret_cast<TemporaryMaskMutexContext*>(parameter);
  const bool acquired = context->mutex.acquire();
  if (acquired) {
    context->holderAcquired += 1;
  }
  context->holderReady.release();

  bool released = false;
  if (acquired) {
    released = context->releaseHolder.acquireForCompletion();
    context->mutex.release();
  }
  context->holderReturned += 1;
  return acquired && released ? 0 : 1;
}

int waitForSemaphoreWakeCollision(void* parameter) {
  SemaphoreWakeCollisionContext* context =
      reinterpret_cast<SemaphoreWakeCollisionContext*>(parameter);
  context->entered += 1;

  Semaphore::SemaphoreError error = Semaphore::NoError;
  const bool acquired = context->semaphore.acquireWithError(1, 0, 0, error);
  context->acquired = acquired ? 1 : 0;
  context->error = static_cast<size_t>(error);

  Thread* thread = Processor::information().getCurrentThread();
  context->interruption = static_cast<size_t>(thread->getInterruptionReason());
  thread->clearInterruption();
  context->returned += 1;
  return 0;
}

void semaphoreWakeCollisionRescue(WaitQueue* queue, Thread* thread,
                                  const WaitQueue::Channel& channel, size_t debugState) {
  (void)queue;
  SemaphoreWakeCollisionContext* context = g_SemaphoreWakeCollision;
  if (!context || thread != context->waiter || channel.owner != &context->semaphore ||
      channel.value || debugState != Thread::SemWait) {
    return;
  }

  context->rescueWaits += 1;
  context->semaphore.release();
}

int waitForConditionWakeCollision(void* parameter) {
  ConditionWakeCollisionContext* context =
      reinterpret_cast<ConditionWakeCollisionContext*>(parameter);
  if (!context->mutex.acquireForCompletion()) {
    return 1;
  }
  context->entered += 1;

  ConditionVariable::Error error = ConditionVariable::NoError;
  bool result = true;
  while (!context->predicate && result) {
    context->waits += 1;
    result = context->condition.wait(context->mutex, error);
  }
  context->lastResult = result ? 1 : 0;
  context->error = static_cast<size_t>(error);
  context->mutex.release();
  context->returned += 1;
  return 0;
}

void conditionWakeCollisionRescue(WaitQueue* queue, Thread* thread,
                                  const WaitQueue::Channel& channel, size_t debugState) {
  (void)queue;
  ConditionWakeCollisionContext* context = g_ConditionWakeCollision;
  if (!context || thread != context->waiter || channel.owner || channel.value ||
      debugState != Thread::CondWait) {
    return;
  }

  context->rescueWaits += 1;
  if (context->mutex.acquireForCompletion()) {
    context->predicate = 1;
    context->condition.signal();
    context->mutex.release();
  }
}

Thread* startInterrupter(SignalContext& context) {
  Thread* thread = new Thread(Scheduler::instance().getKernelProcess(), interruptPublishedWait,
                              &context, nullptr, false, true);
  thread->setName("hosted signal interrupter");
  return thread;
}

int publishDefaultActionDuringSemaphoreWait(void* parameter) {
  DefaultActionSemaphoreContext* context =
      reinterpret_cast<DefaultActionSemaphoreContext*>(parameter);
  if (waitUntilQueued(context->target, Thread::SemWait)) {
    context->published += 1;
  }

  SignalEvent* event = new SignalEvent(
      reinterpret_cast<uintptr_t>(&hostedDefaultActionHandler), HostedSignalNumber, ~0UL, 0, true,
      true, Event::HandlerPrivilege::Kernel, SignalEvent::DeliveryDisposition::DefaultAction);
  if (context->target->sendEvent(event)) {
    context->sent += 1;
  } else {
    delete event;
  }

  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (!g_DefaultActionHandlerCalls && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  context->gate->release();
  context->released += 1;
  return 0;
}

bool temporarySignalMaskNestedPrequeued(Thread* thread) {
  constexpr const char* Test = "temporary-signal-mask-nested-prequeued";
  constexpr uint64_t SignalBit = static_cast<uint64_t>(1) << (HostedSignalNumber - 1);
  const uint64_t originalMask = thread->getSignalMask();
  const uint64_t blockedMask = originalMask | SignalBit;
  const uint64_t temporaryMask = blockedMask & ~SignalBit;
  const size_t initialStateLevel = thread->getStateLevel();

  thread->setSignalMask(blockedMask);
  thread->clearInterruption();
  g_NestedWaitHandlerCalls = 0;
  g_NestedWaitHandlerLevel = 0;
  g_NestedWaitReturned = 0;
  g_NestedSignalHandlerCalls = 0;
  g_NestedSignalHandlerLevel = 0;

  HostedNestedWaitEvent outerEvent;
  SignalEvent signalEvent(reinterpret_cast<uintptr_t>(&hostedNestedSignalHandler),
                          HostedSignalNumber);
  const bool outerQueued = thread->sendEvent(&outerEvent);
  const bool signalQueued = outerQueued && thread->sendEvent(&signalEvent);

  bool activeMaskObserved = false;
  bool nestedStateRestored = false;
  bool interrupted = false;
  bool exactMaskRestored = false;
  bool interruptionConsumed = false;
  if (outerQueued && signalQueued) {
    Thread::TemporarySignalMask signalWait(*thread, temporaryMask);
    activeMaskObserved = thread->getSignalMask() == temporaryMask;
    thread->waitForEvent();
    nestedStateRestored = thread->getStateLevel() == initialStateLevel;
    interrupted = signalWait.finish();
    exactMaskRestored = thread->getSignalMask() == blockedMask;
    interruptionConsumed = thread->getInterruptionReason() == Thread::NotInterrupted;
  }

  const bool queuesDrained = !thread->hasEvent(&outerEvent) && !thread->hasEvent(&signalEvent);
  if (thread->hasEvent(&outerEvent)) {
    thread->cullEvent(&outerEvent);
  }
  if (thread->hasEvent(&signalEvent)) {
    thread->cullEvent(&signalEvent);
  }
  thread->setSignalMask(originalMask);
  thread->clearInterruption();

  const bool passed = check(
      outerQueued && signalQueued && activeMaskObserved && nestedStateRestored && interrupted &&
          exactMaskRestored && interruptionConsumed && queuesDrained &&
          g_NestedWaitHandlerCalls == 1 && g_NestedWaitHandlerLevel == (initialStateLevel + 1) &&
          g_NestedWaitReturned == 1 && g_NestedSignalHandlerCalls == 1 &&
          g_NestedSignalHandlerLevel == (initialStateLevel + 2),
      "a nested prequeued signal missed its owning temporary mask or exact restoration");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

bool temporarySignalMaskAcrossMutex(Thread* thread) {
  constexpr const char* Test = "temporary-signal-mask-across-mutex";
  constexpr uint64_t SignalBit = static_cast<uint64_t>(1) << (HostedSignalNumber - 1);
  const uint64_t originalMask = thread->getSignalMask();
  const uint64_t blockedMask = originalMask | SignalBit;
  const uint64_t temporaryMask = blockedMask & ~SignalBit;

  thread->setSignalMask(blockedMask);
  thread->clearInterruption();
  g_SignalHandlerCalls = 0;

  TemporaryMaskMutexContext context;
  Thread* holder = new Thread(Scheduler::instance().getKernelProcess(), holdTemporaryMaskMutex,
                              &context, nullptr, false, true, true);
  holder->setName("hosted temporary signal-mask mutex holder");
  const bool holderStarted = holder->start();
  const bool holderReady = holderStarted && context.holderReady.acquireForCompletion();

  bool activeMaskObserved = false;
  bool mutexAcquired = false;
  bool stickyAfterMutex = false;
  bool interruptibleWaitReturned = false;
  Semaphore::SemaphoreError waitError = Semaphore::NoError;
  bool interrupted = false;
  bool exactMaskRestored = false;
  bool interruptionConsumed = false;
  bool interrupterJoined = false;
  SignalContext signalContext(thread, Thread::SemWait, &context.releaseHolder);
  if (holderReady && context.holderAcquired == 1) {
    Thread::TemporarySignalMask signalWait(*thread, temporaryMask);
    activeMaskObserved = thread->getSignalMask() == temporaryMask;
    Thread* interrupter = startInterrupter(signalContext);

    mutexAcquired = context.mutex.acquire();
    stickyAfterMutex = thread->hasTemporarySignalWaitInterruption();
    if (mutexAcquired) {
      context.mutex.release();
    }

    if (stickyAfterMutex) {
      Semaphore interruptible(0);
      interruptibleWaitReturned = !interruptible.acquireWithError(1, 0, 0, waitError);
    }

    interrupted = signalWait.finish();
    exactMaskRestored = thread->getSignalMask() == blockedMask;
    interruptionConsumed = thread->getInterruptionReason() == Thread::NotInterrupted;
    interrupterJoined = interrupter->join();
  } else {
    context.releaseHolder.release();
  }

  const bool holderJoined = holderStarted && holder->join();
  if (!holderStarted) {
    delete holder;
  }
  thread->setSignalMask(originalMask);
  thread->clearInterruption();

  const bool passed = check(
      holderStarted && holderReady && context.holderAcquired == 1 && context.holderReturned == 1 &&
          activeMaskObserved && mutexAcquired && stickyAfterMutex && interruptibleWaitReturned &&
          waitError == Semaphore::Interrupted && interrupted && exactMaskRestored &&
          interruptionConsumed && interrupterJoined && holderJoined &&
          signalContext.published == 1 && signalContext.sent == 1 && signalContext.released == 1 &&
          g_SignalHandlerCalls == 1,
      "a non-interruptible Mutex lost or consumed its armed temporary-wait signal");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

bool temporarySignalMaskIgnoresDefaultAction(Thread* thread) {
  constexpr const char* Test = "temporary-signal-mask-default-action";
  const uint64_t originalMask = thread->getSignalMask();
  thread->clearInterruption();
  g_DefaultActionHandlerCalls = 0;

  Semaphore gate(0);
  DefaultActionSemaphoreContext context(thread, &gate);
  Thread* publisher =
      new Thread(Scheduler::instance().getKernelProcess(), publishDefaultActionDuringSemaphoreWait,
                 &context, nullptr, false, true);
  publisher->setName("hosted default-action signal publisher");

  Semaphore::SemaphoreError error = Semaphore::NoError;
  bool acquired = false;
  bool interrupted = true;
  {
    Thread::TemporarySignalMask signalWait(*thread, originalMask);
    acquired = gate.acquireWithError(1, 0, 0, error);
    interrupted = signalWait.finish();
  }
  const bool joined = publisher->join();
  const bool interruptionConsumed = thread->getInterruptionReason() == Thread::NotInterrupted;
  thread->setSignalMask(originalMask);
  thread->clearInterruption();

  const bool passed =
      check(acquired && error == Semaphore::NoError && !interrupted && joined &&
                interruptionConsumed && context.published == 1 && context.sent == 1 &&
                context.released == 1 && g_DefaultActionHandlerCalls == 1,
            "a default signal action terminated a temporary-mask Semaphore wait");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

bool conditionVariableSignalInterruption(Thread* thread) {
  ConditionVariable condition;
  Mutex mutex;
  SignalContext context(thread, Thread::CondWait);

  if (!mutex.acquire()) {
    return check(false, "the ConditionVariable mutex was unavailable");
  }
  bool passed = true;
  g_SignalHandlerCalls = 0;
  Thread* interrupter = startInterrupter(context);

  ConditionVariable::Error error = ConditionVariable::NoError;
  const bool waited = condition.wait(mutex, error);
  const bool mutexHeld = mutex.isOwnedByCurrentThread();
  if (mutexHeld) {
    mutex.release();
  }
  const bool joined = interrupter->join();

  passed &= check(!waited && error == ConditionVariable::Interrupted,
                  "ConditionVariable did not report Interrupted");
  passed &= check(mutexHeld, "ConditionVariable did not reacquire its mutex");
  passed &=
      check(joined && context.published == 1 && context.sent == 1 && g_SignalHandlerCalls == 1,
            "ConditionVariable wait did not receive one published signal");
  return passed;
}

bool bufferSignalInterruption(Thread* thread) {
  Buffer<char> buffer(8);
  SignalContext context(thread, Thread::CondWait);

  thread->clearInterruption();
  g_SignalHandlerCalls = 0;
  Thread* interrupter = startInterrupter(context);

  char value = 0;
  const size_t read = buffer.read(&value, 1, true);
  const Thread::InterruptionReason reason = thread->getInterruptionReason();
  thread->clearInterruption();
  const bool joined = interrupter->join();

  return check(read == 0 && reason == Thread::InterruptedBySignal && joined &&
                   context.published == 1 && context.sent == 1 && g_SignalHandlerCalls == 1,
               "Buffer did not preserve its signal interruption");
}

bool semaphoreSignalInterruption(Thread* thread) {
  Semaphore semaphore(0);
  SignalContext context(thread, Thread::SemWait);

  thread->clearInterruption();
  g_SignalHandlerCalls = 0;
  Thread* interrupter = startInterrupter(context);

  Semaphore::SemaphoreError error = Semaphore::NoError;
  const bool acquired = semaphore.acquireWithError(1, 0, 0, error);
  const Thread::InterruptionReason reason = thread->getInterruptionReason();
  thread->clearInterruption();
  const bool joined = interrupter->join();

  return check(!acquired && error == Semaphore::Interrupted &&
                   reason == Thread::InterruptedBySignal && joined && context.published == 1 &&
                   context.sent == 1 && g_SignalHandlerCalls == 1,
               "Semaphore did not preserve its signal interruption");
}

bool semaphoreSignalAfterOrdinaryWake() {
  SemaphoreWakeCollisionContext context;
  Thread* waiter = new Thread(Scheduler::instance().getKernelProcess(),
                              waitForSemaphoreWakeCollision, &context, nullptr, false, true);
  waiter->setName("hosted semaphore wake/signal collision");
  context.waiter = waiter;

  const bool queued = waitUntilQueued(waiter, Thread::SemWait);
  g_SignalHandlerCalls = 0;
  g_SemaphoreWakeCollision = &context;
  WaitQueue::setBeforeBlockHook(semaphoreWakeCollisionRescue);

  // Win waiter.reason with an ordinary release, then make that release
  // unavailable and publish a signal before the waiter can run.
  context.semaphore.release();
  const bool releaseConsumed = context.semaphore.tryAcquire();
  SignalEvent* event = new SignalEvent(reinterpret_cast<uintptr_t>(&hostedSignalHandler),
                                       HostedSignalNumber, ~0UL, 0, true, true);
  const bool sent = waiter->sendEvent(event);
  if (!sent) {
    delete event;
  }

  const bool joined = waiter->join();
  WaitQueue::setBeforeBlockHook(nullptr);
  g_SemaphoreWakeCollision = nullptr;

  const bool passed = check(queued && releaseConsumed && sent && joined && context.entered == 1 &&
                                context.returned == 1 && context.acquired == 0 &&
                                context.error == Semaphore::Interrupted &&
                                context.interruption == Thread::InterruptedBySignal &&
                                context.rescueWaits == 0 && g_SignalHandlerCalls == 1,
                            "Semaphore re-blocked after a signal lost the waiter.reason race");
  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "semaphore-signal-after-ordinary-wake");
  }
  return passed;
}

bool conditionSignalAfterOrdinaryWake() {
  ConditionWakeCollisionContext context;
  Thread* waiter = new Thread(Scheduler::instance().getKernelProcess(),
                              waitForConditionWakeCollision, &context, nullptr, false, true);
  waiter->setName("hosted condition wake/signal collision");
  context.waiter = waiter;

  const bool queued = waitUntilQueued(waiter, Thread::CondWait);
  g_SignalHandlerCalls = 0;
  g_ConditionWakeCollision = &context;
  WaitQueue::setBeforeBlockHook(conditionWakeCollisionRescue);

  // The condition signal deliberately does not satisfy the predicate. A
  // signal event published after this wake must still become the result.
  context.condition.signal();
  SignalEvent* event = new SignalEvent(reinterpret_cast<uintptr_t>(&hostedSignalHandler),
                                       HostedSignalNumber, ~0UL, 0, true, true);
  const bool sent = waiter->sendEvent(event);
  if (!sent) {
    delete event;
  }

  const bool joined = waiter->join();
  WaitQueue::setBeforeBlockHook(nullptr);
  g_ConditionWakeCollision = nullptr;

  const bool passed =
      check(queued && sent && joined && context.entered == 1 && context.returned == 1 &&
                context.waits == 1 && context.lastResult == 0 &&
                context.error == ConditionVariable::Interrupted && context.rescueWaits == 0 &&
                g_SignalHandlerCalls == 1,
            "ConditionVariable re-blocked after a signal lost the waiter.reason race");
  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "condition-signal-after-ordinary-wake");
  }
  return passed;
}

bool completionSemaphoreSignalDeferral(Thread* thread) {
  Semaphore semaphore(0);
  SignalContext context(thread, Thread::SemWait, &semaphore);

  thread->clearInterruption();
  g_SignalHandlerCalls = 0;
  Thread* interrupter = startInterrupter(context);

  const bool acquired = semaphore.acquireForCompletion();
  const Thread::InterruptionReason reason = thread->getInterruptionReason();
  thread->clearInterruption();
  const bool joined = interrupter->join();

  return check(acquired && reason == Thread::InterruptedBySignal && joined &&
                   context.published == 1 && context.sent == 1 && context.released == 1 &&
                   g_SignalHandlerCalls == 1 && semaphore.getValue() == 0,
               "a completion semaphore returned before its signal-delayed release");
}

bool ringBufferSignalInterruption(Thread* thread) {
  RingBuffer<char> buffer(1);
  SignalContext context(thread, Thread::CondWait);

  thread->clearInterruption();
  g_SignalHandlerCalls = 0;
  Thread* interrupter = startInterrupter(context);

  Time::Timestamp timeout = Time::Infinity;
  char value = 0;
  RingBuffer<char>::Error error = RingBuffer<char>::NoError;
  const bool read = buffer.read(value, timeout, error);
  const Thread::InterruptionReason reason = thread->getInterruptionReason();
  thread->clearInterruption();
  const bool joined = interrupter->join();

  return check(!read && error == RingBuffer<char>::Interrupted &&
                   reason == Thread::InterruptedBySignal && joined && context.published == 1 &&
                   context.sent == 1 && g_SignalHandlerCalls == 1,
               "RingBuffer did not preserve its signal interruption");
}

bool ringBufferMonitorCull(Thread* thread) {
  RingBuffer<char> buffer(1);
  SignalEvent event(reinterpret_cast<uintptr_t>(&hostedSignalHandler), HostedSignalNumber);

  buffer.monitor(thread, &event);
  buffer.cullMonitorTargets(thread);

  return check(!buffer.dataReady() && buffer.canWrite(),
               "RingBuffer monitor culling retained its internal mutex");
}

bool ringBufferMonitorRetirement(Thread* thread) {
  RingBuffer<char> buffer(1);
  HostedMonitorEvent event;
  buffer.monitor(thread, &event);

  EventDrainContext context(&event);
  Thread* drainer = new Thread(Scheduler::instance().getKernelProcess(), drainEventRegistrations,
                               &context, nullptr, false, true);
  drainer->setName("hosted RingBuffer monitor retirement");

  const bool closePublished = waitUntilQueued(drainer, Thread::EventWait);

  // Model an already-dispatched callback trying to re-arm after its owner
  // closed event admission. A distinct target lets the original cull prove
  // that no second registration was accepted.
  buffer.monitor(drainer, &event);
  buffer.cullMonitorTargets(thread);

  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (!context.completed && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  const bool rearmRejected = context.completed == 1;

  // Keep a failing regression from leaving its drain thread behind.
  if (!rearmRejected) {
    buffer.cullMonitorTargets(drainer);
  }
  const bool joined = drainer->join();

  return check(context.entered == 1 && closePublished && rearmRejected && joined,
               "RingBuffer accepted a monitor registration after Event retirement");
}

bool ringBufferMonitorDestructor(Thread* thread) {
  const size_t destructionsBefore = g_MonitorEventDestructions;
  HostedMonitorEvent* event = new HostedMonitorEvent;
  {
    RingBuffer<char> buffer(1);
    buffer.monitor(thread, event);
  }

  // RingBuffer closure legitimately queued one final readiness event. Keep
  // retirement pinned while removing that delivery so deletion now depends
  // only on whether the destroyed RingBuffer released its source lease.
  const bool closeNotified = event->pendingCount() == 1 && thread->hasEvent(event);
  {
    Event::Retirement retirement;
    event->beginRetirement(retirement);
    thread->cullEvent(event);
  }
  return check(closeNotified && g_MonitorEventDestructions == (destructionsBefore + 1),
               "RingBuffer destruction retained its Event source lease after the "
               "queued close notification drained");
}

bool delaySignalInterruption(Thread* thread) {
  SignalContext context(thread, Thread::EventWait);

  thread->clearInterruption();
  g_SignalHandlerCalls = 0;
  Thread* interrupter = startInterrupter(context);

  const bool delayed = Time::delay(5 * Time::Multiplier::Second);
  const Thread::InterruptionReason reason = thread->getInterruptionReason();
  thread->clearInterruption();
  const bool joined = interrupter->join();

  return check(!delayed && reason == Thread::InterruptedBySignal && joined &&
                   context.published == 1 && context.sent == 1 && g_SignalHandlerCalls == 1,
               "Time::delay did not preserve its signal interruption");
}

bool prequeuedDelaySignalInterruption(Thread* thread) {
  thread->clearInterruption();
  g_SignalHandlerCalls = 0;

  SignalEvent* event = new SignalEvent(reinterpret_cast<uintptr_t>(&hostedSignalHandler),
                                       HostedSignalNumber, ~0UL, 0, true, true);
  if (!thread->sendEvent(event)) {
    delete event;
    return check(false, "a prequeued signal could not be published");
  }

  const bool delayed = Time::delay(5 * Time::Multiplier::Second);
  const Thread::InterruptionReason reason = thread->getInterruptionReason();
  thread->clearInterruption();

  return check(!delayed && reason == Thread::InterruptedBySignal && g_SignalHandlerCalls == 1,
               "a prequeued signal did not interrupt Time::delay");
}
}  // namespace

bool runHostedSignalInterruptionRegressions(Thread* thread) {
  const bool passed =
      eventHandlerPrivilege() && signalCullPreservesNumberCollision(thread) &&
      execPreservesNestedSignalMask(thread) && invalidUserHandlerDeliveryFailsClosed(thread) &&
#if !defined(PEDIGREE_HOSTED_CORE_SMOKE)
      ignoredSignalDoesNotInterruptWait(thread->getParent()) &&
      ignoredDispositionDiscardsPendingSignals(thread->getParent()) &&
      execSignalResetRebindsPending(thread->getParent()) &&
      signalContinueStillResumes(thread->getParent()) &&
      opposingJobControlSignalsCancelAcrossThreads(thread->getParent()) &&
      stoppedProcessDefersSignalsUntilContinue(thread->getParent()) &&
#endif
      temporarySignalMaskNestedPrequeued(thread) && temporarySignalMaskAcrossMutex(thread) &&
      temporarySignalMaskIgnoresDefaultAction(thread) &&
      conditionVariableSignalInterruption(thread) && bufferSignalInterruption(thread) &&
      semaphoreSignalInterruption(thread) && semaphoreSignalAfterOrdinaryWake() &&
      conditionSignalAfterOrdinaryWake() && completionSemaphoreSignalDeferral(thread) &&
      ringBufferSignalInterruption(thread) && ringBufferMonitorCull(thread) &&
      ringBufferMonitorRetirement(thread) && ringBufferMonitorDestructor(thread) &&
      delaySignalInterruption(thread) && prequeuedDelaySignalInterruption(thread);
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS signal-interruption");
  }
  return passed;
}
