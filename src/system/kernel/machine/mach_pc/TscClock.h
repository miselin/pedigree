/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_MACH_PC_TSCCLOCK_H
#define PEDIGREE_KERNEL_MACHINE_MACH_PC_TSCCLOCK_H

#include "pedigree/kernel/processor/types.h"

static_assert(
    __atomic_always_lock_free(sizeof(uint64_t), nullptr),
    "the PC monotonic clock requires lock-free 64-bit publication");

namespace PcTscClock
{
constexpr uint64_t MaximumTimestamp = ~static_cast<uint64_t>(0);

struct Calibration
{
    constexpr Calibration(uint64_t cycles = 1, uint64_t nanoseconds = 1)
        : cycles(cycles), nanoseconds(nanoseconds)
    {
    }

    uint64_t cycles;
    uint64_t nanoseconds;
};

inline uint64_t scale(uint64_t cycles, const Calibration &calibration)
{
    if (!calibration.cycles)
    {
        return 0;
    }

    const unsigned __int128 scaled =
        static_cast<unsigned __int128>(cycles) * calibration.nanoseconds;
    const unsigned __int128 nanoseconds = scaled / calibration.cycles;
    return nanoseconds > MaximumTimestamp ? MaximumTimestamp :
                                            static_cast<uint64_t>(nanoseconds);
}

inline uint64_t saturatingAdd(uint64_t first, uint64_t second)
{
    return first > (MaximumTimestamp - second) ? MaximumTimestamp :
                                                 first + second;
}

inline uint64_t fromAnchor(
    uint64_t currentTsc, uint64_t anchorTsc, uint64_t anchorNanoseconds,
    const Calibration &calibration)
{
    if (currentTsc < anchorTsc)
    {
        return anchorNanoseconds;
    }

    return saturatingAdd(
        anchorNanoseconds, scale(currentTsc - anchorTsc, calibration));
}

constexpr uint64_t
publicationResult(uint64_t candidate, uint64_t observed, bool exchanged)
{
    return exchanged ? candidate : observed;
}

/**
 * Publishes a global monotonic floor with at most one compare-exchange.
 *
 * A failed exchange returns the value which won the race. The unpublished
 * candidate is sampled again by the next clock read rather than making a hard
 * interrupt spin on a contended cache line.
 */
class MonotonicPublication
{
  public:
    explicit MonotonicPublication(uint64_t value = 0) : m_Value(value)
    {
    }

    uint64_t publish(uint64_t candidate)
    {
        uint64_t observed = __atomic_load_n(&m_Value, __ATOMIC_ACQUIRE);
        if (candidate <= observed)
        {
            return observed;
        }

        const bool exchanged = __atomic_compare_exchange_n(
            &m_Value, &observed, candidate, false, __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE);
        // On failure compare_exchange replaces observed with the winner.
        return publicationResult(candidate, observed, exchanged);
    }

    uint64_t value() const
    {
        return __atomic_load_n(&m_Value, __ATOMIC_ACQUIRE);
    }

    void reset(uint64_t value = 0)
    {
        __atomic_store_n(&m_Value, value, __ATOMIC_RELEASE);
    }

  private:
    uint64_t m_Value;

    MonotonicPublication(const MonotonicPublication &) = delete;
    MonotonicPublication &operator=(const MonotonicPublication &) = delete;
};
}  // namespace PcTscClock

#endif
