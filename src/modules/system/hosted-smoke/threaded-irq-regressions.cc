/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

namespace
{
constexpr const char *Test = "irq-threaded-dispatcher-coalescing";

bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL " << Test << ": " << detail);
    return false;
}

struct DispatcherContext
{
    DispatcherContext()
        : firstEntered(0), releaseFirst(0), secondEntered(0),
          publisher(nullptr), calls(0), failures(0), lines(), cookies(),
          workers()
    {
    }

    Semaphore firstEntered;
    Semaphore releaseFirst;
    Semaphore secondEntered;
    Thread *publisher;
    Atomic<size_t> calls;
    Atomic<size_t> failures;
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
    ThreadedIrqDispatcher dispatcher(2, dispatchBatch, &context);
    const bool initialised = dispatcher.initialise();

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    const bool firstPublished =
        initialised && dispatcher.publishFromInterrupt(1, 1);
    Processor::setInterrupts(interruptsWereEnabled);

    const bool firstObserved =
        firstPublished && context.firstEntered.acquireForCompletion(1, 2, 0);
    bool laterPublished = false;
    if (firstObserved)
    {
        Processor::setInterrupts(false);
        laterPublished = dispatcher.publishFromInterrupt(1, 2) &&
                         dispatcher.publishFromInterrupt(1, 3) &&
                         dispatcher.publishFromInterrupt(1, 4);
        Processor::setInterrupts(interruptsWereEnabled);
    }

    context.releaseFirst.release();
    const bool secondObserved =
        laterPublished && context.secondEntered.acquireForCompletion(1, 2, 0);
    const bool stopped = dispatcher.shutdown();

    bool passed = true;
    passed &= check(initialised, "the dispatcher did not initialise");
    passed &= check(firstPublished, "wake-before-block publication failed");
    passed &= check(firstObserved, "the first worker batch did not enter");
    passed &= check(laterPublished, "a coalesced publication was rejected");
    passed &= check(secondObserved, "the coalesced worker batch did not enter");
    passed &= check(stopped, "the dispatcher did not drain and join");
    passed &= check(
        context.calls == 2 && context.failures == 0,
        "callbacks did not run exactly twice in ordinary thread context");
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
}  // namespace

bool runHostedThreadedIrqRegressions()
{
    return threadedDispatcherCoalescing();
}
