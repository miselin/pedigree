/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_IRQEVENTCOUNTER_H
#define PEDIGREE_KERNEL_MACHINE_IRQEVENTCOUNTER_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/processor/types.h"

/**
 * Preserves the number of identical events captured by a split IRQ top half.
 *
 * A work bit only says that a bottom half must run. Devices whose elapsed
 * time or completion count matters use this counter alongside that bit so
 * coalescing cannot silently discard occurrences.
 */
class IrqEventCounter
{
  public:
    IrqEventCounter() : m_Count(0)
    {
    }

    /**
     * Records a batch with one bounded atomic operation.
     *
     * Returns false when the exact batch cannot be represented. Callers must
     * treat that loss of fidelity as fatal before assuming the batch was
     * recorded.
     */
    bool recordFromInterrupt(size_t occurrences = 1)
    {
        if (!occurrences)
        {
            return true;
        }

        constexpr size_t Maximum = ~static_cast<size_t>(0);
        const size_t count = m_Count.value();
        if (occurrences > (Maximum - count))
        {
            return false;
        }

        // There is one hard producer. A concurrent consumer can only reset
        // the counter to zero, which creates capacity rather than invalidating
        // this preflight check.
        m_Count += occurrences;
        return true;
    }

    /** Claims every event recorded before this atomic handoff. */
    size_t takeAll()
    {
        size_t count = m_Count.value();
        while (count && !m_Count.compareAndSwap(count, 0))
        {
            count = m_Count.value();
        }
        return count;
    }

    bool pending() const
    {
        return m_Count.value() != 0;
    }

    void reset()
    {
        m_Count = 0;
    }

  private:
    Atomic<size_t> m_Count;

    IrqEventCounter(const IrqEventCounter &) = delete;
    IrqEventCounter &operator=(const IrqEventCounter &) = delete;
};

#endif
