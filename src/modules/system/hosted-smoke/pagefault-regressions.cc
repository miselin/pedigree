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
bool check(
    bool condition, const char *detail,
    const char *test = "pagefault-handler-lifetime")
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
    return false;
}

struct RegistryDispatchContext;
RegistryDispatchContext *g_RegistryDispatchContext = nullptr;
void dispatchWhileWriterLocked();
void dispatchWhileDeferredScopeLocked();

class RegistryDispatchHandler : public MemoryTrapHandler
{
  public:
    explicit RegistryDispatchHandler(RegistryDispatchContext &context)
        : m_Context(context)
    {
    }

    bool trap(InterruptState &, uintptr_t, bool) override;

  private:
    RegistryDispatchContext &m_Context;
};

struct RegistryDispatchContext
{
    explicit RegistryDispatchContext(PageFaultHandler *registry)
        : registry(registry), handler(*this), calls(0), writerRequested(0),
          writerHookCalls(0), writerDispatches(0),
          deferredScopeRequested(0), deferredScopeHookCalls(0),
          deferredScopeDispatches(0), prePinHookCalls(0),
          unregisterSucceeded(0)
    {
    }

    PageFaultHandler *registry;
    RegistryDispatchHandler handler;
    Atomic<size_t> calls;
    Atomic<size_t> writerRequested;
    Atomic<size_t> writerHookCalls;
    Atomic<size_t> writerDispatches;
    Atomic<size_t> deferredScopeRequested;
    Atomic<size_t> deferredScopeHookCalls;
    Atomic<size_t> deferredScopeDispatches;
    Atomic<size_t> prePinHookCalls;
    Atomic<size_t> unregisterSucceeded;
};

bool RegistryDispatchHandler::trap(InterruptState &, uintptr_t, bool)
{
    m_Context.calls += 1;
    if (m_Context.writerRequested.compareAndSwap(1, 2))
    {
        m_Context.registry->withMutationLockForTest(
            dispatchWhileWriterLocked);
    }
    if (m_Context.deferredScopeRequested.compareAndSwap(1, 2))
    {
        Processor::information()
            .getCurrentThread()
            ->withDeferredScopeLockForTest(
                dispatchWhileDeferredScopeLocked);
    }
    return true;
}

void dispatchWhileWriterLocked()
{
    RegistryDispatchContext *context = g_RegistryDispatchContext;
    if (!context)
    {
        return;
    }

    context->writerHookCalls += 1;
    if (context->registry->dispatchHandlerForTest(&context->handler))
    {
        context->writerDispatches += 1;
    }
}

void dispatchWhileDeferredScopeLocked()
{
    RegistryDispatchContext *context = g_RegistryDispatchContext;
    if (!context)
    {
        return;
    }

    context->deferredScopeHookCalls += 1;
    if (context->registry->dispatchHandlerForTest(&context->handler))
    {
        context->deferredScopeDispatches += 1;
    }
}

bool lockIndependentDispatch()
{
    constexpr const char *Test = "pagefault-lock-reentry";
    PageFaultHandler *registry = &PageFaultHandler::instance();
    RegistryDispatchContext context(registry);
    const bool registered = registry->registerHandler(&context.handler);

    g_RegistryDispatchContext = &context;
    context.writerRequested = 1;
    const bool writerDispatched =
        registry->dispatchHandlerForTest(&context.handler);
    context.deferredScopeRequested = 1;
    const bool deferredScopeDispatched =
        registry->dispatchHandlerForTest(&context.handler);
    g_RegistryDispatchContext = nullptr;

    const bool cleaned =
        registered && registry->unregisterHandler(&context.handler);
    bool passed = true;
    passed &= check(
        registered, "the test handler could not be registered", Test);
    passed &= check(
        writerDispatched && context.writerHookCalls == 1 &&
            context.writerDispatches == 1,
        "dispatch or callback return waited for the registry writer lock",
        Test);
    passed &= check(
        deferredScopeDispatched && context.deferredScopeHookCalls == 1 &&
            context.deferredScopeDispatches == 1 && context.calls == 4,
        "dispatch entered the interrupted Thread's cleanup publication lock",
        Test);
    passed &= check(
        registry->activeDispatchCountForTest(&context.handler) == 0,
        "nested dispatch leaked a callback hazard", Test);
    passed &= check(cleaned, "the test handler could not be removed", Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS pagefault-lock-reentry");
    }
    return passed;
}

void unregisterBeforePin(MemoryTrapHandler *handler)
{
    RegistryDispatchContext *context = g_RegistryDispatchContext;
    if (!context || handler != &context->handler ||
        !context->prePinHookCalls.compareAndSwap(0, 1))
    {
        return;
    }

    if (context->registry->unregisterHandler(&context->handler))
    {
        context->unregisterSucceeded += 1;
    }
}

bool admissionCloseRevalidation()
{
    constexpr const char *Test = "pagefault-admission-close-race";
    PageFaultHandler *registry = &PageFaultHandler::instance();
    RegistryDispatchContext context(registry);
    const bool registered = registry->registerHandler(&context.handler);
    const size_t callsBeforeDispatch = context.calls;

    g_RegistryDispatchContext = &context;
    PageFaultHandler::setHandlerPrePinHook(unregisterBeforePin);
    const bool staleDispatch =
        registry->dispatchHandlerForTest(&context.handler);
    PageFaultHandler::setHandlerPrePinHook(nullptr);
    g_RegistryDispatchContext = nullptr;

    const bool reused = registry->registerHandler(&context.handler);
    const bool cleaned =
        reused && registry->unregisterHandler(&context.handler);
    bool passed = true;
    passed &= check(
        registered, "the test handler could not be registered", Test);
    passed &= check(
        context.prePinHookCalls == 1 && context.unregisterSucceeded == 1,
        "unregister did not retire the pre-pin publication", Test);
    passed &= check(
        !staleDispatch && context.calls == callsBeforeDispatch,
        "a stale pre-pin snapshot entered the retired callback", Test);
    passed &= check(
        registry->activeDispatchCountForTest(&context.handler) == 0,
        "rejected admission leaked a callback hazard", Test);
    passed &= check(
        reused && cleaned,
        "the revalidated slot could not be reused and removed", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS pagefault-admission-close-race");
    }
    return passed;
}

struct AtomicReopenContext;
AtomicReopenContext *g_AtomicReopenContext = nullptr;

class AtomicReopenHandler : public MemoryTrapHandler
{
  public:
    explicit AtomicReopenHandler(AtomicReopenContext &context)
        : m_Context(context)
    {
    }

    bool trap(InterruptState &, uintptr_t, bool) override;

  private:
    AtomicReopenContext &m_Context;
};

struct AtomicReopenContext
{
    explicit AtomicReopenContext(PageFaultHandler *registry)
        : registry(registry), handler(*this), remover(nullptr), phase(0),
          handlerCalls(0), pinHookCalls(0), drainHookCalls(0),
          selfRemovalRejected(0), atomicRemovalReturned(0),
          atomicRemovalRejected(0), failures(0)
    {
    }

    PageFaultHandler *registry;
    AtomicReopenHandler handler;
    Thread *remover;
    Atomic<size_t> phase;
    Atomic<size_t> handlerCalls;
    Atomic<size_t> pinHookCalls;
    Atomic<size_t> drainHookCalls;
    Atomic<size_t> selfRemovalRejected;
    Atomic<size_t> atomicRemovalReturned;
    Atomic<size_t> atomicRemovalRejected;
    Atomic<size_t> failures;
};

bool AtomicReopenHandler::trap(InterruptState &, uintptr_t, bool)
{
    m_Context.handlerCalls += 1;
    if (m_Context.phase != static_cast<size_t>(2))
    {
        m_Context.failures += 1;
        return true;
    }

    if (!m_Context.registry->unregisterHandler(this))
    {
        m_Context.selfRemovalRejected += 1;
    }
    m_Context.phase = 3;
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (!m_Context.atomicRemovalReturned &&
           Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }
    if (!m_Context.atomicRemovalReturned)
    {
        m_Context.failures += 1;
    }
    return true;
}

void atomicReopenPinHook(MemoryTrapHandler *handler)
{
    AtomicReopenContext *context = g_AtomicReopenContext;
    if (!context || handler != &context->handler ||
        !context->phase.compareAndSwap(0, 1))
    {
        return;
    }

    context->pinHookCalls += 1;
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (context->phase != static_cast<size_t>(2) &&
           Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }
    if (context->phase != static_cast<size_t>(2))
    {
        context->failures += 1;
    }
}

void atomicReopenDrainHook(MemoryTrapHandler *handler)
{
    AtomicReopenContext *context = g_AtomicReopenContext;
    if (!context || handler != &context->handler)
    {
        return;
    }

    context->drainHookCalls += 1;
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(true);
    context->phase = 2;
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (context->phase != static_cast<size_t>(3) &&
           Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }
    if (context->phase != static_cast<size_t>(3))
    {
        context->failures += 1;
    }
    Processor::setInterrupts(interruptsWereEnabled);
}

int atomicReopenRemover(void *parameter)
{
    AtomicReopenContext *context =
        reinterpret_cast<AtomicReopenContext *>(parameter);
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (context->phase != static_cast<size_t>(1) &&
           Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }
    if (context->phase != static_cast<size_t>(1))
    {
        context->failures += 1;
        return 1;
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    if (!context->registry->unregisterHandler(&context->handler))
    {
        context->atomicRemovalRejected += 1;
    }
    Processor::setInterrupts(interruptsWereEnabled);
    context->atomicRemovalReturned += 1;
    return 0;
}

bool atomicReopenPreservesSelfRemoval()
{
    constexpr const char *Test =
        "pagefault-atomic-reopen-self-removal";
    PageFaultHandler *registry = &PageFaultHandler::instance();
    AtomicReopenContext context(registry);
    context.remover = new Thread(
        Scheduler::instance().getKernelProcess(), atomicReopenRemover,
        &context, nullptr, false, true);
    context.remover->setName("hosted atomic pagefault remover");

    g_AtomicReopenContext = &context;
    PageFaultHandler::setHandlerPinHook(atomicReopenPinHook);
    PageFaultHandler::setAtomicDrainHook(atomicReopenDrainHook);
    const bool registered = registry->registerHandler(&context.handler);
    const bool dispatched =
        registry->dispatchHandlerForTest(&context.handler);
    const bool removerJoined = context.remover->join();
    PageFaultHandler::setAtomicDrainHook(nullptr);
    PageFaultHandler::setHandlerPinHook(nullptr);
    g_AtomicReopenContext = nullptr;

    const bool lateDispatchRejected =
        !registry->dispatchHandlerForTest(&context.handler);
    const bool hazardsDrained =
        registry->activeDispatchCountForTest(&context.handler) == 0;
    const bool reused = registry->registerHandler(&context.handler);
    const bool cleaned =
        reused && registry->unregisterHandler(&context.handler);

    bool passed = true;
    passed &= check(
        registered && dispatched && removerJoined,
        "the atomic-removal workers did not complete", Test);
    passed &= check(
        context.failures == 0 && context.pinHookCalls == 1 &&
            context.drainHookCalls == 1 && context.handlerCalls == 1,
        "the atomic reopen interleaving was not reached", Test);
    passed &= check(
        context.selfRemovalRejected == 1 &&
            context.atomicRemovalReturned == 1 &&
            context.atomicRemovalRejected == 1,
        "atomic reopen erased or replaced callback self-removal", Test);
    passed &= check(
        lateDispatchRejected && hazardsDrained,
        "the self-removal publication remained enabled after callback return",
        Test);
    passed &= check(
        reused && cleaned, "the retired slot was not reusable", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "pagefault-atomic-reopen-self-removal");
    }
    return passed;
}

struct NestedDispatchContext;

class OuterNestedHandler : public MemoryTrapHandler
{
  public:
    explicit OuterNestedHandler(NestedDispatchContext &context)
        : m_Context(context)
    {
    }

    bool trap(InterruptState &, uintptr_t, bool) override;

  private:
    NestedDispatchContext &m_Context;
};

class InnerNestedHandler : public MemoryTrapHandler
{
  public:
    explicit InnerNestedHandler(NestedDispatchContext &context)
        : m_Context(context)
    {
    }

    bool trap(InterruptState &, uintptr_t, bool) override;

  private:
    NestedDispatchContext &m_Context;
};

struct NestedDispatchContext
{
    explicit NestedDispatchContext(PageFaultHandler *registry)
        : registry(registry), outer(*this), inner(*this), orderCount(0),
          innerDispatches(0), selfRemovalRejected(0)
    {
        for (size_t &entry : order)
        {
            entry = 0;
        }
    }

    void record(size_t value)
    {
        if (orderCount < 4)
        {
            order[orderCount++] = value;
        }
    }

    PageFaultHandler *registry;
    OuterNestedHandler outer;
    InnerNestedHandler inner;
    size_t order[4];
    size_t orderCount;
    size_t innerDispatches;
    size_t selfRemovalRejected;
};

bool OuterNestedHandler::trap(InterruptState &, uintptr_t, bool)
{
    m_Context.record(1);
    if (m_Context.registry->dispatchHandlerForTest(&m_Context.inner))
    {
        ++m_Context.innerDispatches;
    }
    m_Context.record(4);
    return true;
}

bool InnerNestedHandler::trap(InterruptState &, uintptr_t, bool)
{
    m_Context.record(2);
    if (!m_Context.registry->unregisterHandler(this))
    {
        ++m_Context.selfRemovalRejected;
    }
    m_Context.record(3);
    return true;
}

bool nestedDispatchRemoval()
{
    constexpr const char *Test = "pagefault-nested-dispatch";
    PageFaultHandler *registry = &PageFaultHandler::instance();
    NestedDispatchContext context(registry);
    const bool outerRegistered = registry->registerHandler(&context.outer);
    const bool innerRegistered = registry->registerHandler(&context.inner);
    const bool dispatched =
        registry->dispatchHandlerForTest(&context.outer);
    const bool innerLateDispatchRejected =
        !registry->dispatchHandlerForTest(&context.inner);
    const bool hazardsDrained =
        registry->activeDispatchCountForTest(&context.outer) == 0 &&
        registry->activeDispatchCountForTest(&context.inner) == 0;
    const bool outerCleaned =
        outerRegistered && registry->unregisterHandler(&context.outer);
    const bool innerReused = registry->registerHandler(&context.inner);
    const bool innerCleaned =
        innerReused && registry->unregisterHandler(&context.inner);

    bool passed = true;
    passed &= check(
        outerRegistered && innerRegistered && dispatched,
        "the nested handlers could not be registered and dispatched", Test);
    passed &= check(
        context.innerDispatches == 1 && context.selfRemovalRejected == 1 &&
            context.orderCount == 4 && context.order[0] == 1 &&
            context.order[1] == 2 && context.order[2] == 3 &&
            context.order[3] == 4,
        "nested dispatch did not unwind in strict LIFO order", Test);
    passed &= check(
        innerLateDispatchRejected && hazardsDrained,
        "nested self-removal did not retire after the final hazard", Test);
    passed &= check(
        outerCleaned && innerReused && innerCleaned,
        "nested handlers did not leave reusable registry slots", Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS pagefault-nested-dispatch");
    }
    return passed;
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
          competitorReturned(0), competitorRejected(0),
          selfRemovalDuringDrain(0), failures(0)
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
    Atomic<size_t> selfRemovalDuringDrain;
    Atomic<size_t> failures;
};

bool LifetimeHandler::trap(InterruptState &, uintptr_t, bool)
{
    m_Context.handlerCalls += 1;
    if (m_Context.unregisterReturned)
    {
        m_Context.callbacksAfterReturn += 1;
    }
    if (m_Context.phase == static_cast<size_t>(3) &&
        !m_Context.registry->unregisterHandler(this))
    {
        m_Context.selfRemovalDuringDrain += 1;
    }
    return true;
}

bool hasCallbackDrainWait(Thread *thread, MemoryTrapHandler *handler)
{
    Thread::WaitDebugInfo wait = {};
    uintptr_t debugAddress = 0;
    return thread && thread->getWaitDebugInfo(wait) && wait.queue &&
           wait.channelOwner && wait.channelValue && wait.queued &&
           wait.reason == WaitQueue::WakeReason::Waiting &&
           thread->getDebugState(debugAddress) == Thread::CallbackDrain &&
           debugAddress == reinterpret_cast<uintptr_t>(handler);
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
        if (
            context->phase == static_cast<size_t>(2) &&
            hasCallbackDrainWait(
                context->remover, &context->handler))
        {
            break;
        }
        Scheduler::instance().yield();
    }

    if (
        context->phase == static_cast<size_t>(2) &&
        hasCallbackDrainWait(context->remover, &context->handler) &&
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
            context.competitorRejected == 1 &&
            context.selfRemovalDuringDrain == 1,
        "draining self-removal was lost or a second drainer was admitted");
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
            "HOSTED-WAIT-TEST: PASS pagefault-handler-waitqueue-drain");
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
        if (
            context->phase == static_cast<size_t>(2) &&
            hasCallbackDrainWait(
                context->remover, &context->handler))
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
    const size_t activeDispatches =
        registry->activeDispatchCountForTest(&context.handler);
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
            activeDispatches == 0,
            "stack abandonment leaked an active callback hazard") &&
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
    bool passed = lockIndependentDispatch();
    passed &= admissionCloseRevalidation();
    passed &= atomicReopenPreservesSelfRemoval();
    passed &= nestedDispatchRemoval();
    passed &= handlerLifetimeBarrier();
    passed &= abandonedDispatchCleanup();
    return passed;
}
