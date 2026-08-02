/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_MACH_PC_LOCALAPICTIMERHANDLERSLOTS_H
#define KERNEL_MACHINE_MACH_PC_LOCALAPICTIMERHANDLERSLOTS_H

#include "pedigree/kernel/processor/ProcessorInformation.h"

class SchedulerTimerHandler;

static_assert(
    __atomic_always_lock_free(sizeof(SchedulerTimerHandler *), nullptr),
    "Local APIC timer-handler slots must be lock-free");

/** Fixed handler publication slots for the 8-bit xAPIC processor envelope. */
class LocalApicTimerHandlerSlots
{
  public:
    static constexpr size_t Capacity = 256;

    LocalApicTimerHandlerSlots() : m_Handlers()
    {
    }

    /** Publish a handler only when the processor slot is currently empty. */
    bool registerHandler(ProcessorId processor, SchedulerTimerHandler *handler)
    {
        if (processor >= Capacity || !handler)
            return false;

        SchedulerTimerHandler *expected = nullptr;
        return __atomic_compare_exchange_n(
            &m_Handlers[processor], &expected, handler, false, __ATOMIC_RELEASE,
            __ATOMIC_RELAXED);
    }

    /**
     * Unpublish only the exact handler currently owned by this processor.
     * This is not a synchronous lifetime drain for cross-CPU removal.
     */
    bool removeHandler(ProcessorId processor, SchedulerTimerHandler *handler)
    {
        if (processor >= Capacity || !handler)
            return false;

        SchedulerTimerHandler *expected = handler;
        return __atomic_compare_exchange_n(
            &m_Handlers[processor], &expected,
            static_cast<SchedulerTimerHandler *>(nullptr), false,
            __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }

    /** Perform the timer interrupt path's single bounded handler load. */
    SchedulerTimerHandler *load(ProcessorId processor) const
    {
        if (processor >= Capacity)
            return nullptr;
        return __atomic_load_n(&m_Handlers[processor], __ATOMIC_ACQUIRE);
    }

  private:
    LocalApicTimerHandlerSlots(const LocalApicTimerHandlerSlots &) = delete;
    LocalApicTimerHandlerSlots &
    operator=(const LocalApicTimerHandlerSlots &) = delete;

    /**
     * Removal only unpublishes the pointer; it does not drain a callback which
     * another CPU has already loaded. LocalApic relies on the scheduler's
     * same-CPU ownership of registration, timer dispatch, and removal for the
     * handler lifetime guarantee.
     */
    SchedulerTimerHandler *m_Handlers[Capacity];
};

#endif
