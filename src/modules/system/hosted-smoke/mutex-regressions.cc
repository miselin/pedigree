/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"

namespace
{
struct MutexOwnershipContext
{
    MutexOwnershipContext(Mutex *mutex, ConditionVariable *condition)
        : mutex(mutex), condition(condition), nonOwnerReleaseRejected(0),
          conditionWaitRejected(0), timedAcquireFinished(0),
          timedAcquireTimedOut(0), timedAcquireSucceeded(0)
    {
    }

    Mutex *mutex;
    ConditionVariable *condition;
    Atomic<size_t> nonOwnerReleaseRejected;
    Atomic<size_t> conditionWaitRejected;
    Atomic<size_t> timedAcquireFinished;
    Atomic<size_t> timedAcquireTimedOut;
    Atomic<size_t> timedAcquireSucceeded;
};

Atomic<size_t> g_AcquireTransitionSeen(0);
Atomic<size_t> g_ReleaseTransitionSeen(0);
Atomic<size_t> g_TransitionInterruptFailures(0);

void mutexTransitionHook(Semaphore::MutexTransitionWindow window)
{
    if (Processor::getInterrupts())
    {
        g_TransitionInterruptFailures += 1;
    }

    if (window == Semaphore::MutexCounterAcquired)
    {
        g_AcquireTransitionSeen += 1;
    }
    else if (window == Semaphore::MutexOwnerReleased)
    {
        g_ReleaseTransitionSeen += 1;
    }
}

bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL mutex-ownership: " << detail);
    return false;
}

int attemptNonOwnerOperations(void *parameter)
{
    MutexOwnershipContext *context =
        reinterpret_cast<MutexOwnershipContext *>(parameter);

    const bool ownedBeforeRelease =
        context->mutex->isOwnedByCurrentThread();
    context->mutex->release();
    if (
        !ownedBeforeRelease && !context->mutex->isOwnedByCurrentThread() &&
        context->mutex->getValue() == 0)
    {
        context->nonOwnerReleaseRejected += 1;
    }

    ConditionVariable::Error error = ConditionVariable::NoError;
    const bool waited =
        context->condition->wait(*context->mutex, error);
    if (
        !waited && error == ConditionVariable::MutexNotLocked &&
        !context->mutex->isOwnedByCurrentThread() &&
        context->mutex->getValue() == 0)
    {
        context->conditionWaitRejected += 1;
    }

    return 0;
}

int attemptTimedMutexAcquire(void *parameter)
{
    MutexOwnershipContext *context =
        reinterpret_cast<MutexOwnershipContext *>(parameter);
    Semaphore::SemaphoreError error = Semaphore::NoError;
    const bool acquired =
        context->mutex->acquireWithError(1, 0, 20000, error);
    if (acquired)
    {
        context->timedAcquireSucceeded += 1;
        context->mutex->release();
    }
    else if (error == Semaphore::TimedOut)
    {
        context->timedAcquireTimedOut += 1;
    }
    context->timedAcquireFinished += 1;
    return 0;
}
}  // namespace

bool runHostedMutexRegressions()
{
    Mutex mutex;
    ConditionVariable condition;
    MutexOwnershipContext context(&mutex, &condition);
    bool passed = true;

    g_AcquireTransitionSeen = 0;
    g_ReleaseTransitionSeen = 0;
    g_TransitionInterruptFailures = 0;

    const bool initialInterruptState = Processor::getInterrupts();
    {
        Spinlock recursiveLock(false, true);
        recursiveLock.acquire(Spinlock::allow_recursion);
        const bool outerDisabled = !Processor::getInterrupts();
        recursiveLock.acquire(Spinlock::allow_recursion);
        const bool nestedDisabled = !Processor::getInterrupts();
        recursiveLock.release();
        const bool innerReleaseKeptDisabled = !Processor::getInterrupts();
        recursiveLock.release();
        passed &= check(
            initialInterruptState && outerDisabled && nestedDisabled &&
                innerReleaseKeptDisabled &&
                Processor::getInterrupts() == initialInterruptState,
            "recursive spinlock acquisition lost the outer interrupt state");
    }

    Semaphore::setMutexTransitionHook(mutexTransitionHook);
    passed &= check(
        mutex.acquire(), "the supervisor could not acquire the mutex");
    Semaphore::setMutexTransitionHook(nullptr);
    passed &= check(
        mutex.isOwnedByCurrentThread() && mutex.getValue() == 0,
        "acquisition did not publish a single current-thread owner");
    passed &= check(
        mutex.getDebugMutexOwner() ==
            Processor::information().getCurrentThread(),
        "the debugger owner snapshot did not identify the mutex owner");
    passed &= check(
        g_AcquireTransitionSeen == 1 &&
            g_TransitionInterruptFailures == 0 &&
            Processor::getInterrupts() == initialInterruptState,
        "acquisition exposed its counter/owner transition to interrupts");

    Thread *peer = new Thread(
        Scheduler::instance().getKernelProcess(),
        attemptNonOwnerOperations, &context, nullptr, false, true);
    peer->setName("hosted mutex ownership regression");

    passed &= check(
        peer->join(), "the non-owner peer could not be joined");
    passed &= check(
        context.nonOwnerReleaseRejected == 1,
        "a non-owner release changed the mutex");
    passed &= check(
        context.conditionWaitRejected == 1,
        "condition wait accepted a mutex owned by another thread");
    passed &= check(
        mutex.isOwnedByCurrentThread() && mutex.getValue() == 0,
        "the non-owner peer disturbed the supervisor's ownership");

    Thread *timedPeer = new Thread(
        Scheduler::instance().getKernelProcess(), attemptTimedMutexAcquire,
        &context, nullptr, false, true);
    timedPeer->setName("hosted timed mutex acquisition regression");

    const Time::Timestamp timedAcquireDeadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (
        !context.timedAcquireFinished &&
        Time::getTicks() < timedAcquireDeadline)
    {
        Scheduler::instance().yield();
    }

    // If the timeout path regresses, release the mutex so the peer and test
    // suite can finish and report the failure instead of hanging indefinitely.
    const bool timedAcquireNeededRescue = !context.timedAcquireFinished;
    if (timedAcquireNeededRescue)
    {
        mutex.release();
    }

    passed &= check(
        timedPeer->join(), "the timed-acquire peer could not be joined");
    const bool timedAcquirePassed =
        !timedAcquireNeededRescue &&
        context.timedAcquireTimedOut == 1 &&
        context.timedAcquireSucceeded == 0;
    passed &= check(
        timedAcquirePassed,
        "a held Mutex ignored its acquisition timeout");
    if (timedAcquirePassed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS hosted-timer-timeout-cleanup");
    }

    if (timedAcquireNeededRescue)
    {
        passed &= check(
            mutex.acquire(),
            "the supervisor could not restore ownership after timeout rescue");
    }
    passed &= check(
        mutex.isOwnedByCurrentThread() && mutex.getValue() == 0,
        "the timed-acquire peer disturbed the supervisor's ownership");

    const bool releaseInterruptState = Processor::getInterrupts();
    Semaphore::setMutexTransitionHook(mutexTransitionHook);
    mutex.release();
    Semaphore::setMutexTransitionHook(nullptr);
    passed &= check(
        !mutex.isOwnedByCurrentThread() && mutex.getValue() == 1 &&
            mutex.getDebugMutexOwner() == nullptr,
        "owner release did not restore one available item");
    passed &= check(
        g_ReleaseTransitionSeen == 1 &&
            g_TransitionInterruptFailures == 0 &&
            Processor::getInterrupts() == releaseInterruptState,
        "release exposed its owner/counter transition to interrupts");

    mutex.release();
    passed &= check(
        !mutex.isOwnedByCurrentThread() && mutex.getValue() == 1,
        "double release changed the mutex's binary count");

    Semaphore counting(1, false);
    const size_t acquireTransitions = g_AcquireTransitionSeen;
    const size_t releaseTransitions = g_ReleaseTransitionSeen;
    const bool countingInterruptState = Processor::getInterrupts();
    Semaphore::setMutexTransitionHook(mutexTransitionHook);
    passed &= check(
        counting.tryAcquire(),
        "the counting-semaphore control acquisition failed");
    counting.release();
    Semaphore::setMutexTransitionHook(nullptr);
    passed &= check(
        g_AcquireTransitionSeen == acquireTransitions &&
            g_ReleaseTransitionSeen == releaseTransitions &&
            Processor::getInterrupts() == countingInterruptState,
        "counting Semaphore entered a Mutex transition window");
    passed &= check(
        Processor::getInterrupts() == initialInterruptState,
        "thread join or mutex teardown lost the caller interrupt state");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS mutex-ownership");
    }
    return passed;
}
