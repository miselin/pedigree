/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/SplitIrqHandler.h"
#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/machine/TimerHandler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "system/kernel/core/processor/DeviceHardIrqContext.h"

extern bool runHostedInterruptManagerRegressions();
extern bool runHostedSchedulerRegressions();
extern void system_reset();

namespace
{
constexpr Time::Timestamp TestTimeout = 2 * Time::Multiplier::Second;

bool check(bool condition, const char *test, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-IRQ-CLOSURE: FAIL " << test << ": " << detail);
    return false;
}

template <class Predicate>
bool waitUntil(Predicate predicate)
{
    const Time::Timestamp deadline = Time::getTicks() + TestTimeout;
    while (!predicate() && Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }
    return predicate();
}

struct DispatcherContext
{
    DispatcherContext()
        : calls(0), lastCookie(0), contextFailures(0), driver(nullptr)
    {
    }

    Atomic<size_t> calls;
    Atomic<size_t> lastCookie;
    Atomic<size_t> contextFailures;
    Thread *driver;
};

void dispatchThreaded(void *opaque, uint8_t line, size_t cookie)
{
    DispatcherContext *context =
        reinterpret_cast<DispatcherContext *>(opaque);
    Thread *current = Processor::information().getCurrentThread();
    if (
        line != 0 || !current || current == context->driver ||
        !Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
        Processor::executionContext() != ExecutionContext::WaitableThread ||
        current->getHostedSignalDepth())
    {
        context->contextFailures += 1;
    }
    context->lastCookie = cookie;
    context->calls += 1;
}

bool threadedDispatcherLifecycle()
{
    constexpr const char *Test = "threaded-dispatcher-lifecycle";
    DispatcherContext context;
    context.driver = Processor::information().getCurrentThread();
    ThreadedIrqDispatcher dispatcher(
        MakeConstantString("IRQ closure dispatcher"), 1, dispatchThreaded,
        &context);

    bool passed = check(dispatcher.initialise(), Test, "initialise failed");
    passed &= check(
        !dispatcher.publishFromInterrupt(1, 1) &&
            !dispatcher.publishFromInterrupt(0, 0),
        Test, "invalid publications were accepted");

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    const bool firstPublished = dispatcher.publishFromInterrupt(0, 7);
    const bool newerPublished = dispatcher.publishFromInterrupt(0, 9);
    Processor::setInterrupts(interruptsWereEnabled);

    passed &= check(
        firstPublished && newerPublished &&
            waitUntil([&dispatcher] {
                return dispatcher.completedCookie(0) == 9;
            }),
        Test, "the worker did not consume the published high-water cookie");

    dispatcher.rejectNextPublicationForTest();
    Processor::setInterrupts(false);
    const bool rejectionObserved = !dispatcher.publishFromInterrupt(0, 11);
    const bool recoveryPublished = dispatcher.publishFromInterrupt(0, 12);
    Processor::setInterrupts(interruptsWereEnabled);
    passed &= check(
        rejectionObserved && recoveryPublished &&
            waitUntil([&dispatcher] {
                return dispatcher.completedCookie(0) == 12;
            }),
        Test, "a rejected publication poisoned the next occurrence");

    const size_t callsBeforeShutdown = context.calls.value();
    const bool shutdown = dispatcher.shutdown();
    passed &= check(
        shutdown && callsBeforeShutdown && !context.contextFailures &&
            !dispatcher.isInitialised() &&
            !dispatcher.publishFromInterrupt(0, 13),
        Test, "shutdown or callback context invariants failed");

    if (passed)
    {
        NOTICE("HOSTED-IRQ-CLOSURE: PASS threaded-dispatcher-lifecycle");
    }
    return passed;
}

class SplitLifecycleProbe final : public SplitIrqHandler
{
  public:
    explicit SplitLifecycleProbe(Thread *driver)
        : SplitIrqHandler(MakeConstantString("IRQ closure split handler")),
          threadedWork(0), rearmedWork(0), quiesceCalls(0), contextFailures(0),
          quiesceContextFailures(0), rearmContextFailures(0), workerCalls(0),
          m_Driver(driver), m_Active(false)
    {
    }

    ~SplitLifecycleProbe() override
    {
        if (m_Active)
        {
            shutdownSplitIrq();
        }
    }

    bool start()
    {
        m_Active = initialiseSplitIrq();
        return m_Active;
    }

    bool publish(size_t work)
    {
        return publishWorkForTest(work);
    }

    bool stop()
    {
        const bool stopped = shutdownSplitIrq();
        if (stopped)
        {
            m_Active = false;
        }
        return stopped;
    }

    Atomic<size_t> threadedWork;
    Atomic<size_t> rearmedWork;
    Atomic<size_t> quiesceCalls;
    Atomic<size_t> contextFailures;
    Atomic<size_t> quiesceContextFailures;
    Atomic<size_t> rearmContextFailures;
    Atomic<size_t> workerCalls;

    using SplitIrqHandler::completedBatchesForTest;
    using SplitIrqHandler::deferredIrqsForTest;
    using SplitIrqHandler::pendingWorkForTest;
    using SplitIrqHandler::publicationFailuresForTest;
    using SplitIrqHandler::rejectNextPublicationForTest;

  private:
    HardStageDisposition
    hardIrq(irq_id_t, InterruptState &, size_t &) override
    {
        return HardStageDisposition::NotHandled;
    }

    void threadedIrq(size_t work) override
    {
        Thread *current = Processor::information().getCurrentThread();
        if (
            !current || !Processor::getInterrupts() ||
            Processor::inDeviceHardIrq() ||
            Processor::executionContext() != ExecutionContext::WaitableThread ||
            current->getHostedSignalDepth())
        {
            contextFailures += 1;
        }
        if (current != m_Driver)
        {
            workerCalls += 1;
        }
        threadedWork |= work;
    }

    bool quiesceIrqSources() override
    {
        if (
            !Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
            Processor::executionContext() != ExecutionContext::WaitableThread)
        {
            quiesceContextFailures += 1;
        }
        quiesceCalls += 1;
        return true;
    }

    void rearmIrqSources(size_t work) override
    {
        if (
            Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
            Processor::executionContext() != ExecutionContext::AtomicThread)
        {
            rearmContextFailures += 1;
        }
        rearmedWork |= work;
    }

    Thread *m_Driver;
    bool m_Active;
};

bool splitHandlerLifecycle()
{
    constexpr const char *Test = "split-handler-lifecycle";
    SplitLifecycleProbe probe(Processor::information().getCurrentThread());
    if (!check(probe.start(), Test, "initialise failed"))
    {
        return false;
    }
    bool passed = true;

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    bool firstPublished = false;
    bool secondPublished = false;
    {
        size_t previousDepth = 0;
        bool restorationArmed = false;
        DeviceHardIrqContext hardIrq(previousDepth, restorationArmed);
        firstPublished = probe.publish(1);
        secondPublished = probe.publish(2);
    }
    Processor::setInterrupts(interruptsWereEnabled);

    passed &= check(
        firstPublished && secondPublished &&
            waitUntil([&probe] {
                return probe.completedBatchesForTest() &&
                       !probe.pendingWorkForTest();
            }) &&
            probe.threadedWork.value() == 3 &&
            probe.rearmedWork.value() == 3 && probe.workerCalls.value() &&
            probe.deferredIrqsForTest() == 2 &&
            !probe.publicationFailuresForTest(),
        Test, "accepted hard work did not drain and rearm on the worker");

    probe.rejectNextPublicationForTest();
    Processor::setInterrupts(false);
    bool rejectionObserved = false;
    {
        size_t previousDepth = 0;
        bool restorationArmed = false;
        DeviceHardIrqContext hardIrq(previousDepth, restorationArmed);
        rejectionObserved = !probe.publish(4);
    }
    Processor::setInterrupts(interruptsWereEnabled);

    const size_t rearmsBeforeShutdown = probe.rearmedWork.value();
    const bool orphanRecorded = probe.pendingWorkForTest() == 4;
    const bool stopped = probe.stop();
    passed &= check(
        rejectionObserved && orphanRecorded &&
            probe.publicationFailuresForTest() == 1,
        Test, "the forced rejection did not leave one orphan batch");
    passed &= check(stopped, Test, "shutdown rejected an ordinary caller");
    passed &= check(
        !probe.pendingWorkForTest() && probe.threadedWork.value() == 7,
        Test, "shutdown did not drain the orphan batch");
    passed &= check(
        probe.rearmedWork.value() == rearmsBeforeShutdown, Test,
        "shutdown rearmed a quiesced source");
    passed &= check(
        probe.quiesceCalls.value() == 2, Test,
        "shutdown did not reassert source quiescence after draining");
    passed &= check(
        !probe.contextFailures, Test,
        "the threaded callback ran outside ordinary waitable context");
    passed &= check(
        !probe.quiesceContextFailures, Test,
        "source quiescence ran outside ordinary waitable context");
    passed &= check(
        !probe.rearmContextFailures, Test,
        "source rearm did not run with interrupts disabled");

    if (passed)
    {
        NOTICE("HOSTED-IRQ-CLOSURE: PASS split-handler-lifecycle");
    }
    return passed;
}

class TimerContextProbe final : public TimerHandler
{
  public:
    explicit TimerContextProbe(Thread *driver)
        : calls(0), elapsed(0), contextFailures(0), m_Driver(driver)
    {
    }

    void timer(uint64_t delta) override
    {
        Thread *current = Processor::information().getCurrentThread();
        if (
            !delta || !current || current == m_Driver ||
            !Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
            Processor::executionContext() != ExecutionContext::WaitableThread ||
            current->getHostedSignalDepth())
        {
            contextFailures += 1;
        }
        elapsed += delta;
        calls += 1;
    }

    Atomic<size_t> calls;
    Atomic<uint64_t> elapsed;
    Atomic<size_t> contextFailures;

  private:
    Thread *m_Driver;
};

bool hostedTimerSplitDelivery()
{
    constexpr const char *Test = "hosted-timer-split-delivery";
    Timer *timer = Machine::instance().getTimer();
    TimerContextProbe probe(Processor::information().getCurrentThread());
    bool passed = check(
        timer && timer->registerHandler(&probe), Test,
        "timer handler registration failed");
    passed &= check(
        waitUntil([&probe] { return probe.calls.value() >= 3; }), Test,
        "the hard timer source did not reach its threaded callback");

    const bool removed = timer && timer->unregisterHandler(&probe);
    const size_t callsAfterRemoval = probe.calls.value();
    const Time::Timestamp drainDeadline =
        Time::getTicks() + (10 * Time::Multiplier::Millisecond);
    while (Time::getTicks() < drainDeadline)
    {
        Scheduler::instance().yield();
    }
    passed &= check(
        removed && callsAfterRemoval >= 3 &&
            probe.calls.value() == callsAfterRemoval && probe.elapsed.value() &&
            !probe.contextFailures,
        Test, "threaded delivery context or unregister drain failed");

    if (passed)
    {
        NOTICE("HOSTED-IRQ-CLOSURE: PASS hosted-timer-split-delivery");
    }
    return passed;
}

constexpr size_t ExpectedGuardDenials = 7;

struct GuardContext
{
    GuardContext() : calls(0), overflow(0), operations()
    {
    }

    size_t calls;
    size_t overflow;
    DeviceHardIrqOperation operations[ExpectedGuardDenials];
};

GuardContext *g_GuardContext = nullptr;

bool denyHardIrqOperation(DeviceHardIrqOperation operation)
{
    GuardContext *context =
        __atomic_load_n(&g_GuardContext, __ATOMIC_ACQUIRE);
    if (!context)
    {
        return false;
    }
    const size_t call = __atomic_fetch_add(
        &context->calls, static_cast<size_t>(1), __ATOMIC_RELAXED);
    if (call < ExpectedGuardDenials)
    {
        context->operations[call] = operation;
    }
    else
    {
        context->overflow = 1;
    }
    return true;
}

bool hardIrqOperationGuards()
{
    constexpr const char *Test = "hard-irq-operation-guards";
    GuardContext context;
    Semaphore semaphore(0);
    WaitQueue waitQueue;
    __atomic_store_n(&g_GuardContext, &context, __ATOMIC_RELEASE);
    Processor::setDeviceHardIrqOperationHookForTest(denyHardIrqOperation);
    const size_t denialsBefore =
        Processor::deviceHardIrqOperationDenialsForTest();

    bool marked = false;
    bool hardContext = false;
    bool scheduleReturned = false;
    bool semaphoreAcquireDenied = false;
    bool semaphoreReleaseDenied = false;
    bool waitDenied = false;
    bool wakeDenied = false;
    bool allocateDenied = false;
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    {
        size_t previousDepth = 0;
        bool restorationArmed = false;
        DeviceHardIrqContext hardIrq(previousDepth, restorationArmed);
        marked = Processor::inDeviceHardIrq() &&
                 Processor::deviceHardIrqDepthForTest() == 1;
        hardContext =
            Processor::executionContext() == ExecutionContext::HardDeviceIrq;

        Scheduler::instance().yield();
        scheduleReturned = true;

        Semaphore::SemaphoreError error = Semaphore::NoError;
        semaphoreAcquireDenied =
            !semaphore.acquireWithError(1, 0, 0, error) &&
            error == Semaphore::Interrupted;
        semaphore.release(1);
        semaphoreReleaseDenied = semaphore.getValue() == 0;

        {
            auto guard = waitQueue.acquire();
            waitDenied = guard.wait() == WaitQueue::WakeReason::Spurious;
        }
        wakeDenied =
            waitQueue.wakeAll(WaitQueue::WakeReason::Signalled) == 0;
        allocateDenied = SlamAllocator::guardedAllocateForTest(64) == 0;
        SlamAllocator::guardedFreeForTest(0);
    }
    Processor::setInterrupts(interruptsWereEnabled);

    Processor::setDeviceHardIrqOperationHookForTest(nullptr);
    __atomic_store_n(
        &g_GuardContext, static_cast<GuardContext *>(nullptr),
        __ATOMIC_RELEASE);
    const size_t denialsAfter =
        Processor::deviceHardIrqOperationDenialsForTest();

    const DeviceHardIrqOperation expected[ExpectedGuardDenials] = {
        DeviceHardIrqOperation::Schedule,
        DeviceHardIrqOperation::SemaphoreAcquire,
        DeviceHardIrqOperation::SemaphoreRelease,
        DeviceHardIrqOperation::WaitQueueAccess,
        DeviceHardIrqOperation::WaitQueueAccess,
        DeviceHardIrqOperation::HeapAllocate,
        DeviceHardIrqOperation::HeapFree,
    };
    bool operationsMatch =
        context.calls == ExpectedGuardDenials && !context.overflow;
    for (size_t i = 0; i < ExpectedGuardDenials && operationsMatch; ++i)
    {
        operationsMatch = context.operations[i] == expected[i];
    }

    const bool passed = check(
        marked && hardContext && scheduleReturned && semaphoreAcquireDenied &&
            semaphoreReleaseDenied && waitDenied && wakeDenied &&
            allocateDenied && operationsMatch &&
            denialsAfter - denialsBefore == ExpectedGuardDenials &&
            !Processor::inDeviceHardIrq() &&
            Processor::executionContext() == ExecutionContext::WaitableThread &&
            waitQueue.waiterCount() == 0 && semaphore.getValue() == 0,
        Test, "a forbidden operation escaped or the hard context leaked");
    if (passed)
    {
        NOTICE("HOSTED-IRQ-CLOSURE: PASS hard-irq-operation-guards");
    }
    return passed;
}
}  // namespace

bool runHostedIrqClosureRegressions()
{
    NOTICE("HOSTED-IRQ-CLOSURE: BEGIN");
    bool passed = hardIrqOperationGuards();
    passed &= threadedDispatcherLifecycle();
    passed &= splitHandlerLifecycle();
    passed &= hostedTimerSplitDelivery();
    passed &= runHostedInterruptManagerRegressions();
    passed &= runHostedSchedulerRegressions();
    if (passed)
    {
        NOTICE("HOSTED-IRQ-CLOSURE: PASS all");
    }
    else
    {
        ERROR("HOSTED-IRQ-CLOSURE: FAIL suite");
    }
    system_reset();
    return passed;
}
