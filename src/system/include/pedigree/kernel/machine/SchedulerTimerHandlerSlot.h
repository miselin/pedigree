/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_SCHEDULERTIMERHANDLERSLOT_H
#define KERNEL_MACHINE_SCHEDULERTIMERHANDLERSLOT_H
#include <config.h>
#include <stddef.h>

class SchedulerTimerHandler;

static_assert(__atomic_always_lock_free(sizeof(SchedulerTimerHandler*), nullptr),
              "scheduler timer-handler publication must be lock-free");
static_assert(__atomic_always_lock_free(sizeof(size_t), nullptr),
              "scheduler timer-handler ownership must be lock-free");

/**
 * Lock-free, processor-owned publication for a hard scheduler-timer callback.
 *
 * A scheduler timer invokes only the handler owned by the processor which
 * received the tick. A callback may context-switch away with its interrupt
 * frame still suspended, so admission is counted rather than exclusive.
 * Removal succeeds only when it can close an idle publication; it never waits
 * on a callback which needs this same processor to run again.
 */
class SchedulerTimerHandlerSlot {
 public:
  static constexpr size_t NoOwner = ~static_cast<size_t>(0);

  /** Scoped proof that one callback body owns this slot's admission. */
  class DispatchGuard {
   public:
    DispatchGuard() : m_Slot(nullptr), m_Handler(nullptr) {}

    ~DispatchGuard() {
      release();
    }

    SchedulerTimerHandler* handler() const {
      return m_Handler;
    }

    /**
     * Releases this admission exactly once.
     *
     * Scheduler frames can be abandoned without running destructors. A
     * Thread state-cleanup record uses this explicit operation in that
     * path; the eventual destructor is then a harmless no-op.
     */
    void release() {
      SchedulerTimerHandlerSlot* slot = m_Slot;
      m_Slot = nullptr;
      m_Handler = nullptr;
      if (slot) {
        slot->completeDispatch();
      }
    }

   private:
    friend class SchedulerTimerHandlerSlot;
    DispatchGuard(const DispatchGuard&) = delete;
    DispatchGuard& operator=(const DispatchGuard&) = delete;

    SchedulerTimerHandlerSlot* m_Slot;
    SchedulerTimerHandler* m_Handler;
  };

  SchedulerTimerHandlerSlot() : m_Handler(nullptr), m_Owner(NoOwner), m_State(Empty) {}

  /** Publish only when the slot has no owner, including no duplicate owner. */
  bool publish(size_t owner, SchedulerTimerHandler* handler) {
    if (!handler || owner == NoOwner)
      return false;

    size_t expected = Empty;
    if (!__atomic_compare_exchange_n(&m_State, &expected, Publishing, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
      return false;
    }

    __atomic_store_n(&m_Owner, owner, __ATOMIC_RELAXED);
    __atomic_store_n(&m_Handler, handler, __ATOMIC_RELEASE);
    __atomic_store_n(&m_State, Published, __ATOMIC_RELEASE);
    return true;
  }

  /**
   * Unpublish only when handler belongs to the calling processor.
   *
   * A true return is a callback-lifetime barrier: this processor cannot
   * concurrently execute its timer hard frame, and future frames reject the
   * closing slot before loading the callback pointer.
   */
  bool unpublish(size_t owner, SchedulerTimerHandler* handler) {
    if (!handler || owner == NoOwner || __atomic_load_n(&m_Owner, __ATOMIC_ACQUIRE) != owner)
      return false;

    if (__atomic_load_n(&m_Handler, __ATOMIC_ACQUIRE) != handler)
      return false;

    // Published with a zero admission count is the only removable state.
    // A suspended timer frame therefore makes removal fail immediately
    // instead of deadlocking its own processor while waiting to drain.
    size_t expected = Published;
    if (!__atomic_compare_exchange_n(&m_State, &expected, Removing, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
      return false;
    }

    __atomic_store_n(&m_Handler, static_cast<SchedulerTimerHandler*>(nullptr), __ATOMIC_RELEASE);
    __atomic_store_n(&m_Owner, NoOwner, __ATOMIC_RELEASE);
    __atomic_store_n(&m_State, Empty, __ATOMIC_RELEASE);
    return true;
  }

  /** Admit one callback and bind its lifetime to the supplied guard. */
  bool beginDispatch(size_t owner, DispatchGuard& guard) {
    if (guard.m_Slot || owner == NoOwner)
      return false;

    if (__atomic_load_n(&m_Owner, __ATOMIC_ACQUIRE) != owner) {
      return false;
    }

    size_t observed = __atomic_load_n(&m_State, __ATOMIC_ACQUIRE);
    while ((observed & ModeMask) == Published) {
      if ((observed & DispatchCountMask) == DispatchCountMask) {
        return false;
      }

      const size_t admitted = observed + DispatchIncrement;
      if (__atomic_compare_exchange_n(&m_State, &observed, admitted, false, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE)) {
        break;
      }
    }
    if ((observed & ModeMask) != Published) {
      return false;
    }

    SchedulerTimerHandler* handler = __atomic_load_n(&m_Handler, __ATOMIC_ACQUIRE);
    if (!handler || __atomic_load_n(&m_Owner, __ATOMIC_ACQUIRE) != owner) {
      completeDispatch();
      return false;
    }

    guard.m_Handler = handler;
    guard.m_Slot = this;
    return true;
  }

  /** Test/diagnostic predicate which never exposes a callback pointer. */
  bool isPublished(size_t owner, SchedulerTimerHandler* handler) const {
    const size_t before = __atomic_load_n(&m_State, __ATOMIC_ACQUIRE);
    if ((before & ModeMask) != Published)
      return false;
    return handler && __atomic_load_n(&m_Owner, __ATOMIC_ACQUIRE) == owner &&
           __atomic_load_n(&m_Handler, __ATOMIC_ACQUIRE) == handler &&
           __atomic_load_n(&m_State, __ATOMIC_ACQUIRE) == before;
  }

  /** Lock-free diagnostic count of admitted, not-yet-returned callbacks. */
  size_t activeDispatches() const {
    const size_t state = __atomic_load_n(&m_State, __ATOMIC_ACQUIRE);
    return (state & ModeMask) == Published ? (state & DispatchCountMask) / DispatchIncrement : 0;
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  /** Hosted lifecycle-test snapshot; production dispatch uses a guard. */
  SchedulerTimerHandler* publishedHandlerForTest(size_t owner) const {
    SchedulerTimerHandler* handler = __atomic_load_n(&m_Handler, __ATOMIC_ACQUIRE);
    return isPublished(owner, handler) ? handler : nullptr;
  }
#endif

 private:
  enum State : size_t {
    Empty,
    Publishing,
    Published,
    Removing,
  };

  static constexpr size_t ModeMask = 3;
  static constexpr size_t DispatchIncrement = ModeMask + 1;
  static constexpr size_t DispatchCountMask = ~ModeMask;

  static_assert(Removing <= ModeMask, "timer slot modes must fit their mask");

  SchedulerTimerHandlerSlot(const SchedulerTimerHandlerSlot&) = delete;
  SchedulerTimerHandlerSlot& operator=(const SchedulerTimerHandlerSlot&) = delete;

  void completeDispatch() {
    // Only a successfully attached guard can call this. Unpublish accepts
    // only Published with a zero count, so the mode cannot change while an
    // admission is live.
    __atomic_fetch_sub(&m_State, DispatchIncrement, __ATOMIC_ACQ_REL);
  }

  SchedulerTimerHandler* m_Handler;
  size_t m_Owner;
  size_t m_State;
};

#endif
