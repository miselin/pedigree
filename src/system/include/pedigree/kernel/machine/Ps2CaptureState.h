/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_PS2CAPTURESTATE_H
#define PEDIGREE_KERNEL_MACHINE_PS2CAPTURESTATE_H

#include "pedigree/kernel/processor/types.h"

static_assert(
    __atomic_always_lock_free(sizeof(size_t), nullptr),
    "PS/2 hard-stage queue and admission words must be lock-free");

/** One byte captured from the 8042 output register. */
struct Ps2CapturedByte
{
    Ps2CapturedByte() : value(0), secondPort(false)
    {
    }

    Ps2CapturedByte(uint8_t capturedValue, bool fromSecondPort)
        : value(capturedValue), secondPort(fromSecondPort)
    {
    }

    uint8_t value;
    bool secondPort;
};

/**
 * Fixed single-consumer queue for bytes captured by the bounded PS/2 hard
 * stage. Producers are serialised by Ps2IoAdmissionGate before entering here.
 */
class Ps2CaptureQueue
{
  public:
    static constexpr size_t Capacity = 256;

    Ps2CaptureQueue() : m_Records(), m_WriteSequence(0), m_ReadSequence(0)
    {
    }

    bool tryPush(const Ps2CapturedByte &record)
    {
        const size_t write =
            __atomic_load_n(&m_WriteSequence, __ATOMIC_RELAXED);
        const size_t read = __atomic_load_n(&m_ReadSequence, __ATOMIC_ACQUIRE);
        if ((write - read) >= Capacity)
        {
            return false;
        }

        m_Records[write % Capacity] = record;
        __atomic_store_n(&m_WriteSequence, write + 1, __ATOMIC_RELEASE);
        return true;
    }

    /** Safe preflight while the caller still owns the single-producer gate. */
    bool hasCapacity() const
    {
        const size_t write =
            __atomic_load_n(&m_WriteSequence, __ATOMIC_RELAXED);
        const size_t read = __atomic_load_n(&m_ReadSequence, __ATOMIC_ACQUIRE);
        return (write - read) < Capacity;
    }

    bool pop(Ps2CapturedByte &record)
    {
        const size_t read = __atomic_load_n(&m_ReadSequence, __ATOMIC_RELAXED);
        const size_t write =
            __atomic_load_n(&m_WriteSequence, __ATOMIC_ACQUIRE);
        if (read == write)
        {
            return false;
        }

        record = m_Records[read % Capacity];
        __atomic_store_n(&m_ReadSequence, read + 1, __ATOMIC_RELEASE);
        return true;
    }

    size_t pending() const
    {
        const size_t write =
            __atomic_load_n(&m_WriteSequence, __ATOMIC_ACQUIRE);
        const size_t read = __atomic_load_n(&m_ReadSequence, __ATOMIC_ACQUIRE);
        return write - read;
    }

    /** Only reset while IRQ admission and the consumer are stopped. */
    void reset()
    {
        __atomic_store_n(
            &m_WriteSequence, static_cast<size_t>(0), __ATOMIC_RELEASE);
        __atomic_store_n(
            &m_ReadSequence, static_cast<size_t>(0), __ATOMIC_RELEASE);
    }

  private:
    Ps2CapturedByte m_Records[Capacity];
    size_t m_WriteSequence;
    size_t m_ReadSequence;
};

/** Shared 8042 admission gate; hard IRQ callers attempt exactly one CAS. */
class Ps2IoAdmissionGate
{
  public:
    Ps2IoAdmissionGate() : m_Owned(0)
    {
    }

    bool tryAcquire()
    {
        size_t expected = 0;
        return __atomic_compare_exchange_n(
            &m_Owned, &expected, static_cast<size_t>(1), false,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }

    void release()
    {
        __atomic_store_n(&m_Owned, static_cast<size_t>(0), __ATOMIC_RELEASE);
    }

    bool owned() const
    {
        return __atomic_load_n(&m_Owned, __ATOMIC_ACQUIRE) != 0;
    }

  private:
    size_t m_Owned;
};

#endif
