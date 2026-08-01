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

bool hasCallbackDrainWait(Thread *thread, IrqHandlerBase *handler)
{
    Thread::WaitDebugInfo wait = {};
    uintptr_t address = 0;
    return thread && thread->getWaitDebugInfo(wait) && wait.queue &&
           wait.channelOwner && wait.queued &&
           wait.reason == WaitQueue::WakeReason::Waiting &&
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
          hardShutdownAttempts(0), hardShutdownSucceeded(0), m_Started(false),
          m_NextWork(1), m_HoldNext(0), m_ShutdownFromHard(0), m_EventPhase(0)
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
        if (!registerIsaSplitIrq(manager, irq, true))
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

    void setNextWork(size_t work)
    {
        m_NextWork = work;
    }

    void holdNextBottom()
    {
        m_HoldNext = 1;
    }

    void shutdownFromNextHardIrq()
    {
        m_ShutdownFromHard = 1;
    }

    ::HardIrqHandler *hardHandler()
    {
        return hardHandlerForTest();
    }

    WaitQueue *workerWaitQueue()
    {
        return workerWaitQueueForTest();
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

  protected:
    HardIrqDisposition
    hardIrq(irq_id_t, InterruptState &state, size_t &work) override
    {
        if (state.getInterruptSource())
        {
            return HardIrqDisposition::NotHandled;
        }

        hardCalls += 1;
        if (!m_ExactEvents.recordFromInterrupt())
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
    Atomic<size_t> m_HoldNext;
    Atomic<size_t> m_ShutdownFromHard;
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

struct WakeBeforeBlockContext
{
    explicit WakeBeforeBlockContext(HostedSplitIrq *handler)
        : handler(handler), fired(0), failures(0)
    {
    }

    HostedSplitIrq *handler;
    Atomic<size_t> fired;
    Atomic<size_t> failures;
};

WakeBeforeBlockContext *g_WakeBeforeBlock = nullptr;

void publishBeforeWorkerBlocks(
    WaitQueue *queue, Thread *, const WaitQueue::Channel &, size_t)
{
    WakeBeforeBlockContext *context = g_WakeBeforeBlock;
    if (!context || queue != context->handler->workerWaitQueue() ||
        !context->fired.compareAndSwap(0, 1))
    {
        return;
    }

    context->handler->setNextWork(2);
    if (raise(SIGUSR1) != 0)
    {
        context->failures += 1;
    }
}

bool wakeBeforeBlockRegression()
{
    constexpr const char *Test = "split-irq-wake-before-block";
    HostedSplitIrq handler;
    bool passed = check(handler.start(), "the handler could not start", Test);
    if (!passed)
    {
        return false;
    }

    handler.holdNextBottom();
    handler.setNextWork(1);
    const bool firstRaised = raise(SIGUSR1) == 0;
    const bool firstEntered = waitForSemaphore(handler.bottomEntered);

    WakeBeforeBlockContext context(&handler);
    g_WakeBeforeBlock = &context;
    WaitQueue::setBeforeBlockHook(publishBeforeWorkerBlocks);
    handler.releaseBottom.release();
    const bool firstCompleted = waitForSemaphore(handler.bottomCompleted);
    const bool secondCompleted = waitForSemaphore(handler.bottomCompleted);
    WaitQueue::setBeforeBlockHook(nullptr);
    g_WakeBeforeBlock = nullptr;

    const bool batchesCompleted = waitForCompletedBatches(handler, 2);
    const bool stopped = handler.stop();
    passed &= check(
        firstRaised && firstEntered && firstCompleted && secondCompleted &&
            batchesCompleted,
        "the controlled worker handoff did not complete", Test);
    passed &= check(
        context.fired == 1 && context.failures == 0,
        "the pre-block publication hook did not run exactly once", Test);
    passed &= check(
        handler.hardCalls == 2 && handler.bottomCalls == 2 &&
            handler.observedWork == 3,
        "work published before block was lost or duplicated", Test);
    if (handler.hardCalls.value() != 2 || handler.bottomCalls.value() != 2 ||
        handler.observedWork.value() != 3)
    {
        ERROR(
            "HOSTED-WAIT-TEST: INFO "
            << Test << ": hard=" << handler.hardCalls.value()
            << ", bottom=" << handler.bottomCalls.value()
            << ", work=" << handler.observedWork.value());
    }
    passed &= check(
        handler.hardOutsideSignal == 0 && handler.bottomInsideSignal == 0,
        "hard and threaded callbacks crossed execution contexts", Test);
    passed &= check(
        handler.rearmCalls == 2 && handler.rearmedWork == 3 &&
            handler.quiesceCalls == 2,
        "worker rearm or shutdown quiescence ran out of order", Test);
    passed &= check(
        stopped && handler.pendingWork() == 0 && handler.deferredIrqs() == 2 &&
            handler.completedBatches() == 2 &&
            handler.publicationFailures() == 0,
        "the threaded handler did not stop cleanly", Test);
    if (!stopped || handler.pendingWork() != 0 || handler.deferredIrqs() != 2 ||
        handler.completedBatches() != 2 || handler.publicationFailures() != 0)
    {
        ERROR(
            "HOSTED-WAIT-TEST: INFO "
            << Test << ": stopped=" << stopped << ", pending="
            << handler.pendingWork() << ", deferred=" << handler.deferredIrqs()
            << ", batches=" << handler.completedBatches()
            << ", failures=" << handler.publicationFailures());
    }
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS split-irq-wake-before-block");
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
    bool raised = raise(SIGUSR1) == 0;
    const bool entered = waitForSemaphore(handler.bottomEntered);
    for (size_t work = 2; work <= 8; work <<= 1)
    {
        handler.setNextWork(work);
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
            handler.observedWork == 15 && handler.observedEvents == 4 &&
            handler.observedEventTime == 3906250,
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
    const bool draining = waitForThreadState(shutdown, Thread::CallbackDrain);
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

struct HardCallbackDrainContext
{
    explicit HardCallbackDrainContext(HostedSplitIrq *handler)
        : handler(handler), shutdown(nullptr), phase(0), hookCalls(0),
          hookObservedDrain(0), shutdownFinished(0), shutdownResult(0),
          failures(0)
    {
    }

    HostedSplitIrq *handler;
    Thread *shutdown;
    Atomic<size_t> phase;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookObservedDrain;
    Atomic<size_t> shutdownFinished;
    Atomic<size_t> shutdownResult;
    Atomic<size_t> failures;
};

HardCallbackDrainContext *g_HardCallbackDrain = nullptr;

void holdPinnedHardCallback(IrqHandlerBase *handler)
{
    constexpr size_t YieldLimit = 10000;
    HardCallbackDrainContext *context = g_HardCallbackDrain;
    if (!context || handler != context->handler->hardHandler() ||
        !context->phase.compareAndSwap(0, 1))
    {
        return;
    }

    context->hookCalls += 1;
    for (size_t attempt = 0; attempt < YieldLimit; ++attempt)
    {
        if (context->phase == static_cast<size_t>(2) &&
            hasCallbackDrainWait(
                context->shutdown, context->handler->hardHandler()))
        {
            break;
        }
        Scheduler::instance().yield();
    }

    if (context->phase == static_cast<size_t>(2) &&
        !context->shutdownFinished &&
        hasCallbackDrainWait(
            context->shutdown, context->handler->hardHandler()))
    {
        context->hookObservedDrain += 1;
    }
    else
    {
        context->failures += 1;
    }
    context->phase = 3;
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
    bool passed = check(handler.start(1), "the handler could not start", Test);
    if (!passed)
    {
        return false;
    }

    HardCallbackDrainContext context(&handler);
    context.shutdown = new Thread(
        Scheduler::instance().getKernelProcess(), shutdownPinnedHardCallback,
        &context, nullptr, false, true);
    context.shutdown->setName("hosted pinned split IRQ shutdown");

    g_HardCallbackDrain = &context;
    HostedIrqManager::setHandlerPinHook(holdPinnedHardCallback);
    handler.setNextWork(1);
    const bool raised = raise(SIGUSR2) == 0;
    HostedIrqManager::setHandlerPinHook(nullptr);
    g_HardCallbackDrain = nullptr;
    const bool shutdownJoined = context.shutdown->join();

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
        raised && shutdownJoined && context.hookCalls == 1 &&
            context.hookObservedDrain == 1 && context.failures == 0,
        "shutdown did not wait for the admitted hard callback", Test);
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
    bool passed = wakeBeforeBlockRegression();
    passed &= coalescingRegression();
    passed &= unregisterDrainRegression();
    passed &= atomicShutdownRejectionRegression();
    passed &= hardShutdownRejectionRegression();
    passed &= hardCallbackDrainRegression();
    return passed;
}
