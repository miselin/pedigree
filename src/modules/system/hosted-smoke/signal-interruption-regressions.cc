/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/RingBuffer.h"

namespace
{
constexpr size_t HostedSignalNumber = 10;

Atomic<size_t> g_SignalHandlerCalls(0);
Atomic<size_t> g_MonitorEventDestructions(0);

void hostedSignalHandler(size_t)
{
    g_SignalHandlerCalls += 1;
}

class HostedMonitorEvent : public Event
{
  public:
    HostedMonitorEvent()
        : Event(
              reinterpret_cast<uintptr_t>(&hostedSignalHandler),
              false)
    {
    }

    ~HostedMonitorEvent() override
    {
        g_MonitorEventDestructions += 1;
    }

    size_t serialize(uint8_t *) override
    {
        return 0;
    }

    size_t getNumber() override
    {
        return 0x4d4f4e49;
    }
};

bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR(
        "HOSTED-WAIT-TEST: FAIL signal-interruption: "
        << detail);
    return false;
}

bool waitUntilQueued(Thread *thread, size_t debugState)
{
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (Time::getTicks() < deadline)
    {
        Thread::WaitDebugInfo info = {};
        uintptr_t debugAddress = 0;
        if (
            thread->getWaitDebugInfo(info) && info.queue && info.queued &&
            thread->getDebugState(debugAddress) == debugState)
        {
            return true;
        }
        Scheduler::instance().yield();
    }
    return false;
}

struct SignalContext
{
    SignalContext(
        Thread *target, size_t debugState,
        Semaphore *releaseAfterDelivery = nullptr)
        : target(target), debugState(debugState),
          releaseAfterDelivery(releaseAfterDelivery), published(0), sent(0),
          released(0)
    {
    }

    Thread *target;
    size_t debugState;
    Semaphore *releaseAfterDelivery;
    Atomic<size_t> published;
    Atomic<size_t> sent;
    Atomic<size_t> released;
};

struct SemaphoreWakeCollisionContext
{
    SemaphoreWakeCollisionContext()
        : semaphore(0), waiter(nullptr), entered(0), returned(0), acquired(0),
          error(Semaphore::NoError), interruption(Thread::NotInterrupted),
          rescueWaits(0)
    {
    }

    Semaphore semaphore;
    Thread *waiter;
    Atomic<size_t> entered;
    Atomic<size_t> returned;
    Atomic<size_t> acquired;
    Atomic<size_t> error;
    Atomic<size_t> interruption;
    Atomic<size_t> rescueWaits;
};

SemaphoreWakeCollisionContext *g_SemaphoreWakeCollision = nullptr;

struct ConditionWakeCollisionContext
{
    ConditionWakeCollisionContext()
        : waiter(nullptr), predicate(0), entered(0), returned(0), waits(0),
          lastResult(0), error(ConditionVariable::NoError), rescueWaits(0)
    {
    }

    Mutex mutex;
    ConditionVariable condition;
    Thread *waiter;
    Atomic<size_t> predicate;
    Atomic<size_t> entered;
    Atomic<size_t> returned;
    Atomic<size_t> waits;
    Atomic<size_t> lastResult;
    Atomic<size_t> error;
    Atomic<size_t> rescueWaits;
};

ConditionWakeCollisionContext *g_ConditionWakeCollision = nullptr;

struct EventDrainContext
{
    explicit EventDrainContext(Event *event)
        : event(event), entered(0), completed(0)
    {
    }

    Event *event;
    Atomic<size_t> entered;
    Atomic<size_t> completed;
};

int drainEventRegistrations(void *parameter)
{
    EventDrainContext *context =
        reinterpret_cast<EventDrainContext *>(parameter);
    context->entered += 1;
    context->event->waitForDeliveries();
    context->completed += 1;
    return 0;
}

int interruptPublishedWait(void *parameter)
{
    SignalContext *context =
        reinterpret_cast<SignalContext *>(parameter);
    if (waitUntilQueued(context->target, context->debugState))
    {
        context->published += 1;
    }

    SignalEvent *event = new SignalEvent(
        reinterpret_cast<uintptr_t>(&hostedSignalHandler),
        HostedSignalNumber, ~0UL, 0, true, true);
    if (context->target->sendEvent(event))
    {
        context->sent += 1;
    }

    if (context->releaseAfterDelivery)
    {
        const Time::Timestamp deadline =
            Time::getTicks() + (500 * Time::Multiplier::Millisecond);
        while (!g_SignalHandlerCalls && Time::getTicks() < deadline)
        {
            Scheduler::instance().yield();
        }
        context->releaseAfterDelivery->release();
        context->released += 1;
    }
    return 0;
}

int waitForSemaphoreWakeCollision(void *parameter)
{
    SemaphoreWakeCollisionContext *context =
        reinterpret_cast<SemaphoreWakeCollisionContext *>(parameter);
    context->entered += 1;

    Semaphore::SemaphoreError error = Semaphore::NoError;
    const bool acquired =
        context->semaphore.acquireWithError(1, 0, 0, error);
    context->acquired = acquired ? 1 : 0;
    context->error = static_cast<size_t>(error);

    Thread *thread = Processor::information().getCurrentThread();
    context->interruption =
        static_cast<size_t>(thread->getInterruptionReason());
    thread->clearInterruption();
    context->returned += 1;
    return 0;
}

void semaphoreWakeCollisionRescue(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    (void) queue;
    SemaphoreWakeCollisionContext *context =
        g_SemaphoreWakeCollision;
    if (
        !context || thread != context->waiter ||
        channel.owner != &context->semaphore || channel.value ||
        debugState != Thread::SemWait)
    {
        return;
    }

    context->rescueWaits += 1;
    context->semaphore.release();
}

int waitForConditionWakeCollision(void *parameter)
{
    ConditionWakeCollisionContext *context =
        reinterpret_cast<ConditionWakeCollisionContext *>(parameter);
    if (!context->mutex.acquireForCompletion())
    {
        return 1;
    }
    context->entered += 1;

    ConditionVariable::Error error = ConditionVariable::NoError;
    bool result = true;
    while (!context->predicate && result)
    {
        context->waits += 1;
        result = context->condition.wait(context->mutex, error);
    }
    context->lastResult = result ? 1 : 0;
    context->error = static_cast<size_t>(error);
    context->mutex.release();
    context->returned += 1;
    return 0;
}

void conditionWakeCollisionRescue(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    (void) queue;
    ConditionWakeCollisionContext *context =
        g_ConditionWakeCollision;
    if (
        !context || thread != context->waiter || channel.owner ||
        channel.value || debugState != Thread::CondWait)
    {
        return;
    }

    context->rescueWaits += 1;
    if (context->mutex.acquireForCompletion())
    {
        context->predicate = 1;
        context->condition.signal();
        context->mutex.release();
    }
}

Thread *startInterrupter(SignalContext &context)
{
    Thread *thread = new Thread(
        Scheduler::instance().getKernelProcess(),
        interruptPublishedWait, &context, nullptr, false, true);
    thread->setName("hosted signal interrupter");
    return thread;
}

bool conditionVariableSignalInterruption(Thread *thread)
{
    ConditionVariable condition;
    Mutex mutex;
    SignalContext context(thread, Thread::CondWait);

    if (!mutex.acquire())
    {
        return check(
            false, "the ConditionVariable mutex was unavailable");
    }
    bool passed = true;
    g_SignalHandlerCalls = 0;
    Thread *interrupter = startInterrupter(context);

    ConditionVariable::Error error = ConditionVariable::NoError;
    const bool waited = condition.wait(mutex, error);
    const bool mutexHeld = mutex.isOwnedByCurrentThread();
    if (mutexHeld)
    {
        mutex.release();
    }
    const bool joined = interrupter->join();

    passed &= check(
        !waited && error == ConditionVariable::Interrupted,
        "ConditionVariable did not report Interrupted");
    passed &= check(
        mutexHeld, "ConditionVariable did not reacquire its mutex");
    passed &= check(
        joined && context.published == 1 && context.sent == 1 &&
            g_SignalHandlerCalls == 1,
        "ConditionVariable wait did not receive one published signal");
    return passed;
}

bool bufferSignalInterruption(Thread *thread)
{
    Buffer<char> buffer(8);
    SignalContext context(thread, Thread::CondWait);

    thread->clearInterruption();
    g_SignalHandlerCalls = 0;
    Thread *interrupter = startInterrupter(context);

    char value = 0;
    const size_t read = buffer.read(&value, 1, true);
    const Thread::InterruptionReason reason =
        thread->getInterruptionReason();
    thread->clearInterruption();
    const bool joined = interrupter->join();

    return check(
        read == 0 && reason == Thread::InterruptedBySignal &&
            joined && context.published == 1 && context.sent == 1 &&
            g_SignalHandlerCalls == 1,
        "Buffer did not preserve its signal interruption");
}

bool semaphoreSignalInterruption(Thread *thread)
{
    Semaphore semaphore(0);
    SignalContext context(thread, Thread::SemWait);

    thread->clearInterruption();
    g_SignalHandlerCalls = 0;
    Thread *interrupter = startInterrupter(context);

    Semaphore::SemaphoreError error = Semaphore::NoError;
    const bool acquired =
        semaphore.acquireWithError(1, 0, 0, error);
    const Thread::InterruptionReason reason =
        thread->getInterruptionReason();
    thread->clearInterruption();
    const bool joined = interrupter->join();

    return check(
        !acquired && error == Semaphore::Interrupted &&
            reason == Thread::InterruptedBySignal &&
            joined && context.published == 1 && context.sent == 1 &&
            g_SignalHandlerCalls == 1,
        "Semaphore did not preserve its signal interruption");
}

bool semaphoreSignalAfterOrdinaryWake()
{
    SemaphoreWakeCollisionContext context;
    Thread *waiter = new Thread(
        Scheduler::instance().getKernelProcess(),
        waitForSemaphoreWakeCollision, &context, nullptr, false, true);
    waiter->setName("hosted semaphore wake/signal collision");
    context.waiter = waiter;

    const bool queued = waitUntilQueued(waiter, Thread::SemWait);
    g_SignalHandlerCalls = 0;
    g_SemaphoreWakeCollision = &context;
    WaitQueue::setBeforeBlockHook(semaphoreWakeCollisionRescue);

    // Win waiter.reason with an ordinary release, then make that release
    // unavailable and publish a signal before the waiter can run.
    context.semaphore.release();
    const bool releaseConsumed = context.semaphore.tryAcquire();
    SignalEvent *event = new SignalEvent(
        reinterpret_cast<uintptr_t>(&hostedSignalHandler),
        HostedSignalNumber, ~0UL, 0, true, true);
    const bool sent = waiter->sendEvent(event);
    if (!sent)
    {
        delete event;
    }

    const bool joined = waiter->join();
    WaitQueue::setBeforeBlockHook(nullptr);
    g_SemaphoreWakeCollision = nullptr;

    const bool passed = check(
        queued && releaseConsumed && sent && joined &&
            context.entered == 1 && context.returned == 1 &&
            context.acquired == 0 &&
            context.error == Semaphore::Interrupted &&
            context.interruption == Thread::InterruptedBySignal &&
            context.rescueWaits == 0 && g_SignalHandlerCalls == 1,
        "Semaphore re-blocked after a signal lost the waiter.reason race");
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "semaphore-signal-after-ordinary-wake");
    }
    return passed;
}

bool conditionSignalAfterOrdinaryWake()
{
    ConditionWakeCollisionContext context;
    Thread *waiter = new Thread(
        Scheduler::instance().getKernelProcess(),
        waitForConditionWakeCollision, &context, nullptr, false, true);
    waiter->setName("hosted condition wake/signal collision");
    context.waiter = waiter;

    const bool queued = waitUntilQueued(waiter, Thread::CondWait);
    g_SignalHandlerCalls = 0;
    g_ConditionWakeCollision = &context;
    WaitQueue::setBeforeBlockHook(conditionWakeCollisionRescue);

    // The condition signal deliberately does not satisfy the predicate. A
    // signal event published after this wake must still become the result.
    context.condition.signal();
    SignalEvent *event = new SignalEvent(
        reinterpret_cast<uintptr_t>(&hostedSignalHandler),
        HostedSignalNumber, ~0UL, 0, true, true);
    const bool sent = waiter->sendEvent(event);
    if (!sent)
    {
        delete event;
    }

    const bool joined = waiter->join();
    WaitQueue::setBeforeBlockHook(nullptr);
    g_ConditionWakeCollision = nullptr;

    const bool passed = check(
        queued && sent && joined && context.entered == 1 &&
            context.returned == 1 && context.waits == 1 &&
            context.lastResult == 0 &&
            context.error == ConditionVariable::Interrupted &&
            context.rescueWaits == 0 && g_SignalHandlerCalls == 1,
        "ConditionVariable re-blocked after a signal lost the waiter.reason race");
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "condition-signal-after-ordinary-wake");
    }
    return passed;
}

bool completionSemaphoreSignalDeferral(Thread *thread)
{
    Semaphore semaphore(0);
    SignalContext context(thread, Thread::SemWait, &semaphore);

    thread->clearInterruption();
    g_SignalHandlerCalls = 0;
    Thread *interrupter = startInterrupter(context);

    const bool acquired = semaphore.acquireForCompletion();
    const Thread::InterruptionReason reason =
        thread->getInterruptionReason();
    thread->clearInterruption();
    const bool joined = interrupter->join();

    return check(
        acquired && reason == Thread::InterruptedBySignal &&
            joined && context.published == 1 && context.sent == 1 &&
            context.released == 1 && g_SignalHandlerCalls == 1 &&
            semaphore.getValue() == 0,
        "a completion semaphore returned before its signal-delayed release");
}

bool ringBufferSignalInterruption(Thread *thread)
{
    RingBuffer<char> buffer(1);
    SignalContext context(thread, Thread::CondWait);

    thread->clearInterruption();
    g_SignalHandlerCalls = 0;
    Thread *interrupter = startInterrupter(context);

    Time::Timestamp timeout = Time::Infinity;
    char value = 0;
    RingBuffer<char>::Error error = RingBuffer<char>::NoError;
    const bool read = buffer.read(value, timeout, error);
    const Thread::InterruptionReason reason =
        thread->getInterruptionReason();
    thread->clearInterruption();
    const bool joined = interrupter->join();

    return check(
        !read && error == RingBuffer<char>::Interrupted &&
            reason == Thread::InterruptedBySignal &&
            joined && context.published == 1 && context.sent == 1 &&
            g_SignalHandlerCalls == 1,
        "RingBuffer did not preserve its signal interruption");
}

bool ringBufferMonitorCull(Thread *thread)
{
    RingBuffer<char> buffer(1);
    SignalEvent event(
        reinterpret_cast<uintptr_t>(&hostedSignalHandler),
        HostedSignalNumber);

    buffer.monitor(thread, &event);
    buffer.cullMonitorTargets(thread);

    return check(
        !buffer.dataReady() && buffer.canWrite(),
        "RingBuffer monitor culling retained its internal mutex");
}

bool ringBufferMonitorRetirement(Thread *thread)
{
    RingBuffer<char> buffer(1);
    HostedMonitorEvent event;
    buffer.monitor(thread, &event);

    EventDrainContext context(&event);
    Thread *drainer = new Thread(
        Scheduler::instance().getKernelProcess(),
        drainEventRegistrations, &context, nullptr, false, true);
    drainer->setName("hosted RingBuffer monitor retirement");

    const bool closePublished =
        waitUntilQueued(drainer, Thread::EventWait);

    // Model an already-dispatched callback trying to re-arm after its owner
    // closed event admission. A distinct target lets the original cull prove
    // that no second registration was accepted.
    buffer.monitor(drainer, &event);
    buffer.cullMonitorTargets(thread);

    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (!context.completed && Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }
    const bool rearmRejected = context.completed == 1;

    // Keep a failing regression from leaving its drain thread behind.
    if (!rearmRejected)
    {
        buffer.cullMonitorTargets(drainer);
    }
    const bool joined = drainer->join();

    return check(
        context.entered == 1 && closePublished && rearmRejected && joined,
        "RingBuffer accepted a monitor registration after Event retirement");
}

bool ringBufferMonitorDestructor(Thread *thread)
{
    const size_t destructionsBefore = g_MonitorEventDestructions;
    HostedMonitorEvent *event = new HostedMonitorEvent;
    {
        RingBuffer<char> buffer(1);
        buffer.monitor(thread, event);
    }

    // RingBuffer closure legitimately queued one final readiness event. Keep
    // retirement pinned while removing that delivery so deletion now depends
    // only on whether the destroyed RingBuffer released its source lease.
    const bool closeNotified =
        event->pendingCount() == 1 && thread->hasEvent(event);
    {
        Event::Retirement retirement;
        event->beginRetirement(retirement);
        thread->cullEvent(event);
    }
    return check(
        closeNotified &&
            g_MonitorEventDestructions == (destructionsBefore + 1),
        "RingBuffer destruction retained its Event source lease after the "
        "queued close notification drained");
}

bool delaySignalInterruption(Thread *thread)
{
    SignalContext context(thread, Thread::EventWait);

    thread->clearInterruption();
    g_SignalHandlerCalls = 0;
    Thread *interrupter = startInterrupter(context);

    const bool delayed = Time::delay(5 * Time::Multiplier::Second);
    const Thread::InterruptionReason reason =
        thread->getInterruptionReason();
    thread->clearInterruption();
    const bool joined = interrupter->join();

    return check(
        !delayed && reason == Thread::InterruptedBySignal &&
            joined && context.published == 1 && context.sent == 1 &&
            g_SignalHandlerCalls == 1,
        "Time::delay did not preserve its signal interruption");
}

bool prequeuedDelaySignalInterruption(Thread *thread)
{
    thread->clearInterruption();
    g_SignalHandlerCalls = 0;

    SignalEvent *event = new SignalEvent(
        reinterpret_cast<uintptr_t>(&hostedSignalHandler),
        HostedSignalNumber, ~0UL, 0, true, true);
    if (!thread->sendEvent(event))
    {
        delete event;
        return check(false, "a prequeued signal could not be published");
    }

    const bool delayed = Time::delay(5 * Time::Multiplier::Second);
    const Thread::InterruptionReason reason =
        thread->getInterruptionReason();
    thread->clearInterruption();

    return check(
        !delayed && reason == Thread::InterruptedBySignal &&
            g_SignalHandlerCalls == 1,
        "a prequeued signal did not interrupt Time::delay");
}
}  // namespace

bool runHostedSignalInterruptionRegressions(Thread *thread)
{
    const bool passed =
        conditionVariableSignalInterruption(thread) &&
        bufferSignalInterruption(thread) &&
        semaphoreSignalInterruption(thread) &&
        semaphoreSignalAfterOrdinaryWake() &&
        conditionSignalAfterOrdinaryWake() &&
        completionSemaphoreSignalDeferral(thread) &&
        ringBufferSignalInterruption(thread) &&
        ringBufferMonitorCull(thread) &&
        ringBufferMonitorRetirement(thread) &&
        ringBufferMonitorDestructor(thread) &&
        delaySignalInterruption(thread) &&
        prequeuedDelaySignalInterruption(thread);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS signal-interruption");
    }
    return passed;
}
