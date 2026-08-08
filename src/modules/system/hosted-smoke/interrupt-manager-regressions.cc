/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/InterruptHandler.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "system/kernel/core/processor/hosted/InterruptManager.h"

#include <signal.h>

namespace
{
constexpr size_t TestSignal = SIGWINCH;

bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR(
        "HOSTED-WAIT-TEST: FAIL interrupt-manager-lock-independent: "
        << detail);
    return false;
}

class CountingHandler : public InterruptHandler
{
  public:
    explicit CountingHandler(size_t identity)
        : m_Identity(identity), calls(0), observedIdentity(0),
          observedSignal(0), observedContext(
                                 static_cast<size_t>(
                                     ExecutionContext::AtomicThread))
    {
    }

    void interrupt(size_t interruptNumber, InterruptState &) override
    {
        observedIdentity = m_Identity;
        observedSignal = interruptNumber;
        observedContext = static_cast<size_t>(Processor::executionContext());
        calls += 1;
    }

    const size_t m_Identity;
    Atomic<size_t> calls;
    Atomic<size_t> observedIdentity;
    Atomic<size_t> observedSignal;
    Atomic<size_t> observedContext;
};

Atomic<size_t> g_MutationHookCalls(0);
Atomic<size_t> g_RaiseSucceeded(0);

void dispatchWhileMutationLocked()
{
    g_MutationHookCalls += 1;
    if (__pedigree_hosted::raise(TestSignal) == 0)
    {
        // raise() returns only after the synchronous production dispatch path
        // has returned, so reaching here proves it did not wait on m_Lock.
        g_RaiseSucceeded += 1;
    }
}
}  // namespace

bool runHostedInterruptManagerRegressions()
{
    InterruptManager &manager = InterruptManager::instance();
    CountingHandler first(1);
    CountingHandler second(2);

    g_MutationHookCalls = 0;
    g_RaiseSucceeded = 0;

    const bool firstInstalled =
        manager.registerInterruptHandler(TestSignal, &first);
    const bool duplicateRejected =
        !manager.registerInterruptHandler(TestSignal, &second);
    if (firstInstalled)
    {
        HostedInterruptManager::withMutationLockForTest(
            dispatchWhileMutationLocked);
    }

    const bool firstRemoved =
        firstInstalled && manager.registerInterruptHandler(TestSignal, nullptr);
    const bool emptyRemovalRejected =
        firstRemoved && !manager.registerInterruptHandler(TestSignal, nullptr);
    const bool secondInstalled =
        firstRemoved && manager.registerInterruptHandler(TestSignal, &second);
    if (secondInstalled)
    {
        HostedInterruptManager::withMutationLockForTest(
            dispatchWhileMutationLocked);
    }
    const bool secondRemoved =
        secondInstalled &&
        manager.registerInterruptHandler(TestSignal, nullptr);
    const bool restoredToWaitable =
        Processor::executionContext() == ExecutionContext::WaitableThread;

    bool passed = true;
    passed &= check(
        firstInstalled && duplicateRejected && firstRemoved &&
            emptyRemovalRejected && secondInstalled && secondRemoved,
        "install/remove exclusivity changed");
    passed &= check(
        g_MutationHookCalls == 2 && g_RaiseSucceeded == 2,
        "production dispatch did not return while the mutation lock was held");
    passed &= check(
        first.calls == 1 && first.observedIdentity == 1 &&
            first.observedSignal == TestSignal && second.calls == 1 &&
            second.observedIdentity == 2 && second.observedSignal == TestSignal,
        "dispatch observed a stale or invalid handler publication");
    passed &= check(
        first.observedContext ==
                static_cast<size_t>(ExecutionContext::HostedSyntheticIrq) &&
            second.observedContext ==
                static_cast<size_t>(ExecutionContext::HostedSyntheticIrq) &&
            restoredToWaitable,
        "hosted signal context leaked or was not classified");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS interrupt-manager-lock-independent");
    }
    return passed;
}
