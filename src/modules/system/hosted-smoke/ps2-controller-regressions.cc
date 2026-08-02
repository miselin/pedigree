/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Ps2CaptureState.h"

namespace
{
bool check(bool condition, const char *test, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
    return false;
}

bool oneShotAdmission()
{
    constexpr const char *Test = "ps2-one-shot-hard-admission";
    Ps2IoAdmissionGate gate;

    const bool first = gate.tryAcquire();
    const bool contended = !gate.tryAcquire();
    const bool remainedOwned = gate.owned();
    gate.release();
    const bool reacquired = gate.tryAcquire();
    gate.release();

    const bool passed = check(
        first && contended && remainedOwned && reacquired && !gate.owned(),
        Test,
        "a failed hard-stage admission changed ownership or required a retry");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS ps2-one-shot-hard-admission");
    }
    return passed;
}

bool captureQueueFidelity()
{
    constexpr const char *Test = "ps2-capture-queue-fidelity";
    Ps2CaptureQueue queue;
    bool passed = true;

    passed &= check(
        queue.pushFromInterrupt(Ps2CapturedByte(0x1E, false)) &&
            queue.pushFromInterrupt(Ps2CapturedByte(0xA5, true)) &&
            queue.pending() == 2,
        Test, "captured keyboard and auxiliary bytes were not retained");

    Ps2CapturedByte record;
    passed &= check(
        queue.pop(record) && record.value == 0x1E && !record.secondPort &&
            queue.pop(record) && record.value == 0xA5 && record.secondPort &&
            !queue.pop(record) && !queue.pending(),
        Test, "capture order or status-based destination identity changed");

    for (size_t i = 0; i < Ps2CaptureQueue::Capacity; ++i)
    {
        passed &= queue.pushFromInterrupt(
            Ps2CapturedByte(static_cast<uint8_t>(i), (i & 1) != 0));
    }
    passed &= check(
        !queue.canPushFromInterrupt() &&
            !queue.pushFromInterrupt(Ps2CapturedByte(0xFF, false)) &&
            queue.pending() == Ps2CaptureQueue::Capacity,
        Test, "a full hard-stage queue overwrote an unread byte");

    for (size_t i = 0; i < Ps2CaptureQueue::Capacity; ++i)
    {
        passed &= queue.pop(record) &&
                  record.value == static_cast<uint8_t>(i) &&
                  record.secondPort == ((i & 1) != 0);
    }
    passed &= check(
        !queue.pending() && queue.canPushFromInterrupt() &&
            queue.pushFromInterrupt(Ps2CapturedByte(0x42, false)) &&
            queue.pop(record) && record.value == 0x42 && !record.secondPort,
        Test, "the bounded queue did not recover after wraparound");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS ps2-capture-queue-fidelity");
    }
    return passed;
}
}  // namespace

bool runHostedPs2ControllerRegressions()
{
    return oneShotAdmission() && captureQueueFidelity();
}
