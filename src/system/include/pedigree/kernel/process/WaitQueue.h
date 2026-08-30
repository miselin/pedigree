/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_WAITQUEUE_H
#define PEDIGREE_KERNEL_PROCESS_WAITQUEUE_H
#include <config.h>

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

class Mutex;
class PerProcessorScheduler;
class Thread;

/**
 * Owns the complete transition between testing a wait predicate and blocking.
 *
 * A Guard serialises the predicate with wakeOne/wakeAll. wait() consumes the
 * Guard and installs a persistent wait record before releasing the queue lock.
 * Consequently, a wake arriving before the scheduler commits Sleeping is
 * retained and causes the scheduler to abort the sleep.
 */
class EXPORTED_PUBLIC WaitQueue {
 private:
  struct Waiter;

 public:
  using StackDiscardCleanup = void (*)(void*);
  using AbandonCallback = StackDiscardCleanup;  // Source compatibility.

  enum class WakeReason {
    Waiting,
    Signalled,
    Event,
    Unwinding,
    Terminating,
    Spurious,
  };

  struct Channel {
    /**
     * A synchronization key: a wake only matches an equal Channel.
     * Diagnostic-only metadata belongs in debugState/debugAddress.
     */
    Channel(const void* owner = nullptr, uintptr_t value = 0) : owner(owner), value(value) {}

    bool operator==(const Channel& other) const {
      return owner == other.owner && value == other.value;
    }

    const void* owner;
    uintptr_t value;
  };

  class EXPORTED_PUBLIC Guard {
   public:
    Guard(Guard&& other);
    ~Guard();

    /**
     * Enrols and blocks the current thread. The Guard is consumed.
     */
    MUST_USE_RESULT WakeReason wait(const Channel& channel = Channel(), size_t debugState = 0,
                                    uintptr_t debugAddress = 0,
                                    StackDiscardCleanup onStackDiscard = nullptr,
                                    void* stackDiscardContext = nullptr);

    /**
     * Waits for an ownership/lifetime barrier which must complete before
     * terminal thread teardown may consume the stack. Terminal requests
     * wake the waiter but are deferred across subsequent enrolment until
     * the caller's predicate is satisfied.
     */
    MUST_USE_RESULT WakeReason waitForCompletion(const Channel& channel = Channel(),
                                                 size_t debugState = 0, uintptr_t debugAddress = 0);

    /**
     * Releases a caller mutex before enrolling and blocking. The mutex is
     * reacquired before every returning outcome, including a pending process
     * or thread exit.
     */
    MUST_USE_RESULT WakeReason waitAndUnlock(Mutex& mutex, const Channel& channel = Channel(),
                                             size_t debugState = 0, uintptr_t debugAddress = 0,
                                             StackDiscardCleanup onStackDiscard = nullptr,
                                             void* stackDiscardContext = nullptr);

    /**
     * Releases a caller mutex while waiting for an ownership/lifetime
     * barrier. Terminal requests and signal events may wake the wait, but
     * neither may abandon reacquisition of the caller mutex. A signal
     * interruption remains visible to the caller after reacquisition.
     */
    MUST_USE_RESULT WakeReason waitAndUnlockForCompletion(Mutex& mutex,
                                                          const Channel& channel = Channel(),
                                                          size_t debugState = 0,
                                                          uintptr_t debugAddress = 0);

    bool wakeOne(WakeReason reason = WakeReason::Signalled, const Channel& channel = Channel());
    size_t wakeAll(WakeReason reason = WakeReason::Signalled, const Channel& channel = Channel());

   private:
    friend class WaitQueue;

    explicit Guard(WaitQueue& queue);
    NOT_COPYABLE_OR_ASSIGNABLE(Guard);

    void queueSchedulerNotification(Waiter* waiter);
    void release();

    WaitQueue* m_Queue;
    bool m_OwnsLock;
    Waiter* m_pFirstReady;
    Waiter* m_pLastReady;
  };

  WaitQueue();
  ~WaitQueue();

  MUST_USE_RESULT Guard acquire() {
    return Guard(*this);
  }

  bool wakeOne(WakeReason reason = WakeReason::Signalled, const Channel& channel = Channel());
  size_t wakeAll(WakeReason reason = WakeReason::Signalled, const Channel& channel = Channel());

  size_t waiterCount();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  using BeforeBlockHook = void (*)(WaitQueue* queue, Thread* thread, const Channel& channel,
                                   size_t debugState);

  static void setBeforeBlockHook(BeforeBlockHook hook);
#endif

 private:
  friend class Thread;

  struct Waiter {
    WaitQueue* loadQueue() const {
      return __atomic_load_n(&queue, __ATOMIC_ACQUIRE);
    }

    void storeQueue(WaitQueue* value) {
      __atomic_store_n(&queue, value, __ATOMIC_RELEASE);
    }

    WakeReason loadReason() const {
      return static_cast<WakeReason>(__atomic_load_n(&reason, __ATOMIC_ACQUIRE));
    }

    void storeReason(WakeReason value) {
      __atomic_store_n(&reason, static_cast<size_t>(value), __ATOMIC_RELEASE);
    }

    bool isQueued() const {
      return __atomic_load_n(&queued, __ATOMIC_ACQUIRE);
    }

    void setQueued(bool value) {
      __atomic_store_n(&queued, value, __ATOMIC_RELEASE);
    }

    Thread* thread = nullptr;
    PerProcessorScheduler* scheduler = nullptr;
    WaitQueue* queue = nullptr;
    Channel channel;
    size_t stateLevel = 0;
    size_t reason = static_cast<size_t>(WakeReason::Waiting);
    bool queued = false;

    // Kept inert so Waiter and Thread::StateLevel retain their exported
    // binary layout.
    StackDiscardCleanup legacyOnAbandon = nullptr;
    void* legacyAbandonContext = nullptr;
    Waiter* previous = nullptr;
    Waiter* next = nullptr;
    Waiter* notificationNext = nullptr;
  };

  WakeReason wait(Guard& guard, Mutex* mutex, const Channel& channel, size_t debugState,
                  uintptr_t debugAddress, bool deferTerminal);
  bool wakeOneLocked(Guard& guard, WakeReason reason, const Channel& channel);
  size_t wakeAllLocked(Guard& guard, WakeReason reason, const Channel& channel);
  bool completeWaiter(Guard& guard, Waiter* waiter, WakeReason reason);
  static void publishReady(Waiter* waiter);
  void removeWaiterLocked(Waiter* waiter);
  void cancel(Waiter* waiter, WakeReason reason);

  Spinlock m_Lock;
  Waiter* m_pFirstWaiter;
  Waiter* m_pLastWaiter;
  size_t m_WaiterCount;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  static BeforeBlockHook m_BeforeBlockHook;
#endif
};

#endif
