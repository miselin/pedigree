/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Completion.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/MemoryPressureManager.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/MemoryPool.h"
#include "pedigree/kernel/utilities/RadixTree.h"
#include "pedigree/kernel/utilities/RingBuffer.h"
#include "pedigree/kernel/utilities/Vector.h"

namespace {
bool check(bool condition, const char* test, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
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

bool radixTreeExportedAbi() {
  RadixTree<void*> tree;
  const String key("hosted-radix-tree-abi");
  void* const expected = &tree;

  void* value = expected;
  const bool missing = !tree.lookup(key, value) && value == nullptr;

  tree.insert(key, expected);
  value = nullptr;
  const bool found = tree.lookup(key, value) && value == expected;

  tree.remove(key);
  value = expected;
  const bool removed = !tree.lookup(key, value) && value == nullptr;

  const bool passed = check(missing && found && removed, "radix-tree-exported-abi",
                            "module-to-kernel bool-and-output lookup contract failed");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS radix-tree-exported-abi");
  }
  return passed;
}

bool semaphoreDrainAvailable() {
  Semaphore semaphore(3);
  const size_t firstDrain = semaphore.drainAvailable();
  const bool emptyAfterFirstDrain = !semaphore.tryAcquire();
  semaphore.release(2);
  const size_t secondDrain = semaphore.drainAvailable();
  const size_t emptyDrain = semaphore.drainAvailable();

  const bool passed =
      check(firstDrain == 3 && emptyAfterFirstDrain && secondDrain == 2 && emptyDrain == 0 &&
                !semaphore.tryAcquire(),
            "semaphore-drain-available", "available items were not removed exactly once");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS semaphore-drain-available");
  }
  return passed;
}

struct CompletionContext {
  explicit CompletionContext(Completion* completion)
      : completion(completion), entered(0), completed(0) {}

  Completion* completion;
  Atomic<size_t> entered;
  Atomic<size_t> completed;
};

struct TerminalCompletionContext {
  explicit TerminalCompletionContext(Semaphore* completion)
      : completion(completion), entered(0), acquired(0), returned(0) {}

  Semaphore* completion;
  Atomic<size_t> entered;
  Atomic<size_t> acquired;
  Atomic<size_t> returned;
};

struct OperationBarrierContext {
  explicit OperationBarrierContext(OperationBarrier* barrier)
      : barrier(barrier), workEntered(0), workFinished(0), closeFinished(0), releaseWork(0) {}

  OperationBarrier* barrier;
  Atomic<size_t> workEntered;
  Atomic<size_t> workFinished;
  Atomic<size_t> closeFinished;
  Semaphore releaseWork;
};

int waitForCompletion(void* parameter) {
  CompletionContext* context = reinterpret_cast<CompletionContext*>(parameter);
  context->entered += 1;
  if (context->completion->wait()) {
    context->completed += 1;
  }
  return 0;
}

int waitForTerminalCompletion(void* parameter) {
  TerminalCompletionContext* context = reinterpret_cast<TerminalCompletionContext*>(parameter);
  context->entered += 1;
  if (context->completion->acquireForCompletion()) {
    context->acquired += 1;
  }
  context->returned += 1;
  return 0;
}

int runAdmittedOperation(void* parameter) {
  OperationBarrierContext* context = reinterpret_cast<OperationBarrierContext*>(parameter);
  context->workEntered += 1;
  const bool released = context->releaseWork.acquireForCompletion();
  (void)released;
  context->workFinished += 1;
  context->barrier->leave();
  return 0;
}

int closeOperationBarrier(void* parameter) {
  OperationBarrierContext* context = reinterpret_cast<OperationBarrierContext*>(parameter);
  context->barrier->closeAndWait();
  context->closeFinished += 1;
  return 0;
}

bool completionLifecycle() {
  bool passed = true;

  Completion latched;
  passed &=
      check(latched.complete(), "completion-lifecycle", "the first early completion was rejected");
  passed &=
      check(!latched.complete(), "completion-lifecycle", "duplicate early completion was accepted");
  passed &=
      check(latched.wait(), "completion-lifecycle", "complete-before-wait did not stay latched");

  Completion delayed;
  CompletionContext context(&delayed);
  Thread* waiter = new Thread(Scheduler::instance().getKernelProcess(), waitForCompletion, &context,
                              nullptr, false, true);
  waiter->setName("hosted Completion waiter");

  const bool queued = waitUntilQueued(waiter, Thread::SemWait);
  const bool completed = delayed.complete();
  const bool duplicateRejected = !delayed.complete();
  const bool joined = waiter->join();

  passed &= check(context.entered == 1 && queued, "completion-lifecycle",
                  "waiter-before-complete did not publish its wait");
  passed &= check(completed && duplicateRejected, "completion-lifecycle",
                  "delayed completion was not exactly-once");
  passed &= check(joined && context.completed == 1, "completion-lifecycle",
                  "the delayed waiter did not wake exactly once");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS completion-lifecycle");
  }
  return passed;
}

bool terminalCompletionBarrier() {
  Semaphore completion(0);
  TerminalCompletionContext context(&completion);
  Thread* waiter = new Thread(Scheduler::instance().getKernelProcess(), waitForTerminalCompletion,
                              &context, nullptr, false, true);
  waiter->setName("hosted terminal completion waiter");

  const bool queued = waitUntilQueued(waiter, Thread::SemWait);
  waiter->setUnwindState(Thread::TerminateThread);

  // Let the terminal wake reach acquireForCompletion(). It must re-enrol
  // rather than abandon storage still owned by the producer.
  for (size_t i = 0; i < 8; ++i) {
    Scheduler::instance().yield();
  }
  const bool deferred = context.entered == 1 && context.returned == 0;

  completion.release();
  const bool joined = waiter->join();

  const bool passed = check(
      queued && deferred && joined && context.acquired == 1 && context.returned == 1,
      "terminal-completion-barrier", "terminal teardown escaped an unfinished completion barrier");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS terminal-completion-barrier");
  }
  return passed;
}

bool operationBarrierLifecycle() {
  OperationBarrier barrier;
  OperationBarrierContext context(&barrier);
  Process* process = Scheduler::instance().getKernelProcess();

  const bool admitted = barrier.tryEnter();
  Thread* worker = new Thread(process, runAdmittedOperation, &context, nullptr, false, true);
  worker->setName("hosted admitted operation");

  while (!context.workEntered) {
    Scheduler::instance().yield();
  }

  Thread* closer = new Thread(process, closeOperationBarrier, &context, nullptr, false, true);
  closer->setName("hosted operation barrier closer");

  const bool closeQueued = waitUntilQueued(closer, Thread::CallbackDrain);
  const bool closeBlocked = closeQueued && context.closeFinished == 0 && context.workFinished == 0;
  const bool lateRejected = !barrier.tryEnter();

  context.releaseWork.release();
  const bool workerJoined = worker->join();
  const bool closerJoined = closer->join();
  const bool drained = barrier.isClosedAndDrained();

  const bool passed =
      check(admitted && closeBlocked && lateRejected && workerJoined && closerJoined &&
                context.workFinished == 1 && context.closeFinished == 1 && drained,
            "operation-barrier-lifecycle",
            "close did not reject late work and drain the admitted operation");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS operation-barrier-lifecycle");
  }
  return passed;
}

struct ConditionTimeoutContext {
  ConditionTimeoutContext(ConditionVariable* condition, Thread* waiter, Time::Timestamp signalAt)
      : condition(condition), waiter(waiter), signalAt(signalAt), published(0), signals(0) {}

  ConditionVariable* condition;
  Thread* waiter;
  Time::Timestamp signalAt;
  Atomic<size_t> published;
  Atomic<size_t> signals;
};

Atomic<size_t> g_ZeroTimeoutPublications(0);
Thread* g_ZeroTimeoutThread = nullptr;

void observeZeroTimeoutPublication(WaitQueue*, Thread* thread, const WaitQueue::Channel&,
                                   size_t debugState) {
  if (thread == g_ZeroTimeoutThread && debugState == Thread::CondWait) {
    g_ZeroTimeoutPublications += 1;
  }
}

int delayedConditionSignal(void* parameter) {
  ConditionTimeoutContext* context = reinterpret_cast<ConditionTimeoutContext*>(parameter);
  if (waitUntilQueued(context->waiter, Thread::CondWait)) {
    context->published += 1;
  }

  while (Time::getTicks() < context->signalAt) {
    Scheduler::instance().yield();
  }
  context->condition->signal();
  context->signals += 1;
  return 0;
}

bool conditionVariableTimeoutAccounting(Thread* thread) {
  ConditionVariable condition;
  Mutex mutex;
  bool passed = true;

  constexpr Time::Timestamp InitialTimeout = 250 * Time::Multiplier::Millisecond;
  Time::Timestamp remaining = InitialTimeout;
  ConditionTimeoutContext context(&condition, thread,
                                  Time::getTicks() + (20 * Time::Multiplier::Millisecond));
  Thread* signaler = new Thread(Scheduler::instance().getKernelProcess(), delayedConditionSignal,
                                &context, nullptr, false, true);
  signaler->setName("hosted timed ConditionVariable signaler");

  passed &= check(mutex.acquire(), "condition-variable-timeout",
                  "the signalled wait mutex could not be acquired");
  ConditionVariable::Error error = ConditionVariable::NoError;
  const bool signalled = condition.wait(mutex, remaining, error);
  const bool mutexHeldAfterSignal = mutex.isOwnedByCurrentThread();
  mutex.release();
  const bool signalerJoined = signaler->join();

  passed &= check(signalled && error == ConditionVariable::NoError, "condition-variable-timeout",
                  "the delayed signal was reported as an error");
  passed &= check(context.published == 1 && context.signals == 1 && signalerJoined,
                  "condition-variable-timeout",
                  "the delayed signal did not observe one published waiter");
  passed &= check(mutexHeldAfterSignal, "condition-variable-timeout",
                  "the signalled wait did not reacquire its mutex");
  passed &= check(remaining > 0 && remaining < InitialTimeout, "condition-variable-timeout",
                  "remaining timeout underflowed or used incompatible clock units");

  Time::Timestamp expiring = 20 * Time::Multiplier::Millisecond;
  passed &= check(mutex.acquire(), "condition-variable-timeout",
                  "the expiring wait mutex could not be acquired");
  error = ConditionVariable::NoError;
  const bool timedWait = condition.wait(mutex, expiring, error);
  const bool mutexHeldAfterTimeout = mutex.isOwnedByCurrentThread();
  mutex.release();

  passed &= check(!timedWait && error == ConditionVariable::TimedOut, "condition-variable-timeout",
                  "an expired wait did not report TimedOut");
  passed &= check(expiring == 0, "condition-variable-timeout",
                  "an expired wait retained a nonzero timeout");
  passed &= check(mutexHeldAfterTimeout, "condition-variable-timeout",
                  "the expired wait did not reacquire its mutex");

  Time::Timestamp immediate = 0;
  passed &= check(mutex.acquire(), "condition-variable-timeout",
                  "the immediate wait mutex could not be acquired");
  const size_t alarmCreatesBeforeImmediate = Time::getHostedAlarmCreateCount();
  g_ZeroTimeoutPublications = 0;
  g_ZeroTimeoutThread = thread;
  WaitQueue::setBeforeBlockHook(observeZeroTimeoutPublication);
  error = ConditionVariable::NoError;
  const bool immediateWait = condition.wait(mutex, immediate, error);
  WaitQueue::setBeforeBlockHook(nullptr);
  g_ZeroTimeoutThread = nullptr;
  const bool mutexHeldAfterImmediate = mutex.isOwnedByCurrentThread();
  mutex.release();

  passed &= check(!immediateWait && error == ConditionVariable::TimedOut && immediate == 0,
                  "condition-variable-timeout", "zero did not request an immediate timeout");
  passed &= check(mutexHeldAfterImmediate, "condition-variable-timeout",
                  "the immediate timeout released its mutex");
  passed &= check(g_ZeroTimeoutPublications == 0 &&
                      Time::getHostedAlarmCreateCount() == alarmCreatesBeforeImmediate,
                  "condition-variable-timeout",
                  "the immediate timeout published a waiter or allocated an alarm");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS condition-variable-timeout");
  }
  return passed;
}

struct MemoryPoolContext {
  explicit MemoryPoolContext(MemoryPool* pool) : pool(pool), entered(0), returned(0), result(0) {}

  MemoryPool* pool;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<uintptr_t> result;
};

int allocateFromExhaustedPool(void* parameter) {
  MemoryPoolContext* context = reinterpret_cast<MemoryPoolContext*>(parameter);
  context->entered += 1;
  context->result = context->pool->allocate();
  context->returned += 1;
  return 0;
}

bool memoryPoolBlockingAndStride() {
  constexpr size_t BufferSize = 512;
  MemoryPool pool("hosted-memory-pool-regression");
  bool passed = true;
  if (!pool.initialise(1, BufferSize)) {
    return check(false, "memory-pool-lifecycle", "a one-page hosted pool could not be initialised");
  }

  const size_t bufferCount = PhysicalMemoryManager::getPageSize() / BufferSize;
  pool.acquireHostedMappingLock();
  const bool compactedWhileMapping = pool.trim();
  pool.releaseHostedMappingLock();
  passed &= check(!compactedWhileMapping, "memory-pool-lifecycle",
                  "memory-pressure compaction waited on its own mapping lock");

  Vector<uintptr_t> buffers;
  for (size_t i = 0; i < bufferCount; ++i) {
    const uintptr_t buffer = pool.allocateNow();
    buffers.pushBack(buffer);
    passed &= check(buffer != 0, "memory-pool-lifecycle",
                    "the pool exhausted before its advertised capacity");
    if (i) {
      passed &= check(buffer == buffers[0] + (i * BufferSize), "memory-pool-lifecycle",
                      "buffers did not use the configured fixed stride");
    }
    passed &= check((buffer % BufferSize) == 0, "memory-pool-lifecycle",
                    "a buffer did not retain its configured alignment");
  }
  passed &= check(pool.allocateNow() == 0, "memory-pool-lifecycle",
                  "nonblocking allocation succeeded after exhaustion");

  MemoryPoolContext context(&pool);
  Thread* waiter = new Thread(Scheduler::instance().getKernelProcess(), allocateFromExhaustedPool,
                              &context, nullptr, false, true);
  waiter->setName("hosted MemoryPool waiter");
  const bool queued = waitUntilQueued(waiter, Thread::CondWait);

  const size_t freedIndex = bufferCount / 2;
  const uintptr_t freed = buffers[freedIndex];
  pool.free(freed);
  const bool joined = waiter->join();
  const uintptr_t reused = context.result;

  passed &= check(context.entered == 1 && queued, "memory-pool-lifecycle",
                  "the exhausted-pool allocation did not block");
  passed &= check(joined && reused == freed, "memory-pool-lifecycle",
                  "free did not wake the waiter with the released buffer");

  for (size_t i = 0; i < buffers.count(); ++i) {
    if (i != freedIndex) {
      pool.free(buffers[i]);
    }
  }
  if (reused) {
    pool.free(reused);
  }

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS memory-pool-lifecycle");
  }
  return passed;
}

bool memoryPoolCloseAndDrain() {
  constexpr size_t BufferSize = 512;
  MemoryPool* pool = new MemoryPool("hosted-memory-pool-close-regression");
  bool passed = true;
  if (!pool->initialise(1, BufferSize)) {
    delete pool;
    return check(false, "memory-pool-close-drain",
                 "a one-page hosted pool could not be initialised");
  }

  const size_t bufferCount = PhysicalMemoryManager::getPageSize() / BufferSize;
  for (size_t i = 0; i < bufferCount; ++i) {
    passed &= check(pool->allocateNow() != 0, "memory-pool-close-drain",
                    "the close regression could not exhaust the pool");
  }

  MemoryPoolContext context(pool);
  Thread* waiter = new Thread(Scheduler::instance().getKernelProcess(), allocateFromExhaustedPool,
                              &context, nullptr, false, true);
  waiter->setName("hosted MemoryPool close waiter");
  const bool queued = waitUntilQueued(waiter, Thread::CondWait);

  // Destruction must wake this already-entered allocation and wait until it
  // has stopped touching the pool's mutex and condition variable.
  delete pool;
  const bool joined = waiter->join();

  passed &= check(context.entered == 1 && queued, "memory-pool-close-drain",
                  "the exhausted allocation was not active before close");
  passed &= check(joined && context.result == 0, "memory-pool-close-drain",
                  "close did not drain the blocked allocation with a null result");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS memory-pool-close-drain");
  }
  return passed;
}

bool memoryPoolTerminalDrain() {
  constexpr size_t BufferSize = 512;
  MemoryPool* pool = new MemoryPool("hosted-memory-pool-terminal-regression");
  bool passed = true;
  if (!pool->initialise(1, BufferSize)) {
    delete pool;
    return check(false, "memory-pool-terminal-drain",
                 "a one-page hosted pool could not be initialised");
  }

  const size_t bufferCount = PhysicalMemoryManager::getPageSize() / BufferSize;
  for (size_t i = 0; i < bufferCount; ++i) {
    passed &= check(pool->allocateNow() != 0, "memory-pool-terminal-drain",
                    "the terminal regression could not exhaust the pool");
  }

  MemoryPoolContext context(pool);
  Thread* waiter = new Thread(Scheduler::instance().getKernelProcess(), allocateFromExhaustedPool,
                              &context, nullptr, false, true);
  waiter->setName("hosted MemoryPool terminal waiter");
  const bool queued = waitUntilQueued(waiter, Thread::CondWait);

  waiter->setUnwindState(Thread::TerminateThread);
  const bool joined = waiter->join();

  // The terminal request returns through the protected operation scope so
  // its ordinary RAII cleanup retires the pool reference before teardown.
  delete pool;

  passed &= check(
      context.entered == 1 && queued && joined && context.returned == 1 && context.result == 0,
      "memory-pool-terminal-drain", "terminal cancellation skipped or corrupted operation cleanup");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS memory-pool-terminal-drain");
  }
  return passed;
}

class CountingPressureHandler : public MemoryPressureHandler {
 public:
  CountingPressureHandler() : calls(0) {}

  const char* getMemoryPressureDescription() override {
    return "hosted registry-reentry peer";
  }

  bool compact() override {
    calls += 1;
    return false;
  }

  Atomic<size_t> calls;
};

class BlockingPressureHandler : public MemoryPressureHandler {
 public:
  BlockingPressureHandler(MemoryPressureManager* manager, MemoryPressureHandler* reentryPeer)
      : entered(0),
        releaseCallback(0),
        calls(0),
        reentries(0),
        manager(manager),
        reentryPeer(reentryPeer) {}

  const char* getMemoryPressureDescription() override {
    return "hosted callback-lifetime regression";
  }

  bool compact() override {
    calls += 1;

    // Registry mutation from inside a callback must not recurse on a lock.
    manager->removeHandler(reentryPeer);
    manager->registerHandler(MemoryPressureManager::LowPriority, reentryPeer);
    reentries += 1;

    entered.release();
    const bool released = releaseCallback.acquireForCompletion();
    assert(released);
    return false;
  }

  Semaphore entered;
  Semaphore releaseCallback;
  Atomic<size_t> calls;
  Atomic<size_t> reentries;
  MemoryPressureManager* manager;
  MemoryPressureHandler* reentryPeer;
};

struct PressureManagerContext {
  PressureManagerContext(MemoryPressureManager* manager, MemoryPressureHandler* handler)
      : manager(manager),
        handler(handler),
        compactEntered(0),
        compactReturned(0),
        removeEntered(0),
        removeReturned(0) {}

  MemoryPressureManager* manager;
  MemoryPressureHandler* handler;
  Atomic<size_t> compactEntered;
  Atomic<size_t> compactReturned;
  Atomic<size_t> removeEntered;
  Atomic<size_t> removeReturned;
};

int compactPressureManager(void* parameter) {
  PressureManagerContext* context = reinterpret_cast<PressureManagerContext*>(parameter);
  context->compactEntered += 1;
  context->manager->compact();
  context->compactReturned += 1;
  return 0;
}

int removePressureHandler(void* parameter) {
  PressureManagerContext* context = reinterpret_cast<PressureManagerContext*>(parameter);
  context->removeEntered += 1;
  context->manager->removeHandler(context->handler);
  context->removeReturned += 1;
  return 0;
}

bool memoryPressureCallbackBarrier() {
  MemoryPressureManager manager;
  CountingPressureHandler reentryPeer;
  BlockingPressureHandler handler(&manager, &reentryPeer);
  PressureManagerContext context(&manager, &handler);
  bool passed = true;

  manager.registerHandler(MemoryPressureManager::LowPriority, &reentryPeer);
  manager.registerHandler(MemoryPressureManager::HighestPriority, &handler);

  Thread* compactor = new Thread(Scheduler::instance().getKernelProcess(), compactPressureManager,
                                 &context, nullptr, false, true);
  compactor->setName("hosted pressure compactor");

  const bool callbackEntered = handler.entered.acquire(1, 0, 500000);
  Thread* remover = new Thread(Scheduler::instance().getKernelProcess(), removePressureHandler,
                               &context, nullptr, false, true);
  remover->setName("hosted pressure remover");
  const Time::Timestamp removerDeadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (context.removeEntered != 1 && Time::getTicks() < removerDeadline) {
    Scheduler::instance().yield();
  }

  Thread* followingCompactor = new Thread(Scheduler::instance().getKernelProcess(),
                                          compactPressureManager, &context, nullptr, false, true);
  followingCompactor->setName("hosted pressure following compactor");
  const Time::Timestamp followerDeadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (context.compactEntered < 2 && Time::getTicks() < followerDeadline) {
    Scheduler::instance().yield();
  }

  bool removerWaitPublished = false;
  bool compactorWaitPublished = false;
  const Time::Timestamp publicationDeadline =
      Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (!(removerWaitPublished && compactorWaitPublished) &&
         Time::getTicks() < publicationDeadline) {
    Thread::WaitDebugInfo waitInfo;
    removerWaitPublished = remover->getStatus() == Thread::Sleeping &&
                           remover->getWaitDebugInfo(waitInfo) && waitInfo.channelOwner == &handler;
    compactorWaitPublished = followingCompactor->getStatus() == Thread::Sleeping &&
                             followingCompactor->getWaitDebugInfo(waitInfo) &&
                             waitInfo.channelOwner == &manager;
    if (!(removerWaitPublished && compactorWaitPublished)) {
      Scheduler::instance().yield();
    }
  }

  passed &=
      check(callbackEntered && context.removeEntered == 1 && context.removeReturned == 0 &&
                handler.reentries == 1 && context.compactEntered == 2 &&
                context.compactReturned == 0 && removerWaitPublished && compactorWaitPublished,
            "memory-pressure-callback-barrier",
            "callback reentry failed or a lifetime wait was not published");

  // Always release the callback before joining, including failure paths.
  handler.releaseCallback.release();
  const bool compactJoined = compactor->join();
  const bool followingCompactJoined = followingCompactor->join();
  const bool removeJoined = remover->join();

  passed &= check(
      compactJoined && followingCompactJoined && removeJoined && context.compactReturned == 2 &&
          context.removeReturned == 1 && handler.calls == 1,
      "memory-pressure-callback-barrier", "the callback and removal did not complete exactly once");

  const size_t peerCallsBefore = reentryPeer.calls;
  passed &=
      check(!manager.compact() && handler.calls == 1 && reentryPeer.calls == peerCallsBefore + 1,
            "memory-pressure-callback-barrier",
            "a removed handler ran again or the reentrant registration was lost");
  manager.removeHandler(&reentryPeer);

  manager.registerHandler(MemoryPressureManager::LowPriority, &reentryPeer);
  const size_t callsBeforeAtomicAttempt = reentryPeer.calls;
  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  const bool atomicCompactResult = manager.compact();
  Processor::setInterrupts(interruptsWereEnabled);
  manager.removeHandler(&reentryPeer);
  passed &= check(!atomicCompactResult && reentryPeer.calls == callsBeforeAtomicAttempt,
                  "memory-pressure-callback-barrier",
                  "an atomic-context pressure pass entered a blocking callback");

  for (size_t i = 0; i < 3; ++i) {
    const size_t callsBefore = reentryPeer.calls;
    manager.registerHandler(MemoryPressureManager::LowPriority, &reentryPeer);
    const bool compactResult = manager.compact();
    manager.removeHandler(&reentryPeer);
    const size_t callsAfterRemoval = reentryPeer.calls;
    const bool emptyCompactResult = manager.compact();

    passed &= check(!compactResult && !emptyCompactResult && callsAfterRemoval == callsBefore + 1 &&
                        reentryPeer.calls == callsAfterRemoval,
                    "memory-pressure-callback-barrier",
                    "repeated register/compact/remove was not stable");
  }

  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "memory-pressure-callback-barrier");
  }
  return passed;
}

struct RingBufferCloseContext {
  enum Operation {
    Read,
    Write,
  };

  RingBufferCloseContext(RingBuffer<char>* buffer, Operation operation)
      : buffer(buffer),
        operation(operation),
        entered(0),
        returned(0),
        succeeded(1),
        error(RingBuffer<char>::NoError) {}

  RingBuffer<char>* buffer;
  Operation operation;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<size_t> succeeded;
  Atomic<size_t> error;
};

int runBlockingRingBufferOperation(void* parameter) {
  RingBufferCloseContext* context = reinterpret_cast<RingBufferCloseContext*>(parameter);
  context->entered += 1;

  if (context->operation == RingBufferCloseContext::Read) {
    char value = 0;
    Time::Timestamp timeout = Time::Infinity;
    RingBuffer<char>::Error error = RingBuffer<char>::NoError;
    context->succeeded = context->buffer->read(value, timeout, error) ? 1 : 0;
    context->error = error;
  } else {
    Time::Timestamp timeout = Time::Infinity;
    context->error = context->buffer->write('b', timeout);
    context->succeeded = context->error == RingBuffer<char>::NoError ? 1 : 0;
  }

  context->returned += 1;
  return 0;
}

bool runRingBufferCloseCase(RingBufferCloseContext::Operation operation, bool fill) {
  RingBuffer<char>* buffer = new RingBuffer<char>(1);
  bool passed = true;
  if (fill) {
    passed &= buffer->write('a') == RingBuffer<char>::NoError;
  }

  RingBufferCloseContext context(buffer, operation);
  Thread* waiter = new Thread(Scheduler::instance().getKernelProcess(),
                              runBlockingRingBufferOperation, &context, nullptr, false, true);
  waiter->setName("hosted RingBuffer close waiter");
  const bool queued = waitUntilQueued(waiter, Thread::CondWait);

  delete buffer;
  const bool joined = waiter->join();
  return passed && context.entered == 1 && queued && joined && context.returned == 1 &&
         context.succeeded == 0 && context.error == RingBuffer<char>::Closed;
}

bool ringBufferCloseAndDrain() {
  bool passed = true;
  passed &=
      check(runRingBufferCloseCase(RingBufferCloseContext::Read, false), "ringbuffer-close-drain",
            "close did not wake and drain a blocked reader with Closed");
  passed &=
      check(runRingBufferCloseCase(RingBufferCloseContext::Write, true), "ringbuffer-close-drain",
            "close did not wake and drain a full-buffer writer with Closed");

  RingBuffer<char>* buffer = new RingBuffer<char>(1);
  Semaphore monitor(0, false);
  buffer->monitor(&monitor);
  delete buffer;
  passed &= check(monitor.tryAcquire(), "ringbuffer-close-drain",
                  "close did not wake a registered readiness monitor");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ringbuffer-close-drain");
  }
  return passed;
}

struct BufferCloseContext {
  enum Operation {
    Read,
    Write,
    CanRead,
    CanWrite,
  };

  BufferCloseContext(Buffer<char>* buffer, Operation operation)
      : buffer(buffer), operation(operation), entered(0), returned(0), result(1) {}

  Buffer<char>* buffer;
  Operation operation;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<size_t> result;
};

struct BufferTryWriteContext {
  explicit BufferTryWriteContext(Buffer<char>* buffer)
      : buffer(buffer), entered(0), returned(0), result(1) {}

  Buffer<char>* buffer;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<size_t> result;
};

int runBufferTryWrite(void* parameter) {
  BufferTryWriteContext* context = reinterpret_cast<BufferTryWriteContext*>(parameter);
  const char input[] = {'a', 'b'};
  context->entered += 1;
  context->result = context->buffer->tryWrite(input, sizeof(input)) ? 1 : 0;
  context->returned += 1;
  return 0;
}

int runBlockingBufferOperation(void* parameter) {
  BufferCloseContext* context = reinterpret_cast<BufferCloseContext*>(parameter);
  context->entered += 1;

  char value = 'x';
  switch (context->operation) {
    case BufferCloseContext::Read:
      context->result = context->buffer->read(&value, 1, true);
      break;
    case BufferCloseContext::Write:
      context->result = context->buffer->write(&value, 1, true);
      break;
    case BufferCloseContext::CanRead:
      context->result = context->buffer->canRead(true) ? 1 : 0;
      break;
    case BufferCloseContext::CanWrite:
      context->result = context->buffer->canWrite(true) ? 1 : 0;
      break;
  }
  context->returned += 1;
  return 0;
}

bool runBufferCloseCase(BufferCloseContext::Operation operation, bool fill) {
  Buffer<char>* buffer = new Buffer<char>(1);
  bool passed = true;
  if (fill) {
    const char initial = 'a';
    passed &= buffer->write(&initial, 1, false) == 1;
  }

  BufferCloseContext context(buffer, operation);
  Thread* waiter = new Thread(Scheduler::instance().getKernelProcess(), runBlockingBufferOperation,
                              &context, nullptr, false, true);
  waiter->setName("hosted Buffer close waiter");
  const bool queued = waitUntilQueued(waiter, Thread::CondWait);

  delete buffer;
  const bool joined = waiter->join();
  return passed && context.entered == 1 && queued && joined && context.returned == 1 &&
         context.result == 0;
}

bool bufferCloseAndDrain() {
  bool passed = true;
  passed &= check(runBufferCloseCase(BufferCloseContext::Read, false), "buffer-close-drain",
                  "close did not drain an already-entered blocking read");
  passed &= check(runBufferCloseCase(BufferCloseContext::Write, true), "buffer-close-drain",
                  "close did not drain an already-entered blocking write");
  passed &= check(runBufferCloseCase(BufferCloseContext::CanRead, false), "buffer-close-drain",
                  "close did not drain an already-entered canRead");
  passed &= check(runBufferCloseCase(BufferCloseContext::CanWrite, true), "buffer-close-drain",
                  "close did not drain an already-entered canWrite");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS buffer-close-drain");
  }
  return passed;
}

bool bufferTryWriteDoesNotWaitForLock() {
  Buffer<char> buffer(2);
  BufferTryWriteContext context(&buffer);
  buffer.acquireHostedOperationLock();
  Thread* writer = new Thread(Scheduler::instance().getKernelProcess(), runBufferTryWrite, &context,
                              nullptr, false, true);
  writer->setName("hosted Buffer try-write contention");

  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (!context.returned && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  const bool returnedWhileLocked = context.entered == 1 && context.returned == 1;
  buffer.releaseHostedOperationLock();
  const bool joined = writer->join();

  const bool passed =
      check(returnedWhileLocked && joined && context.result == 0 && buffer.getDataSize() == 0,
            "buffer-try-write", "tryWrite waited for the lock or changed the buffer on failure");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS buffer-try-write");
  }
  return passed;
}

bool runBufferTerminalCase(BufferCloseContext::Operation operation, bool fill) {
  Buffer<char>* buffer = new Buffer<char>(1);
  bool passed = true;
  if (fill) {
    const char initial = 'a';
    passed &= buffer->write(&initial, 1, false) == 1;
  }

  BufferCloseContext context(buffer, operation);
  Thread* waiter = new Thread(Scheduler::instance().getKernelProcess(), runBlockingBufferOperation,
                              &context, nullptr, false, true);
  waiter->setName("hosted Buffer terminal waiter");
  const bool queued = waitUntilQueued(waiter, Thread::CondWait);

  waiter->setUnwindState(Thread::TerminateThread);
  const bool joined = waiter->join();
  delete buffer;

  return passed && context.entered == 1 && queued && joined && context.returned == 1 &&
         context.result == 0;
}

bool bufferTerminalDrain() {
  bool passed = true;
  passed &= check(runBufferTerminalCase(BufferCloseContext::Read, false), "buffer-terminal-drain",
                  "a terminated blocking read retained an active operation");
  passed &= check(runBufferTerminalCase(BufferCloseContext::Write, true), "buffer-terminal-drain",
                  "a terminated blocking write retained an active operation");
  passed &= check(runBufferTerminalCase(BufferCloseContext::CanRead, false),
                  "buffer-terminal-drain", "a terminated canRead retained an active operation");
  passed &= check(runBufferTerminalCase(BufferCloseContext::CanWrite, true),
                  "buffer-terminal-drain", "a terminated canWrite retained an active operation");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS buffer-terminal-drain");
  }
  return passed;
}

bool terminalOperationAdmissionScope() {
  bool passed = true;

  Buffer<char>* buffer = new Buffer<char>(1);
  BufferCloseContext bufferContext(buffer, BufferCloseContext::Read);
  buffer->acquireHostedOperationLock();
  Thread* bufferWaiter =
      new Thread(Scheduler::instance().getKernelProcess(), runBlockingBufferOperation,
                 &bufferContext, nullptr, false, true);
  bufferWaiter->setName("hosted Buffer operation-admission waiter");
  const bool bufferQueued = waitUntilQueued(bufferWaiter, Thread::SemWait);
  bufferWaiter->setUnwindState(Thread::TerminateThread);
  for (size_t i = 0; i < 8; ++i) {
    Scheduler::instance().yield();
  }
  const bool bufferStayedBlocked = bufferContext.returned == 0;
  buffer->releaseHostedOperationLock();
  const bool bufferJoined = bufferWaiter->join();
  const bool bufferRetired = buffer->getHostedActiveOperationCount() == 0;
  delete buffer;

  passed &= check(bufferQueued && bufferStayedBlocked && bufferJoined &&
                      bufferContext.returned == 1 && bufferContext.result == 0 && bufferRetired,
                  "terminal-operation-admission",
                  "Buffer termination escaped the admission mutex or leaked its pin");

  constexpr size_t BufferSize = 512;
  MemoryPool* pool = new MemoryPool("hosted-terminal-operation-admission");
  if (!pool->initialise(1, BufferSize)) {
    delete pool;
    return check(false, "terminal-operation-admission",
                 "the MemoryPool admission fixture could not initialise");
  }

  const size_t bufferCount = PhysicalMemoryManager::getPageSize() / BufferSize;
  for (size_t i = 0; i < bufferCount; ++i) {
    passed &= pool->allocateNow() != 0;
  }

  MemoryPoolContext poolContext(pool);
  pool->acquireHostedOperationLock();
  Thread* poolWaiter = new Thread(Scheduler::instance().getKernelProcess(),
                                  allocateFromExhaustedPool, &poolContext, nullptr, false, true);
  poolWaiter->setName("hosted MemoryPool operation-admission waiter");
  const bool poolQueued = waitUntilQueued(poolWaiter, Thread::SemWait);
  poolWaiter->setUnwindState(Thread::TerminateThread);
  for (size_t i = 0; i < 8; ++i) {
    Scheduler::instance().yield();
  }
  const bool poolStayedBlocked = poolContext.returned == 0;
  pool->releaseHostedOperationLock();
  const bool poolJoined = poolWaiter->join();
  const bool poolRetired = pool->getHostedActiveOperationCount() == 0;
  delete pool;

  passed &= check(poolQueued && poolStayedBlocked && poolJoined && poolContext.returned == 1 &&
                      poolContext.result == 0 && poolRetired,
                  "terminal-operation-admission",
                  "MemoryPool termination escaped the admission mutex or leaked its pin");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS terminal-operation-admission");
  }
  return passed;
}

struct TerminalTimeoutContext {
  TerminalTimeoutContext(Semaphore* semaphore, ConditionVariable* condition, Mutex* mutex)
      : semaphore(semaphore),
        condition(condition),
        mutex(mutex),
        semaphoreEntered(0),
        semaphoreReturned(0),
        semaphoreInterrupted(0),
        semaphoreDestructed(0),
        conditionEntered(0),
        conditionReturned(0),
        conditionTerminal(0),
        conditionMutexHeld(0),
        conditionDestructed(0),
        delayEntered(0),
        delayReturned(0),
        delayInterrupted(0),
        delayDestructed(0) {}

  Semaphore* semaphore;
  ConditionVariable* condition;
  Mutex* mutex;
  Atomic<size_t> semaphoreEntered;
  Atomic<size_t> semaphoreReturned;
  Atomic<size_t> semaphoreInterrupted;
  Atomic<size_t> semaphoreDestructed;
  Atomic<size_t> conditionEntered;
  Atomic<size_t> conditionReturned;
  Atomic<size_t> conditionTerminal;
  Atomic<size_t> conditionMutexHeld;
  Atomic<size_t> conditionDestructed;
  Atomic<size_t> delayEntered;
  Atomic<size_t> delayReturned;
  Atomic<size_t> delayInterrupted;
  Atomic<size_t> delayDestructed;
};

class TerminalTimeoutStackCanary {
 public:
  explicit TerminalTimeoutStackCanary(Atomic<size_t>* destructed) : m_Destructed(destructed) {}

  ~TerminalTimeoutStackCanary() {
    *m_Destructed += 1;
  }

 private:
  Atomic<size_t>* m_Destructed;
};

int terminalTimedSemaphoreWait(void* parameter) {
  TerminalTimeoutContext* context = reinterpret_cast<TerminalTimeoutContext*>(parameter);
  TerminalTimeoutStackCanary stackCanary(&context->semaphoreDestructed);
  context->semaphoreEntered += 1;
  Semaphore::SemaphoreError error = Semaphore::NoError;
  const bool acquired = context->semaphore->acquireWithError(1, 0, 500000, error);
  context->semaphoreInterrupted =
      !acquired && error == Semaphore::Interrupted ? static_cast<size_t>(1) : 0;
  context->semaphoreReturned += 1;
  return 0;
}

int terminalTimedConditionWait(void* parameter) {
  TerminalTimeoutContext* context = reinterpret_cast<TerminalTimeoutContext*>(parameter);
  TerminalTimeoutStackCanary stackCanary(&context->conditionDestructed);
  context->mutex->acquire();
  context->conditionEntered += 1;
  Time::Timestamp timeout = 500 * Time::Multiplier::Millisecond;
  ConditionVariable::Error error = ConditionVariable::NoError;
  const bool signalled = context->condition->wait(*context->mutex, timeout, error);
  context->conditionTerminal =
      !signalled && error == ConditionVariable::TerminationDeferred ? static_cast<size_t>(1) : 0;
  context->conditionMutexHeld = context->mutex->isOwnedByCurrentThread() ? 1 : 0;
  context->conditionReturned += 1;
  if (context->conditionMutexHeld) {
    context->mutex->release();
  }
  return 0;
}

int terminalDelay(void* parameter) {
  TerminalTimeoutContext* context = reinterpret_cast<TerminalTimeoutContext*>(parameter);
  TerminalTimeoutStackCanary stackCanary(&context->delayDestructed);
  context->delayEntered += 1;
  context->delayInterrupted = Time::delay(500 * Time::Multiplier::Millisecond) ? 0 : 1;
  context->delayReturned += 1;
  return 0;
}

bool terminalTimeoutCleanup() {
  Semaphore semaphore(0);
  ConditionVariable condition;
  Mutex mutex;
  TerminalTimeoutContext context(&semaphore, &condition, &mutex);
  Process* process = Scheduler::instance().getKernelProcess();

  const size_t semaphoreCreates = Semaphore::getHostedTimeoutCreateCount();
  const size_t semaphoreDestroys = Semaphore::getHostedTimeoutDestroyCount();
  const size_t alarmCreates = Time::getHostedAlarmCreateCount();
  const size_t alarmDestroys = Time::getHostedAlarmDestroyCount();
  const Time::Timestamp deadline = Time::getTicks() + (550 * Time::Multiplier::Millisecond);

  Thread* semaphoreWaiter =
      new Thread(process, terminalTimedSemaphoreWait, &context, nullptr, false, true);
  Thread* conditionWaiter =
      new Thread(process, terminalTimedConditionWait, &context, nullptr, false, true);
  Thread* delayWaiter = new Thread(process, terminalDelay, &context, nullptr, false, true);

  const bool semaphoreQueued = waitUntilQueued(semaphoreWaiter, Thread::SemWait);
  const bool conditionQueued = waitUntilQueued(conditionWaiter, Thread::CondWait);
  const bool delayQueued = waitUntilQueued(delayWaiter, Thread::EventWait);

  semaphoreWaiter->setUnwindState(Thread::TerminateThread);
  conditionWaiter->setUnwindState(Thread::TerminateThread);
  delayWaiter->setUnwindState(Thread::TerminateThread);

  const bool semaphoreJoined = semaphoreWaiter->join();
  const bool conditionJoined = conditionWaiter->join();
  const bool delayJoined = delayWaiter->join();

  bool passed = true;
  passed &= check(semaphoreQueued && conditionQueued && delayQueued, "terminal-timeout-cleanup",
                  "not every timed waiter published its wait");
  passed &=
      check(semaphoreJoined && conditionJoined && delayJoined && context.semaphoreEntered == 1 &&
                context.conditionEntered == 1 && context.delayEntered == 1 &&
                context.semaphoreReturned == 1 && context.semaphoreInterrupted == 1 &&
                context.semaphoreDestructed == 1 && context.conditionReturned == 1 &&
                context.conditionTerminal == 1 && context.conditionMutexHeld == 1 &&
                context.conditionDestructed == 1 && context.delayReturned == 1 &&
                context.delayInterrupted == 1 && context.delayDestructed == 1,
            "terminal-timeout-cleanup",
            "a terminal timed wait did not return through ordinary stack cleanup");
  passed &=
      check(Semaphore::getHostedTimeoutCreateCount() == semaphoreCreates + 1 &&
                Semaphore::getHostedTimeoutDestroyCount() == semaphoreDestroys + 1 &&
                Time::getHostedAlarmCreateCount() == alarmCreates + 2 &&
                Time::getHostedAlarmDestroyCount() == alarmDestroys + 2,
            "terminal-timeout-cleanup", "a cancelled timeout event was not destroyed exactly once");

  // Advance beyond every original deadline. A stale timer target would now
  // deliver into a joined and freed Thread; hosted ASan makes that fatal.
  while (Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS terminal-timeout-cleanup");
  }
  return passed;
}
}  // namespace

bool runHostedPrimitiveRegressions(Thread* thread) {
  return radixTreeExportedAbi() && semaphoreDrainAvailable() && completionLifecycle() &&
         terminalCompletionBarrier() && operationBarrierLifecycle() &&
         conditionVariableTimeoutAccounting(thread) && memoryPoolBlockingAndStride() &&
         memoryPoolCloseAndDrain() && memoryPoolTerminalDrain() &&
         memoryPressureCallbackBarrier() && bufferCloseAndDrain() &&
         bufferTryWriteDoesNotWaitForLock() && bufferTerminalDrain() &&
         terminalOperationAdmissionScope() && terminalTimeoutCleanup();
}

bool runHostedRingBufferRegressions() {
  return ringBufferCloseAndDrain();
}
