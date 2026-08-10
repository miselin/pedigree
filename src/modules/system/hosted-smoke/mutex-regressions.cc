/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"

namespace {
struct MutexOwnershipContext {
  MutexOwnershipContext(Mutex* mutex, ConditionVariable* condition)
      : mutex(mutex),
        condition(condition),
        nonOwnerReleaseRejected(0),
        conditionWaitRejected(0),
        timedAcquireFinished(0),
        timedAcquireTimedOut(0),
        timedAcquireSucceeded(0) {}

  Mutex* mutex;
  ConditionVariable* condition;
  Atomic<size_t> nonOwnerReleaseRejected;
  Atomic<size_t> conditionWaitRejected;
  Atomic<size_t> timedAcquireFinished;
  Atomic<size_t> timedAcquireTimedOut;
  Atomic<size_t> timedAcquireSucceeded;
};

struct MutexGuardContext {
  MutexGuardContext(Mutex* mutex, bool constexprGuard)
      : mutex(mutex),
        worker(nullptr),
        constexprGuard(constexprGuard),
        phase(0),
        entered(0),
        ownedInCritical(0),
        terminalPending(0),
        deferredInCritical(0),
        releaseTransitions(0),
        releaseSawDeferral(0),
        releasedAfterScope(0),
        returned(0) {}

  Mutex* mutex;
  Thread* worker;
  bool constexprGuard;
  Atomic<size_t> phase;
  Atomic<size_t> entered;
  Atomic<size_t> ownedInCritical;
  Atomic<size_t> terminalPending;
  Atomic<size_t> deferredInCritical;
  Atomic<size_t> releaseTransitions;
  Atomic<size_t> releaseSawDeferral;
  Atomic<size_t> releasedAfterScope;
  Atomic<size_t> returned;
};

MutexGuardContext* g_MutexGuardContext = nullptr;

Atomic<size_t> g_AcquireTransitionSeen(0);
Atomic<size_t> g_ReleaseTransitionSeen(0);
Atomic<size_t> g_TransitionInterruptFailures(0);

void mutexTransitionHook(Semaphore::MutexTransitionWindow window) {
  if (Processor::getInterrupts()) {
    g_TransitionInterruptFailures += 1;
  }

  if (window == Semaphore::MutexCounterAcquired) {
    g_AcquireTransitionSeen += 1;
  } else if (window == Semaphore::MutexOwnerReleased) {
    g_ReleaseTransitionSeen += 1;
  }
}

void mutexGuardTransitionHook(Semaphore::MutexTransitionWindow window) {
  MutexGuardContext* context = g_MutexGuardContext;
  Thread* thread = Processor::information().getCurrentThread();
  if (!context || thread != context->worker || window != Semaphore::MutexOwnerReleased ||
      context->phase != static_cast<size_t>(1)) {
    return;
  }

  context->releaseTransitions += 1;
  if (thread->isTerminationDeferred()) {
    context->releaseSawDeferral += 1;
  }
}

bool check(bool condition, const char* detail, const char* test = "mutex-ownership") {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
  return false;
}

void observeGuardedCriticalSection(MutexGuardContext* context) {
  Thread* thread = Processor::information().getCurrentThread();
  context->entered += 1;
  context->ownedInCritical = context->mutex->isOwnedByCurrentThread() ? 1 : 0;
  context->terminalPending = thread->getUnwindState() == Thread::TerminateThread ? 1 : 0;
  context->deferredInCritical = thread->isTerminationDeferred() ? 1 : 0;
  context->phase = 1;
}

int acquireTerminalMutexGuard(void* parameter) {
  MutexGuardContext* context = reinterpret_cast<MutexGuardContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  if (context->constexprGuard) {
    ConstexprLockGuard<Mutex, true> guard(*context->mutex);
    observeGuardedCriticalSection(context);
  } else {
    LockGuard<Mutex> guard(*context->mutex);
    observeGuardedCriticalSection(context);
  }

  context->phase = 2;
  context->releasedAfterScope =
      !context->mutex->isOwnedByCurrentThread() && context->mutex->getValue() == 1 ? 1 : 0;
  thread->setUnwindState(Thread::Continue);
  context->returned += 1;
  return 0;
}

bool waitForMutexGuardBlock(Thread* thread) {
  for (size_t i = 0; i < 10000; ++i) {
    Thread::WaitDebugInfo wait = {};
    uintptr_t debugAddress = 0;
    if (thread->getWaitDebugInfo(wait) && wait.queued &&
        thread->getDebugState(debugAddress) == Thread::SemWait) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool terminalMutexGuardScenario(bool constexprGuard) {
  Mutex mutex;
  MutexGuardContext context(&mutex, constexprGuard);
  const bool supervisorAcquired = mutex.acquireForCompletion();

  Thread* worker = new Thread(Scheduler::instance().getKernelProcess(), acquireTerminalMutexGuard,
                              &context, nullptr, false, true);
  if (constexprGuard) {
    worker->setName("hosted constexpr terminal Mutex guard");
  } else {
    worker->setName("hosted terminal Mutex guard");
  }
  context.worker = worker;

  const bool queued = waitForMutexGuardBlock(worker);
  const bool enteredBeforeRelease = context.entered != static_cast<size_t>(0);
  g_MutexGuardContext = &context;
  Semaphore::setMutexTransitionHook(mutexGuardTransitionHook);
  worker->setUnwindState(Thread::TerminateThread);
  if (supervisorAcquired) {
    mutex.release();
  }
  const bool joined = worker->joinForCompletion();
  Semaphore::setMutexTransitionHook(nullptr);
  g_MutexGuardContext = nullptr;

  const bool recoverable = mutex.tryAcquire();
  if (recoverable) {
    mutex.release();
  }

  return supervisorAcquired && queued && !enteredBeforeRelease && joined && context.entered == 1 &&
         context.ownedInCritical == 1 && context.terminalPending == 1 &&
         context.deferredInCritical == 1 && context.releaseTransitions == 1 &&
         context.releaseSawDeferral == 1 && context.releasedAfterScope == 1 &&
         context.returned == 1 && recoverable;
}

bool mutexGuardTerminalCompletion() {
  constexpr const char* Test = "mutex-guard-terminal-completion";
  bool passed = true;
  passed &= check(terminalMutexGuardScenario(false),
                  "LockGuard did not retain ownership and teardown deferral", Test);
  passed &= check(terminalMutexGuardScenario(true),
                  "ConstexprLockGuard did not retain ownership and teardown deferral", Test);

  Mutex conditionalMutex;
  {
    LockGuard<Mutex> guard(conditionalMutex, false);
    passed &= check(!guard.ownsLock() && !conditionalMutex.isOwnedByCurrentThread() &&
                        conditionalMutex.getValue() == 1,
                    "condition=false acquired or claimed the mutex", Test);
  }

  Mutex disownedMutex;
  {
    LockGuard<Mutex> guard(disownedMutex);
    const bool acquired = guard.ownsLock() && disownedMutex.isOwnedByCurrentThread();
    disownedMutex.release();
    guard.disown();
    passed &= check(acquired && !guard.ownsLock() && !disownedMutex.isOwnedByCurrentThread() &&
                        disownedMutex.getValue() == 1,
                    "disown did not transfer release responsibility", Test);
  }

  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "mutex-guard-terminal-completion");
  }
  return passed;
}

int attemptNonOwnerOperations(void* parameter) {
  MutexOwnershipContext* context = reinterpret_cast<MutexOwnershipContext*>(parameter);

  const bool ownedBeforeRelease = context->mutex->isOwnedByCurrentThread();
  context->mutex->release();
  if (!ownedBeforeRelease && !context->mutex->isOwnedByCurrentThread() &&
      context->mutex->getValue() == 0) {
    context->nonOwnerReleaseRejected += 1;
  }

  ConditionVariable::Error error = ConditionVariable::NoError;
  const bool waited = context->condition->wait(*context->mutex, error);
  if (!waited && error == ConditionVariable::MutexNotLocked &&
      !context->mutex->isOwnedByCurrentThread() && context->mutex->getValue() == 0) {
    context->conditionWaitRejected += 1;
  }

  return 0;
}

int attemptTimedMutexAcquire(void* parameter) {
  MutexOwnershipContext* context = reinterpret_cast<MutexOwnershipContext*>(parameter);
  Semaphore::SemaphoreError error = Semaphore::NoError;
  const bool acquired = context->mutex->acquireWithError(1, 0, 20000, error);
  if (acquired) {
    context->timedAcquireSucceeded += 1;
    context->mutex->release();
  } else if (error == Semaphore::TimedOut) {
    context->timedAcquireTimedOut += 1;
  }
  context->timedAcquireFinished += 1;
  return 0;
}
}  // namespace

bool runHostedMutexRegressions() {
  Mutex mutex;
  ConditionVariable condition;
  MutexOwnershipContext context(&mutex, &condition);
  bool passed = true;

  g_AcquireTransitionSeen = 0;
  g_ReleaseTransitionSeen = 0;
  g_TransitionInterruptFailures = 0;

  const bool initialInterruptState = Processor::getInterrupts();
  {
    Spinlock recursiveLock(false, true);
    recursiveLock.acquire(Spinlock::allow_recursion);
    const bool outerDisabled = !Processor::getInterrupts();
    recursiveLock.acquire(Spinlock::allow_recursion);
    const bool nestedDisabled = !Processor::getInterrupts();
    recursiveLock.release();
    const bool innerReleaseKeptDisabled = !Processor::getInterrupts();
    recursiveLock.release();
    passed &=
        check(initialInterruptState && outerDisabled && nestedDisabled &&
                  innerReleaseKeptDisabled && Processor::getInterrupts() == initialInterruptState,
              "recursive spinlock acquisition lost the outer interrupt state");
  }

  Semaphore::setMutexTransitionHook(mutexTransitionHook);
  passed &= check(mutex.acquire(), "the supervisor could not acquire the mutex");
  Semaphore::setMutexTransitionHook(nullptr);
  passed &= check(mutex.isOwnedByCurrentThread() && mutex.getValue() == 0,
                  "acquisition did not publish a single current-thread owner");
  passed &= check(mutex.getDebugMutexOwner() == Processor::information().getCurrentThread(),
                  "the debugger owner snapshot did not identify the mutex owner");
  passed &= check(g_AcquireTransitionSeen == 1 && g_TransitionInterruptFailures == 0 &&
                      Processor::getInterrupts() == initialInterruptState,
                  "acquisition exposed its counter/owner transition to interrupts");

  Thread* peer = new Thread(Scheduler::instance().getKernelProcess(), attemptNonOwnerOperations,
                            &context, nullptr, false, true);
  peer->setName("hosted mutex ownership regression");

  passed &= check(peer->join(), "the non-owner peer could not be joined");
  passed &= check(context.nonOwnerReleaseRejected == 1, "a non-owner release changed the mutex");
  passed &= check(context.conditionWaitRejected == 1,
                  "condition wait accepted a mutex owned by another thread");
  passed &= check(mutex.isOwnedByCurrentThread() && mutex.getValue() == 0,
                  "the non-owner peer disturbed the supervisor's ownership");

  Thread* timedPeer = new Thread(Scheduler::instance().getKernelProcess(), attemptTimedMutexAcquire,
                                 &context, nullptr, false, true);
  timedPeer->setName("hosted timed mutex acquisition regression");

  const Time::Timestamp timedAcquireDeadline =
      Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (!context.timedAcquireFinished && Time::getTicks() < timedAcquireDeadline) {
    Scheduler::instance().yield();
  }

  // If the timeout path regresses, release the mutex so the peer and test
  // suite can finish and report the failure instead of hanging indefinitely.
  const bool timedAcquireNeededRescue = !context.timedAcquireFinished;
  if (timedAcquireNeededRescue) {
    mutex.release();
  }

  passed &= check(timedPeer->join(), "the timed-acquire peer could not be joined");
  const bool timedAcquirePassed = !timedAcquireNeededRescue && context.timedAcquireTimedOut == 1 &&
                                  context.timedAcquireSucceeded == 0;
  passed &= check(timedAcquirePassed, "a held Mutex ignored its acquisition timeout");
  if (timedAcquirePassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS hosted-timer-timeout-cleanup");
  }

  if (timedAcquireNeededRescue) {
    passed &=
        check(mutex.acquire(), "the supervisor could not restore ownership after timeout rescue");
  }
  passed &= check(mutex.isOwnedByCurrentThread() && mutex.getValue() == 0,
                  "the timed-acquire peer disturbed the supervisor's ownership");

  const bool releaseInterruptState = Processor::getInterrupts();
  Semaphore::setMutexTransitionHook(mutexTransitionHook);
  mutex.release();
  Semaphore::setMutexTransitionHook(nullptr);
  passed &= check(!mutex.isOwnedByCurrentThread() && mutex.getValue() == 1 &&
                      mutex.getDebugMutexOwner() == nullptr,
                  "owner release did not restore one available item");
  passed &= check(g_ReleaseTransitionSeen == 1 && g_TransitionInterruptFailures == 0 &&
                      Processor::getInterrupts() == releaseInterruptState,
                  "release exposed its owner/counter transition to interrupts");

  mutex.release();
  passed &= check(!mutex.isOwnedByCurrentThread() && mutex.getValue() == 1,
                  "double release changed the mutex's binary count");

  Semaphore counting(1, false);
  const size_t acquireTransitions = g_AcquireTransitionSeen;
  const size_t releaseTransitions = g_ReleaseTransitionSeen;
  const bool countingInterruptState = Processor::getInterrupts();
  Semaphore::setMutexTransitionHook(mutexTransitionHook);
  passed &= check(counting.tryAcquire(), "the counting-semaphore control acquisition failed");
  counting.release();
  Semaphore::setMutexTransitionHook(nullptr);
  passed &= check(g_AcquireTransitionSeen == acquireTransitions &&
                      g_ReleaseTransitionSeen == releaseTransitions &&
                      Processor::getInterrupts() == countingInterruptState,
                  "counting Semaphore entered a Mutex transition window");
  passed &= check(Processor::getInterrupts() == initialInterruptState,
                  "thread join or mutex teardown lost the caller interrupt state");

  passed &= mutexGuardTerminalCompletion();

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS mutex-ownership");
  }
  return passed;
}
