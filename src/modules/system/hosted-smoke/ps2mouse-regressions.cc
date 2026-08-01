/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "modules/drivers/x86/ps2mouse/Ps2MouseCallbackRegistry.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/time/Time.h"

namespace
{
bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL ps2mouse-callback-lifetime: " << detail);
    return false;
}

struct CallbackLifetimeContext
{
    CallbackLifetimeContext()
        : registration(), remover(nullptr), phase(0), hookCalls(0),
          hookObservedDrain(0), callbackCalls(0), callbacksAfterReturn(0),
          unregisterReturned(0), failures(0)
    {
    }

    Ps2MouseCallbackRegistry::Registration registration;
    Thread *remover;
    Atomic<size_t> phase;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookObservedDrain;
    Atomic<size_t> callbackCalls;
    Atomic<size_t> callbacksAfterReturn;
    Atomic<size_t> unregisterReturned;
    Atomic<size_t> failures;
};

CallbackLifetimeContext *g_CallbackLifetimeContext = nullptr;

void lifetimeCallback(void *parameter, const void *, size_t)
{
    CallbackLifetimeContext *context =
        reinterpret_cast<CallbackLifetimeContext *>(parameter);
    context->callbackCalls += 1;
    if (context->unregisterReturned)
    {
        context->callbacksAfterReturn += 1;
    }
}

void callbackPinHook(
    Ps2MouseCallbackRegistry::Handler handler, void *parameter)
{
    CallbackLifetimeContext *context = g_CallbackLifetimeContext;
    if (
        !context || handler != lifetimeCallback || parameter != context ||
        !context->phase.compareAndSwap(0, 1))
    {
        return;
    }

    context->hookCalls += 1;
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
            debugAddress == reinterpret_cast<uintptr_t>(context))
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
        debugAddress == reinterpret_cast<uintptr_t>(context))
    {
        context->hookObservedDrain += 1;
    }
    else
    {
        context->failures += 1;
    }
    context->phase = 3;
}

int unregisterPinnedCallback(void *parameter)
{
    CallbackLifetimeContext *context =
        reinterpret_cast<CallbackLifetimeContext *>(parameter);
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
    context->registration.reset();
    context->unregisterReturned += 1;
    context->phase = 4;
    return 0;
}

struct SelfRemovalContext
{
    SelfRemovalContext() : registration(), calls(0)
    {
    }

    Ps2MouseCallbackRegistry::Registration registration;
    Atomic<size_t> calls;
};

void selfRemovingCallback(void *parameter, const void *, size_t)
{
    SelfRemovalContext *context =
        reinterpret_cast<SelfRemovalContext *>(parameter);
    context->calls += 1;
    context->registration.reset();
}

bool callbackLifetimeBarrier()
{
    Ps2MouseCallbackRegistry registry;
    CallbackLifetimeContext context;
    context.remover = new Thread(
        Scheduler::instance().getKernelProcess(), unregisterPinnedCallback,
        &context, nullptr, false, true);
    context.remover->setName("hosted PS/2 callback remover");

    g_CallbackLifetimeContext = &context;
    registry.setCallbackPinHook(callbackPinHook);
    const bool registered =
        registry.subscribe(lifetimeCallback, &context, context.registration);
    const bool duplicateRejected =
        !registry.subscribe(
            lifetimeCallback, &context, context.registration);
    const uint8_t byte = 0x7F;
    registry.dispatch(&byte, 1);
    const bool joined = context.remover->join();
    registry.setCallbackPinHook(nullptr);
    g_CallbackLifetimeContext = nullptr;

    const size_t callsAtUnregisterReturn = context.callbackCalls;
    registry.dispatch(&byte, 1);
    const bool lateDispatchRejected =
        context.callbackCalls == callsAtUnregisterReturn &&
        context.callbacksAfterReturn == 0;

    const bool reregistered =
        registry.subscribe(
            lifetimeCallback, &context, context.registration);
    if (reregistered)
    {
        registry.dispatch(&byte, 1);
        context.registration.reset();
    }

    SelfRemovalContext selfRemoval;
    const bool selfRegistered =
        registry.subscribe(
            selfRemovingCallback, &selfRemoval, selfRemoval.registration);
    if (selfRegistered)
    {
        registry.dispatch(&byte, 1);
        registry.dispatch(&byte, 1);
    }
    const bool selfReregistered =
        registry.subscribe(
            selfRemovingCallback, &selfRemoval, selfRemoval.registration);
    selfRemoval.registration.reset();

    bool passed = true;
    passed &= check(
        registered && duplicateRejected,
        "registration or duplicate rejection failed");
    passed &= check(
        joined && context.failures == 0,
        "the concurrent callback remover did not finish cleanly");
    passed &= check(
        context.hookCalls == 1 && context.hookObservedDrain == 1 &&
            context.unregisterReturned == 1,
        "unregister returned instead of waiting for the admitted callback");
    passed &= check(
        context.callbackCalls >= 1 && lateDispatchRejected,
        "a callback began after its registration was reset");
    passed &= check(
        reregistered && context.callbackCalls == callsAtUnregisterReturn + 1,
        "the drained callback slot could not be reused");
    passed &= check(
        selfRegistered && selfRemoval.calls == 1 &&
            !selfRemoval.registration && selfReregistered,
        "self-removal did not retire the callback after it returned");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS ps2mouse-callback-lifetime");
    }
    return passed;
}
}  // namespace

bool runHostedPs2MouseRegressions()
{
    return callbackLifetimeBarrier();
}
