/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqEventCounter.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/SplitIrqHandler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/time/Time.h"
#include "system/kernel/machine/hosted/IrqManager.h"

#include <signal.h>

namespace
{
constexpr Time::Timestamp WaitTimeout = 2 * Time::Multiplier::Second;

bool check(bool condition, const char *detail, const char *test)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
    return false;
}

bool waitForSemaphore(Semaphore &semaphore)
{
    return semaphore.acquireForCompletion(1, 2, 0);
}

bool irqEventCounterArithmeticRegression()
{
    constexpr const char *Test = "irq-event-counter-bounded-arithmetic";
    constexpr size_t Maximum = ~static_cast<size_t>(0);
    IrqEventCounter counter;

    bool passed = check(
        counter.recordFromInterrupt(1) &&
            counter.recordFromInterrupt(14) && counter.takeAll() == 15 &&
            counter.recordFromInterrupt(0) && !counter.pending(),
        "bounded batches did not preserve their exact event count", Test);
    passed &= check(
        counter.recordFromInterrupt(Maximum) &&
            !counter.recordFromInterrupt(1) && counter.takeAll() == Maximum,
        "counter overflow changed state or accepted lost event fidelity",
        Test);
    counter.reset();
    passed &= check(
        !counter.pending() && counter.recordFromInterrupt(8) &&
            counter.takeAll() == 8,
        "an overflowed counter could not be explicitly reset", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-event-counter-bounded-arithmetic");
    }
    return passed;
}

bool waitForThreadState(Thread *thread, Thread::DebugState state)
{
    const Time::Timestamp deadline = Time::getTicks() + WaitTimeout;
    while (Time::getTicks() < deadline)
    {
        Thread::WaitDebugInfo wait = {};
        uintptr_t address = 0;
        if (thread->getWaitDebugInfo(wait) && wait.queued &&
            thread->getDebugState(address) == state)
        {
            return true;
        }
        Scheduler::instance().yield();
    }
    return false;
}

bool hasCallbackDrainState(Thread *thread, IrqHandlerBase *handler)
{
    Thread::WaitDebugInfo wait = {};
    uintptr_t address = 0;
    return thread && !thread->getWaitDebugInfo(wait) &&
           thread->getDebugState(address) == Thread::CallbackDrain &&
           address == reinterpret_cast<uintptr_t>(handler);
}

class HostedSplitIrq final : public SplitIrqHandler
{
  public:
    HostedSplitIrq()
        : SplitIrqHandler(MakeConstantString("Hosted split IRQ regression")),
          hardCalls(0), hardOutsideSignal(0), bottomCalls(0),
          bottomInsideSignal(0), observedWork(0), observedEvents(0),
          observedEventTime(0), bottomEntered(0), releaseBottom(0),
          bottomCompleted(0), quiesceCalls(0), rearmCalls(0), rearmedWork(0),
          hardShutdownAttempts(0), hardShutdownSucceeded(0),
          bottomShutdownAttempts(0), bottomShutdownSucceeded(0),
          m_Started(false),
          m_NextWork(1), m_NextEvents(1), m_HoldNext(0),
          m_ShutdownFromHard(0), m_ShutdownFromBottom(0),
          m_FailQuiesceCall(0), m_EventPhase(0)
    {
    }

    ~HostedSplitIrq() override
    {
        if (m_Started && !stop())
        {
            FATAL("Hosted split IRQ regression could not stop its worker.");
        }
    }

    bool start(uint8_t irq = 0)
    {
        if (!initialiseSplitIrq())
        {
            return false;
        }
        m_Started = true;

        IrqManager &manager = *Machine::instance().getIrqManager();
        if (!registerIsaSplitIrq(
                manager, irq, IrqPolicy::syntheticHard()))
        {
            const bool stopped = shutdownSplitIrq();
            m_Started = !stopped;
            return false;
        }
        return true;
    }

    bool stop()
    {
        if (!m_Started)
        {
            return true;
        }

        const bool stopped = shutdownSplitIrq();
        if (stopped)
        {
            m_Started = false;
        }
        return stopped;
    }

    bool registerAdditional(uint8_t irq)
    {
        IrqManager &manager = *Machine::instance().getIrqManager();
        return registerIsaSplitIrq(
                   manager, irq, IrqPolicy::syntheticHard()) != 0;
    }

    void setRegistrationPublishedHook(RegistrationPublishedHook hook)
    {
        setRegistrationPublishedHookForTest(hook);
    }

    void setNextWork(size_t work)
    {
        m_NextWork = work;
    }

    void setNextEventCount(size_t events)
    {
        m_NextEvents = events;
    }

    void holdNextBottom()
    {
        m_HoldNext = 1;
    }

    void shutdownFromNextHardIrq()
    {
        m_ShutdownFromHard = 1;
    }

    void shutdownFromNextBottom()
    {
        m_ShutdownFromBottom = 1;
    }

    void failQuiesceCall(size_t call)
    {
        m_FailQuiesceCall = call;
    }

    ::HardIrqHandler *hardHandler()
    {
        return hardHandlerForTest();
    }

    size_t publicationFailures() const
    {
        return publicationFailuresForTest();
    }

    size_t pendingWork() const
    {
        return pendingWorkForTest();
    }

    size_t deferredIrqs() const
    {
        return deferredIrqsForTest();
    }

    size_t completedBatches() const
    {
        return completedBatchesForTest();
    }

    Atomic<size_t> hardCalls;
    Atomic<size_t> hardOutsideSignal;
    Atomic<size_t> bottomCalls;
    Atomic<size_t> bottomInsideSignal;
    Atomic<size_t> observedWork;
    Atomic<size_t> observedEvents;
    Atomic<uint64_t> observedEventTime;
    Semaphore bottomEntered;
    Semaphore releaseBottom;
    Semaphore bottomCompleted;
    Atomic<size_t> quiesceCalls;
    Atomic<size_t> rearmCalls;
    Atomic<size_t> rearmedWork;
    Atomic<size_t> hardShutdownAttempts;
    Atomic<size_t> hardShutdownSucceeded;
    Atomic<size_t> bottomShutdownAttempts;
    Atomic<size_t> bottomShutdownSucceeded;

  protected:
    HardIrqDisposition
    hardIrq(irq_id_t, InterruptState &state, size_t &work) override
    {
        if (state.getInterruptSource())
        {
            return HardIrqDisposition::NotHandled;
        }

        hardCalls += 1;
        if (!m_ExactEvents.recordFromInterrupt(m_NextEvents.value()))
        {
            FATAL_NOLOCK("Split IRQ event counter saturated.");
        }
        Thread *current = Processor::information().getCurrentThread();
        if (!current || !current->getHostedSignalDepth())
        {
            hardOutsideSignal += 1;
        }

        if (m_ShutdownFromHard.compareAndSwap(1, 0))
        {
            hardShutdownAttempts += 1;
            if (shutdownSplitIrq())
            {
                hardShutdownSucceeded += 1;
            }
        }

        work = m_NextWork;
        return HardIrqDisposition::Deferred;
    }

    void threadedIrq(size_t work) override
    {
        bottomCalls += 1;
        observedWork |= work;
        size_t events = m_ExactEvents.takeAll();
        observedEvents += events;
        while (events--)
        {
            observedEventTime += m_EventPhase ? 976563 : 976562;
            m_EventPhase ^= 1;
        }

        Thread *current = Processor::information().getCurrentThread();
        if (current && current->getHostedSignalDepth())
        {
            bottomInsideSignal += 1;
        }

        if (m_ShutdownFromBottom.compareAndSwap(1, 0))
        {
            bottomShutdownAttempts += 1;
            if (shutdownSplitIrq())
            {
                bottomShutdownSucceeded += 1;
            }
        }

        if (m_HoldNext.compareAndSwap(1, 0))
        {
            bottomEntered.release();
            const bool released = releaseBottom.acquireForCompletion();
            (void) released;
        }
        bottomCompleted.release();
    }

    bool quiesceIrqSources() override
    {
        quiesceCalls += 1;
        if (m_FailQuiesceCall.compareAndSwap(quiesceCalls.value(), 0))
        {
            return false;
        }
        return true;
    }

    void rearmIrqSources(size_t work) override
    {
        rearmCalls += 1;
        rearmedWork |= work;
    }

  private:
    bool m_Started;
    Atomic<size_t> m_NextWork;
    Atomic<size_t> m_NextEvents;
    Atomic<size_t> m_HoldNext;
    Atomic<size_t> m_ShutdownFromHard;
    Atomic<size_t> m_ShutdownFromBottom;
    Atomic<size_t> m_FailQuiesceCall;
    size_t m_EventPhase;
    IrqEventCounter m_ExactEvents;
};

bool waitForCompletedBatches(HostedSplitIrq &handler, size_t batches)
{
    const Time::Timestamp deadline = Time::getTicks() + WaitTimeout;
    while (handler.completedBatches() < batches && Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }
    return handler.completedBatches() >= batches;
}

struct LifecycleSerializationContext
{
    explicit LifecycleSerializationContext(HostedSplitIrq *handler)
        : handler(handler), shutdown(nullptr), hookCalls(0), shutdownReturned(0),
          shutdownSucceeded(0), ordinaryContext(0), failures(0)
    {
    }

    HostedSplitIrq *handler;
    Thread *shutdown;
    Atomic<size_t> hookCalls;
    Atomic<size_t> shutdownReturned;
    Atomic<size_t> shutdownSucceeded;
    Atomic<size_t> ordinaryContext;
    Atomic<size_t> failures;
};

LifecycleSerializationContext *g_LifecycleSerialization = nullptr;

int attemptConcurrentSplitShutdown(void *parameter)
{
    LifecycleSerializationContext *context =
        reinterpret_cast<LifecycleSerializationContext *>(parameter);
    Thread *current = Processor::information().getCurrentThread();
    context->ordinaryContext =
        current && Processor::getInterrupts() &&
                !current->getHostedSignalDepth()
            ? 1
            : 0;
    const bool stopped = context->handler->stop();
    context->shutdownSucceeded = stopped ? 1 : 0;
    context->shutdownReturned = 1;
    return 0;
}

void registrationPublishedBeforeBookkeeping(SplitIrqHandler *handler)
{
    LifecycleSerializationContext *context = g_LifecycleSerialization;
    if (!context || handler != context->handler ||
        !context->hookCalls.compareAndSwap(0, 1))
    {
        return;
    }

    if (!context->shutdown->start() ||
        !context->shutdown->joinForCompletion())
    {
        context->failures += 1;
    }
}

bool lifecycleSerializationRegression()
{
    constexpr const char *Test = "split-irq-lifecycle-serialization";
    HostedSplitIrq handler;
    bool passed = check(handler.start(), "the handler could not start", Test);
    if (!passed)
    {
        return false;
    }

    LifecycleSerializationContext context(&handler);
    context.shutdown = new Thread(
        Scheduler::instance().getKernelProcess(),
        attemptConcurrentSplitShutdown, &context, nullptr, false, true, true);
    context.shutdown->setName("hosted split IRQ lifecycle contender");

    g_LifecycleSerialization = &context;
    handler.setRegistrationPublishedHook(
        registrationPublishedBeforeBookkeeping);
    const bool registered = handler.registerAdditional(2);
    handler.setRegistrationPublishedHook(nullptr);
    g_LifecycleSerialization = nullptr;

    handler.setNextWork(1);
    const bool raised = registered && raise(SIGURG) == 0;
    const bool bottomCompleted =
        raised && waitForSemaphore(handler.bottomCompleted);
    const bool stopped = handler.stop();

    passed &= check(
        registered && context.hookCalls == 1 && context.failures == 0 &&
            context.shutdownReturned == 1 &&
            context.shutdownSucceeded == 0 && context.ordinaryContext == 1,
        "shutdown crossed an in-flight registration lifecycle operation",
        Test);
    passed &= check(
        bottomCompleted && handler.hardCalls == 1 &&
            handler.bottomCalls == 1,
        "the serialized registration did not remain live", Test);
    passed &= check(
        stopped && handler.publicationFailures() == 0,
        "the final lifecycle owner could not drain both registrations", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "split-irq-lifecycle-serialization");
    }
    return passed;
}

bool coalescingRegression()
{
    constexpr const char *Test = "split-irq-coalescing";
    HostedSplitIrq handler;
    bool passed = check(handler.start(), "the handler could not start", Test);
    if (!passed)
    {
        return false;
    }

    handler.holdNextBottom();
    handler.setNextWork(1);
    handler.setNextEventCount(1);
    bool raised = raise(SIGUSR1) == 0;
    const bool entered = waitForSemaphore(handler.bottomEntered);
    for (size_t work = 2; work <= 8; work <<= 1)
    {
        handler.setNextWork(work);
        handler.setNextEventCount(work);
        raised &= raise(SIGUSR1) == 0;
    }

    handler.releaseBottom.release();
    const bool firstCompleted = waitForSemaphore(handler.bottomCompleted);
    const bool secondCompleted = waitForSemaphore(handler.bottomCompleted);
    const bool batchesCompleted = waitForCompletedBatches(handler, 2);
    const bool stopped = handler.stop();

    passed &= check(
        raised && entered && firstCompleted && secondCompleted &&
            batchesCompleted,
        "the held worker did not complete both batches", Test);
    passed &= check(
        handler.hardCalls == 4 && handler.bottomCalls == 2 &&
            handler.observedWork == 15 && handler.observedEvents == 15 &&
            handler.observedEventTime == 14648437,
        "an IRQ storm was not coalesced into one follow-up batch", Test);
    passed &= check(
        handler.hardOutsideSignal == 0 && handler.bottomInsideSignal == 0 &&
            handler.pendingWork() == 0 && handler.deferredIrqs() == 4 &&
            handler.completedBatches() == 2 &&
            handler.publicationFailures() == 0 && handler.rearmCalls == 2 &&
            handler.rearmedWork == 15 && handler.quiesceCalls == 2 && stopped,
        "coalesced work crossed context or failed teardown", Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS split-irq-coalescing");
    }
    return passed;
}

struct ShutdownContext
{
    explicit ShutdownContext(HostedSplitIrq *handler)
        : handler(handler), entered(0), finished(0), result(0)
    {
    }

    HostedSplitIrq *handler;
    Atomic<size_t> entered;
    Atomic<size_t> finished;
    Atomic<size_t> result;
};

int shutdownSplitHandler(void *parameter)
{
    ShutdownContext *context = reinterpret_cast<ShutdownContext *>(parameter);
    context->entered = 1;
    context->result = context->handler->stop() ? 1 : 0;
    context->finished = 1;
    return 0;
}

bool unregisterDrainRegression()
{
    constexpr const char *Test = "split-irq-unregister-drain";
    HostedSplitIrq handler;
    bool passed = check(handler.start(), "the handler could not start", Test);
    if (!passed)
    {
        return false;
    }

    handler.holdNextBottom();
    handler.setNextWork(1);
    const bool raised = raise(SIGUSR1) == 0;
    const bool bottomHeld = waitForSemaphore(handler.bottomEntered);

    ShutdownContext context(&handler);
    Thread *shutdown = new Thread(
        Scheduler::instance().getKernelProcess(), shutdownSplitHandler,
        &context, nullptr, false, true);
    shutdown->setName("hosted split IRQ shutdown");
    const bool draining = waitForThreadState(shutdown, Thread::Joining);
    const size_t callsBeforeClosedSignal = handler.hardCalls;
    const bool closedSignalRaised = raise(SIGUSR1) == 0;
    const size_t callsAfterClosedSignal = handler.hardCalls;
    const size_t enteredWhileHeld = context.entered;
    const size_t finishedWhileHeld = context.finished;

    handler.releaseBottom.release();
    const bool bottomCompleted = waitForSemaphore(handler.bottomCompleted);
    const bool shutdownJoined = shutdown->join();

    passed &= check(
        raised && bottomHeld && draining && enteredWhileHeld == 1 &&
            finishedWhileHeld == 0,
        "shutdown did not wait for the running bottom half", Test);
    if (!raised || !bottomHeld || !draining || enteredWhileHeld != 1 ||
        finishedWhileHeld != 0)
    {
        ERROR(
            "HOSTED-WAIT-TEST: INFO "
            << Test << ": raised=" << raised << ", held=" << bottomHeld
            << ", draining=" << draining << ", entered=" << enteredWhileHeld
            << ", finished=" << finishedWhileHeld
            << ", result=" << context.result.value());
    }
    passed &= check(
        closedSignalRaised && callsAfterClosedSignal == callsBeforeClosedSignal,
        "a hard callback began after unregister closed admission", Test);
    passed &= check(
        bottomCompleted && shutdownJoined && context.finished == 1 &&
            context.result == 1,
        "the bottom half did not drain before teardown returned", Test);
    passed &= check(
        handler.bottomCalls == 1 && handler.pendingWork() == 0 &&
            handler.deferredIrqs() == 1 && handler.completedBatches() == 1 &&
            handler.publicationFailures() == 0 && handler.rearmCalls == 0 &&
            handler.rearmedWork == 0 && handler.quiesceCalls == 2,
        "teardown duplicated work or rejected a publication", Test);
    if (handler.bottomCalls.value() != 1 || handler.pendingWork() != 0 ||
        handler.deferredIrqs() != 1 || handler.completedBatches() != 1 ||
        handler.publicationFailures() != 0)
    {
        ERROR(
            "HOSTED-WAIT-TEST: INFO "
            << Test << ": bottom=" << handler.bottomCalls.value()
            << ", pending=" << handler.pendingWork()
            << ", deferred=" << handler.deferredIrqs()
            << ", batches=" << handler.completedBatches()
            << ", failures=" << handler.publicationFailures());
    }
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS split-irq-unregister-drain");
    }
    return passed;
}

bool atomicShutdownRejectionRegression()
{
    constexpr const char *Test = "split-irq-atomic-shutdown-rejected";
    HostedSplitIrq handler;
    bool passed = check(handler.start(1), "the handler could not start", Test);
    if (!passed)
    {
        return false;
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    const bool atomicShutdownRejected = !handler.stop();
    Processor::setInterrupts(interruptsWereEnabled);
    const size_t quiesceCallsBeforeSignal = handler.quiesceCalls;

    handler.setNextWork(1);
    const bool raised = interruptsWereEnabled && raise(SIGUSR2) == 0;
    const bool bottomCompleted = waitForSemaphore(handler.bottomCompleted);
    const bool batchCompleted = waitForCompletedBatches(handler, 1);
    const bool stopped = handler.stop();

    passed &= check(
        atomicShutdownRejected && quiesceCallsBeforeSignal == 0,
        "atomic shutdown changed lifecycle or hardware state", Test);
    passed &= check(
        raised && bottomCompleted && batchCompleted && handler.hardCalls == 1 &&
            handler.bottomCalls == 1,
        "rejected shutdown did not leave the registration usable", Test);
    passed &= check(
        stopped && handler.rearmCalls == 1 && handler.rearmedWork == 1 &&
            handler.quiesceCalls == 2 && handler.publicationFailures() == 0,
        "external shutdown could not safely retire the live handler", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "split-irq-atomic-shutdown-rejected");
    }
    return passed;
}

bool hardShutdownRejectionRegression()
{
    constexpr const char *Test = "split-irq-hard-shutdown-rejected";
    HostedSplitIrq handler;
    bool passed = check(handler.start(1), "the handler could not start", Test);
    if (!passed)
    {
        return false;
    }

    handler.shutdownFromNextHardIrq();
    handler.setNextWork(1);
    const bool raised = raise(SIGUSR2) == 0;
    const bool bottomCompleted = waitForSemaphore(handler.bottomCompleted);
    const bool batchCompleted = waitForCompletedBatches(handler, 1);
    const size_t quiesceCallsBeforeStop = handler.quiesceCalls;
    const bool stopped = handler.stop();

    passed &= check(
        raised && handler.hardShutdownAttempts == 1 &&
            handler.hardShutdownSucceeded == 0 && quiesceCallsBeforeStop == 0,
        "hard-context shutdown mutated the handler lifecycle", Test);
    passed &= check(
        bottomCompleted && batchCompleted && handler.hardCalls == 1 &&
            handler.bottomCalls == 1,
        "hard-context rejection did not preserve accepted work", Test);
    passed &= check(
        stopped && handler.rearmCalls == 1 && handler.rearmedWork == 1 &&
            handler.quiesceCalls == 2 && handler.publicationFailures() == 0,
        "external shutdown could not retire the hard callback", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "split-irq-hard-shutdown-rejected");
    }
    return passed;
}

bool workerShutdownRejectionRegression()
{
    constexpr const char *Test = "split-irq-worker-shutdown-rejected";
    HostedSplitIrq handler;
    bool passed = check(handler.start(), "the handler could not start", Test);
    if (!passed)
    {
        return false;
    }

    handler.shutdownFromNextBottom();
    handler.setNextWork(1);
    const bool raised = raise(SIGUSR1) == 0;
    const bool bottomCompleted = waitForSemaphore(handler.bottomCompleted);
    const bool batchCompleted = waitForCompletedBatches(handler, 1);
    const bool registrationAccepted = handler.registerAdditional(2);
    const size_t quiesceCallsBeforeStop = handler.quiesceCalls;
    const bool stopped = handler.stop();

    passed &= check(
        raised && handler.bottomShutdownAttempts == 1 &&
            handler.bottomShutdownSucceeded == 0 &&
            registrationAccepted && quiesceCallsBeforeStop == 0,
        "worker-context shutdown mutated the handler lifecycle", Test);
    passed &= check(
        bottomCompleted && batchCompleted && handler.hardCalls == 1 &&
            handler.bottomCalls == 1,
        "worker-context rejection did not preserve accepted work", Test);
    passed &= check(
        stopped && handler.rearmCalls == 1 && handler.rearmedWork == 1 &&
            handler.quiesceCalls == 2 && handler.publicationFailures() == 0,
        "external shutdown could not retire the live worker", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "split-irq-worker-shutdown-rejected");
    }
    return passed;
}

bool shutdownRetryRegression()
{
    constexpr const char *Test = "split-irq-shutdown-retry";

    HostedSplitIrq earlyFailure;
    bool passed = check(
        earlyFailure.start(), "the early-failure handler could not start",
        Test);
    earlyFailure.failQuiesceCall(1);
    const bool earlyFirstStop = earlyFailure.stop();
    const bool earlyRegistration = earlyFailure.registerAdditional(2);
    const bool earlyRetry = earlyFailure.stop();
    passed &= check(
        !earlyFirstStop && !earlyRegistration && earlyRetry &&
            earlyFailure.quiesceCalls == 3,
        "retry after initial quiesce failure reopened or stranded lifecycle state",
        Test);

    HostedSplitIrq lateFailure;
    passed &= check(
        lateFailure.start(), "the late-failure handler could not start", Test);
    lateFailure.failQuiesceCall(2);
    const bool lateFirstStop = lateFailure.stop();
    const bool lateRegistration = lateFailure.registerAdditional(2);
    const bool lateRetry = lateFailure.stop();
    passed &= check(
        !lateFirstStop && !lateRegistration && lateRetry &&
            lateFailure.quiesceCalls == 4,
        "retry after worker drain reopened or stranded lifecycle state", Test);

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS split-irq-shutdown-retry");
    }
    return passed;
}

struct HardCallbackDrainContext
{
    explicit HardCallbackDrainContext(HostedSplitIrq *handler)
        : handler(handler), dispatcher(nullptr), shutdown(nullptr),
          dispatchEntered(0), releaseDispatch(0), phase(0), hookCalls(0),
          hookObservedDrain(0), dispatchAdmitted(0), dispatchHandled(0),
          shutdownFinished(0), shutdownResult(0), failures(0)
    {
    }

    HostedSplitIrq *handler;
    Thread *dispatcher;
    Thread *shutdown;
    Semaphore dispatchEntered;
    Semaphore releaseDispatch;
    Atomic<size_t> phase;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookObservedDrain;
    Atomic<size_t> dispatchAdmitted;
    Atomic<size_t> dispatchHandled;
    Atomic<size_t> shutdownFinished;
    Atomic<size_t> shutdownResult;
    Atomic<size_t> failures;
};

HardCallbackDrainContext *g_HardCallbackDrain = nullptr;

void holdPinnedTestDispatch(IrqHandlerBase *handler)
{
    HardCallbackDrainContext *context = g_HardCallbackDrain;
    if (!context || handler != context->handler->hardHandler() ||
        !context->phase.compareAndSwap(0, 1))
    {
        return;
    }

    context->hookCalls += 1;
    context->dispatchEntered.release();
    if (!context->releaseDispatch.acquireForCompletion())
    {
        context->failures += 1;
    }
    context->phase = 3;
}

int dispatchPinnedSplitHandler(void *parameter)
{
    HardCallbackDrainContext *context =
        reinterpret_cast<HardCallbackDrainContext *>(parameter);
    bool handled = false;
    const bool admitted = HostedIrqManager::dispatchHandlerForTest(
        2, context->handler->hardHandler(), handled);
    context->dispatchAdmitted = admitted ? 1 : 0;
    context->dispatchHandled = handled ? 1 : 0;
    return admitted && handled ? 0 : 1;
}

int shutdownPinnedHardCallback(void *parameter)
{
    constexpr size_t YieldLimit = 10000;
    HardCallbackDrainContext *context =
        reinterpret_cast<HardCallbackDrainContext *>(parameter);
    for (size_t attempt = 0;
         context->phase != static_cast<size_t>(1) && attempt < YieldLimit;
         ++attempt)
    {
        Scheduler::instance().yield();
    }

    if (context->phase != static_cast<size_t>(1))
    {
        context->failures += 1;
        context->shutdownFinished = 1;
        return 1;
    }

    context->phase = 2;
    context->shutdownResult = context->handler->stop() ? 1 : 0;
    context->shutdownFinished = 1;
    context->phase = 4;
    return 0;
}

bool hardCallbackDrainRegression()
{
    constexpr const char *Test = "split-irq-hard-callback-drain";
    HostedSplitIrq handler;
    bool passed = check(handler.start(2), "the handler could not start", Test);
    if (!passed)
    {
        return false;
    }

    HardCallbackDrainContext context(&handler);
    context.shutdown = new Thread(
        Scheduler::instance().getKernelProcess(), shutdownPinnedHardCallback,
        &context, nullptr, false, true, true);
    context.shutdown->setName("hosted pinned split IRQ shutdown");
    context.dispatcher = new Thread(
        Scheduler::instance().getKernelProcess(), dispatchPinnedSplitHandler,
        &context, nullptr, false, true, true);
    context.dispatcher->setName("hosted pinned split IRQ dispatch");

    g_HardCallbackDrain = &context;
    HostedIrqManager::setHandlerPinHook(holdPinnedTestDispatch);
    handler.setNextWork(1);
    const bool dispatcherStarted = context.dispatcher->start();
    const bool dispatchEntered =
        dispatcherStarted && context.dispatchEntered.acquireForCompletion();
    const bool shutdownStarted = dispatchEntered && context.shutdown->start();

    bool drainObserved = false;
    const Time::Timestamp drainDeadline = Time::getTicks() + WaitTimeout;
    while (shutdownStarted && Time::getTicks() < drainDeadline)
    {
        if (context.phase == static_cast<size_t>(2) &&
            !context.shutdownFinished &&
            hasCallbackDrainState(
                context.shutdown, context.handler->hardHandler()))
        {
            drainObserved = true;
            context.hookObservedDrain += 1;
            break;
        }
        Scheduler::instance().yield();
    }
    if (!drainObserved)
    {
        context.failures += 1;
    }
    context.releaseDispatch.release();
    const bool dispatcherJoined =
        dispatcherStarted && context.dispatcher->joinForCompletion();
    const bool shutdownJoined =
        shutdownStarted && context.shutdown->joinForCompletion();
    HostedIrqManager::setHandlerPinHook(nullptr);
    g_HardCallbackDrain = nullptr;

    const size_t quiesceCalls = handler.quiesceCalls;
    const size_t rearmCalls = handler.rearmCalls;
    const size_t rearmedWork = handler.rearmedWork;
    const size_t hardCalls = handler.hardCalls;
    const size_t bottomCalls = handler.bottomCalls;
    const size_t deferredIrqs = handler.deferredIrqs();
    const size_t completedBatches = handler.completedBatches();
    const size_t publicationFailures = handler.publicationFailures();
    const bool cleanup = context.shutdownResult || handler.stop();

    passed &= check(
        dispatcherJoined && shutdownJoined && context.hookCalls == 1 &&
            context.hookObservedDrain == 1 && context.failures == 0,
        "shutdown did not wait for the admitted hard callback", Test);
    passed &= check(
        context.dispatchAdmitted == 1 && context.dispatchHandled == 1,
        "the controlled hard dispatch did not reach the split handler", Test);
    passed &= check(
        context.shutdownFinished == 1 && context.shutdownResult == 1 &&
            context.phase == 4,
        "shutdown returned before the hard callback publication drained", Test);
    passed &= check(
        hardCalls == 1 && bottomCalls == 1 && deferredIrqs == 1 &&
            completedBatches == 1 && publicationFailures == 0,
        "accepted hard-callback work was lost or duplicated", Test);
    passed &= check(
        quiesceCalls == 2 && rearmCalls == 0 && rearmedWork == 0 && cleanup,
        "teardown rearmed hardware after quiescing", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "split-irq-hard-callback-drain");
    }
    return passed;
}
}  // namespace

bool runHostedSplitIrqRegressions()
{
    bool passed = irqEventCounterArithmeticRegression();
    passed &= lifecycleSerializationRegression();
    passed &= coalescingRegression();
    passed &= unregisterDrainRegression();
    passed &= atomicShutdownRejectionRegression();
    passed &= hardShutdownRejectionRegression();
    passed &= workerShutdownRejectionRegression();
    passed &= shutdownRetryRegression();
    passed &= hardCallbackDrainRegression();
    return passed;
}
