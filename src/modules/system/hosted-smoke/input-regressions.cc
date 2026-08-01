/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"

namespace
{
constexpr size_t Attempts = 10000;

bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL input-callback-lifetime: " << detail);
    return false;
}

struct CallbackLifetimeContext
{
    CallbackLifetimeContext()
        : removers{nullptr, nullptr}, phase(0), removersStarted(0), hookCalls(0),
          hookObservedDrain(0), callbackCalls(0), callbackAfterRemoval(0),
          removalReturned(0), failures(0)
    {
    }

    Thread *removers[2];
    Atomic<size_t> phase;
    Atomic<size_t> removersStarted;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookObservedDrain;
    Atomic<size_t> callbackCalls;
    Atomic<size_t> callbackAfterRemoval;
    Atomic<size_t> removalReturned;
    Atomic<size_t> failures;
};

CallbackLifetimeContext *g_CallbackLifetimeContext = nullptr;

void lifetimeCallback(InputManager::InputNotification &notification)
{
    CallbackLifetimeContext *context =
        reinterpret_cast<CallbackLifetimeContext *>(notification.meta);
    context->callbackCalls += 1;
    if (context->removalReturned)
    {
        context->callbackAfterRemoval += 1;
    }
}

void callbackPinHook(InputManager::callback_t callback, void *meta)
{
    CallbackLifetimeContext *context = g_CallbackLifetimeContext;
    if (
        !context || callback != lifetimeCallback || meta != context ||
        !context->phase.compareAndSwap(0, 1))
    {
        return;
    }

    context->hookCalls += 1;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        bool allDraining = context->removersStarted == 2;
        for (size_t i = 0; allDraining && i < 2; ++i)
        {
            Thread::WaitDebugInfo info = {};
            uintptr_t debugAddress = 0;
            allDraining &=
                context->removers[i]->getWaitDebugInfo(info) && info.queue &&
                info.channelOwner && info.queued &&
                context->removers[i]->getDebugState(debugAddress) ==
                    Thread::CallbackDrain &&
                debugAddress ==
                    reinterpret_cast<uintptr_t>(lifetimeCallback);
        }
        if (allDraining)
        {
            context->hookObservedDrain += 1;
            break;
        }
        Scheduler::instance().yield();
    }

    if (!context->hookObservedDrain)
    {
        context->failures += 1;
    }
    context->phase = 2;
}

int removePinnedCallback(void *parameter)
{
    CallbackLifetimeContext *context =
        reinterpret_cast<CallbackLifetimeContext *>(parameter);
    for (
        size_t attempt = 0;
        attempt < Attempts &&
        context->phase != static_cast<size_t>(1);
        ++attempt)
    {
        Scheduler::instance().yield();
    }

    if (context->phase != static_cast<size_t>(1))
    {
        context->failures += 1;
        return 1;
    }

    context->removersStarted += 1;
    InputManager::instance().removeCallback(lifetimeCallback, context);
    context->removalReturned += 1;
    return 0;
}

struct SelfRemovalContext
{
    SelfRemovalContext() : calls(0)
    {
    }

    Atomic<size_t> calls;
};

void selfRemovingCallback(InputManager::InputNotification &notification)
{
    SelfRemovalContext *context =
        reinterpret_cast<SelfRemovalContext *>(notification.meta);
    context->calls += 1;
    InputManager::instance().removeCallback(selfRemovingCallback, context);
}

bool callbackLifetimeBarrier()
{
    InputManager &manager = InputManager::instance();
    CallbackLifetimeContext context;
    for (size_t i = 0; i < 2; ++i)
    {
        context.removers[i] = new Thread(
            Scheduler::instance().getKernelProcess(), removePinnedCallback,
            &context, nullptr, false, true);
        context.removers[i]->setName("hosted input-callback remover");
    }

    manager.installCallback(InputManager::Key, lifetimeCallback, &context);
    g_CallbackLifetimeContext = &context;
    InputManager::setCallbackPinHook(callbackPinHook);
    manager.keyPressed('i');

    bool joined = true;
    for (auto remover : context.removers)
    {
        joined &= remover->join();
    }
    InputManager::setCallbackPinHook(nullptr);
    g_CallbackLifetimeContext = nullptr;

    // Failed setup must not leave a module callback in the core registry.
    manager.removeCallback(lifetimeCallback, &context);

    for (size_t attempt = 0; attempt < 100; ++attempt)
    {
        Scheduler::instance().yield();
    }
    const size_t callsAfterRemoval = context.callbackCalls;
    manager.keyPressed('j');
    for (size_t attempt = 0; attempt < 100; ++attempt)
    {
        Scheduler::instance().yield();
    }

    SelfRemovalContext selfRemoval;
    manager.installCallback(
        InputManager::Key, selfRemovingCallback, &selfRemoval);
    manager.keyPressed('k');
    for (
        size_t attempt = 0;
        attempt < Attempts && !selfRemoval.calls; ++attempt)
    {
        Scheduler::instance().yield();
    }
    const size_t callsAfterSelfRemoval = selfRemoval.calls;
    manager.keyPressed('l');
    for (size_t attempt = 0; attempt < 100; ++attempt)
    {
        Scheduler::instance().yield();
    }
    manager.removeCallback(selfRemovingCallback, &selfRemoval);

    bool passed = true;
    passed &= check(
        joined && context.failures == 0,
        "the concurrent removal worker did not complete cleanly");
    passed &= check(
        context.hookCalls == 1 && context.hookObservedDrain == 1,
        "removal did not wait on the committed callback invocation");
    passed &= check(
        context.callbackCalls == 1 &&
            context.callbackCalls == callsAfterRemoval &&
            context.callbackAfterRemoval == 0 &&
            context.removalReturned == 2,
        "a callback began after removeCallback returned");
    passed &= check(
        callsAfterSelfRemoval == 1 && selfRemoval.calls == 1,
        "self-removal did not retire the callback after its active invocation");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS input-callback-lifetime");
    }
    return passed;
}
}  // namespace

bool runHostedInputRegressions()
{
    return callbackLifetimeBarrier();
}
