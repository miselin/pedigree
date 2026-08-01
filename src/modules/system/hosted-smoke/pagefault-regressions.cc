/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/PageFaultHandler.h"
#include "pedigree/kernel/time/Time.h"

namespace
{
bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR(
        "HOSTED-WAIT-TEST: FAIL pagefault-handler-lifetime: "
        << detail);
    return false;
}

struct HandlerLifetimeContext;
HandlerLifetimeContext *g_HandlerLifetimeContext = nullptr;

class LifetimeHandler : public MemoryTrapHandler
{
  public:
    explicit LifetimeHandler(HandlerLifetimeContext &context)
        : m_Context(context)
    {
    }

    bool trap(InterruptState &, uintptr_t, bool) override;

  private:
    HandlerLifetimeContext &m_Context;
};

struct HandlerLifetimeContext
{
    explicit HandlerLifetimeContext(PageFaultHandler *registry)
        : registry(registry), handler(*this), remover(nullptr),
          competitor(nullptr), phase(0), competitorStart(0), hookCalls(0),
          hookObservedDrain(0), handlerCalls(0), callbacksAfterReturn(0),
          unregisterReturned(0), unregisterSucceeded(0),
          competitorReturned(0), competitorRejected(0), failures(0)
    {
    }

    PageFaultHandler *registry;
    LifetimeHandler handler;
    Thread *remover;
    Thread *competitor;
    Atomic<size_t> phase;
    Atomic<size_t> competitorStart;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookObservedDrain;
    Atomic<size_t> handlerCalls;
    Atomic<size_t> callbacksAfterReturn;
    Atomic<size_t> unregisterReturned;
    Atomic<size_t> unregisterSucceeded;
    Atomic<size_t> competitorReturned;
    Atomic<size_t> competitorRejected;
    Atomic<size_t> failures;
};

bool LifetimeHandler::trap(InterruptState &, uintptr_t, bool)
{
    m_Context.handlerCalls += 1;
    if (m_Context.unregisterReturned)
    {
        m_Context.callbacksAfterReturn += 1;
    }
    return true;
}

class SelfRemovingHandler : public MemoryTrapHandler
{
  public:
    explicit SelfRemovingHandler(PageFaultHandler *registry)
        : m_Registry(registry), calls(0), rejectionSeen(0)
    {
    }

    bool trap(InterruptState &, uintptr_t, bool) override
    {
        calls += 1;
        if (!m_Registry->unregisterHandler(this))
        {
            rejectionSeen += 1;
        }
        return true;
    }

    PageFaultHandler *m_Registry;
    Atomic<size_t> calls;
    Atomic<size_t> rejectionSeen;
};

void handlerPinHook(MemoryTrapHandler *handler)
{
    HandlerLifetimeContext *context = g_HandlerLifetimeContext;
    if (
        !context || handler != &context->handler ||
        !context->phase.compareAndSwap(0, 1))
    {
        return;
    }

    context->hookCalls += 1;
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (
        Time::getTicks() < deadline)
    {
        Thread::WaitDebugInfo info = {};
        uintptr_t debugAddress = 0;
        if (
            context->phase == static_cast<size_t>(2) &&
            context->remover->getWaitDebugInfo(info) && info.queued &&
            context->remover->getDebugState(debugAddress) ==
                Thread::CallbackDrain &&
            debugAddress ==
                reinterpret_cast<uintptr_t>(&context->handler))
        {
            break;
        }
        Scheduler::instance().yield();
    }

    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (
        context->phase == static_cast<size_t>(2) &&
        context->remover->getWaitDebugInfo(info) && info.queued &&
        context->remover->getDebugState(debugAddress) ==
            Thread::CallbackDrain &&
        debugAddress == reinterpret_cast<uintptr_t>(&context->handler) &&
        !context->unregisterReturned)
    {
        context->hookObservedDrain += 1;
        context->competitorStart = 1;
        while (
            !context->competitorReturned &&
            Time::getTicks() < deadline)
        {
            Scheduler::instance().yield();
        }
        if (!context->competitorReturned)
        {
            context->failures += 1;
        }
        context->remover->setUnwindState(Thread::TerminateThread);
    }
    else
    {
        context->failures += 1;
    }
    context->phase = 3;
}

int unregisterPinnedHandler(void *parameter)
{
    HandlerLifetimeContext *context =
        reinterpret_cast<HandlerLifetimeContext *>(parameter);
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (
        context->phase != static_cast<size_t>(1) &&
        Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }

    if (context->phase != static_cast<size_t>(1))
    {
        context->failures += 1;
        return 1;
    }

    context->phase = 2;
    if (context->registry->unregisterHandler(&context->handler))
    {
        context->unregisterSucceeded += 1;
    }
    context->unregisterReturned += 1;
    context->phase = 4;
    return 0;
}

int competingUnregister(void *parameter)
{
    HandlerLifetimeContext *context =
        reinterpret_cast<HandlerLifetimeContext *>(parameter);
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (!context->competitorStart && Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }

    if (!context->competitorStart)
    {
        context->failures += 1;
        return 1;
    }

    if (!context->registry->unregisterHandler(&context->handler))
    {
        context->competitorRejected += 1;
    }
    context->competitorReturned += 1;
    return 0;
}

bool handlerLifetimeBarrier()
{
    PageFaultHandler *registry = &PageFaultHandler::instance();
    HandlerLifetimeContext context(registry);
    context.remover = new Thread(
        Scheduler::instance().getKernelProcess(), unregisterPinnedHandler,
        &context, nullptr, false, true);
    context.remover->setName("hosted pagefault-handler remover");
    context.competitor = new Thread(
        Scheduler::instance().getKernelProcess(), competingUnregister,
        &context, nullptr, false, true);
    context.competitor->setName("hosted pagefault-handler competitor");

    g_HandlerLifetimeContext = &context;
    PageFaultHandler::setHandlerPinHook(handlerPinHook);
    const bool registered = registry->registerHandler(&context.handler);
    const bool duplicateRejected =
        !registry->registerHandler(&context.handler);
    const bool dispatched =
        registry->dispatchHandlerForTest(&context.handler);
    const bool joined = context.remover->join();
    const bool competitorJoined = context.competitor->join();
    PageFaultHandler::setHandlerPinHook(nullptr);
    g_HandlerLifetimeContext = nullptr;

    if (!context.unregisterSucceeded)
    {
        registry->unregisterHandler(&context.handler);
    }

    const size_t callsAtUnregisterReturn = context.handlerCalls;
    const bool lateDispatchRejected =
        !registry->dispatchHandlerForTest(&context.handler);
    const bool unregisterBoundaryHeld =
        lateDispatchRejected &&
        context.handlerCalls == callsAtUnregisterReturn &&
        context.callbacksAfterReturn == 0;

    const bool reregistered = registry->registerHandler(&context.handler);
    const bool redispatched =
        registry->dispatchHandlerForTest(&context.handler);
    const bool recleaned =
        reregistered && registry->unregisterHandler(&context.handler);

    SelfRemovingHandler selfRemoving(registry);
    const bool selfRegistered = registry->registerHandler(&selfRemoving);
    const bool selfDispatched =
        registry->dispatchHandlerForTest(&selfRemoving);
    const bool selfLateDispatchRejected =
        !registry->dispatchHandlerForTest(&selfRemoving);
    const bool selfReregistered =
        registry->registerHandler(&selfRemoving);
    const bool selfCleanup =
        selfReregistered && registry->unregisterHandler(&selfRemoving);

    bool passed = true;
    passed &= check(
        registered && duplicateRejected && dispatched,
        "registration, duplicate rejection, or initial dispatch failed");
    passed &= check(
        joined && competitorJoined && context.failures == 0,
        "the concurrent unregister workers did not finish cleanly");
    passed &= check(
        context.hookCalls == 1 && context.hookObservedDrain == 1,
        "unregister did not wait for the callback pin");
    passed &= check(
        context.unregisterSucceeded == 1 &&
            context.unregisterReturned == 1 &&
            context.competitorReturned == 1 &&
            context.competitorRejected == 1,
        "terminal teardown abandoned the drain or a second drainer was admitted");
    passed &= check(
        unregisterBoundaryHeld,
        "a callback began after unregisterHandler returned");
    passed &= check(
        reregistered && redispatched && recleaned &&
            context.handlerCalls == callsAtUnregisterReturn + 1,
        "a drained handler could not be registered and dispatched again");
    passed &= check(
        selfRegistered && selfDispatched &&
            selfRemoving.rejectionSeen == 1 &&
            selfRemoving.calls == 1 && selfLateDispatchRejected,
        "self-unregister did not reject its own drain and retire on return");
    passed &= check(
        selfReregistered && selfCleanup,
        "deferred self-removal did not release its registry slot");

    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS pagefault-handler-lifetime");
    }
    return passed;
}

struct AbandonedDispatchContext;
AbandonedDispatchContext *g_AbandonedDispatchContext = nullptr;

class AbandonedDispatchHandler : public MemoryTrapHandler
{
  public:
    explicit AbandonedDispatchHandler(AbandonedDispatchContext &context)
        : m_Context(context)
    {
    }

    bool trap(InterruptState &, uintptr_t, bool) override;

  private:
    AbandonedDispatchContext &m_Context;
};

struct AbandonedDispatchContext
{
    explicit AbandonedDispatchContext(PageFaultHandler *registry)
        : registry(registry), handler(*this), remover(nullptr),
          dispatcher(nullptr), phase(0), hookCalls(0), hookObservedDrain(0),
          handlerCalls(0), dispatchReturned(0), unregisterReturned(0),
          unregisterSucceeded(0), failures(0)
    {
    }

    PageFaultHandler *registry;
    AbandonedDispatchHandler handler;
    Thread *remover;
    Thread *dispatcher;
    Atomic<size_t> phase;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookObservedDrain;
    Atomic<size_t> handlerCalls;
    Atomic<size_t> dispatchReturned;
    Atomic<size_t> unregisterReturned;
    Atomic<size_t> unregisterSucceeded;
    Atomic<size_t> failures;
};

bool AbandonedDispatchHandler::trap(InterruptState &, uintptr_t, bool)
{
    m_Context.handlerCalls += 1;
    return true;
}

void abandonedDispatchPinHook(MemoryTrapHandler *handler)
{
    AbandonedDispatchContext *context = g_AbandonedDispatchContext;
    if (
        !context || handler != &context->handler ||
        !context->phase.compareAndSwap(0, 1))
    {
        return;
    }

    context->hookCalls += 1;
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (Time::getTicks() < deadline)
    {
        Thread::WaitDebugInfo info = {};
        uintptr_t debugAddress = 0;
        if (
            context->phase == static_cast<size_t>(2) &&
            context->remover->getWaitDebugInfo(info) && info.queued &&
            context->remover->getDebugState(debugAddress) ==
                Thread::CallbackDrain &&
            debugAddress == reinterpret_cast<uintptr_t>(&context->handler))
        {
            context->hookObservedDrain += 1;
            Processor::information()
                .getCurrentThread()
                ->getScheduler()
                ->killCurrentThread();
        }
        Scheduler::instance().yield();
    }

    // Reaching the deadline means the unregister path never published its
    // drain wait. Returning would exercise the ordinary cleanup path instead
    // of the abandoned-stack path this regression is intended to prove.
    context->failures += 1;
    Processor::information()
        .getCurrentThread()
        ->getScheduler()
        ->killCurrentThread();
}

int unregisterAbandonedDispatch(void *parameter)
{
    AbandonedDispatchContext *context =
        reinterpret_cast<AbandonedDispatchContext *>(parameter);
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (
        context->phase != static_cast<size_t>(1) &&
        Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }

    if (context->phase != static_cast<size_t>(1))
    {
        context->failures += 1;
        return 1;
    }

    context->phase = 2;
    if (context->registry->unregisterHandler(&context->handler))
    {
        context->unregisterSucceeded += 1;
    }
    context->unregisterReturned += 1;
    return 0;
}

int dispatchThenAbandon(void *parameter)
{
    AbandonedDispatchContext *context =
        reinterpret_cast<AbandonedDispatchContext *>(parameter);
    context->registry->dispatchHandlerForTest(&context->handler);
    context->dispatchReturned += 1;
    return 1;
}

bool abandonedDispatchCleanup()
{
    PageFaultHandler *registry = &PageFaultHandler::instance();
    AbandonedDispatchContext context(registry);
    context.remover = new Thread(
        Scheduler::instance().getKernelProcess(),
        unregisterAbandonedDispatch, &context, nullptr, false, true);
    context.remover->setName("hosted abandoned pagefault remover");
    context.dispatcher = new Thread(
        Scheduler::instance().getKernelProcess(), dispatchThenAbandon,
        &context, nullptr, false, true, true);
    context.dispatcher->setName("hosted abandoned pagefault dispatch");

    g_AbandonedDispatchContext = &context;
    PageFaultHandler::setHandlerPinHook(abandonedDispatchPinHook);
    const bool registered = registry->registerHandler(&context.handler);
    const bool started = context.dispatcher->start();
    const bool dispatcherJoined = context.dispatcher->join();
    const bool removerJoined = context.remover->join();
    PageFaultHandler::setHandlerPinHook(nullptr);
    g_AbandonedDispatchContext = nullptr;

    if (!context.unregisterSucceeded)
    {
        registry->unregisterHandler(&context.handler);
    }
    const bool lateDispatchRejected =
        !registry->dispatchHandlerForTest(&context.handler);

    const bool passed =
        check(
            registered && started && dispatcherJoined && removerJoined,
            "the abandoned-dispatch workers did not complete") &&
        check(
            context.failures == 0 && context.hookCalls == 1 &&
                context.hookObservedDrain == 1,
            "the forced abandonment window was not reached") &&
        check(
            context.handlerCalls == 0 && context.dispatchReturned == 0,
            "the abandoned fault stack returned through normal dispatch") &&
        check(
            context.unregisterSucceeded == 1 &&
                context.unregisterReturned == 1 && lateDispatchRejected,
            "state cleanup did not release the pinned callback exactly once");
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS pagefault-abandoned-dispatch");
    }
    return passed;
}
}  // namespace

bool runHostedPageFaultRegressions()
{
    return handlerLifetimeBarrier() && abandonedDispatchCleanup();
}
