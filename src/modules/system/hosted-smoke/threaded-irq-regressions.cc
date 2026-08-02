/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "system/kernel/machine/mach_pc/PicIrqState.h"

#include <signal.h>

namespace
{
constexpr const char *Test = "irq-threaded-dispatcher-coalescing";

bool check(bool condition, const char *detail, const char *test = Test)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
    return false;
}

struct DispatcherContext
{
    DispatcherContext()
        : firstEntered(0), releaseFirst(0), secondEntered(0),
          publisher(nullptr), dispatcher(nullptr), calls(0), failures(0),
          selfShutdownRejected(false), lines(), cookies(), workers()
    {
    }

    Semaphore firstEntered;
    Semaphore releaseFirst;
    Semaphore secondEntered;
    Thread *publisher;
    ThreadedIrqDispatcher *dispatcher;
    Atomic<size_t> calls;
    Atomic<size_t> failures;
    bool selfShutdownRejected;
    uint8_t lines[2];
    size_t cookies[2];
    Thread *workers[2];
};

void dispatchBatch(void *opaque, uint8_t line, size_t cookie)
{
    DispatcherContext *context = reinterpret_cast<DispatcherContext *>(opaque);
    const size_t call = (context->calls += 1) - 1;
    if (call >= 2)
    {
        context->failures += 1;
        return;
    }

    Thread *current = Processor::information().getCurrentThread();
    context->lines[call] = line;
    context->cookies[call] = cookie;
    context->workers[call] = current;
    if (!current || current == context->publisher ||
        !Processor::getInterrupts() || current->getHostedSignalDepth())
    {
        context->failures += 1;
    }

    if (!call)
    {
        context->selfShutdownRejected =
            context->dispatcher && !context->dispatcher->shutdown();
        context->firstEntered.release();
        if (!context->releaseFirst.acquireForCompletion())
        {
            context->failures += 1;
        }
    }
    else
    {
        context->secondEntered.release();
    }
}

bool threadedDispatcherCoalescing()
{
    DispatcherContext context;
    context.publisher = Processor::information().getCurrentThread();
    ThreadedIrqDispatcher dispatcher(
        MakeConstantString("hosted threaded IRQ regression"), 2,
        dispatchBatch, &context);
    context.dispatcher = &dispatcher;
    const bool initialised = dispatcher.initialise();

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    const bool firstPublished =
        initialised && dispatcher.publishFromInterrupt(1, 1);
    const bool firstDoorbell =
        PerProcessorScheduler::currentIrqWorkDoorbellPendingForTest();
    Processor::setInterrupts(interruptsWereEnabled);
    PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();

    const bool firstObserved =
        firstPublished && context.firstEntered.acquireForCompletion(1, 2, 0);
    bool laterPublished = false;
    bool laterDoorbell = false;
    if (firstObserved)
    {
        Processor::setInterrupts(false);
        laterPublished = dispatcher.publishFromInterrupt(1, 2) &&
                         dispatcher.publishFromInterrupt(1, 3) &&
                         dispatcher.publishFromInterrupt(1, 4);
        laterDoorbell =
            PerProcessorScheduler::currentIrqWorkDoorbellPendingForTest();
        Processor::setInterrupts(interruptsWereEnabled);
        PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();
    }

    context.releaseFirst.release();
    const bool secondObserved =
        laterPublished && context.secondEntered.acquireForCompletion(1, 2, 0);
    const bool stopped = dispatcher.shutdown();

    bool passed = true;
    passed &= check(initialised, "the dispatcher did not initialise");
    passed &= check(firstPublished, "wake-before-block publication failed");
    passed &= check(
        firstDoorbell && laterDoorbell,
        "hard publication did not leave an atomic IRQ-work doorbell", Test);
    passed &= check(firstObserved, "the first worker batch did not enter");
    passed &= check(laterPublished, "a coalesced publication was rejected");
    passed &= check(secondObserved, "the coalesced worker batch did not enter");
    passed &= check(stopped, "the dispatcher did not drain and join");
    passed &= check(
        context.calls == 2 && context.failures == 0,
        "callbacks did not run exactly twice in ordinary thread context");
    passed &= check(
        context.selfShutdownRejected,
        "a callback mutated its own dispatcher before rejecting self-join");
    passed &= check(
        context.lines[0] == 1 && context.lines[1] == 1 &&
            context.cookies[0] == 1 && context.cookies[1] == 4,
        "the pending cookies were not coalesced to the newest occurrence");
    passed &= check(
        context.workers[0] && context.workers[0] == context.workers[1],
        "one physical line did not retain one stable worker");
    passed &= check(
        dispatcher.completedBatchesForTest(1) == 2 &&
            dispatcher.completedCookieForTest(1) == 4,
        "completion generation did not follow callback return");

    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-threaded-dispatcher-coalescing");
    }
    return passed;
}

class HostedThreadedHandler : public IrqHandler
{
  public:
    HostedThreadedHandler()
        : entered(0), publisher(nullptr), hardReturned(0), calls(0),
          callbacksBeforeHardReturn(0), failures(0)
    {
    }

    IrqDisposition irq(irq_id_t) override
    {
        calls += 1;
        if (!hardReturned)
        {
            callbacksBeforeHardReturn += 1;
        }
        Thread *current = Processor::information().getCurrentThread();
        if (!current || current == publisher || !Processor::getInterrupts() ||
            current->getHostedSignalDepth())
        {
            failures += 1;
        }
        entered.release();
        return IrqDisposition::Handled;
    }

    Semaphore entered;
    Thread *publisher;
    Atomic<size_t> hardReturned;
    Atomic<size_t> calls;
    Atomic<size_t> callbacksBeforeHardReturn;
    Atomic<size_t> failures;
};

bool hostedThreadedSignalDelivery()
{
    constexpr const char *SignalTest = "irq-threaded-hosted-signal";
    IrqManager *manager = Machine::instance().getIrqManager();
    HostedThreadedHandler handler;
    handler.publisher = Processor::information().getCurrentThread();

    const irq_id_t id = manager->registerIsaIrqHandler(2, &handler);
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    const bool raised = id && raise(SIGURG) == 0;
    handler.hardReturned = 1;
    const bool deferredUntilHardReturn = handler.calls == 0;
    const bool doorbellPending =
        PerProcessorScheduler::currentIrqWorkDoorbellPendingForTest();
    Processor::setInterrupts(interruptsWereEnabled);
    const Time::Timestamp deadline =
        Time::getTicks() + 2 * Time::Multiplier::Second;
    while (!handler.calls && Time::getTicks() < deadline)
    {
        // Only the natural scheduler tick may move this CPU to the worker.
        Processor::pause();
    }
    const bool observed = raised && handler.calls == 1 &&
                          handler.entered.acquireForCompletion(1, 2, 0);
    const bool removed = id && manager->unregisterHandler(id, &handler);

    bool passed = true;
    passed &= check(
        id != 0, "the threaded handler was not registered", SignalTest);
    passed &= check(raised, "the hosted IRQ signal was not raised", SignalTest);
    passed &= check(
        deferredUntilHardReturn && doorbellPending &&
            handler.callbacksBeforeHardReturn == 0,
        "the bottom half ran before the hard IRQ frame returned", SignalTest);
    passed &= check(
        observed && handler.calls == 1 && handler.failures == 0,
        "the handler did not run once in an enabled ordinary thread",
        SignalTest);
    passed &= check(removed, "the threaded handler was not removed", SignalTest);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-hosted-signal");
    }
    return passed;
}

bool picThreadedTriggerPolicy()
{
    constexpr const char *PolicyTest = "pic-threaded-trigger-policy";
    PicIrqState level;
    level.setAllEnabled(false);
    level.handlerRegistered(10, false);

    const size_t handledGeneration = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    bool passed = check(
        !level.enabled(10) && level.threadedPending(10),
        "a level line was not masked before its bottom half", PolicyTest);
    passed &= check(
        level.completeThreadedDispatch(10, handledGeneration, true) &&
            level.enabled(10) && !level.threadedPending(10),
        "a handled level batch did not rearm its line", PolicyTest);

    const size_t unhandledGeneration = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    passed &= check(
        level.completeThreadedDispatch(10, unhandledGeneration, false) &&
            !level.enabled(10) && level.threadedPending(10),
        "an unhandled level batch reopened an interrupt storm", PolicyTest);
    level.handlerUnregistered(10);
    level.completeThreadedDispatch(10, unhandledGeneration, true);
    passed &= check(
        level.handlerCount(10) == 0 && !level.enabled(10) &&
            !level.threadedPending(10),
        "final unregister or stale completion reopened the line", PolicyTest);

    level.handlerRegistered(10, false);
    const size_t administrativelyDisabled = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    level.setEnabled(10, false);
    passed &= check(
        level.completeThreadedDispatch(10, administrativelyDisabled, true) &&
            !level.enabled(10) && !level.threadedPending(10),
        "bottom-half completion overrode an administrative mask", PolicyTest);
    level.setEnabled(10, true);

    const size_t administrativelyReenabled = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    level.setEnabled(10, false);
    level.setEnabled(10, true);
    passed &= check(
        !level.enabled(10) && level.threadedPending(10),
        "administrative enable overrode an active bottom-half mask",
        PolicyTest);
    passed &= check(
        level.completeThreadedDispatch(10, administrativelyReenabled, true) &&
            level.enabled(10),
        "a completed re-enabled line remained masked", PolicyTest);

    const size_t staleGeneration = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    const size_t currentGeneration = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    passed &= check(
        !level.completeThreadedDispatch(10, staleGeneration, true) &&
            !level.enabled(10) && level.threadedPending(10),
        "a stale bottom half rearmed a newer level occurrence", PolicyTest);
    passed &= check(
        level.completeThreadedDispatch(10, currentGeneration, true) &&
            level.enabled(10) && !level.threadedPending(10),
        "the newest level occurrence could not rearm", PolicyTest);

    PicIrqState edge;
    edge.setAllEnabled(false);
    edge.handlerRegistered(5, true);
    const uint16_t enabledEdgeMask = edge.mask();
    const size_t edgeGeneration = edge.beginDispatch(5);
    edge.beginThreadedDispatch(5);
    passed &= check(
        edge.enabled(5) && !edge.threadedPending(5),
        "an edge line was masked while its worker ran", PolicyTest);
    passed &= check(
        edge.completeThreadedDispatch(5, edgeGeneration, false) &&
            edge.enabled(5) && edge.mask() == enabledEdgeMask,
        "an edge disposition incorrectly controlled line masking", PolicyTest);
    edge.setEnabled(5, false);
    const uint16_t disabledEdgeMask = edge.mask();
    const size_t disabledEdgeGeneration = edge.beginDispatch(5);
    edge.beginThreadedDispatch(5);
    passed &= check(
        edge.completeThreadedDispatch(5, disabledEdgeGeneration, true) &&
            !edge.enabled(5) && edge.mask() == disabledEdgeMask,
        "edge completion overrode an administrative mask", PolicyTest);

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS pic-threaded-trigger-policy");
    }
    return passed;
}
}  // namespace

bool runHostedThreadedIrqRegressions()
{
    bool passed = threadedDispatcherCoalescing();
    passed &= hostedThreadedSignalDelivery();
    passed &= picThreadedTriggerPolicy();
    return passed;
}
