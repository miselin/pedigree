/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "modules/system/usb/UsbPnP.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Ipc.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/UnlikelyLock.h"

bool runHostedRequestQueueRegressions();
bool runHostedSchedulerRegressions();
bool runHostedCacheRegressions();
bool runHostedInputRegressions();
bool runHostedInterruptManagerRegressions();
EXPORTED_PUBLIC bool runHostedFatSectorRegressions();
EXPORTED_PUBLIC bool runHostedRawFsContractRegressions();
EXPORTED_PUBLIC bool runHostedUnixDatagramRegressions();
bool runHostedIrqRegressions();
bool runHostedLifetimeLeaseRegressions();
bool runHostedLogRegressions();
bool runHostedMutexRegressions();
bool runHostedNetworkFilterRegressions();
bool runHostedPageFaultRegressions();
bool runHostedPipeRegressions();
bool runHostedPrimitiveRegressions(Thread *thread);
bool runHostedRingBufferRegressions();
bool runHostedProcessExitRegressions();
bool runHostedPs2MouseRegressions();
bool runHostedSignalInterruptionRegressions(Thread *thread);
bool runHostedSyscallRegressions();
bool runHostedTimerRegressions(Thread *thread);
EXPORTED_PUBLIC bool runHostedUsbSyncOwnershipRegression();

namespace
{
Atomic<size_t> g_ImmediateHookCalls(0);
Atomic<size_t> g_ImmediateHookFailures(0);
WaitQueue *g_ImmediateQueue = nullptr;
WaitQueue::Channel g_ImmediateChannel;
Thread *g_ImmediateWaiter = nullptr;

struct SemaphoreHookContext
{
    explicit SemaphoreHookContext(Semaphore *semaphore)
        : semaphore(semaphore), waiter(nullptr), hookCalls(0), hookFailures(0)
    {
    }

    Semaphore *semaphore;
    Thread *waiter;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookFailures;
};

SemaphoreHookContext *g_SemaphoreContext = nullptr;

struct TerminalCancelOrderContext
{
    TerminalCancelOrderContext()
        : waiter(nullptr), entered(0), abandoned(0), resumed(0),
          callbackBeforeResume(0), hookCalls(0), hookFailures(0)
    {
    }

    WaitQueue queue;
    Thread *waiter;
    Atomic<size_t> entered;
    Atomic<size_t> abandoned;
    Atomic<size_t> resumed;
    Atomic<size_t> callbackBeforeResume;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookFailures;
};

struct TerminalCancelStackState
{
    TerminalCancelOrderContext *context;
    uintptr_t magic;
};

constexpr uintptr_t TerminalCancelStackMagic = 0x43414e43454c;
constexpr uintptr_t TerminalCancelStackAbandoned = 0x4142414e444f4e;
TerminalCancelOrderContext *g_TerminalCancelOrderContext = nullptr;

struct ConditionVariableHookContext
{
    ConditionVariableHookContext(ConditionVariable *condition, Mutex *mutex)
        : condition(condition), mutex(mutex), waiter(nullptr), hookCalls(0),
          hookFailures(0), acquiredMutex(0)
    {
    }

    ConditionVariable *condition;
    Mutex *mutex;
    Thread *waiter;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookFailures;
    Atomic<size_t> acquiredMutex;
};

ConditionVariableHookContext *g_ConditionVariableContext = nullptr;

struct CompletionConditionContext
{
    CompletionConditionContext()
        : waiter(nullptr), gate(0), entered(0), returned(0), predicate(0),
          terminalReported(0), terminalMutexHeld(0), signalPreserved(0)
    {
    }

    Thread *waiter;
    Mutex mutex;
    ConditionVariable condition;
    Semaphore gate;
    Atomic<size_t> entered;
    Atomic<size_t> returned;
    Atomic<size_t> predicate;
    Atomic<size_t> terminalReported;
    Atomic<size_t> terminalMutexHeld;
    Atomic<size_t> signalPreserved;
};

struct ContendedConditionContext
{
    enum Trigger
    {
        Signal,
        Timeout,
    };

    ContendedConditionContext(Thread *waiter, Trigger trigger)
        : waiter(waiter), trigger(trigger), holderAcquired(0),
          conditionWaitPublished(0), triggerSent(0),
          reacquireWaitPublished(0), holderReleased(0)
    {
    }

    Thread *waiter;
    Trigger trigger;
    Mutex mutex;
    ConditionVariable condition;
    Atomic<size_t> holderAcquired;
    Atomic<size_t> conditionWaitPublished;
    Atomic<size_t> triggerSent;
    Atomic<size_t> reacquireWaitPublished;
    Atomic<size_t> holderReleased;
};

struct TerminalConditionContext
{
    TerminalConditionContext()
        : waiter(nullptr), entered(0), waitReturned(0)
    {
    }

    Thread *waiter;
    Mutex mutex;
    ConditionVariable condition;
    Atomic<size_t> entered;
    Atomic<size_t> waitReturned;
};

Atomic<size_t> g_ContendedSignalHandlerCalls(0);

struct UnlikelyLockHookContext
{
    enum HeldMode
    {
        WriterHeld,
        ReaderHeld,
    };

    UnlikelyLockHookContext(UnlikelyLock *lock, HeldMode heldMode)
        : lock(lock), thread(nullptr), heldMode(heldMode), hookCalls(0),
          hookFailures(0), acquired(0), returned(0), signalPreserved(0)
    {
    }

    UnlikelyLock *lock;
    Thread *thread;
    HeldMode heldMode;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookFailures;
    Atomic<size_t> acquired;
    Atomic<size_t> returned;
    Atomic<size_t> signalPreserved;
};

UnlikelyLockHookContext *g_UnlikelyLockContext = nullptr;

struct UnlikelyLockFairnessContext
{
    explicit UnlikelyLockFairnessContext(UnlikelyLock *lock)
        : lock(lock), admissionSequence(0), writerOrder(0),
          writerReturned(0), readersEntered(0)
    {
    }

    UnlikelyLock *lock;
    Atomic<size_t> admissionSequence;
    Atomic<size_t> writerOrder;
    Atomic<size_t> writerReturned;
    Atomic<size_t> readersEntered;
};

bool waitForDebugState(Thread *thread, size_t debugState)
{
    const Time::Timestamp deadline =
        Time::getTicks() + (2 * Time::Multiplier::Second);
    while (Time::getTicks() < deadline)
    {
        Thread::WaitDebugInfo wait = {};
        uintptr_t debugAddress = 0;
        if (
            thread->getWaitDebugInfo(wait) && wait.queued &&
            thread->getDebugState(debugAddress) == debugState)
        {
            return true;
        }
        Scheduler::instance().yield();
    }
    return false;
}

void contendedConditionSignalHandler(size_t)
{
    g_ContendedSignalHandlerCalls += 1;
}

int holdConditionMutex(void *parameter)
{
    ContendedConditionContext *context =
        reinterpret_cast<ContendedConditionContext *>(parameter);
    if (!context->mutex.acquireForCompletion())
    {
        return 1;
    }
    context->holderAcquired += 1;

    if (waitForDebugState(context->waiter, Thread::CondWait))
    {
        context->conditionWaitPublished += 1;
    }

    if (context->trigger == ContendedConditionContext::Signal)
    {
        SignalEvent *event = new SignalEvent(
            reinterpret_cast<uintptr_t>(&contendedConditionSignalHandler),
            10, ~0UL, 0, true, true);
        if (context->waiter->sendEvent(event))
        {
            context->triggerSent += 1;
        }
        else
        {
            delete event;
        }
    }

    const Time::Timestamp holdTime =
        context->trigger == ContendedConditionContext::Signal
            ? 50 * Time::Multiplier::Millisecond
            : 200 * Time::Multiplier::Millisecond;
    Time::delay(holdTime);
    if (waitForDebugState(context->waiter, Thread::SemWait))
    {
        context->reacquireWaitPublished += 1;
    }

    context->mutex.release();
    context->holderReleased += 1;
    return 0;
}

int waitForTerminalCondition(void *parameter)
{
    TerminalConditionContext *context =
        reinterpret_cast<TerminalConditionContext *>(parameter);
    if (!context->mutex.acquireForCompletion())
    {
        return 1;
    }
    context->entered += 1;

    ConditionVariable::Error error = ConditionVariable::NoError;
    const bool waited = context->condition.wait(context->mutex, error);
    (void) waited;
    context->waitReturned += 1;

    if (context->mutex.isOwnedByCurrentThread())
    {
        context->mutex.release();
    }
    return 1;
}

struct ProcessSuspendContext
{
    enum Mode
    {
        ObserveBlock,
        ResumeBeforeBlock,
        EventThenResume,
    };

    ProcessSuspendContext(Process *process, Mode mode)
        : process(process), mode(mode), waiter(nullptr), hookCalls(0),
          hookFailures(0), phase(0), eventWakes(0), resumes(0)
    {
    }

    Process *process;
    Mode mode;
    Thread *waiter;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookFailures;
    Atomic<size_t> phase;
    Atomic<size_t> eventWakes;
    Atomic<size_t> resumes;
};

ProcessSuspendContext *g_ProcessSuspendContext = nullptr;

Atomic<size_t> g_ImmediateThreadExits(0);

struct JoinAbandonmentContext
{
    explicit JoinAbandonmentContext(Semaphore *gate)
        : gate(gate), target(nullptr), targetEntered(0), joinReturned(0),
          joinSucceeded(0)
    {
    }

    Semaphore *gate;
    Thread *target;
    Atomic<size_t> targetEntered;
    Atomic<size_t> joinReturned;
    Atomic<size_t> joinSucceeded;
};

struct JoinPublicationContext
{
    JoinPublicationContext()
        : targetGate(0), target(nullptr), joiner(nullptr), targetEntered(0),
          hookCalls(0), hookFailures(0), detachRejected(0),
          targetReapableInHook(0), joinReturned(0), joinSucceeded(0)
    {
    }

    Semaphore targetGate;
    Thread *target;
    Thread *joiner;
    Atomic<size_t> targetEntered;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookFailures;
    Atomic<size_t> detachRejected;
    Atomic<size_t> targetReapableInHook;
    Atomic<size_t> joinReturned;
    Atomic<size_t> joinSucceeded;
};

JoinPublicationContext *g_JoinPublicationContext = nullptr;

void immediateWakeHook(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    g_ImmediateHookCalls += 1;
    g_ImmediateWaiter = thread;
    if (
        queue != g_ImmediateQueue ||
        !(channel == g_ImmediateChannel) ||
        debugState != Thread::EventWait)
    {
        g_ImmediateHookFailures += 1;
        return;
    }

    Thread::WaitDebugInfo wait = {};
    thread->getLock().acquire();
    const bool waitVisible = thread->getWaitDebugInfo(wait);
    thread->getLock().release();
    if (
        !waitVisible || wait.queue != queue ||
        wait.channelOwner != channel.owner ||
        wait.channelValue != channel.value ||
        wait.reason != WaitQueue::WakeReason::Waiting ||
        wait.stateLevel != thread->getStateLevel() || !wait.queued)
    {
        g_ImmediateHookFailures += 1;
        return;
    }

    if (!queue->wakeOne(WaitQueue::WakeReason::Signalled, channel))
    {
        g_ImmediateHookFailures += 1;
    }
}

void semaphoreReleaseHook(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    SemaphoreHookContext *context = g_SemaphoreContext;
    if (!context)
    {
        return;
    }

    context->hookCalls += 1;
    context->waiter = thread;
    if (
        !queue || channel.owner != context->semaphore || channel.value ||
        debugState != Thread::SemWait)
    {
        context->hookFailures += 1;
    }

    context->semaphore->release();
}

void abandonTerminalCancelWait(void *parameter)
{
    TerminalCancelStackState *state =
        reinterpret_cast<TerminalCancelStackState *>(parameter);
    TerminalCancelOrderContext *context = state->context;
    if (state->magic != TerminalCancelStackMagic)
    {
        context->hookFailures += 1;
        return;
    }

    state->magic = TerminalCancelStackAbandoned;
    context->abandoned += 1;
}

int waitForTerminalCancellation(void *parameter)
{
    TerminalCancelOrderContext *context =
        reinterpret_cast<TerminalCancelOrderContext *>(parameter);
    TerminalCancelStackState stackState = {
        context, TerminalCancelStackMagic};

    context->entered += 1;
    auto guard = context->queue.acquire();
    const WaitQueue::WakeReason reason = guard.wait(
        WaitQueue::Channel(&context->queue), Thread::EventWait,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)),
        abandonTerminalCancelWait, &stackState);

    context->resumed += 1;
    if (
        reason == WaitQueue::WakeReason::Terminating &&
        context->abandoned == 1 &&
        stackState.magic == TerminalCancelStackAbandoned)
    {
        context->callbackBeforeResume += 1;
    }
    return 0;
}

void beforeCancelAbandonHook(Thread *thread)
{
    TerminalCancelOrderContext *context =
        g_TerminalCancelOrderContext;
    if (!context)
    {
        return;
    }

    context->hookCalls += 1;
    if (
        thread != context->waiter || context->abandoned ||
        context->resumed)
    {
        context->hookFailures += 1;
        return;
    }

    // Give a prematurely published Ready target repeated opportunities to
    // resume before its stack-owned abandonment callback.
    for (size_t i = 0; i < 32; ++i)
    {
        Scheduler::instance().yield();
    }
    if (context->resumed)
    {
        context->hookFailures += 1;
    }
}

void conditionVariableSignalHook(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    ConditionVariableHookContext *context = g_ConditionVariableContext;
    if (!context)
    {
        return;
    }

    context->hookCalls += 1;
    context->waiter = thread;
    const bool mutexAcquired = context->mutex->tryAcquire();
    if (
        !queue || channel.owner || channel.value ||
        debugState != Thread::CondWait || !mutexAcquired)
    {
        context->hookFailures += 1;
    }
    else
    {
        context->acquiredMutex += 1;
    }

    // Signal while the caller's mutex is released. If that invariant failed,
    // release its stale hold too so the regression reports instead of hanging.
    context->condition->signal();
    context->mutex->release();
}

void unlikelyLockAdmissionHook(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    UnlikelyLockHookContext *context = g_UnlikelyLockContext;
    if (!context || thread != context->thread)
    {
        return;
    }

    context->hookCalls += 1;
    if (
        !queue || channel.owner || channel.value ||
        debugState != Thread::CondWait)
    {
        context->hookFailures += 1;
        return;
    }

    if (context->heldMode == UnlikelyLockHookContext::WriterHeld)
    {
        context->lock->release();
    }
    else
    {
        context->lock->leave();
    }
}

int waitForTerminalCompletionCondition(void *parameter)
{
    CompletionConditionContext *context =
        reinterpret_cast<CompletionConditionContext *>(parameter);
    TerminationDeferral terminationDeferral;
    if (!context->gate.acquireForCompletion())
    {
        return 1;
    }

    if (!context->mutex.acquireForCompletion())
    {
        return 1;
    }
    context->entered += 1;
    Thread *thread = context->waiter;

    ConditionVariable::Error terminalError = ConditionVariable::NoError;
    const bool terminalWait =
        context->condition.wait(context->mutex, terminalError);
    context->terminalReported =
        !terminalWait &&
                terminalError == ConditionVariable::TerminationDeferred
            ? 1
            : 0;
    context->terminalMutexHeld =
        context->mutex.isOwnedByCurrentThread() ? 1 : 0;
    if (!context->terminalMutexHeld)
    {
        return 1;
    }

    thread->setInterruptionReason(Thread::InterruptedBySignal);
    while (!context->predicate)
    {
        context->condition.waitForCompletion(context->mutex);
    }
    context->signalPreserved =
        thread->getInterruptionReason() == Thread::InterruptedBySignal ? 1 : 0;
    thread->clearInterruption();
    thread->setUnwindState(Thread::Continue);
    context->mutex.release();
    context->returned += 1;
    return 0;
}

int enterUnlikelyLock(void *parameter)
{
    UnlikelyLockHookContext *context =
        reinterpret_cast<UnlikelyLockHookContext *>(parameter);
    Thread *thread = context->thread;
    thread->setInterruptionReason(Thread::InterruptedBySignal);
    context->lock->enter();
    context->acquired += 1;
    context->signalPreserved =
        thread->getInterruptionReason() == Thread::InterruptedBySignal ? 1 : 0;
    thread->clearInterruption();
    context->lock->leave();
    context->returned += 1;
    return 0;
}

int acquireTerminalUnlikelyLock(void *parameter)
{
    UnlikelyLockHookContext *context =
        reinterpret_cast<UnlikelyLockHookContext *>(parameter);
    Thread *thread = context->thread;
    thread->setUnwindState(Thread::TerminateThread);
    context->lock->acquire();
    context->acquired += 1;
    context->lock->release();
    context->returned += 1;
    return 0;
}

int acquireFairnessWriter(void *parameter)
{
    UnlikelyLockFairnessContext *context =
        reinterpret_cast<UnlikelyLockFairnessContext *>(parameter);
    context->lock->acquire();
    context->writerOrder = context->admissionSequence += 1;
    context->lock->release();
    context->writerReturned += 1;
    return 0;
}

int enterFairnessReader(void *parameter)
{
    UnlikelyLockFairnessContext *context =
        reinterpret_cast<UnlikelyLockFairnessContext *>(parameter);
    context->lock->enter();
    context->admissionSequence += 1;
    context->readersEntered += 1;
    context->lock->leave();
    return 0;
}

void processSuspendHook(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    ProcessSuspendContext *context = g_ProcessSuspendContext;
    if (!context || debugState != Thread::ProcessWait)
    {
        return;
    }

    context->hookCalls += 1;
    context->waiter = thread;
    if (!queue || channel.owner || channel.value)
    {
        context->hookFailures += 1;
    }

    switch (context->mode)
    {
        case ProcessSuspendContext::ObserveBlock:
            if (!context->phase.compareAndSwap(0, 1))
            {
                context->hookFailures += 1;
            }
            break;
        case ProcessSuspendContext::ResumeBeforeBlock:
            context->process->resume();
            context->resumes += 1;
            break;
        case ProcessSuspendContext::EventThenResume:
            if (context->hookCalls == 1)
            {
                if (queue->wakeOne(
                        WaitQueue::WakeReason::Event, channel))
                {
                    context->eventWakes += 1;
                }
                else
                {
                    context->hookFailures += 1;
                }
            }
            else if (context->hookCalls == 2)
            {
                context->process->resume();
                context->resumes += 1;
            }
            else
            {
                context->hookFailures += 1;
            }
            break;
    }
}

int immediateThreadExit(void *)
{
    g_ImmediateThreadExits += 1;
    return 0;
}

int blockedJoinTarget(void *parameter)
{
    JoinAbandonmentContext *context =
        reinterpret_cast<JoinAbandonmentContext *>(parameter);
    context->targetEntered += 1;
    return context->gate->acquireForCompletion() ? 0 : 1;
}

int joinTarget(void *parameter)
{
    JoinAbandonmentContext *context =
        reinterpret_cast<JoinAbandonmentContext *>(parameter);
    if (context->target->join())
    {
        context->joinSucceeded += 1;
    }
    context->joinReturned += 1;
    return 0;
}

int blockedJoinPublicationTarget(void *parameter)
{
    JoinPublicationContext *context =
        reinterpret_cast<JoinPublicationContext *>(parameter);
    context->targetEntered += 1;
    return context->targetGate.acquireForCompletion() ? 0 : 1;
}

int joinPublicationTarget(void *parameter)
{
    JoinPublicationContext *context =
        reinterpret_cast<JoinPublicationContext *>(parameter);
    if (context->target->join())
    {
        context->joinSucceeded += 1;
    }
    context->joinReturned += 1;
    return 0;
}

void joinPublicationHook(
    WaitQueue *, Thread *thread, const WaitQueue::Channel &, size_t debugState)
{
    JoinPublicationContext *context =
        __atomic_load_n(&g_JoinPublicationContext, __ATOMIC_ACQUIRE);
    if (!context)
    {
        return;
    }

    context->hookCalls += 1;
    Thread::WaitDebugInfo wait = {};
    if (
        thread != context->joiner || debugState != Thread::Joining ||
        !thread->getWaitDebugInfo(wait) || !wait.queue || !wait.queued ||
        wait.reason != WaitQueue::WakeReason::Waiting)
    {
        context->hookFailures += 1;
    }

    // The exclusive join claim is already published before the waiter. A
    // concurrent detach must not steal ownership of the target.
    if (!context->target->detach())
    {
        context->detachRejected += 1;
    }
    else
    {
        context->hookFailures += 1;
    }

    // Complete the target after join() has published its waiter but before it
    // asks the scheduler to sleep. markReapable() must retain this wake.
    context->targetGate.release();
    constexpr size_t Attempts = 10000;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        if (context->target->isReapableForHostedTest())
        {
            context->targetReapableInHook += 1;
            return;
        }
        Scheduler::instance().yield();
    }
    context->hookFailures += 1;
}

int resumeBlockedProcess(void *parameter)
{
    ProcessSuspendContext *context =
        reinterpret_cast<ProcessSuspendContext *>(parameter);
    while (context->phase < 1)
    {
        Scheduler::instance().yield();
    }

    while (true)
    {
        context->waiter->getLock().acquire();
        const bool sleeping =
            context->waiter->getStatus() == Thread::Sleeping;
        context->waiter->getLock().release();
        if (sleeping)
        {
            break;
        }
        Scheduler::instance().yield();
    }

    context->process->resume();
    context->resumes += 1;
    if (!context->phase.compareAndSwap(1, 2))
    {
        context->hookFailures += 1;
        return 1;
    }
    return 0;
}

struct BlockingContext
{
    BlockingContext()
        : channel(this, 0x57414954), waiter(nullptr), phase(0), hookCalls(0),
          hookFailures(0), workerWoke(0)
    {
    }

    WaitQueue queue;
    WaitQueue::Channel channel;
    Thread *waiter;
    Atomic<size_t> phase;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookFailures;
    Atomic<size_t> workerWoke;
};

BlockingContext *g_BlockingContext = nullptr;

void blockingHook(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    BlockingContext *context = g_BlockingContext;
    if (!context)
    {
        return;
    }

    context->hookCalls += 1;
    context->waiter = thread;
    if (
        queue != &context->queue || !(channel == context->channel) ||
        debugState != Thread::EventWait ||
        !context->phase.compareAndSwap(0, 1))
    {
        context->hookFailures += 1;
    }
}

int blockingWaker(void *parameter)
{
    BlockingContext *context =
        reinterpret_cast<BlockingContext *>(parameter);
    while (context->phase < 1)
    {
        Scheduler::instance().yield();
    }
    // This is deliberately later than the hook: prove the waiter committed
    // Sleeping before issuing the ordinary wake.
    while (true)
    {
        context->waiter->getLock().acquire();
        const bool sleeping =
            context->waiter->getStatus() == Thread::Sleeping;
        context->waiter->getLock().release();

        if (sleeping)
        {
            break;
        }
        Scheduler::instance().yield();
    }
    if (!context->phase.compareAndSwap(1, 2))
    {
        context->hookFailures += 1;
        return 1;
    }

    if (context->queue.wakeOne(
            WaitQueue::WakeReason::Signalled, context->channel))
    {
        context->workerWoke += 1;
    }
    else
    {
        context->hookFailures += 1;
    }

    if (!context->phase.compareAndSwap(2, 3))
    {
        context->hookFailures += 1;
        return 1;
    }
    return 0;
}

bool check(bool condition, const char *test, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR(
        "HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
    return false;
}

bool wakeBeforeBlock()
{
    WaitQueue queue;
    const WaitQueue::Channel channel(&queue, 0x5055424c);

    g_ImmediateQueue = &queue;
    g_ImmediateChannel = channel;
    WaitQueue::setBeforeBlockHook(immediateWakeHook);

    auto guard = queue.acquire();
    const WaitQueue::WakeReason reason = guard.wait(
        channel, Thread::EventWait,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));

    WaitQueue::setBeforeBlockHook(nullptr);
    g_ImmediateQueue = nullptr;

    bool passed = true;
    passed &= check(
        reason == WaitQueue::WakeReason::Signalled, "wake-before-block",
        "wait returned the wrong wake reason");
    passed &= check(
        g_ImmediateHookCalls == 1, "wake-before-block",
        "the pre-block hook did not run exactly once");
    passed &= check(
        g_ImmediateHookFailures == 0, "wake-before-block",
        "the published waiter was not visible to the hook");
    passed &= check(
        queue.waiterCount() == 0, "wake-before-block",
        "the completed waiter remained queued");
    passed &= check(
        g_ImmediateWaiter &&
            g_ImmediateWaiter->getStatus() == Thread::Running,
        "wake-before-block",
        "the scheduler committed a stale Sleeping transition");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS wake-before-block");
    }
    return passed;
}

bool semaphoreReleaseBeforeBlock()
{
    Semaphore semaphore(0);
    SemaphoreHookContext context(&semaphore);
    g_SemaphoreContext = &context;
    WaitQueue::setBeforeBlockHook(semaphoreReleaseHook);

    const bool acquired = semaphore.acquire();

    WaitQueue::setBeforeBlockHook(nullptr);
    g_SemaphoreContext = nullptr;

    bool passed = true;
    passed &= check(
        acquired, "semaphore-pre-block",
        "acquire did not consume the hook release");
    passed &= check(
        context.hookCalls == 1, "semaphore-pre-block",
        "the pre-block hook did not run exactly once");
    passed &= check(
        context.hookFailures == 0, "semaphore-pre-block",
        "the semaphore waiter was not published in the expected state");
    passed &= check(
        context.waiter && context.waiter->getStatus() == Thread::Running,
        "semaphore-pre-block",
        "the scheduler committed Sleeping after the release");
    passed &= check(
        semaphore.getValue() == 0, "semaphore-pre-block",
        "the released semaphore unit was not consumed");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS semaphore-pre-block");
    }
    return passed;
}

bool terminalCancelCallbackOrdering()
{
    TerminalCancelOrderContext context;
    Thread *waiter = new Thread(
        Scheduler::instance().getKernelProcess(),
        waitForTerminalCancellation, &context, nullptr, false, true);
    waiter->setName("hosted terminal cancellation ordering");
    context.waiter = waiter;

    const bool queued =
        waitForDebugState(waiter, Thread::EventWait);
    g_TerminalCancelOrderContext = &context;
    WaitQueue::setBeforeCancelAbandonHook(beforeCancelAbandonHook);
    const bool cancelled =
        WaitQueue::cancelThreadWaitForTest(waiter);
    WaitQueue::setBeforeCancelAbandonHook(nullptr);
    g_TerminalCancelOrderContext = nullptr;
    const bool joined = waiter->join();

    const bool passed = check(
        context.entered == 1 && queued && cancelled && joined &&
            context.hookCalls == 1 && context.hookFailures == 0 &&
            context.abandoned == 1 && context.resumed == 1 &&
            context.callbackBeforeResume == 1,
        "terminal-cancel-callback-order",
        "the target became schedulable before stack abandonment completed");
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "terminal-cancel-callback-order");
    }
    return passed;
}

bool conditionVariableSignalBeforeBlock()
{
    Mutex mutex;
    ConditionVariable condition;
    ConditionVariableHookContext context(&condition, &mutex);

    const bool initiallyAcquired = mutex.acquire();
    g_ConditionVariableContext = &context;
    WaitQueue::setBeforeBlockHook(conditionVariableSignalHook);

    ConditionVariable::Error waitError = ConditionVariable::NoError;
    const bool waitSucceeded = condition.wait(mutex, waitError);

    WaitQueue::setBeforeBlockHook(nullptr);
    g_ConditionVariableContext = nullptr;

    // A failed tryAcquire proves wait() returned with the mutex held. Either
    // way, release the one unit held by this test.
    const bool mutexWasUnlocked = mutex.tryAcquire();
    mutex.release();

    bool passed = true;
    passed &= check(
        initiallyAcquired, "condition-variable-pre-block",
        "the caller could not acquire its mutex");
    passed &= check(
        waitSucceeded && waitError == ConditionVariable::NoError,
        "condition-variable-pre-block",
        "wait did not report a successful signal");
    passed &= check(
        context.hookCalls == 1, "condition-variable-pre-block",
        "the pre-block hook did not run exactly once");
    passed &= check(
        context.hookFailures == 0, "condition-variable-pre-block",
        "the signal window did not release the caller mutex");
    passed &= check(
        context.acquiredMutex == 1, "condition-variable-pre-block",
        "the hook could not acquire the released caller mutex");
    passed &= check(
        !mutexWasUnlocked, "condition-variable-pre-block",
        "wait returned without reacquiring the caller mutex");
    passed &= check(
        context.waiter && context.waiter->getStatus() == Thread::Running,
        "condition-variable-pre-block",
        "the scheduler committed Sleeping after the signal");

    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS condition-variable-pre-block");
    }
    return passed;
}

bool conditionVariableCompletionBarrier()
{
    CompletionConditionContext context;
    Thread *waiter = new Thread(
        Scheduler::instance().getKernelProcess(),
        waitForTerminalCompletionCondition, &context, nullptr, false, true,
        true);
    waiter->setName("hosted terminal-pending CV completion waiter");
    context.waiter = waiter;
    const bool started = waiter->start();

    const bool gateQueued = [&waiter]() {
        for (size_t i = 0; i < 10000; ++i)
        {
            Thread::WaitDebugInfo wait = {};
            uintptr_t debugAddress = 0;
            if (
                waiter->getWaitDebugInfo(wait) && wait.queued &&
                waiter->getDebugState(debugAddress) == Thread::SemWait)
            {
                return true;
            }
            Scheduler::instance().yield();
        }
        return false;
    }();

    waiter->setUnwindState(Thread::TerminateThread);
    context.gate.release();

    const bool conditionQueued = [&waiter]() {
        for (size_t i = 0; i < 10000; ++i)
        {
            Thread::WaitDebugInfo wait = {};
            uintptr_t debugAddress = 0;
            if (
                waiter->getWaitDebugInfo(wait) && wait.queued &&
                waiter->getDebugState(debugAddress) == Thread::CondWait)
            {
                return true;
            }
            Scheduler::instance().yield();
        }
        return false;
    }();

    const bool mutexAcquired = context.mutex.acquireForCompletion();
    context.predicate = 1;
    context.condition.signal();
    if (mutexAcquired)
    {
        context.mutex.release();
    }
    const bool joined = waiter->join();

    const bool passed = check(
        started && gateQueued && conditionQueued && mutexAcquired && joined &&
            context.entered == 1 && context.returned == 1 &&
            context.terminalReported == 1 &&
            context.terminalMutexHeld == 1 &&
            context.signalPreserved == 1,
        "condition-variable-completion-barrier",
        "terminal state escaped the barrier, mutex ownership was lost, or "
        "its signal marker was discarded");
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "condition-variable-completion-barrier");
    }
    return passed;
}

bool conditionVariableContendedReacquire(
    Thread *waiter, ContendedConditionContext::Trigger trigger)
{
    const char *test =
        trigger == ContendedConditionContext::Signal
            ? "condition-variable-contended-signal-reacquire"
            : "condition-variable-contended-timeout-reacquire";
    ContendedConditionContext context(waiter, trigger);

    const bool initiallyAcquired = context.mutex.acquire();
    g_ContendedSignalHandlerCalls = 0;
    Thread *holder = new Thread(
        Scheduler::instance().getKernelProcess(), holdConditionMutex,
        &context, nullptr, false, true);
    holder->setName("hosted contended condition mutex holder");

    ConditionVariable::Error error = ConditionVariable::NoError;
    Time::Timestamp timeout =
        trigger == ContendedConditionContext::Timeout
            ? 100 * Time::Multiplier::Millisecond
            : Time::Infinity;
    const bool waited =
        context.condition.wait(context.mutex, timeout, error);
    const bool mutexHeld = context.mutex.isOwnedByCurrentThread();
    if (mutexHeld)
    {
        context.mutex.release();
    }
    const bool joined = holder->joinForCompletion();

    const bool expectedError =
        trigger == ContendedConditionContext::Signal
            ? error == ConditionVariable::Interrupted
            : error == ConditionVariable::TimedOut;
    bool passed = true;
    passed &= check(
        initiallyAcquired && !waited && expectedError, test,
        "the wait did not report its interruption after mutex reacquisition");
    passed &= check(
        mutexHeld, test,
        "the interrupted wait returned without owning its mutex");
    passed &= check(
        joined && context.holderAcquired == 1 &&
            context.conditionWaitPublished == 1 &&
            context.reacquireWaitPublished == 1 &&
            context.holderReleased == 1,
        test,
        "the contender did not hold the mutex across interruption");
    if (trigger == ContendedConditionContext::Signal)
    {
        passed &= check(
            context.triggerSent == 1 &&
                g_ContendedSignalHandlerCalls == 1,
            test, "the published signal was not delivered exactly once");
    }
    else
    {
        passed &= check(
            context.triggerSent == 0 && timeout == 0, test,
            "the timeout did not expire while the mutex was contended");
    }

    if (passed)
    {
        if (trigger == ContendedConditionContext::Signal)
        {
            NOTICE(
                "HOSTED-WAIT-TEST: PASS "
                "condition-variable-contended-signal-reacquire");
        }
        else
        {
            NOTICE(
                "HOSTED-WAIT-TEST: PASS "
                "condition-variable-contended-timeout-reacquire");
        }
    }
    return passed;
}

bool conditionVariableTerminalReacquire()
{
    TerminalConditionContext context;
    Thread *waiter = new Thread(
        Scheduler::instance().getKernelProcess(), waitForTerminalCondition,
        &context, nullptr, false, true, true);
    waiter->setName("hosted terminal condition mutex waiter");
    context.waiter = waiter;
    const bool started = waiter->start();

    const bool conditionQueued = waitForDebugState(waiter, Thread::CondWait);
    const bool mutexAcquired =
        conditionQueued && context.mutex.acquireForCompletion();
    context.condition.signal();
    const bool reacquireQueued =
        waitForDebugState(waiter, Thread::SemWait);
    waiter->setUnwindState(Thread::TerminateThread);
    if (mutexAcquired)
    {
        context.mutex.release();
    }

    const bool joined = waiter->joinForCompletion();
    const bool mutexRecoverable = context.mutex.tryAcquire();
    if (mutexRecoverable)
    {
        context.mutex.release();
    }

    const bool passed = check(
        started && context.entered == 1 && conditionQueued &&
            mutexAcquired && reacquireQueued && joined &&
            context.waitReturned == 0 && mutexRecoverable,
        "condition-variable-terminal-reacquire",
        "terminal abandonment returned through wait or stranded its mutex");
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "condition-variable-terminal-reacquire");
    }
    return passed;
}

bool unlikelyLockWriterPreference()
{
    UnlikelyLock lock;
    UnlikelyLockFairnessContext context(&lock);
    lock.enter();

    Thread *writer = new Thread(
        Scheduler::instance().getKernelProcess(), acquireFairnessWriter,
        &context, nullptr, false, true);
    writer->setName("hosted queued UnlikelyLock writer");
    const bool writerQueued = waitForDebugState(writer, Thread::CondWait);

    Thread *readerA = new Thread(
        Scheduler::instance().getKernelProcess(), enterFairnessReader,
        &context, nullptr, false, true);
    readerA->setName("hosted later UnlikelyLock reader A");
    Thread *readerB = new Thread(
        Scheduler::instance().getKernelProcess(), enterFairnessReader,
        &context, nullptr, false, true);
    readerB->setName("hosted later UnlikelyLock reader B");
    const bool readerAQueued =
        waitForDebugState(readerA, Thread::CondWait);
    const bool readerBQueued =
        waitForDebugState(readerB, Thread::CondWait);

    lock.leave();
    const bool writerJoined = writer->joinForCompletion();
    const bool readerAJoined = readerA->joinForCompletion();
    const bool readerBJoined = readerB->joinForCompletion();

    return check(
        writerQueued && readerAQueued && readerBQueued && writerJoined &&
            readerAJoined && readerBJoined && context.writerOrder == 1 &&
            context.writerReturned == 1 && context.readersEntered == 2 &&
            context.admissionSequence == 3,
        "unlikely-lock-admission",
        "later readers bypassed a queued writer");
}

bool unlikelyLockAdmission()
{
    UnlikelyLock lock;
    bool passed = true;

    lock.acquire();
    UnlikelyLockHookContext reader(
        &lock, UnlikelyLockHookContext::WriterHeld);
    g_UnlikelyLockContext = &reader;
    WaitQueue::setBeforeBlockHook(unlikelyLockAdmissionHook);
    Thread *readerThread = new Thread(
        Scheduler::instance().getKernelProcess(), enterUnlikelyLock,
        &reader, nullptr, false, true, true);
    readerThread->setName("hosted UnlikelyLock reader");
    reader.thread = readerThread;
    const bool readerStarted = readerThread->start();
    for (size_t i = 0; i < 10000 && reader.hookCalls == 0; ++i)
    {
        Scheduler::instance().yield();
    }
    if (reader.hookCalls)
    {
        WaitQueue::setBeforeBlockHook(nullptr);
        g_UnlikelyLockContext = nullptr;
    }
    const bool readerJoined = readerThread->join();
    WaitQueue::setBeforeBlockHook(nullptr);
    g_UnlikelyLockContext = nullptr;

    passed &= check(
        readerStarted && readerJoined && reader.hookCalls == 1 &&
            reader.hookFailures == 0 && reader.acquired == 1 &&
            reader.returned == 1 && reader.signalPreserved == 1,
        "unlikely-lock-admission",
        "reader admission lost a publication wake or signal marker");

    lock.enter();
    UnlikelyLockHookContext writer(
        &lock, UnlikelyLockHookContext::ReaderHeld);
    g_UnlikelyLockContext = &writer;
    WaitQueue::setBeforeBlockHook(unlikelyLockAdmissionHook);
    Thread *writerThread = new Thread(
        Scheduler::instance().getKernelProcess(),
        acquireTerminalUnlikelyLock, &writer, nullptr, false, true, true);
    writerThread->setName("hosted terminal-pending UnlikelyLock writer");
    writer.thread = writerThread;
    const bool writerStarted = writerThread->start();
    for (size_t i = 0; i < 10000 && writer.hookCalls == 0; ++i)
    {
        Scheduler::instance().yield();
    }
    if (writer.hookCalls)
    {
        WaitQueue::setBeforeBlockHook(nullptr);
        g_UnlikelyLockContext = nullptr;
    }
    const bool writerJoined = writerThread->join();
    WaitQueue::setBeforeBlockHook(nullptr);
    g_UnlikelyLockContext = nullptr;

    passed &= check(
        writerStarted && writerJoined && writer.hookCalls == 1 &&
            writer.hookFailures == 0 && writer.acquired == 1 &&
            writer.returned == 1,
        "unlikely-lock-admission",
        "writer admission abandoned ownership for terminal state");

    passed &= unlikelyLockWriterPreference();

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS unlikely-lock-admission");
    }
    return passed;
}

bool ordinaryBlockAndWake()
{
    BlockingContext context;
    g_BlockingContext = &context;

    Thread *waker = new Thread(
        Scheduler::instance().getKernelProcess(), blockingWaker, &context,
        nullptr, false, true);
    waker->setName("hosted wait regression waker");

    WaitQueue::setBeforeBlockHook(blockingHook);
    auto guard = context.queue.acquire();
    const WaitQueue::WakeReason reason = guard.wait(
        context.channel, Thread::EventWait,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    WaitQueue::setBeforeBlockHook(nullptr);
    g_BlockingContext = nullptr;

    const bool joined = waker->join();

    bool passed = true;
    passed &= check(
        reason == WaitQueue::WakeReason::Signalled, "ordinary-block-wake",
        "wait returned the wrong wake reason");
    passed &= check(
        context.hookCalls == 1, "ordinary-block-wake",
        "the publication hook did not run exactly once");
    passed &= check(
        context.hookFailures == 0, "ordinary-block-wake",
        "the waker did not observe the expected wait state");
    passed &= check(
        context.workerWoke == 1, "ordinary-block-wake",
        "the worker did not wake one published waiter");
    passed &= check(
        context.phase == 3, "ordinary-block-wake",
        "the worker did not complete the blocking sequence");
    passed &= check(
        joined, "ordinary-block-wake", "the waker could not be joined");
    passed &= check(
        context.queue.waiterCount() == 0, "ordinary-block-wake",
        "the completed waiter remained queued");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS ordinary-block-wake");
    }
    return passed;
}

bool processSuspendResume()
{
    Process *process = Scheduler::instance().getKernelProcess();
    bool passed = true;

    // Discard any earlier lifecycle observations before testing exact flags.
    process->hasSuspended();
    process->hasResumed();

    ProcessSuspendContext blocking(
        process, ProcessSuspendContext::ObserveBlock);
    Thread *resumer = new Thread(
        process, resumeBlockedProcess, &blocking, nullptr, false, true);
    resumer->setName("hosted process resume regression");

    g_ProcessSuspendContext = &blocking;
    WaitQueue::setBeforeBlockHook(processSuspendHook);
    process->suspend();
    WaitQueue::setBeforeBlockHook(nullptr);
    g_ProcessSuspendContext = nullptr;

    passed &= check(
        resumer->join(), "process-suspend-resume",
        "the ordinary resumer could not be joined");
    passed &= check(
        blocking.hookCalls == 1 && blocking.hookFailures == 0 &&
            blocking.phase == 2 && blocking.resumes == 1,
        "process-suspend-resume",
        "the ordinary suspend did not block and resume exactly once");
    const bool blockingSuspended = process->hasSuspended();
    const bool blockingResumed = process->hasResumed();
    passed &= check(
        process->getState() == Process::Active &&
            blockingSuspended && blockingResumed,
        "process-suspend-resume",
        "ordinary suspend/resume state or reporting flags were lost");

    ProcessSuspendContext immediate(
        process, ProcessSuspendContext::ResumeBeforeBlock);
    g_ProcessSuspendContext = &immediate;
    WaitQueue::setBeforeBlockHook(processSuspendHook);
    process->suspend();
    WaitQueue::setBeforeBlockHook(nullptr);
    g_ProcessSuspendContext = nullptr;

    passed &= check(
        immediate.hookCalls == 1 && immediate.hookFailures == 0 &&
            immediate.resumes == 1,
        "process-suspend-resume",
        "resume in the publication window was not retained");
    const bool immediateSuspended = process->hasSuspended();
    const bool immediateResumed = process->hasResumed();
    passed &= check(
        process->getState() == Process::Active &&
            immediateSuspended && immediateResumed,
        "process-suspend-resume",
        "pre-block resume state or reporting flags were lost");

    ProcessSuspendContext eventThenResume(
        process, ProcessSuspendContext::EventThenResume);
    g_ProcessSuspendContext = &eventThenResume;
    WaitQueue::setBeforeBlockHook(processSuspendHook);
    process->suspend();
    WaitQueue::setBeforeBlockHook(nullptr);
    g_ProcessSuspendContext = nullptr;

    passed &= check(
        eventThenResume.hookCalls == 2 &&
            eventThenResume.hookFailures == 0 &&
            eventThenResume.eventWakes == 1 &&
            eventThenResume.resumes == 1,
        "process-suspend-resume",
        "an event wake bypassed the Suspended predicate recheck");
    const bool eventSuspended = process->hasSuspended();
    const bool eventResumed = process->hasResumed();
    passed &= check(
        process->getState() == Process::Active &&
            eventSuspended && eventResumed,
        "process-suspend-resume",
        "event/recheck resume state or reporting flags were lost");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS process-suspend-resume");
    }
    return passed;
}

bool prequeuedEventDispatch()
{
    Thread *thread = g_ImmediateWaiter;
    if (!check(
            thread != nullptr, "prequeued-event",
            "the suite did not capture its current thread"))
    {
        return false;
    }

    const bool passed = check(
        thread->runHostedPrequeuedEventRegression(), "prequeued-event",
        "a ready event was not dispatched exactly once without blocking");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS prequeued-event");
    }
    return passed;
}

bool stateLevelPublication()
{
    Thread *thread = g_ImmediateWaiter;
    if (!check(
            thread != nullptr, "state-level-publication",
            "the suite did not capture its current thread"))
    {
        return false;
    }

    const bool passed = check(
        thread->runHostedStatePublicationRegression(),
        "state-level-publication",
        "an event sender observed a partially published state level");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS state-level-publication");
    }
    return passed;
}

bool stateCleanupOrder()
{
    Thread *thread = g_ImmediateWaiter;
    if (!check(
            thread != nullptr, "state-cleanup-order",
            "the suite did not capture its current thread"))
    {
        return false;
    }

    const bool passed = check(
        thread->runHostedStateCleanupRegression(),
        "state-cleanup-order",
        "cleanup was not LIFO, checkpointed, or level-scoped");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS state-cleanup-order");
    }
    return passed;
}

bool activeEventDeliveryLease()
{
    Thread *thread = g_ImmediateWaiter;
    if (!check(
            thread != nullptr, "event-delivery-lease",
            "the suite did not capture its current thread"))
    {
        return false;
    }

    const bool passed = check(
        thread->runHostedEventDeliveryLeaseRegression(),
        "event-delivery-lease",
        "a dequeued Event was not retained through scheduler dispatch");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS event-delivery-lease");
    }
    return passed;
}

bool eventQueueShutdown()
{
    Thread *thread = g_ImmediateWaiter;
    if (!check(
            thread != nullptr, "event-shutdown-drain",
            "the suite did not capture its current thread"))
    {
        return false;
    }

    const bool passed = check(
        thread->runHostedEventShutdownRegression(),
        "event-shutdown-drain",
        "thread shutdown did not reject or drain queued event deliveries");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS event-shutdown-drain");
    }
    return passed;
}

bool immediateExitJoinLifecycle()
{
    constexpr size_t JoinIterations = 4;
    bool passed = true;

    for (size_t iteration = 0; iteration < JoinIterations; ++iteration)
    {
        Thread *thread = new Thread(
            Scheduler::instance().getKernelProcess(), immediateThreadExit,
            nullptr, nullptr, false, true);
        thread->setName("hosted immediate-exit join regression");

        while (g_ImmediateThreadExits < (iteration + 1))
        {
            Scheduler::instance().yield();
        }

        // The entry point has returned. join() may now race only the
        // scheduler's final stack handoff and reapable publication.
        passed &= check(
            thread->join(), "thread-join-lifecycle",
            "an immediate-exit thread could not be joined");
    }

    passed &= check(
        g_ImmediateThreadExits == JoinIterations,
        "thread-join-lifecycle",
        "the repeated immediate-exit entries did not all run");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS thread-join-lifecycle");
    }
    return passed;
}

bool joinPublicationAndDetachExclusion()
{
    Process *process = Scheduler::instance().getKernelProcess();
    JoinPublicationContext context;
    bool passed = true;

    context.target = new Thread(
        process, blockedJoinPublicationTarget, &context, nullptr, false,
        true);
    context.target->setName("hosted join publication target");

    constexpr size_t Attempts = 10000;
    bool targetBlocked = false;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        if (
            context.targetEntered == 1 &&
            waitForDebugState(context.target, Thread::SemWait))
        {
            targetBlocked = true;
            break;
        }
        Scheduler::instance().yield();
    }
    passed &= check(
        targetBlocked, "thread-join-pre-block-wake",
        "the target did not publish its completion gate");
    if (!targetBlocked)
    {
        context.targetGate.release();
        context.target->join();
        return false;
    }

    context.joiner = new Thread(
        process, joinPublicationTarget, &context, nullptr, false, true,
        true);
    context.joiner->setName("hosted exact-window joiner");

    __atomic_store_n(
        &g_JoinPublicationContext, &context, __ATOMIC_RELEASE);
    WaitQueue::setBeforeBlockHook(joinPublicationHook);
    const bool joinerStarted = context.joiner->start();

    bool joinerCompleted = false;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        if (context.joinReturned == 1)
        {
            joinerCompleted = true;
            break;
        }
        Scheduler::instance().yield();
    }
    WaitQueue::setBeforeBlockHook(nullptr);
    __atomic_store_n(
        &g_JoinPublicationContext,
        static_cast<JoinPublicationContext *>(nullptr), __ATOMIC_RELEASE);

    const bool joinerJoined = context.joiner->join();
    passed &= check(
        joinerStarted && joinerCompleted && joinerJoined &&
            context.hookCalls == 1 && context.hookFailures == 0 &&
            context.targetReapableInHook == 1 &&
            context.joinSucceeded == 1,
        "thread-join-pre-block-wake",
        "a target wake in join's published-before-sleep window was lost");
    passed &= check(
        context.detachRejected == 1, "thread-join-detach-exclusion",
        "detach stole a target after an exclusive join claim was published");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS thread-join-pre-block-wake");
        NOTICE("HOSTED-WAIT-TEST: PASS thread-join-detach-exclusion");
    }
    return passed;
}

bool terminalJoinAbandonment()
{
    Process *process = Scheduler::instance().getKernelProcess();
    Semaphore targetGate(0);
    JoinAbandonmentContext context(&targetGate);
    bool passed = true;

    context.target = new Thread(
        process, blockedJoinTarget, &context, nullptr, false, true);
    context.target->setName("hosted join-abandonment target");

    while (context.targetEntered != 1)
    {
        Scheduler::instance().yield();
    }

    Thread *abandonedJoiner = new Thread(
        process, joinTarget, &context, nullptr, false, true);
    abandonedJoiner->setName("hosted terminal join-abandonment waiter");

    bool enrolled = false;
    constexpr size_t EnrolmentAttempts = 10000;
    for (size_t attempt = 0; attempt < EnrolmentAttempts; ++attempt)
    {
        Thread::WaitDebugInfo info = {};
        uintptr_t debugAddress = 0;
        if (
            abandonedJoiner->getWaitDebugInfo(info) && info.queue &&
            info.queued && abandonedJoiner->getDebugState(debugAddress) ==
                               Thread::Joining &&
            abandonedJoiner->getStatus() == Thread::Sleeping)
        {
            enrolled = true;
            break;
        }
        Scheduler::instance().yield();
    }
    passed &= check(
        enrolled, "join-terminal-abandonment",
        "the first joiner did not publish its join wait");

    abandonedJoiner->setUnwindState(Thread::TerminateThread);
    passed &= check(
        abandonedJoiner->join(), "join-terminal-abandonment",
        "the terminated joiner did not become reapable");
    passed &= check(
        context.joinReturned == 0 && context.joinSucceeded == 0,
        "join-terminal-abandonment",
        "terminal wait abandonment incorrectly returned through join()");

    Thread *replacementJoiner = new Thread(
        process, joinTarget, &context, nullptr, false, true);
    replacementJoiner->setName("hosted replacement join waiter");

    bool replacementEnrolled = false;
    for (size_t attempt = 0; attempt < EnrolmentAttempts; ++attempt)
    {
        Thread::WaitDebugInfo info = {};
        if (
            replacementJoiner->getWaitDebugInfo(info) && info.queue &&
            info.queued &&
            replacementJoiner->getStatus() == Thread::Sleeping)
        {
            replacementEnrolled = true;
            break;
        }
        Scheduler::instance().yield();
    }
    passed &= check(
        replacementEnrolled, "join-terminal-abandonment",
        "the abandoned exclusive join claim blocked a replacement joiner");

    targetGate.release();
    passed &= check(
        replacementJoiner->join(), "join-terminal-abandonment",
        "the replacement joiner did not become reapable");
    passed &= check(
        context.joinReturned == 1 && context.joinSucceeded == 1,
        "join-terminal-abandonment",
        "the replacement join did not consume the released claim");

    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS join-terminal-abandonment");
    }
    return passed;
}

}  // namespace

bool runHostedWaitRegressions()
{
    NOTICE("HOSTED-WAIT-TEST: BEGIN");

    const bool passed =
        wakeBeforeBlock() && semaphoreReleaseBeforeBlock() &&
        terminalCancelCallbackOrdering() &&
        conditionVariableSignalBeforeBlock() &&
        runHostedRingBufferRegressions() &&
        Scheduler::instance()
            .getBootstrapProcessorScheduler()
            ->runHostedNewThreadWorkerRegressions() &&
        runHostedSchedulerRegressions() &&
        runHostedInterruptManagerRegressions() &&
        runHostedIrqRegressions() &&
        runHostedInputRegressions() &&
        runHostedNetworkFilterRegressions() &&
        runHostedCacheRegressions() &&
        runHostedFatSectorRegressions() &&
        runHostedRawFsContractRegressions() &&
        runHostedUnixDatagramRegressions() &&
        runHostedLifetimeLeaseRegressions() &&
        runHostedLogRegressions() &&
        runHostedProcessExitRegressions() &&
        runHostedPs2MouseRegressions() &&
        conditionVariableContendedReacquire(
            g_ImmediateWaiter, ContendedConditionContext::Signal) &&
        conditionVariableContendedReacquire(
            g_ImmediateWaiter, ContendedConditionContext::Timeout) &&
        conditionVariableTerminalReacquire() &&
        conditionVariableCompletionBarrier() &&
        unlikelyLockAdmission() &&
        runHostedMutexRegressions() &&
        runHostedPageFaultRegressions() &&
        runHostedPipeRegressions() &&
        runHostedUsbSyncOwnershipRegression() &&
        UsbPnP::runHostedRegistrationRegression() &&
        Ipc::runHostedIpcInterruptionRegression() &&
        runHostedTimerRegressions(g_ImmediateWaiter) &&
        runHostedPrimitiveRegressions(g_ImmediateWaiter) &&
        runHostedSignalInterruptionRegressions(g_ImmediateWaiter) &&
        runHostedSyscallRegressions() &&
        ordinaryBlockAndWake() &&
        processSuspendResume() && runHostedRequestQueueRegressions() &&
        immediateExitJoinLifecycle() && joinPublicationAndDetachExclusion() &&
        terminalJoinAbandonment() &&
        prequeuedEventDispatch() && stateLevelPublication() &&
        stateCleanupOrder() &&
        activeEventDeliveryLease() && eventQueueShutdown();
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS all");
    }
    else
    {
        ERROR("HOSTED-WAIT-TEST: FAIL suite");
    }
    return passed;
}
