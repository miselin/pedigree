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
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/time/Time.h"
#include "system/kernel/machine/hosted/IrqManager.h"

namespace
{
bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL irq-handler-lifetime: " << detail);
    return false;
}

struct HandlerLifetimeContext;
HandlerLifetimeContext *g_HandlerLifetimeContext = nullptr;

class LifetimeHandler : public IrqHandler
{
  public:
    explicit LifetimeHandler(HandlerLifetimeContext &context)
        : m_Context(context)
    {
    }

    bool irq(irq_id_t, InterruptState &) override;

  private:
    HandlerLifetimeContext &m_Context;
};

struct HandlerLifetimeContext
{
    explicit HandlerLifetimeContext(IrqManager *manager)
        : manager(manager), handler(*this), id(0), remover(nullptr), phase(0),
          hookCalls(0), hookObservedDrain(0), handlerCalls(0),
          callbacksAfterReturn(0), unregisterReturned(0),
          unregisterSucceeded(0), failures(0)
    {
    }

    IrqManager *manager;
    LifetimeHandler handler;
    irq_id_t id;
    Thread *remover;
    Atomic<size_t> phase;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookObservedDrain;
    Atomic<size_t> handlerCalls;
    Atomic<size_t> callbacksAfterReturn;
    Atomic<size_t> unregisterReturned;
    Atomic<size_t> unregisterSucceeded;
    Atomic<size_t> failures;
};

bool LifetimeHandler::irq(irq_id_t, InterruptState &)
{
    m_Context.handlerCalls += 1;
    if (m_Context.unregisterReturned)
    {
        m_Context.callbacksAfterReturn += 1;
    }
    return true;
}

class SelfRemovingHandler : public IrqHandler
{
  public:
    SelfRemovingHandler(IrqManager *manager, irq_id_t id)
        : m_Manager(manager), m_Id(id), calls(0), rejectionSeen(0)
    {
    }

    bool irq(irq_id_t, InterruptState &) override
    {
        calls += 1;
        if (!m_Manager->unregisterHandler(m_Id, this))
        {
            rejectionSeen += 1;
        }
        return true;
    }

    IrqManager *m_Manager;
    irq_id_t m_Id;
    Atomic<size_t> calls;
    Atomic<size_t> rejectionSeen;
};

void handlerPinHook(IrqHandler *handler)
{
    HandlerLifetimeContext *context = g_HandlerLifetimeContext;
    if (
        !context || handler != &context->handler ||
        !context->phase.compareAndSwap(0, 1))
    {
        return;
    }

    context->hookCalls += 1;
    // Hosted IRQ signals remain masked while this signal frame is live, so a
    // tick-based deadline cannot advance here. Bound scheduler handoffs
    // directly while the remover reaches the callback-drain queue.
    for (size_t attempt = 0; attempt < 10000; ++attempt)
    {
        Thread::WaitDebugInfo info = {};
        uintptr_t debugAddress = 0;
        if (
            context->phase == static_cast<size_t>(2) &&
            context->remover->getWaitDebugInfo(info) && info.queue &&
            info.channelOwner && info.queued &&
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
        !context->unregisterReturned &&
        context->remover->getWaitDebugInfo(info) && info.queue &&
        info.channelOwner && info.queued &&
        context->remover->getDebugState(debugAddress) ==
            Thread::CallbackDrain &&
        debugAddress == reinterpret_cast<uintptr_t>(&context->handler))
    {
        context->hookObservedDrain += 1;
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
    if (context->manager->unregisterHandler(context->id, &context->handler))
    {
        context->unregisterSucceeded += 1;
    }
    context->unregisterReturned += 1;
    context->phase = 4;
    return 0;
}

bool handlerLifetimeBarrier()
{
    IrqManager *manager = Machine::instance().getIrqManager();
    HandlerLifetimeContext context(manager);

    // Learn the manager-owned identifier before enabling the pin hook. The
    // identifier is assigned after registerIsaIrqHandler returns, while an
    // IRQ is free to arrive as soon as the slot becomes visible.
    context.id = manager->registerIsaIrqHandler(0, &context.handler);
    const bool identifierSeeded =
        context.id && manager->unregisterHandler(context.id, &context.handler);

    context.remover = new Thread(
        Scheduler::instance().getKernelProcess(), unregisterPinnedHandler,
        &context, nullptr, false, true);
    context.remover->setName("hosted IRQ-handler remover");

    g_HandlerLifetimeContext = &context;
    HostedIrqManager::setHandlerPinHook(handlerPinHook);
    const irq_id_t activeId =
        manager->registerIsaIrqHandler(0, &context.handler);
    const bool registered =
        identifierSeeded && activeId && activeId == context.id;
    const bool joined = context.remover->join();
    HostedIrqManager::setHandlerPinHook(nullptr);
    g_HandlerLifetimeContext = nullptr;

    bool primaryCleanup = true;
    if (activeId && !context.unregisterSucceeded)
    {
        primaryCleanup =
            manager->unregisterHandler(activeId, &context.handler);
    }

    const size_t callsAtUnregisterReturn = context.handlerCalls;
    const Time::Timestamp postReturnDeadline =
        Time::getTicks() + (10 * Time::Multiplier::Millisecond);
    while (Time::getTicks() < postReturnDeadline)
    {
        Scheduler::instance().yield();
    }

    SelfRemovingHandler selfRemoving(manager, context.id);
    const irq_id_t selfId =
        manager->registerIsaIrqHandler(0, &selfRemoving);
    const Time::Timestamp selfDeadline =
        Time::getTicks() + (250 * Time::Multiplier::Millisecond);
    while (!selfRemoving.calls && Time::getTicks() < selfDeadline)
    {
        Scheduler::instance().yield();
    }
    const size_t selfCallsAfterRetirement = selfRemoving.calls;
    const Time::Timestamp selfQuietDeadline =
        Time::getTicks() + (10 * Time::Multiplier::Millisecond);
    while (Time::getTicks() < selfQuietDeadline)
    {
        Scheduler::instance().yield();
    }

    // A callback cannot wait for its own pin. The rejected synchronous
    // contract still closes admission and retires the slot on return.
    const irq_id_t selfReregisteredId =
        manager->registerIsaIrqHandler(0, &selfRemoving);
    const bool selfReregistered = selfReregisteredId != 0;
    bool selfCleanup = false;
    if (selfReregistered)
    {
        selfCleanup =
            manager->unregisterHandler(selfReregisteredId, &selfRemoving);
    }
    else if (selfId)
    {
        // Keep a failed assertion from leaving a stack-owned handler in the
        // live registry.
        selfCleanup = manager->unregisterHandler(selfId, &selfRemoving);
    }

    bool passed = true;
    passed &= check(
        registered, "the test handler could not be registered consistently");
    passed &= check(
        joined && context.failures == 0,
        "the concurrent unregister worker did not complete cleanly");
    passed &= check(
        context.hookCalls == 1 && context.hookObservedDrain == 1,
        "unregister returned instead of waiting for the pinned callback");
    passed &= check(
        context.unregisterSucceeded == 1 &&
            context.unregisterReturned == 1,
        "the pinned handler did not unregister successfully");
    passed &= check(
        primaryCleanup,
        "failed primary assertions left the test handler registered");
    passed &= check(
        context.handlerCalls >= 1 &&
            context.handlerCalls == callsAtUnregisterReturn &&
            context.callbacksAfterReturn == 0,
        "a callback began after unregisterHandler returned");
    passed &= check(
        selfId != 0 && selfRemoving.rejectionSeen == 1 &&
            selfCallsAfterRetirement == 1 &&
            selfRemoving.calls == selfCallsAfterRetirement,
        "self-unregister was not deferred and retired after callback return");
    passed &= check(
        selfReregistered && selfCleanup,
        "deferred self-removal did not release its registry slot");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-handler-lifetime");
    }
    return passed;
}
}  // namespace

bool runHostedIrqRegressions()
{
    return handlerLifetimeBarrier();
}
