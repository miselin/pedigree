/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/assert.h"

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
WaitQueue::BeforeBlockHook WaitQueue::m_BeforeBlockHook = nullptr;
#endif

namespace {
WaitQueue::WakeReason terminalWakeReason(Thread::UnwindType state) {
  if (state == Thread::Exit) {
    return WaitQueue::WakeReason::Unwinding;
  }
  if (state == Thread::TerminateThread) {
    return WaitQueue::WakeReason::Terminating;
  }
  FATAL("Unknown terminal WaitQueue unwind state.");
  return WaitQueue::WakeReason::Spurious;
}
}  // namespace

WaitQueue::Guard::Guard(WaitQueue& queue)
    : m_Queue(&queue), m_OwnsLock(false), m_pFirstReady(nullptr), m_pLastReady(nullptr) {
  if (!Processor::guardDeviceHardIrqOperation(DeviceHardIrqOperation::WaitQueueAccess)) {
    return;
  }

  m_Queue->m_Lock.acquire();
  m_OwnsLock = true;
}

WaitQueue::Guard::Guard(Guard&& other)
    : m_Queue(other.m_Queue),
      m_OwnsLock(other.m_OwnsLock),
      m_pFirstReady(other.m_pFirstReady),
      m_pLastReady(other.m_pLastReady) {
  other.m_Queue = nullptr;
  other.m_OwnsLock = false;
  other.m_pFirstReady = nullptr;
  other.m_pLastReady = nullptr;
}

WaitQueue::Guard::~Guard() {
  release();
}

void WaitQueue::Guard::release() {
  if (m_Queue && m_OwnsLock) {
    m_Queue->m_Lock.release();
    m_OwnsLock = false;

    // Ready publication takes scheduler-owned locks. Keep that ordering
    // outside the WaitQueue lock because scheduler lifecycle paths can
    // themselves complete waits.
    while (m_pFirstReady) {
      Waiter* waiter = m_pFirstReady;
      m_pFirstReady = waiter->notificationNext;
      waiter->notificationNext = nullptr;
      WaitQueue::publishReady(waiter);
    }
    m_pLastReady = nullptr;
  }
}

void WaitQueue::Guard::queueSchedulerNotification(Waiter* waiter) {
  assert(waiter);
  assert(!waiter->notificationNext);
  if (m_pLastReady) {
    m_pLastReady->notificationNext = waiter;
  } else {
    m_pFirstReady = waiter;
  }
  m_pLastReady = waiter;
}

WaitQueue::WakeReason WaitQueue::Guard::wait(const Channel& channel, size_t debugState,
                                             uintptr_t debugAddress,
                                             StackDiscardCleanup onStackDiscard,
                                             void* stackDiscardContext) {
  assert(m_Queue);
  if (!m_OwnsLock) {
    return WakeReason::Spurious;
  }
  assert(m_OwnsLock);
  Thread::StackDiscardScope discardScope(onStackDiscard, stackDiscardContext);
  return m_Queue->wait(*this, nullptr, channel, debugState, debugAddress, false);
}

WaitQueue::WakeReason WaitQueue::Guard::waitForCompletion(const Channel& channel, size_t debugState,
                                                          uintptr_t debugAddress) {
  assert(m_Queue);
  if (!m_OwnsLock) {
    return WakeReason::Spurious;
  }
  assert(m_OwnsLock);
  return m_Queue->wait(*this, nullptr, channel, debugState, debugAddress, true);
}

WaitQueue::WakeReason WaitQueue::Guard::waitAndUnlock(Mutex& mutex, const Channel& channel,
                                                      size_t debugState, uintptr_t debugAddress,
                                                      StackDiscardCleanup onStackDiscard,
                                                      void* stackDiscardContext) {
  assert(m_Queue);
  if (!m_OwnsLock) {
    return WakeReason::Spurious;
  }
  assert(m_OwnsLock);
  Thread::StackDiscardScope discardScope(onStackDiscard, stackDiscardContext);
  return m_Queue->wait(*this, &mutex, channel, debugState, debugAddress, false);
}

WaitQueue::WakeReason WaitQueue::Guard::waitAndUnlockForCompletion(Mutex& mutex,
                                                                   const Channel& channel,
                                                                   size_t debugState,
                                                                   uintptr_t debugAddress) {
  assert(m_Queue);
  if (!m_OwnsLock) {
    return WakeReason::Spurious;
  }
  assert(m_OwnsLock);
  return m_Queue->wait(*this, &mutex, channel, debugState, debugAddress, true);
}

bool WaitQueue::Guard::wakeOne(WakeReason reason, const Channel& channel) {
  assert(m_Queue);
  if (!m_OwnsLock) {
    return false;
  }
  assert(m_OwnsLock);
  assert(reason != WakeReason::Waiting);
  return m_Queue->wakeOneLocked(*this, reason, channel);
}

size_t WaitQueue::Guard::wakeAll(WakeReason reason, const Channel& channel) {
  assert(m_Queue);
  if (!m_OwnsLock) {
    return 0;
  }
  assert(m_OwnsLock);
  assert(reason != WakeReason::Waiting);
  return m_Queue->wakeAllLocked(*this, reason, channel);
}

WaitQueue::WaitQueue()
    : m_Lock(false), m_pFirstWaiter(nullptr), m_pLastWaiter(nullptr), m_WaiterCount(0) {}

WaitQueue::~WaitQueue() {
  if (waiterCount()) {
    FATAL("Destroying a WaitQueue with live waiters.");
  }
}

WaitQueue::WakeReason WaitQueue::wait(Guard& guard, Mutex* mutex, const Channel& channel,
                                      size_t debugState, uintptr_t debugAddress,
                                      bool deferTerminal) {
  if (mutex && !mutex->isOwnedByCurrentThread()) {
    FATAL(
        "WaitQueue::waitAndUnlock requires current-thread mutex "
        "ownership");
  }

  Thread* thread = Processor::information().getCurrentThread();
  assert(thread);
  const size_t stateLevel = thread->getStateLevel();
  Waiter& waiter = thread->m_StateLevels[stateLevel].m_Waiter;

  thread->m_Lock.acquire();
  if (waiter.loadQueue()) {
    FATAL("Thread attempted to enter two wait queues at once.");
  }
  thread->clearTerminalWaitCancelledBeforeBlockUnlocked(stateLevel);
  const Thread::UnwindType unwindState = thread->getUnwindState();
  if (unwindState != Thread::Continue && !deferTerminal) {
    thread->m_Lock.release();
    guard.release();
    return terminalWakeReason(unwindState);
  }
  waiter.thread = thread;
  waiter.scheduler = thread->m_pScheduler;
  assert(waiter.scheduler);
  waiter.channel = channel;
  waiter.stateLevel = stateLevel;
  waiter.storeReason(WakeReason::Waiting);
  waiter.setQueued(false);
  waiter.notificationNext = nullptr;
  waiter.previous = m_pLastWaiter;
  waiter.next = nullptr;
  thread->setDebugState(static_cast<Thread::DebugState>(debugState), debugAddress);

  if (m_pLastWaiter) {
    m_pLastWaiter->next = &waiter;
  } else {
    m_pFirstWaiter = &waiter;
  }
  m_pLastWaiter = &waiter;
  ++m_WaiterCount;
  waiter.setQueued(true);

  // Publish only after every field and the queue membership are complete.
  // Debugger snapshots never need to take the target thread's spinlock.
  waiter.storeQueue(this);
  thread->m_Lock.release();

  if (mutex) {
    mutex->release();
  }

  // The persistent waiter is visible before the queue lock is released.
  // Any wake after this point changes waiter.reason, even if the thread has
  // not yet committed the Sleeping state.
  guard.release();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (m_BeforeBlockHook) {
    m_BeforeBlockHook(this, thread, channel, debugState);
  }
#endif

  Processor::information().getScheduler().blockCurrent();

  m_Lock.acquire();
  thread->m_Lock.acquire();
  thread->clearTerminalWaitCancelledBeforeBlockUnlocked(stateLevel);
  if (waiter.loadQueue() == this) {
    // Unpublish before the persistent record can be reused.
    waiter.storeQueue(nullptr);
  }
  WakeReason reason = waiter.loadReason();
  waiter.scheduler = nullptr;
  thread->setDebugState(Thread::None, 0);
  thread->m_Lock.release();
  removeWaiterLocked(&waiter);
  m_Lock.release();

  if (reason == WakeReason::Waiting) {
    reason = WakeReason::Spurious;
  }

  Thread::UnwindType terminalState = thread->getUnwindState();
  if (terminalState != Thread::Continue && !deferTerminal) {
    reason = terminalWakeReason(terminalState);
  }

  // Event handlers may themselves block. Dispatch only after removing the
  // outer wait record so nested event state gets an independent wait.
  // An ordinary wake and an event publication can race. Once the outer wait
  // record is retired, dispatch any event which is now deliverable regardless
  // of which wake reason won.
  if (terminalState == Thread::Continue || deferTerminal) {
    thread->m_StateLevels[stateLevel].m_bDispatchingWaitEvent = true;
    Processor::information().getScheduler().checkEventState(0);
    thread->m_StateLevels[stateLevel].m_bDispatchingWaitEvent = false;
  }

  // An event handler can itself request process termination. Do not enter a
  // fresh external-mutex wait after that terminal decision.
  terminalState = thread->getUnwindState();
  if (terminalState != Thread::Continue && !deferTerminal) {
    reason = terminalWakeReason(terminalState);
  }

  if (mutex) {
    // Event delivery happens before reacquiring the external mutex, avoiding
    // re-entry into a handler that needs the same lock. Reacquisition is
    // itself an ownership barrier: an outer signal/timeout marker is
    // retained, and terminal propagation happens only after mutex ownership
    // has been restored.
    const bool acquired = mutex->acquireForCompletion();
    if (!acquired) {
      FATAL("WaitQueue could not reacquire its caller mutex");
    }

    terminalState = thread->getUnwindState();
    if (terminalState != Thread::Continue && !deferTerminal) {
      reason = terminalWakeReason(terminalState);
    }
  }
  return reason;
}

bool WaitQueue::wakeOne(WakeReason reason, const Channel& channel) {
  Guard guard(*this);
  return guard.wakeOne(reason, channel);
}

size_t WaitQueue::wakeAll(WakeReason reason, const Channel& channel) {
  Guard guard(*this);
  return guard.wakeAll(reason, channel);
}

bool WaitQueue::wakeOneLocked(Guard& guard, WakeReason reason, const Channel& channel) {
  assert(reason == WakeReason::Signalled || reason == WakeReason::Event ||
         reason == WakeReason::Spurious);
  for (Waiter* waiter = m_pFirstWaiter; waiter; waiter = waiter->next) {
    if (!(waiter->channel == channel)) {
      continue;
    }

    if (completeWaiter(guard, waiter, reason)) {
      return true;
    }
  }

  return false;
}

size_t WaitQueue::wakeAllLocked(Guard& guard, WakeReason reason, const Channel& channel) {
  assert(reason == WakeReason::Signalled || reason == WakeReason::Event ||
         reason == WakeReason::Spurious);
  size_t count = 0;
  for (Waiter* waiter = m_pFirstWaiter; waiter; waiter = waiter->next) {
    if (!(waiter->channel == channel)) {
      continue;
    }

    if (completeWaiter(guard, waiter, reason)) {
      ++count;
    }
  }

  return count;
}

bool WaitQueue::completeWaiter(Guard& guard, Waiter* waiter, WakeReason reason) {
  Thread* thread = waiter->thread;
  bool becameReady = false;
  bool completed = false;

  thread->m_Lock.acquire();
  if (thread->m_StateLevels[waiter->stateLevel].m_Waiter.loadQueue() == this &&
      waiter->loadReason() == WakeReason::Waiting) {
    waiter->storeReason(reason);
    completed = true;
    if (thread->m_Status == Thread::Sleeping) {
      thread->m_Status = Thread::Ready;
      becameReady = true;
    }
  }
  thread->m_Lock.release();

  if (becameReady) {
    guard.queueSchedulerNotification(waiter);
  }
  return completed;
}

void WaitQueue::publishReady(Waiter* waiter) {
  assert(waiter);
  Thread* thread = waiter->thread;
  PerProcessorScheduler* scheduler = waiter->scheduler;
  assert(thread);
  assert(scheduler);
  scheduler->publishReadyFromWait(thread);
}

void WaitQueue::removeWaiterLocked(Waiter* waiter) {
  if (!waiter->isQueued()) {
    return;
  }

  if (waiter->previous) {
    waiter->previous->next = waiter->next;
  } else {
    assert(m_pFirstWaiter == waiter);
    m_pFirstWaiter = waiter->next;
  }

  if (waiter->next) {
    waiter->next->previous = waiter->previous;
  } else {
    assert(m_pLastWaiter == waiter);
    m_pLastWaiter = waiter->previous;
  }

  assert(m_WaiterCount);
  --m_WaiterCount;
  waiter->previous = nullptr;
  waiter->next = nullptr;
  waiter->setQueued(false);
}

void WaitQueue::cancel(Waiter* waiter, WakeReason reason) {
  Thread* thread = waiter->thread;
  bool makeReady = false;
  {
    Guard guard(*this);
    if (!guard.m_OwnsLock) {
      return;
    }
    thread->m_Lock.acquire();
    if (waiter->loadQueue() != this) {
      thread->m_Lock.release();
      return;
    }

    removeWaiterLocked(waiter);
    waiter->storeReason(reason);
    makeReady = thread->m_Status == Thread::Sleeping;

    if (reason == WakeReason::Terminating) {
      waiter->storeQueue(nullptr);
      if (!makeReady && thread->m_Status == Thread::Running) {
        // blockCurrent() has not committed Sleeping yet. Keep no
        // linked waiter, but leave an exact one-shot handoff on the
        // waiter's state level. A nested event may have interrupted
        // that level between publication and blockCurrent().
        thread->markTerminalWaitCancelledBeforeBlockUnlocked(waiter->stateLevel);
      }
    }
    thread->m_Lock.release();
  }

  bool becameReady = false;
  if (makeReady) {
    thread->m_Lock.acquire();
    if (thread->m_Status == Thread::Sleeping) {
      thread->m_Status = Thread::Ready;
      becameReady = true;
    }
    thread->m_Lock.release();
  }

  if (becameReady) {
    publishReady(waiter);
  }
}

size_t WaitQueue::waiterCount() {
  Guard guard(*this);
  if (!guard.m_OwnsLock) {
    return 0;
  }
  return m_WaiterCount;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void WaitQueue::setBeforeBlockHook(BeforeBlockHook hook) {
  m_BeforeBlockHook = hook;
}
#endif
