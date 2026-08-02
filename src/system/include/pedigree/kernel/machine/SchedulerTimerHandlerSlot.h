/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_SCHEDULERTIMERHANDLERSLOT_H
#define KERNEL_MACHINE_SCHEDULERTIMERHANDLERSLOT_H

class SchedulerTimerHandler;

static_assert(
    __atomic_always_lock_free(sizeof(SchedulerTimerHandler *), nullptr),
    "scheduler timer-handler publication must be lock-free");

/** Lock-free single-owner publication for a hard scheduler-timer callback. */
class SchedulerTimerHandlerSlot
{
  public:
    SchedulerTimerHandlerSlot() : m_Handler(nullptr)
    {
    }

    /** Publish only when the slot has no owner, including no duplicate owner. */
    bool publish(SchedulerTimerHandler *handler)
    {
        if (!handler)
            return false;

        SchedulerTimerHandler *expected = nullptr;
        return __atomic_compare_exchange_n(
            &m_Handler, &expected, handler, false, __ATOMIC_RELEASE,
            __ATOMIC_RELAXED);
    }

    /** Unpublish only when handler is the slot's exact current owner. */
    bool unpublish(SchedulerTimerHandler *handler)
    {
        if (!handler)
            return false;

        SchedulerTimerHandler *expected = handler;
        return __atomic_compare_exchange_n(
            &m_Handler, &expected,
            static_cast<SchedulerTimerHandler *>(nullptr), false,
            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }

    /** Perform the hard-IRQ path's single bounded handler load. */
    SchedulerTimerHandler *load() const
    {
        return __atomic_load_n(&m_Handler, __ATOMIC_ACQUIRE);
    }

  private:
    SchedulerTimerHandlerSlot(const SchedulerTimerHandlerSlot &) = delete;
    SchedulerTimerHandlerSlot &
    operator=(const SchedulerTimerHandlerSlot &) = delete;

    /**
     * Unpublication is not a cross-CPU callback drain. SchedulerTimer users
     * keep registration, dispatch, and removal on the owning processor.
     */
    SchedulerTimerHandler *m_Handler;
};

#endif
