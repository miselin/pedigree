/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "modules/system/network-stack/NetworkStack.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/utilities/new"

namespace
{
class HostedNetworkDevice final : public Network
{
  public:
    bool send(size_t, uintptr_t) override
    {
        return true;
    }
};

struct ReceiveAbaContext
{
    explicit ReceiveAbaContext(Network *device)
        : device(device), workerEntered(0), allowDispatch(0), buffer(0),
          generation(0), queued(0), beforeDispatch(0), delivered(0),
          staleDiscards(0), cancellations(0), failures(0)
    {
    }

    Network *device;
    Semaphore workerEntered;
    Semaphore allowDispatch;
    Atomic<uintptr_t> buffer;
    Atomic<size_t> generation;
    Atomic<size_t> queued;
    Atomic<size_t> beforeDispatch;
    Atomic<size_t> delivered;
    Atomic<size_t> staleDiscards;
    Atomic<size_t> cancellations;
    Atomic<size_t> failures;
};

ReceiveAbaContext *g_ReceiveAbaContext = nullptr;

void receiveHook(
    NetworkStack::HostedReceiveEvent event, uintptr_t buffer, Network *device,
    size_t generation)
{
    ReceiveAbaContext *context = g_ReceiveAbaContext;
    if (!context || device != context->device)
    {
        return;
    }

    if (event == NetworkStack::HostedReceiveEvent::Queued)
    {
        context->queued += 1;
        if (!context->buffer.compareAndSwap(0, buffer))
        {
            context->failures += 1;
        }
        context->generation = generation;
        return;
    }

    if (buffer != static_cast<uintptr_t>(context->buffer))
    {
        return;
    }

    switch (event)
    {
        case NetworkStack::HostedReceiveEvent::BeforeDispatch:
            if ((context->beforeDispatch += 1) != 1)
            {
                context->failures += 1;
                return;
            }
            context->workerEntered.release();
            if (!context->allowDispatch.acquireForCompletion())
            {
                context->failures += 1;
            }
            break;
        case NetworkStack::HostedReceiveEvent::Delivered:
            context->delivered += 1;
            break;
        case NetworkStack::HostedReceiveEvent::DiscardedStale:
            context->staleDiscards += 1;
            break;
        case NetworkStack::HostedReceiveEvent::Cancelled:
            context->cancellations += 1;
            break;
        case NetworkStack::HostedReceiveEvent::Queued:
            break;
    }
}

bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-NETWORK-TEST: FAIL receive-generation-aba: " << detail);
    return false;
}

bool queuedReceiveGenerationAba()
{
    NetworkStack &stack = NetworkStack::instance();
    alignas(HostedNetworkDevice)
        uint8_t deviceStorage[sizeof(HostedNetworkDevice)];
    HostedNetworkDevice *original = new (deviceStorage) HostedNetworkDevice();

    stack.registerDevice(original);
    const size_t originalGeneration =
        NetworkStack::getHostedRegistrationGeneration(original);
    bool passed = true;
    passed &= check(
        stack.getInterface(original) && originalGeneration,
        "the original device was not registered");

    ReceiveAbaContext context(original);
    g_ReceiveAbaContext = &context;
    NetworkStack::setHostedReceiveHook(receiveHook);

    uint8_t packet[64] = {};
    stack.receive(
        sizeof(packet), reinterpret_cast<uintptr_t>(packet), original, 0);

    const bool workerHeld = context.workerEntered.acquire(1, 2);
    HostedNetworkDevice *replacement = nullptr;
    size_t replacementGeneration = 0;
    bool unregistered = false;
    bool reusedAddress = false;
    bool replacementRegistered = false;

    if (workerHeld)
    {
        stack.deRegisterDevice(original);
        unregistered = !stack.getInterface(original) &&
                       !NetworkStack::getHostedRegistrationGeneration(original);
        original->~HostedNetworkDevice();

        replacement = new (deviceStorage) HostedNetworkDevice();
        reusedAddress = replacement == original;
        stack.registerDevice(replacement);
        replacementGeneration =
            NetworkStack::getHostedRegistrationGeneration(replacement);
        replacementRegistered =
            stack.getInterface(replacement) && replacementGeneration;
    }

    // Also makes a late-arriving worker safe after a timeout above.
    context.allowDispatch.release();
    const bool drained = stack.drain();

    NetworkStack::setHostedReceiveHook(nullptr);
    g_ReceiveAbaContext = nullptr;

    if (replacement)
    {
        stack.deRegisterDevice(replacement);
        replacement->~HostedNetworkDevice();
    }
    else
    {
        stack.deRegisterDevice(original);
        original->~HostedNetworkDevice();
    }

    const size_t terminalOwnershipEvents =
        static_cast<size_t>(context.delivered) +
        static_cast<size_t>(context.staleDiscards) +
        static_cast<size_t>(context.cancellations);

    passed &= check(
        workerHeld && drained && !context.failures,
        "the queued receive could not be held and drained deterministically");
    passed &= check(
        context.queued == 1 && context.beforeDispatch == 1 &&
            context.generation == originalGeneration,
        "the queued request did not retain the original registration");
    passed &= check(
        unregistered && reusedAddress && replacementRegistered &&
            replacementGeneration != originalGeneration,
        "unregister/re-register did not create a new identity at one address");
    passed &= check(
        context.staleDiscards == 1 && !context.delivered &&
            !context.cancellations,
        "stale queued work reached the replacement or the wrong release path");
    passed &= check(
        terminalOwnershipEvents == 1,
        "the receive buffer did not have exactly one terminal owner");

    if (passed)
    {
        NOTICE("HOSTED-NETWORK-TEST: PASS receive-generation-aba");
    }
    return passed;
}
}  // namespace

bool runHostedNetworkStackRegressions()
{
    return queuedReceiveGenerationAba();
}
