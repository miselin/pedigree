/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/machine/IrqHandlerRegistry.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/assert.h"
#include "system/kernel/core/processor/DeviceHardIrqContext.h"

static_assert(
    __atomic_always_lock_free(sizeof(size_t), nullptr),
    "IRQ callback hazard words must be lock-free");
static_assert(
    __atomic_always_lock_free(sizeof(void *), nullptr),
    "IRQ callback hazard pointers must be lock-free");

IrqHandlerRegistry::IrqHandlerRegistry()
    : m_Handlers(), m_ActiveDispatches(),
      m_ThreadedInvalidationGenerations(),
      m_ThreadedActionMutationGeneration(0),
      m_ThreadedActionMutationWriters(0), m_OccurrenceEpochs(),
      m_OccurrenceReaders(), m_OccurrenceBoundaryLocks(),
      m_HandlerLock(false), m_AdmissionEpoch(0), m_MutationGeneration(0),
      m_MutationWriters(0)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_HandlerPinHook(nullptr), m_HandlerPrePinHook(nullptr),
      m_HandlerHazardHook(nullptr), m_DispatchAbandonHook(nullptr)
#endif
{
}

size_t IrqHandlerRegistry::makePublication(
    size_t generation, uint8_t irq, SlotMode mode, Delivery delivery)
{
    return (generation << GenerationShift) |
           (static_cast<size_t>(irq) << IrqShift) |
           (static_cast<size_t>(delivery) << DeliveryShift) |
           static_cast<size_t>(mode);
}

size_t IrqHandlerRegistry::generationOf(size_t publication)
{
    return publication >> GenerationShift;
}

uint8_t IrqHandlerRegistry::irqOf(size_t publication)
{
    return static_cast<uint8_t>((publication & IrqMask) >> IrqShift);
}

IrqHandlerRegistry::SlotMode IrqHandlerRegistry::modeOf(size_t publication)
{
    return static_cast<SlotMode>(publication & ModeMask);
}

IrqHandlerRegistry::Delivery IrqHandlerRegistry::deliveryOf(size_t publication)
{
    return static_cast<Delivery>((publication & DeliveryMask) >> DeliveryShift);
}

bool IrqHandlerRegistry::generationReached(size_t current, size_t target)
{
    return static_cast<intptr_t>(current - target) >= 0;
}

bool IrqHandlerRegistry::threadedGenerationValid(
    uint8_t irq, size_t generation) const
{
    if (!generation)
    {
        return false;
    }

    const size_t invalidThrough = __atomic_load_n(
        &m_ThreadedInvalidationGenerations[irq], __ATOMIC_ACQUIRE);
    return !invalidThrough || !generationReached(invalidThrough, generation);
}

void IrqHandlerRegistry::publishSlotQuiesced(
    HandlerSlot &slot, uint8_t irq, size_t dispatchGeneration)
{
    if (!dispatchGeneration)
    {
        return;
    }

    ThreadedActionMutationCleanup actionMutation(this);
    beginThreadedActionMutation(actionMutation);
    if (!threadedGenerationValid(irq, dispatchGeneration))
    {
        finishThreadedActionMutation(actionMutation);
        return;
    }

    const size_t rolledBack = __atomic_load_n(
        &slot.rolledBackThreadedGeneration, __ATOMIC_ACQUIRE);
    if (rolledBack == dispatchGeneration)
    {
        dispatchGeneration = __atomic_load_n(
            &slot.previousThreadedGeneration, __ATOMIC_ACQUIRE);
        if (!dispatchGeneration ||
            !threadedGenerationValid(irq, dispatchGeneration))
        {
            finishThreadedActionMutation(actionMutation);
            return;
        }
    }

    publishSlotQuiescedValue(slot, irq, dispatchGeneration);
    finishThreadedActionMutation(actionMutation);
}

void IrqHandlerRegistry::publishSlotQuiescedValue(
    HandlerSlot &slot, uint8_t irq, size_t dispatchGeneration)
{
    if (!dispatchGeneration ||
        !threadedGenerationValid(irq, dispatchGeneration))
    {
        return;
    }

    size_t current = __atomic_load_n(
        &slot.quiescedThreadedGeneration, __ATOMIC_ACQUIRE);
    while (!current || generationReached(dispatchGeneration, current))
    {
        if (__atomic_compare_exchange_n(
                &slot.quiescedThreadedGeneration, &current,
                dispatchGeneration, false, __ATOMIC_RELEASE,
                __ATOMIC_ACQUIRE))
        {
            const size_t finalRollback = __atomic_load_n(
                &slot.rolledBackThreadedGeneration, __ATOMIC_ACQUIRE);
            if (!threadedGenerationValid(irq, dispatchGeneration) ||
                finalRollback == dispatchGeneration)
            {
                size_t stale = dispatchGeneration;
                size_t replacement = 0;
                if (finalRollback == dispatchGeneration)
                {
                    replacement = __atomic_load_n(
                        &slot.previousThreadedGeneration, __ATOMIC_ACQUIRE);
                    if (!threadedGenerationValid(irq, replacement))
                    {
                        replacement = 0;
                    }
                }
                __atomic_compare_exchange_n(
                    &slot.quiescedThreadedGeneration, &stale, replacement,
                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            }
            return;
        }
    }
}

void IrqHandlerRegistry::invalidateThreadedLine(
    uint8_t irq, size_t throughGeneration)
{
    if (!throughGeneration)
    {
        return;
    }

    ThreadedActionMutationCleanup actionMutation(this);
    beginThreadedActionMutation(actionMutation);
    size_t invalidThrough = __atomic_load_n(
        &m_ThreadedInvalidationGenerations[irq], __ATOMIC_ACQUIRE);
    while (!invalidThrough || generationReached(throughGeneration, invalidThrough))
    {
        if (__atomic_compare_exchange_n(
                &m_ThreadedInvalidationGenerations[irq], &invalidThrough,
                throughGeneration, false, __ATOMIC_RELEASE,
                __ATOMIC_ACQUIRE))
        {
            break;
        }
    }
    if (invalidThrough && generationReached(invalidThrough, throughGeneration))
    {
        throughGeneration = invalidThrough;
    }

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (irqOf(publication) != irq ||
            deliveryOf(publication) != Delivery::Threaded)
        {
            continue;
        }

        void *owner = currentDispatchOwner();
        Thread *thread = Processor::information().getCurrentThread();
        DispatchCleanup cleanup(this, &slot, owner, publication, false);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerHazardHook mutationHook =
            __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
        if (mutationHook)
        {
            mutationHook(
                __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE),
                HandlerHazardStage::BeforeActionMutationPin);
        }
#endif
        if (!pinActionMutation(slot, publication, cleanup, thread))
        {
            continue;
        }

        size_t *actions[] = {
            &slot.pendingThreadedGeneration,
            &slot.previousThreadedGeneration,
            &slot.claimedThreadedGeneration,
            &slot.quiescedThreadedGeneration};
        for (size_t action = 0; action < 4; ++action)
        {
            size_t generation = __atomic_load_n(actions[action], __ATOMIC_ACQUIRE);
            while (generation &&
                   generationReached(throughGeneration, generation))
            {
                if (__atomic_compare_exchange_n(
                        actions[action], &generation, static_cast<size_t>(0),
                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                {
                    break;
                }
            }
        }
        unpinActionMutation(slot, publication, cleanup, thread);
    }
    finishThreadedActionMutation(actionMutation);
    tryReclaimTombstones(irq);
}

void IrqHandlerRegistry::cancelThreadedDispatch(
    uint8_t irq, size_t dispatchGeneration)
{
    if (!dispatchGeneration)
    {
        return;
    }

    ThreadedActionMutationCleanup actionMutation(this);
    beginThreadedActionMutation(actionMutation);
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (irqOf(publication) != irq ||
            deliveryOf(publication) != Delivery::Threaded)
        {
            continue;
        }

        void *owner = currentDispatchOwner();
        Thread *thread = Processor::information().getCurrentThread();
        DispatchCleanup cleanup(this, &slot, owner, publication, false);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerHazardHook mutationHook =
            __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
        if (mutationHook)
        {
            mutationHook(
                __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE),
                HandlerHazardStage::BeforeActionMutationPin);
        }
#endif
        if (!pinActionMutation(slot, publication, cleanup, thread))
        {
            continue;
        }

        const size_t previous = __atomic_load_n(
            &slot.previousThreadedGeneration, __ATOMIC_ACQUIRE);
        ThreadedCancellationCleanup cancellation(
            this, &slot, irq, dispatchGeneration, previous);
        cancellation.thread = thread;
        cancellation.previousInterruptState = Processor::getInterrupts();
        Processor::setInterrupts(false);
        cancellation.restoreInterruptState = true;
        if (thread)
        {
            thread->armAtomicStateCleanup(
                cancellation.cleanup, abandonThreadedCancellation,
                &cancellation);
        }
        applyThreadedCancellation(cancellation);
        if (thread)
        {
            thread->disarmAtomicStateCleanup(cancellation.cleanup);
        }
        cancellation.registry = nullptr;
        cancellation.restoreInterruptState = false;
        Processor::setInterrupts(cancellation.previousInterruptState);
        unpinActionMutation(slot, publication, cleanup, thread);
    }
    finishThreadedActionMutation(actionMutation);
    tryReclaimTombstones(irq);
}

size_t IrqHandlerRegistry::encodePolicy(const IrqPolicy *policy)
{
    if (!policy)
    {
        return 0;
    }

    return PolicyValid |
           (static_cast<size_t>(policy->trigger()) << PolicyTriggerShift) |
           (static_cast<size_t>(policy->controllerAck())
            << PolicyControllerAckShift) |
           (static_cast<size_t>(policy->lineRelease())
            << PolicyLineReleaseShift);
}

void IrqHandlerRegistry::decodePolicy(
    size_t policy, LineConfiguration &configuration)
{
    configuration.policyConfigured = policy & PolicyValid;
    if (!configuration.policyConfigured)
    {
        return;
    }

    configuration.trigger =
        static_cast<IrqTrigger>((policy >> PolicyTriggerShift) & 3);
    configuration.controllerAck =
        static_cast<IrqControllerAck>((policy >> PolicyControllerAckShift) & 3);
    configuration.lineRelease =
        static_cast<IrqLineRelease>((policy >> PolicyLineReleaseShift) & 1);
}

bool IrqHandlerRegistry::mixedPoliciesCompatible(size_t first, size_t second)
{
    if (!(first & PolicyValid) || !(second & PolicyValid))
    {
        return false;
    }

    return (first & PolicyMixedCompatibilityMask) ==
           (second & PolicyMixedCompatibilityMask);
}

size_t IrqHandlerRegistry::effectiveMixedPolicy(
    size_t hard, size_t threaded)
{
    size_t effective = hard;
    if (threaded & PolicyLineReleaseMask)
    {
        effective |= PolicyLineReleaseMask;
    }
    return effective;
}

IrqHandlerRegistry::LineMode IrqHandlerRegistry::lineModeForDelivery(
    Delivery delivery)
{
    return delivery == Delivery::Threaded ? LineMode::Threaded :
                                            LineMode::HardOnly;
}

void IrqHandlerRegistry::beginMutation()
{
    // A global epoch keeps the registry compact. Unrelated line churn can only
    // make a diagnostic attempt conservatively fail.
    __atomic_add_fetch(
        &m_MutationWriters, static_cast<size_t>(1), __ATOMIC_SEQ_CST);
}

void IrqHandlerRegistry::finishMutation()
{
    // Publish the new epoch before dropping the final writer. Snapshot readers
    // sample the writer count before the epoch at their closing boundary.
    __atomic_add_fetch(
        &m_MutationGeneration, static_cast<size_t>(1), __ATOMIC_SEQ_CST);
    __atomic_sub_fetch(
        &m_MutationWriters, static_cast<size_t>(1), __ATOMIC_SEQ_CST);
}

void IrqHandlerRegistry::beginThreadedActionMutation(
    ThreadedActionMutationCleanup &cleanup)
{
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    cleanup.thread = Processor::information().getCurrentThread();
    if (cleanup.thread)
    {
        cleanup.thread->armAtomicStateCleanup(
            cleanup.cleanup, abandonThreadedActionMutation, &cleanup);
    }
    __atomic_add_fetch(
        &m_ThreadedActionMutationWriters, static_cast<size_t>(1),
        __ATOMIC_SEQ_CST);
    Processor::setInterrupts(interruptsWereEnabled);
}

void IrqHandlerRegistry::finishThreadedActionMutation(
    ThreadedActionMutationCleanup &cleanup)
{
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    completeThreadedActionMutation();
    if (cleanup.thread)
    {
        cleanup.thread->disarmAtomicStateCleanup(cleanup.cleanup);
    }
    cleanup.registry = nullptr;
    Processor::setInterrupts(interruptsWereEnabled);
}

void IrqHandlerRegistry::completeThreadedActionMutation()
{
    __atomic_add_fetch(
        &m_ThreadedActionMutationGeneration, static_cast<size_t>(1),
        __ATOMIC_SEQ_CST);
    const size_t previous = __atomic_fetch_sub(
        &m_ThreadedActionMutationWriters, static_cast<size_t>(1),
        __ATOMIC_SEQ_CST);
    if (!previous)
    {
        __atomic_store_n(
            &m_ThreadedActionMutationWriters, static_cast<size_t>(0),
            __ATOMIC_SEQ_CST);
        FATAL_NOLOCK("IRQ action-mutation writer count underflowed.");
    }
}

void IrqHandlerRegistry::abandonThreadedActionMutation(void *context)
{
    ThreadedActionMutationCleanup *cleanup =
        reinterpret_cast<ThreadedActionMutationCleanup *>(context);
    if (!cleanup || !cleanup->registry)
    {
        return;
    }

    cleanup->registry->completeThreadedActionMutation();
    cleanup->registry = nullptr;
}

void IrqHandlerRegistry::applyThreadedCancellation(
    ThreadedCancellationCleanup &cleanup)
{
    HandlerSlot &slot = *cleanup.slot;
    __atomic_store_n(
        &slot.rolledBackThreadedGeneration, cleanup.dispatchGeneration,
        __ATOMIC_RELEASE);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HandlerHazardHook mutationHook =
        __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
    if (mutationHook)
    {
        mutationHook(
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE),
            HandlerHazardStage::CancellationMarkerPublished);
    }
#endif

    size_t pending = cleanup.dispatchGeneration;
    __atomic_compare_exchange_n(
        &slot.pendingThreadedGeneration, &pending,
        cleanup.previousThreadedGeneration, false, __ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE);

    size_t quiesced = cleanup.dispatchGeneration;
    __atomic_compare_exchange_n(
        &slot.quiescedThreadedGeneration, &quiesced,
        cleanup.previousThreadedGeneration, false, __ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE);

    const size_t claimed = __atomic_load_n(
        &slot.claimedThreadedGeneration, __ATOMIC_ACQUIRE);
    if (claimed == cleanup.dispatchGeneration)
    {
        publishSlotQuiescedValue(
            slot, cleanup.irq, cleanup.previousThreadedGeneration);
        size_t exactClaim = cleanup.dispatchGeneration;
        if (__atomic_compare_exchange_n(
            &slot.claimedThreadedGeneration, &exactClaim,
            static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE))
        {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            mutationHook =
                __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
            if (mutationHook)
            {
                mutationHook(
                    __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE),
                    HandlerHazardStage::CancellationClaimCleared);
            }
#endif
        }
    }
}

void IrqHandlerRegistry::abandonThreadedCancellation(void *context)
{
    ThreadedCancellationCleanup *cleanup =
        reinterpret_cast<ThreadedCancellationCleanup *>(context);
    if (!cleanup || !cleanup->registry)
    {
        return;
    }

    Processor::setInterrupts(false);
    cleanup->registry->applyThreadedCancellation(*cleanup);
    cleanup->registry = nullptr;
    if (cleanup->restoreInterruptState)
    {
        const bool previousInterruptState = cleanup->previousInterruptState;
        cleanup->restoreInterruptState = false;
        Processor::setInterrupts(previousInterruptState);
    }
}

bool IrqHandlerRegistry::canWaitForActionFinalization()
{
    Thread *current = Processor::information().getCurrentThread();
    bool canWait = current && Processor::getInterrupts() &&
                   !Processor::inDeviceHardIrq();
#if HOSTED
    canWait = canWait && !current->getHostedSignalDepth();
#endif
    return canWait;
}

bool IrqHandlerRegistry::acquireFinalizationGate(
    HandlerSlot &slot, bool canWait)
{
    bool contentionReported = false;
    while (true)
    {
        size_t expected = 0;
        if (__atomic_compare_exchange_n(
                &slot.finalizationGate, &expected, static_cast<size_t>(1),
                false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        {
            return true;
        }
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        if (!contentionReported)
        {
            HandlerHazardHook hook =
                __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
            if (hook)
            {
                hook(
                    __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE),
                    HandlerHazardStage::FinalizationContended);
            }
            contentionReported = true;
        }
#endif
        if (!canWait)
        {
            return false;
        }
        Scheduler::instance().yield();
    }
}

void IrqHandlerRegistry::releaseFinalizationGate(HandlerSlot &slot)
{
    __atomic_store_n(
        &slot.finalizationGate, static_cast<size_t>(0), __ATOMIC_RELEASE);
}

bool IrqHandlerRegistry::pinActionMutation(
    HandlerSlot &slot, size_t publication, DispatchCleanup &cleanup,
    Thread *thread)
{
    if (thread)
    {
        thread->armAtomicStateCleanup(
            cleanup.cleanup, abandonDispatch, &cleanup);
    }
    if (!publishDispatch(
            slot, cleanup.owner, &cleanup, publication, 0, false))
    {
        if (thread)
        {
            thread->disarmAtomicStateCleanup(cleanup.cleanup);
        }
        FATAL_NOLOCK("IRQ action-mutation hazard table exhausted.");
        return false;
    }
    if (__atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST) != publication)
    {
        unpinActionMutation(slot, publication, cleanup, thread);
        return false;
    }
    return true;
}

void IrqHandlerRegistry::unpinActionMutation(
    HandlerSlot &slot, size_t publication, DispatchCleanup &cleanup,
    Thread *thread)
{
    unpublishDispatch(&cleanup, slot, publication, true);
    if (thread)
    {
        thread->disarmAtomicStateCleanup(cleanup.cleanup);
    }
}

bool IrqHandlerRegistry::closeSlotAdmission(
    HandlerSlot &slot, size_t expectedPublication, size_t &closedPublication)
{
    const size_t originalPublication = expectedPublication;
    const uint8_t irq = irqOf(expectedPublication);
    const size_t graceBucket = irq % GraceBucketCount;
    const bool canWait = canWaitForActionFinalization();
    if (!acquireFinalizationGate(slot, canWait))
    {
        if (modeOf(originalPublication) == SlotMode::Draining)
        {
            size_t draining = originalPublication;
            beginMutation();
            __atomic_compare_exchange_n(
                &slot.publication, &draining,
                makePublication(
                    generationOf(originalPublication), irq, SlotMode::Enabled,
                    deliveryOf(originalPublication)),
                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            finishMutation();
        }
        return false;
    }

    size_t boundaryExpected = 0;
    while (!__atomic_compare_exchange_n(
        &m_OccurrenceBoundaryLocks[graceBucket], &boundaryExpected,
        static_cast<size_t>(1), false, __ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED))
    {
        if (!canWait)
        {
            releaseFinalizationGate(slot);
            if (modeOf(originalPublication) == SlotMode::Draining)
            {
                size_t draining = originalPublication;
                beginMutation();
                __atomic_compare_exchange_n(
                    &slot.publication, &draining,
                    makePublication(
                        generationOf(originalPublication), irq,
                        SlotMode::Enabled, deliveryOf(originalPublication)),
                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                finishMutation();
            }
            return false;
        }
        boundaryExpected = 0;
        Scheduler::instance().yield();
    }

    const size_t occurrenceEpoch = __atomic_load_n(
        &m_OccurrenceEpochs[graceBucket], __ATOMIC_SEQ_CST);
    size_t boundary = occurrenceEpoch + 1;
    if (!boundary)
    {
        boundary = 1;
    }
    __atomic_store_n(&slot.retirementEpoch, boundary, __ATOMIC_SEQ_CST);
    const size_t cancellingPublication = makePublication(
        generationOf(expectedPublication), irq, SlotMode::Cancelling,
        deliveryOf(expectedPublication));
    beginMutation();
    if (!__atomic_compare_exchange_n(
            &slot.publication, &expectedPublication, cancellingPublication,
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        __atomic_store_n(
            &slot.retirementEpoch, static_cast<size_t>(0), __ATOMIC_SEQ_CST);
        finishMutation();
        __atomic_store_n(
            &m_OccurrenceBoundaryLocks[graceBucket], static_cast<size_t>(0),
            __ATOMIC_RELEASE);
        releaseFinalizationGate(slot);
        return false;
    }

    // The epoch store is the admission-closure linearization point. The slot
    // already carries its boundary, so a preempting cutoff on either side can
    // decide membership without waiting for this remover.
    __atomic_store_n(
        &m_OccurrenceEpochs[graceBucket], boundary, __ATOMIC_SEQ_CST);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HandlerHazardHook boundaryHook =
        __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
    if (boundaryHook)
    {
        boundaryHook(
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE),
            HandlerHazardStage::RetirementBoundaryPublished);
    }
#endif

    closedPublication = makePublication(
        generationOf(cancellingPublication), irq, SlotMode::Closed,
        deliveryOf(cancellingPublication));
    size_t expectedCancelling = cancellingPublication;
    const bool closed = __atomic_compare_exchange_n(
        &slot.publication, &expectedCancelling, closedPublication, false,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    finishMutation();
    __atomic_store_n(
        &m_OccurrenceBoundaryLocks[graceBucket], static_cast<size_t>(0),
        __ATOMIC_RELEASE);
    releaseFinalizationGate(slot);
    if (!closed)
    {
        FATAL_NOLOCK("IRQ handler closure state changed before publication.");
    }
    return closed;
}

bool IrqHandlerRegistry::occurrencePrecedesRetirement(
    const HandlerSlot &slot, AdmissionCutoff admissionCutoff) const
{
    const size_t retirementEpoch =
        __atomic_load_n(&slot.retirementEpoch, __ATOMIC_SEQ_CST);
    return retirementEpoch &&
           !generationReached(admissionCutoff.occurrenceEpoch, retirementEpoch);
}

void IrqHandlerRegistry::tryReclaimTombstones(uint8_t irq)
{
    const size_t graceBucket = irq % GraceBucketCount;
    if (__atomic_load_n(
            &m_OccurrenceReaders[graceBucket][0], __ATOMIC_SEQ_CST) ||
        __atomic_load_n(
            &m_OccurrenceReaders[graceBucket][1], __ATOMIC_SEQ_CST))
    {
        return;
    }

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        const uint8_t slotIrq = irqOf(publication);
        if (modeOf(publication) != SlotMode::Tombstone ||
            slotIrq % GraceBucketCount != graceBucket ||
            __atomic_load_n(
                &slot.pendingThreadedGeneration, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(
                &slot.claimedThreadedGeneration, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(
                &slot.quiescedThreadedGeneration, __ATOMIC_ACQUIRE))
        {
            continue;
        }

        const size_t reclaimingPublication = makePublication(
            generationOf(publication), slotIrq, SlotMode::Retiring,
            deliveryOf(publication));
        if (!__atomic_compare_exchange_n(
                &slot.publication, &publication, reclaimingPublication, false,
                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        {
            continue;
        }
        if (hasActiveDispatch(slot, publication))
        {
            size_t reclaiming = reclaimingPublication;
            if (!__atomic_compare_exchange_n(
                    &slot.publication, &reclaiming, publication, false,
                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            {
                FATAL_NOLOCK(
                    "IRQ tombstone changed while action mutation was pinned.");
            }
            continue;
        }

        // No pre-retirement reader remains. Readers which announce after the
        // zero sample validate a post-retirement epoch and never need this
        // history, so metadata can be cleared before Empty becomes reusable.
        __atomic_store_n(
            &slot.previousThreadedGeneration, static_cast<size_t>(0),
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &slot.rolledBackThreadedGeneration, static_cast<size_t>(0),
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &slot.admissionEpoch, static_cast<size_t>(0), __ATOMIC_RELEASE);
        __atomic_store_n(
            &slot.retirementEpoch, static_cast<size_t>(0), __ATOMIC_RELEASE);
        __atomic_store_n(
            &slot.publication,
            makePublication(
                generationOf(reclaimingPublication), InvalidIrq,
                SlotMode::Empty, Delivery::Threaded),
            __ATOMIC_SEQ_CST);
    }
}

bool IrqHandlerRegistry::retireSlot(
    HandlerSlot &slot, size_t expectedPublication,
    IrqHandlerBase *expectedHandler)
{
    beginMutation();
    const size_t retiringPublication = makePublication(
        generationOf(expectedPublication), irqOf(expectedPublication),
        SlotMode::Retiring, deliveryOf(expectedPublication));
    if (!__atomic_compare_exchange_n(
            &slot.publication, &expectedPublication, retiringPublication, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        finishMutation();
        return false;
    }

    if (hasActiveDispatch(slot, expectedPublication))
    {
        size_t retiring = retiringPublication;
        if (!__atomic_compare_exchange_n(
                &slot.publication, &retiring, expectedPublication, false,
                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        {
            FATAL_NOLOCK(
                "IRQ slot changed while action mutation was pinned.");
        }
        finishMutation();
        return false;
    }

    assert(__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == expectedHandler);
    const uint8_t irq = irqOf(retiringPublication);
    if (deliveryOf(retiringPublication) == Delivery::Threaded)
    {
        size_t *actions[] = {
            &slot.pendingThreadedGeneration,
            &slot.claimedThreadedGeneration};
        for (size_t action = 0; action < 2; ++action)
        {
            size_t generation =
                __atomic_load_n(actions[action], __ATOMIC_ACQUIRE);
            while (generation)
            {
                // Quiesced is visible before the actionable lane disappears.
                // The worker therefore observes one side of this transition.
                publishSlotQuiesced(slot, irq, generation);
                if (__atomic_compare_exchange_n(
                        actions[action], &generation, static_cast<size_t>(0),
                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                {
                    break;
                }
            }
        }
    }
    __atomic_store_n(&slot.handler, nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(&slot.policy, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &slot.publication,
        makePublication(
            generationOf(retiringPublication), irq, SlotMode::Tombstone,
            deliveryOf(retiringPublication)),
        __ATOMIC_SEQ_CST);
    finishMutation();
    tryReclaimTombstones(irq);
    return true;
}

bool IrqHandlerRegistry::retireSlotOrObserveClosed(
    HandlerSlot &slot, size_t expectedPublication,
    IrqHandlerBase *expectedHandler)
{
    if (retireSlot(slot, expectedPublication, expectedHandler))
    {
        return true;
    }

    const size_t publication =
        __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
    return modeOf(publication) == SlotMode::Empty ||
           modeOf(publication) == SlotMode::Closed ||
           modeOf(publication) == SlotMode::Retiring ||
           modeOf(publication) == SlotMode::Tombstone ||
           generationOf(publication) != generationOf(expectedPublication) ||
           __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) != expectedHandler;
}

IrqHandlerRegistry::ActiveDispatch *IrqHandlerRegistry::publishDispatch(
    HandlerSlot &slot, void *owner, void *token, size_t admittedPublication,
    size_t controllerGeneration, bool callback)
{
    assert(owner);
    assert(token);
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *expectedToken = nullptr;
        if (__atomic_compare_exchange_n(
                &dispatch.token, &expectedToken, token, false, __ATOMIC_SEQ_CST,
                __ATOMIC_SEQ_CST))
        {
            __atomic_add_fetch(
                &dispatch.generation, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
            __atomic_store_n(&dispatch.owner, owner, __ATOMIC_RELAXED);
            __atomic_store_n(
                &dispatch.admittedPublication, admittedPublication,
                __ATOMIC_RELAXED);
            __atomic_store_n(
                &dispatch.controllerGeneration, controllerGeneration,
                __ATOMIC_RELAXED);
            __atomic_store_n(
                &dispatch.callback, callback ? static_cast<size_t>(1) :
                                               static_cast<size_t>(0),
                __ATOMIC_RELAXED);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            HandlerHazardHook hazardHook = nullptr;
            if (callback)
            {
                hazardHook =
                    __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
                if (hazardHook)
                {
                    hazardHook(
                        __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE),
                        HandlerHazardStage::Claimed);
                }
            }
#endif

            // Publishing the slot commits the callback pin. A removal which
            // closes admission before this store can retire the handler; the
            // dispatch revalidation below will then reject this callback.
            __atomic_store_n(&dispatch.slot, &slot, __ATOMIC_SEQ_CST);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            if (callback)
            {
                hazardHook =
                    __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
                if (hazardHook)
                {
                    hazardHook(
                        __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE),
                        HandlerHazardStage::Committed);
                }
            }
#endif
            return &dispatch;
        }
    }

    return nullptr;
}

bool IrqHandlerRegistry::unpublishDispatch(
    void *token, HandlerSlot &slot, size_t admittedPublication, bool required)
{
    assert(token);
    bool found = false;
    bool committed = false;
    bool callback = false;
    size_t controllerGeneration = 0;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        ActiveDispatch &dispatch = m_ActiveDispatches[i];
        if (__atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) != token)
        {
            continue;
        }

        const size_t generation =
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE);
        HandlerSlot *publishedSlot =
            __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) != token ||
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) !=
                generation)
        {
            continue;
        }

        if (publishedSlot && publishedSlot != &slot)
        {
            FATAL_NOLOCK("IRQ callback hazard changed slots during release.");
            return false;
        }

        committed = publishedSlot == &slot;
        callback =
            __atomic_load_n(&dispatch.callback, __ATOMIC_RELAXED) != 0;
        controllerGeneration = __atomic_load_n(
            &dispatch.controllerGeneration, __ATOMIC_RELAXED);
        __atomic_store_n(&dispatch.slot, nullptr, __ATOMIC_SEQ_CST);
        __atomic_store_n(
            &dispatch.admittedPublication, static_cast<size_t>(0),
            __ATOMIC_RELAXED);
        __atomic_store_n(
            &dispatch.controllerGeneration, static_cast<size_t>(0),
            __ATOMIC_RELAXED);
        __atomic_store_n(
            &dispatch.callback, static_cast<size_t>(0), __ATOMIC_RELAXED);
        __atomic_store_n(&dispatch.owner, nullptr, __ATOMIC_RELAXED);
        __atomic_store_n(&dispatch.token, nullptr, __ATOMIC_RELEASE);
        found = true;
        break;
    }

    if (!found)
    {
        if (required)
        {
            FATAL_NOLOCK("IRQ callback hazard was released more than once.");
        }
        return false;
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    IrqHandlerBase *releasedHandler = nullptr;
    HandlerHazardHook releaseHook = nullptr;
    if (committed && callback)
    {
        releasedHandler = __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
        releaseHook = __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
    }
#endif

    if (committed && callback && !required && controllerGeneration &&
        deliveryOf(admittedPublication) == Delivery::Threaded)
    {
        publishSlotQuiesced(
            slot, irqOf(admittedPublication), controllerGeneration);
        size_t claimed = controllerGeneration;
        __atomic_compare_exchange_n(
            &slot.claimedThreadedGeneration, &claimed,
            static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE);
    }

    if (committed && !hasActiveDispatch(slot, admittedPublication))
    {
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (generationOf(publication) == generationOf(admittedPublication) &&
            modeOf(publication) == SlotMode::Closed)
        {
            IrqHandlerBase *handler =
                __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
            if (handler)
            {
                retireSlot(slot, publication, handler);
            }
        }
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (releaseHook)
    {
        releaseHook(releasedHandler, HandlerHazardStage::Released);
    }
#endif
    return true;
}

void IrqHandlerRegistry::abandonDispatch(void *context)
{
    DispatchCleanup *dispatch = reinterpret_cast<DispatchCleanup *>(context);
    const bool callbackBoundaryEntered = dispatch->restoreInterruptState;
    if (dispatch->restoreDeviceHardIrqDepth)
    {
        DeviceHardIrqContext::restoreDepth(
            dispatch->previousDeviceHardIrqDepth);
        dispatch->restoreDeviceHardIrqDepth = false;
    }
    dispatch->registry->unpublishDispatch(
        dispatch, *dispatch->slot, dispatch->publication, false);
    restoreDispatchInterruptState(*dispatch);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (dispatch->callback)
    {
        DispatchAbandonHook hook = __atomic_load_n(
            &dispatch->registry->m_DispatchAbandonHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(dispatch->owner, callbackBoundaryEntered);
        }
    }
#endif
}

void IrqHandlerRegistry::restoreDispatchInterruptState(
    DispatchCleanup &dispatch)
{
    if (!dispatch.restoreInterruptState)
    {
        return;
    }

    const bool previousInterruptState = dispatch.previousInterruptState;
    dispatch.restoreInterruptState = false;
    Processor::setInterrupts(previousInterruptState);
}

bool IrqHandlerRegistry::hasActiveDispatch(
    HandlerSlot &target, size_t admittedPublication) const
{
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        const ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *token = __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE);
        if (!token)
        {
            continue;
        }

        const size_t generation =
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE);
        HandlerSlot *slot = __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        const size_t dispatchPublication =
            __atomic_load_n(&dispatch.admittedPublication, __ATOMIC_RELAXED);
        if (slot == &target &&
            generationOf(dispatchPublication) ==
                generationOf(admittedPublication) &&
            __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) == token &&
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) ==
                generation)
        {
            return true;
        }
    }
    return false;
}

bool IrqHandlerRegistry::findCurrentDispatch(
    void *owner, HandlerSlot *target, size_t targetPublication,
    bool &callbackContext) const
{
    callbackContext = false;
    bool foundTarget = false;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        const ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *token = __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE);
        if (!token)
        {
            continue;
        }

        const size_t generation =
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE);
        void *dispatchOwner =
            __atomic_load_n(&dispatch.owner, __ATOMIC_RELAXED);
        HandlerSlot *slot = __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        const size_t dispatchPublication =
            __atomic_load_n(&dispatch.admittedPublication, __ATOMIC_RELAXED);
        if (__atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) != token ||
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) !=
                generation)
        {
            continue;
        }

        if (dispatchOwner == owner && slot &&
            __atomic_load_n(&dispatch.callback, __ATOMIC_RELAXED))
        {
            callbackContext = true;
            foundTarget |= slot == target && target &&
                           generationOf(dispatchPublication) ==
                               generationOf(targetPublication);
        }
    }
    return foundTarget;
}

void *IrqHandlerRegistry::currentDispatchOwner()
{
    ProcessorInformation &information = Processor::information();
    Thread *thread = information.getCurrentThread();
    return thread ? static_cast<void *>(thread) :
                    static_cast<void *>(&information);
}

bool IrqHandlerRegistry::registerHandler(
    uint8_t irq, IrqHandlerBase *handler, Delivery delivery, size_t policy)
{
    if (!handler ||
        (delivery != Delivery::Threaded && delivery != Delivery::HardOnly))
    {
        return false;
    }

    LockGuard<Spinlock> guard(m_HandlerLock);
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        const SlotMode mode = modeOf(publication);
        if (mode == SlotMode::Empty || mode == SlotMode::Cancelling ||
            mode == SlotMode::Closed || mode == SlotMode::Retiring ||
            mode == SlotMode::Tombstone || irqOf(publication) != irq)
        {
            continue;
        }
        const Delivery existingDelivery = deliveryOf(publication);
        const size_t existingPolicy =
            __atomic_load_n(&slot.policy, __ATOMIC_ACQUIRE);
        if ((existingDelivery == delivery && existingPolicy != policy) ||
            (existingDelivery != delivery &&
             !mixedPoliciesCompatible(existingPolicy, policy)))
        {
            return false;
        }

        if (__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == handler)
        {
            return false;
        }
    }

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) == SlotMode::Empty &&
            !__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) &&
            !hasActiveDispatch(slot, publication))
        {
            const size_t generation = generationOf(publication) + 1;
            if (!generation || generation > MaximumPublicationGeneration)
            {
                continue;
            }
            beginMutation();
            size_t admissionEpoch = __atomic_load_n(
                                        &m_AdmissionEpoch, __ATOMIC_RELAXED) +
                                    1;
            if (!admissionEpoch)
            {
                ++admissionEpoch;
            }
            __atomic_store_n(
                &slot.pendingThreadedGeneration, static_cast<size_t>(0),
                __ATOMIC_RELEASE);
            __atomic_store_n(
                &slot.previousThreadedGeneration, static_cast<size_t>(0),
                __ATOMIC_RELEASE);
            __atomic_store_n(
                &slot.claimedThreadedGeneration, static_cast<size_t>(0),
                __ATOMIC_RELEASE);
            __atomic_store_n(
                &slot.quiescedThreadedGeneration, static_cast<size_t>(0),
                __ATOMIC_RELEASE);
            __atomic_store_n(
                &slot.rolledBackThreadedGeneration, static_cast<size_t>(0),
                __ATOMIC_RELEASE);
            __atomic_store_n(
                &slot.retirementEpoch, static_cast<size_t>(0),
                __ATOMIC_RELEASE);
            __atomic_store_n(&slot.handler, handler, __ATOMIC_RELEASE);
            // Policy remains immutable until Retiring closes this publication.
            __atomic_store_n(&slot.policy, policy, __ATOMIC_RELEASE);
            __atomic_store_n(
                &slot.admissionEpoch, admissionEpoch, __ATOMIC_RELEASE);
            __atomic_store_n(
                &slot.publication,
                makePublication(generation, irq, SlotMode::Enabled, delivery),
                __ATOMIC_SEQ_CST);
            // This is the registration linearization point used by interrupt
            // dispatch cutoffs. The preceding slot publication is visible to
            // a cutoff which observes this epoch.
            __atomic_store_n(
                &m_AdmissionEpoch, admissionEpoch, __ATOMIC_RELEASE);
            finishMutation();
            return true;
        }
    }

    return false;
}

bool IrqHandlerRegistry::registerThreadedHandler(
    uint8_t irq, IrqHandler *handler)
{
    return registerHandler(irq, handler, Delivery::Threaded, 0);
}

bool IrqHandlerRegistry::registerThreadedHandler(
    uint8_t irq, IrqHandler *handler, const IrqPolicy &policy)
{
    return policy.validForThreaded() &&
           registerHandler(
               irq, handler, Delivery::Threaded, encodePolicy(&policy));
}

bool IrqHandlerRegistry::registerHardHandler(
    uint8_t irq, HardIrqHandler *handler)
{
    return registerHandler(irq, handler, Delivery::HardOnly, 0);
}

bool IrqHandlerRegistry::registerHardHandler(
    uint8_t irq, HardIrqHandler *handler, const IrqPolicy &policy)
{
    return policy.validForHard() &&
           registerHandler(
               irq, handler, Delivery::HardOnly, encodePolicy(&policy));
}

IrqHandlerRegistry::UnregisterResult
IrqHandlerRegistry::unregisterHandler(uint8_t irq, IrqHandlerBase *handler)
{
    LineMode ignoredDelivery = LineMode::Empty;
    return unregisterHandler(irq, handler, ignoredDelivery);
}

IrqHandlerRegistry::UnregisterResult IrqHandlerRegistry::unregisterHandler(
    uint8_t irq, IrqHandlerBase *handler, LineMode &removedDelivery)
{
    removedDelivery = LineMode::Empty;
    if (!handler)
    {
        return UnregisterResult::NotFound;
    }

    void *owner = currentDispatchOwner();
    Thread *current = Processor::information().getCurrentThread();
    bool canYield = current && Processor::getInterrupts();
#if HOSTED
    canYield = canYield && !current->getHostedSignalDepth();
#endif

    bool callbackContext = false;
    findCurrentDispatch(owner, nullptr, 0, callbackContext);

    if (!canYield || callbackContext)
    {
        // Callback and early atomic contexts cannot wait on a writer. A
        // callback can close its own admission and let its final pin retire
        // the slot; a non-callback removal completes only when no callback is
        // already committed.
        for (size_t i = 0; i < MaxHandlerSlots; ++i)
        {
            HandlerSlot &candidate = m_Handlers[i];
            size_t publication =
                __atomic_load_n(&candidate.publication, __ATOMIC_SEQ_CST);
            if (modeOf(publication) != SlotMode::Enabled ||
                irqOf(publication) != irq ||
                __atomic_load_n(&candidate.handler, __ATOMIC_ACQUIRE) !=
                    handler)
            {
                continue;
            }

            removedDelivery = lineModeForDelivery(deliveryOf(publication));

            bool currentTargetDispatch = false;
            if (findCurrentDispatch(
                    owner, &candidate, publication, currentTargetDispatch))
            {
                size_t closedPublication = 0;
                const bool deferred = closeSlotAdmission(
                    candidate, publication, closedPublication);
                if (deferred)
                {
                    return UnregisterResult::Deferred;
                }
                return UnregisterResult::Rejected;
            }

            const size_t drainingPublication = makePublication(
                generationOf(publication), irq, SlotMode::Draining,
                deliveryOf(publication));
            beginMutation();
            const bool draining = __atomic_compare_exchange_n(
                &candidate.publication, &publication, drainingPublication,
                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            finishMutation();
            if (!draining)
            {
                return UnregisterResult::Rejected;
            }

            if (hasActiveDispatch(candidate, drainingPublication))
            {
                size_t expectedPublication = drainingPublication;
                beginMutation();
                __atomic_compare_exchange_n(
                    &candidate.publication, &expectedPublication,
                    makePublication(
                        generationOf(drainingPublication), irq,
                        SlotMode::Enabled, deliveryOf(drainingPublication)),
                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                finishMutation();
                return UnregisterResult::Rejected;
            }

            size_t closedPublication = 0;
            if (!closeSlotAdmission(
                    candidate, drainingPublication, closedPublication))
            {
                return UnregisterResult::Rejected;
            }

            return retireSlotOrObserveClosed(
                       candidate, closedPublication, handler) ?
                       UnregisterResult::Completed :
                       UnregisterResult::Rejected;
        }

        return UnregisterResult::NotFound;
    }

    // Only an ordinary thread can synchronously drain. Keep its stack alive
    // after the atomic callback path has returned without touching Thread
    // deferred-scope state.
    TerminationDeferral terminationDeferral;
    m_HandlerLock.acquire();

    HandlerSlot *slot = nullptr;
    size_t publication = 0;
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        size_t candidatePublication =
            __atomic_load_n(&m_Handlers[i].publication, __ATOMIC_SEQ_CST);
        if (modeOf(candidatePublication) != SlotMode::Empty &&
            irqOf(candidatePublication) == irq &&
            __atomic_load_n(&m_Handlers[i].handler, __ATOMIC_ACQUIRE) ==
                handler)
        {
            slot = &m_Handlers[i];
            publication = candidatePublication;
            break;
        }
    }

    if (!slot)
    {
        m_HandlerLock.release();
        return UnregisterResult::NotFound;
    }

    removedDelivery = lineModeForDelivery(deliveryOf(publication));

    const bool selfUnregister =
        findCurrentDispatch(owner, slot, publication, callbackContext);
    if (selfUnregister)
    {
        if (modeOf(publication) != SlotMode::Enabled)
        {
            m_HandlerLock.release();
            return UnregisterResult::Rejected;
        }

        size_t closedPublication = 0;
        const bool deferred =
            closeSlotAdmission(*slot, publication, closedPublication);
        m_HandlerLock.release();
        return deferred ? UnregisterResult::Deferred :
                          UnregisterResult::Rejected;
    }

    if (modeOf(publication) != SlotMode::Enabled)
    {
        m_HandlerLock.release();
        return UnregisterResult::Rejected;
    }

    // Closing may need to wait for a callback's short finalization gate.
    // Spinlock ownership disables interrupts, so retain the exact publication
    // as our mutation token and perform that wait after releasing the writer
    // lock. Concurrent removers or slot reuse are rejected by the closure CAS.
    m_HandlerLock.release();
    size_t closedPublication = 0;
    if (!closeSlotAdmission(*slot, publication, closedPublication))
    {
        return UnregisterResult::Rejected;
    }

    if (!hasActiveDispatch(*slot, closedPublication))
    {
        m_HandlerLock.acquire();
        const bool retired = retireSlotOrObserveClosed(
            *slot, closedPublication, handler);
        m_HandlerLock.release();
        return retired ? UnregisterResult::Completed :
                         UnregisterResult::Rejected;
    }

    uintptr_t previousDebugAddress = 0;
    const Thread::DebugState previousDebugState =
        current->getDebugState(previousDebugAddress);
    current->setDebugState(
        Thread::CallbackDrain, reinterpret_cast<uintptr_t>(handler));
    while (hasActiveDispatch(*slot, closedPublication))
    {
        // Callback release can run in hard IRQ context. It only clears its
        // atomic hazard; this ordinary teardown thread owns all scheduling.
        Scheduler::instance().yield();
    }
    current->setDebugState(previousDebugState, previousDebugAddress);

    m_HandlerLock.acquire();
    const bool retired = retireSlotOrObserveClosed(
        *slot, closedPublication, handler);
    m_HandlerLock.release();
    return retired ? UnregisterResult::Completed : UnregisterResult::Rejected;
}

bool IrqHandlerRegistry::dispatchHard(
    uint8_t irq, InterruptState &state, bool &handled,
    HardIrqHandler *onlyHandler, size_t dispatchGeneration)
{
    AdmissionCutoff admissionCutoff = {};
    if (!captureAdmissionCutoff(irq, admissionCutoff))
    {
        handled = false;
        return false;
    }
    return dispatchHard(
        irq, state, handled, onlyHandler, dispatchGeneration,
        admissionCutoff);
}

bool IrqHandlerRegistry::captureAdmissionCutoff(
    uint8_t irq, AdmissionCutoff &cutoff)
{
    cutoff = {};
    const size_t graceBucket = irq % GraceBucketCount;
    while (true)
    {
        const size_t occurrenceEpoch =
            __atomic_load_n(
                &m_OccurrenceEpochs[graceBucket], __ATOMIC_SEQ_CST);
        const size_t readerBank = occurrenceEpoch & 1;
        if (!acquireOccurrenceReaderLeases(irq, readerBank, 1))
        {
            return false;
        }
        if (__atomic_load_n(
                &m_OccurrenceEpochs[graceBucket], __ATOMIC_SEQ_CST) ==
            occurrenceEpoch)
        {
            const size_t admissionEpoch =
                __atomic_load_n(&m_AdmissionEpoch, __ATOMIC_ACQUIRE);
            cutoff = {
                admissionEpoch, occurrenceEpoch,
                (static_cast<size_t>(irq) * 2) + readerBank + 1};
            return true;
        }
        releaseOccurrenceReaderLeases(irq, readerBank, 1);
    }
}

bool IrqHandlerRegistry::captureMixedAdmissionCutoffs(
    uint8_t irq, MixedAdmissionCutoffs &cutoffs)
{
    cutoffs = {};
    const size_t graceBucket = irq % GraceBucketCount;
    while (true)
    {
        const size_t occurrenceEpoch = __atomic_load_n(
            &m_OccurrenceEpochs[graceBucket], __ATOMIC_SEQ_CST);
        const size_t readerBank = occurrenceEpoch & 1;
        if (!acquireOccurrenceReaderLeases(irq, readerBank, 2))
        {
            return false;
        }
        if (__atomic_load_n(
                &m_OccurrenceEpochs[graceBucket], __ATOMIC_SEQ_CST) ==
            occurrenceEpoch)
        {
            const size_t admissionEpoch =
                __atomic_load_n(&m_AdmissionEpoch, __ATOMIC_ACQUIRE);
            const AdmissionCutoff cutoff = {
                admissionEpoch, occurrenceEpoch,
                (static_cast<size_t>(irq) * 2) + readerBank + 1};
            cutoffs = {cutoff, cutoff};
            return true;
        }
        releaseOccurrenceReaderLeases(irq, readerBank, 2);
    }
}

bool IrqHandlerRegistry::acquireOccurrenceReaderLeases(
    uint8_t irq, size_t readerBank, size_t count)
{
    if (readerBank > 1 || !count)
    {
        FATAL_NOLOCK("Invalid IRQ occurrence reader acquisition.");
        return false;
    }

    const size_t graceBucket = irq % GraceBucketCount;
    size_t current = __atomic_load_n(
        &m_OccurrenceReaders[graceBucket][readerBank], __ATOMIC_SEQ_CST);
    while (true)
    {
        if (current > ~static_cast<size_t>(0) - count)
        {
            FATAL_NOLOCK("IRQ occurrence reader count overflowed.");
            return false;
        }
        const size_t desired = current + count;
        if (__atomic_compare_exchange_n(
                &m_OccurrenceReaders[graceBucket][readerBank], &current,
                desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        {
            return true;
        }
    }
}

void IrqHandlerRegistry::releaseOccurrenceReaderLeases(
    uint8_t irq, size_t readerBank, size_t count)
{
    if (readerBank > 1 || !count)
    {
        FATAL_NOLOCK("Invalid IRQ occurrence reader release.");
        return;
    }

    const size_t graceBucket = irq % GraceBucketCount;
    size_t current = __atomic_load_n(
        &m_OccurrenceReaders[graceBucket][readerBank], __ATOMIC_SEQ_CST);
    size_t remaining = 0;
    while (true)
    {
        if (current < count)
        {
            FATAL_NOLOCK("IRQ occurrence reader count underflowed.");
            return;
        }
        remaining = current - count;
        if (__atomic_compare_exchange_n(
                &m_OccurrenceReaders[graceBucket][readerBank], &current,
                remaining, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        {
            break;
        }
    }

    if (!remaining)
    {
        tryReclaimTombstones(irq);
    }
}

void IrqHandlerRegistry::releaseAdmissionCutoff(
    AdmissionCutoff admissionCutoff)
{
    if (!admissionCutoff.readerToken)
    {
        return;
    }

    const size_t token = admissionCutoff.readerToken - 1;
    if (token >= IrqCount * 2)
    {
        FATAL_NOLOCK("Invalid IRQ occurrence reader token.");
        return;
    }
    const uint8_t irq = static_cast<uint8_t>(token / 2);
    const size_t readerBank = token & 1;
    releaseOccurrenceReaderLeases(irq, readerBank, 1);
}

void IrqHandlerRegistry::beginAdmissionCutoffCleanup(
    AdmissionCutoffCleanup &cleanup)
{
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    cleanup.thread = Processor::information().getCurrentThread();
    cleanup.ownsCutoff = cleanup.cutoff.readerToken != 0;
    if (cleanup.thread && cleanup.ownsCutoff)
    {
        // This remains below each per-slot hazard cleanup. Stack abandonment
        // therefore unpublishes hazards before the final lease can reclaim a
        // tombstone.
        cleanup.thread->armAtomicStateCleanup(
            cleanup.cleanup, abandonAdmissionCutoff, &cleanup);
    }
    Processor::setInterrupts(interruptsWereEnabled);
}

void IrqHandlerRegistry::finishAdmissionCutoffCleanup(
    AdmissionCutoffCleanup &cleanup)
{
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    if (cleanup.ownsCutoff)
    {
        // Transfer ownership before release so an unwind from the release
        // path cannot consume the same lease through this cleanup record.
        cleanup.ownsCutoff = false;
        releaseAdmissionCutoff(cleanup.cutoff);
    }
    if (cleanup.thread && cleanup.cleanup.armed)
    {
        cleanup.thread->disarmAtomicStateCleanup(cleanup.cleanup);
    }
    cleanup.registry = nullptr;
    Processor::setInterrupts(interruptsWereEnabled);
}

void IrqHandlerRegistry::abandonAdmissionCutoff(void *context)
{
    AdmissionCutoffCleanup *cleanup =
        reinterpret_cast<AdmissionCutoffCleanup *>(context);
    if (!cleanup || !cleanup->registry || !cleanup->ownsCutoff)
    {
        return;
    }

    cleanup->ownsCutoff = false;
    cleanup->registry->releaseAdmissionCutoff(cleanup->cutoff);
    cleanup->registry = nullptr;
}

bool IrqHandlerRegistry::dispatchHard(
    uint8_t irq, InterruptState &state, bool &handled,
    HardIrqHandler *onlyHandler, size_t dispatchGeneration,
    AdmissionCutoff admissionCutoff)
{
    AdmissionCutoffCleanup cutoffCleanup(this, admissionCutoff);
    beginAdmissionCutoffCleanup(cutoffCleanup);
    bool admitted = false;
    handled = false;
    const size_t cutoffEpoch = admissionCutoff.epoch;
    const size_t expectedReaderToken =
        (static_cast<size_t>(irq) * 2) +
        (admissionCutoff.occurrenceEpoch & 1) + 1;
    if (admissionCutoff.readerToken != expectedReaderToken)
    {
        finishAdmissionCutoffCleanup(cutoffCleanup);
        return false;
    }

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        const SlotMode mode = modeOf(publication);
        if (irqOf(publication) != irq ||
            deliveryOf(publication) != Delivery::HardOnly)
        {
            continue;
        }
        const size_t admissionEpoch =
            __atomic_load_n(&slot.admissionEpoch, __ATOMIC_ACQUIRE);
        if (!admissionEpoch ||
            (admissionEpoch != cutoffEpoch &&
             generationReached(admissionEpoch, cutoffEpoch)))
        {
            continue;
        }

        if (mode == SlotMode::Cancelling || mode == SlotMode::Closed ||
            mode == SlotMode::Retiring || mode == SlotMode::Tombstone)
        {
            if (occurrencePrecedesRetirement(slot, admissionCutoff))
            {
                admitted = true;
                handled = true;
            }
            continue;
        }
        if (mode != SlotMode::Enabled && mode != SlotMode::Draining)
        {
            continue;
        }

        IrqHandlerBase *handler =
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
        if (!handler || (onlyHandler &&
                         handler != static_cast<IrqHandlerBase *>(onlyHandler)))
        {
            continue;
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerPrePinHook prePinHook =
            __atomic_load_n(&m_HandlerPrePinHook, __ATOMIC_ACQUIRE);
        if (prePinHook)
        {
            prePinHook(handler);
        }
#endif

        void *owner = currentDispatchOwner();
        Thread *thread = Processor::information().getCurrentThread();
        DispatchCleanup dispatchCleanup(this, &slot, owner, publication);
        if (thread)
        {
            // Publish cleanup before committing the active-dispatch hazard.
            // A nested exception can therefore abandon this stack at every
            // later instruction without leaking callback admission.
            thread->armAtomicStateCleanup(
                dispatchCleanup.cleanup, abandonDispatch, &dispatchCleanup);
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerHazardHook hazardHook =
            __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
        if (hazardHook)
        {
            hazardHook(handler, HandlerHazardStage::BeforeClaim);
        }
#endif

        if (!publishDispatch(
                slot, owner, &dispatchCleanup, publication, dispatchGeneration))
        {
            if (thread)
            {
                thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
            }
            finishAdmissionCutoffCleanup(cutoffCleanup);
            FATAL_NOLOCK("IRQ callback hazard table exhausted.");
            return admitted;
        }

        size_t currentPublication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        SlotMode currentMode = modeOf(currentPublication);
        const bool sameLifetime =
            generationOf(currentPublication) == generationOf(publication) &&
            irqOf(currentPublication) == irq &&
            deliveryOf(currentPublication) == Delivery::HardOnly &&
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == handler &&
            __atomic_load_n(&slot.admissionEpoch, __ATOMIC_ACQUIRE) ==
                admissionEpoch;
        if (sameLifetime && currentMode == SlotMode::Draining)
        {
            // The marker is already visible to unregister. Restoring Enabled
            // makes callback admission and removal arbitrate with one CAS:
            // either this wins and removal rejects, or Cancelling wins and no
            // callback starts.
            size_t expectedPublication = currentPublication;
            const size_t enabledPublication = makePublication(
                generationOf(currentPublication), irq, SlotMode::Enabled,
                Delivery::HardOnly);
            beginMutation();
            __atomic_compare_exchange_n(
                &slot.publication, &expectedPublication, enabledPublication,
                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            finishMutation();
            currentPublication = __atomic_load_n(
                &slot.publication, __ATOMIC_SEQ_CST);
            currentMode = modeOf(currentPublication);
        }

        if (generationOf(currentPublication) != generationOf(publication) ||
            irqOf(currentPublication) != irq ||
            deliveryOf(currentPublication) != Delivery::HardOnly ||
            currentMode != SlotMode::Enabled ||
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) != handler ||
            __atomic_load_n(&slot.admissionEpoch, __ATOMIC_ACQUIRE) !=
                admissionEpoch)
        {
            unpublishDispatch(&dispatchCleanup, slot, publication, true);
            if (thread)
            {
                thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
            }
            continue;
        }

        admitted = true;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerPinHook hook =
            __atomic_load_n(&m_HandlerPinHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(handler);
        }
#endif

        {
            // Test dispatch can enter here from ordinary thread context. Keep
            // the lifetime hooks schedulable, but give the callback the same
            // interrupt boundary as a controller-delivered hard IRQ.
            dispatchCleanup.previousInterruptState =
                Processor::getInterrupts();
            dispatchCleanup.restoreInterruptState = true;
            Processor::setInterrupts(false);
            DeviceHardIrqContext deviceHardIrqContext(
                dispatchCleanup.previousDeviceHardIrqDepth,
                dispatchCleanup.restoreDeviceHardIrqDepth);
            handled |= static_cast<HardIrqHandler *>(handler)->irq(irq, state);
        }
        unpublishDispatch(&dispatchCleanup, slot, publication, true);
        if (thread)
        {
            thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
        }
        restoreDispatchInterruptState(dispatchCleanup);
    }

    finishAdmissionCutoffCleanup(cutoffCleanup);
    return admitted;
}

bool IrqHandlerRegistry::publishThreadedDispatch(
    uint8_t irq, size_t dispatchGeneration)
{
    AdmissionCutoff admissionCutoff = {};
    if (!captureAdmissionCutoff(irq, admissionCutoff))
    {
        return false;
    }
    return publishThreadedDispatch(
        irq, dispatchGeneration, admissionCutoff);
}

bool IrqHandlerRegistry::publishThreadedDispatch(
    uint8_t irq, size_t dispatchGeneration, AdmissionCutoff admissionCutoff)
{
    AdmissionCutoffCleanup cutoffCleanup(this, admissionCutoff);
    beginAdmissionCutoffCleanup(cutoffCleanup);
    const size_t expectedReaderToken =
        (static_cast<size_t>(irq) * 2) +
        (admissionCutoff.occurrenceEpoch & 1) + 1;
    if (admissionCutoff.readerToken != expectedReaderToken)
    {
        finishAdmissionCutoffCleanup(cutoffCleanup);
        return false;
    }
    if (!threadedGenerationValid(irq, dispatchGeneration))
    {
        finishAdmissionCutoffCleanup(cutoffCleanup);
        return false;
    }

    struct Candidate
    {
        HandlerSlot *slot;
        size_t publication;
        IrqHandlerBase *handler;
        size_t admissionEpoch;
    };
    Candidate candidates[MaxHandlerSlots];
    size_t candidateCount = 0;
    const size_t cutoffEpoch = admissionCutoff.epoch;
    bool admitted = false;

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        const SlotMode mode = modeOf(publication);
        if (irqOf(publication) != irq ||
            deliveryOf(publication) != Delivery::Threaded)
        {
            continue;
        }

        const size_t admissionEpoch =
            __atomic_load_n(&slot.admissionEpoch, __ATOMIC_ACQUIRE);
        if (!admissionEpoch ||
            (admissionEpoch != cutoffEpoch &&
             generationReached(admissionEpoch, cutoffEpoch)))
        {
            continue;
        }

        if (mode == SlotMode::Cancelling || mode == SlotMode::Closed ||
            mode == SlotMode::Retiring || mode == SlotMode::Tombstone)
        {
            if (occurrencePrecedesRetirement(slot, admissionCutoff))
            {
                publishSlotQuiesced(slot, irq, dispatchGeneration);
                admitted = true;
            }
            continue;
        }
        if (mode != SlotMode::Enabled && mode != SlotMode::Draining)
        {
            continue;
        }

        IrqHandlerBase *handler =
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
        if (handler)
        {
            candidates[candidateCount++] = {
                &slot, publication, handler, admissionEpoch};
        }
    }

    for (size_t i = 0; i < candidateCount; ++i)
    {
        HandlerSlot &slot = *candidates[i].slot;
        const size_t publication = candidates[i].publication;
        IrqHandlerBase *handler = candidates[i].handler;
        const size_t admissionEpoch = candidates[i].admissionEpoch;
        void *owner = currentDispatchOwner();
        Thread *thread = Processor::information().getCurrentThread();
        DispatchCleanup dispatchCleanup(
            this, &slot, owner, publication, false);
        if (thread)
        {
            // Removal drains this short publication hazard before it can reuse
            // the slot for a different handler lifetime.
            thread->armAtomicStateCleanup(
                dispatchCleanup.cleanup, abandonDispatch, &dispatchCleanup);
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerHazardHook hazardHook =
            __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
        if (hazardHook)
        {
            hazardHook(handler, HandlerHazardStage::BeforeClaim);
        }
#endif

        if (!publishDispatch(
                slot, owner, &dispatchCleanup, publication,
                dispatchGeneration, false))
        {
            if (thread)
            {
                thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
            }
            finishAdmissionCutoffCleanup(cutoffCleanup);
            FATAL_NOLOCK("IRQ callback hazard table exhausted.");
            return admitted;
        }

        size_t currentPublication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        SlotMode currentMode = modeOf(currentPublication);
        const bool sameLifetime =
            generationOf(currentPublication) == generationOf(publication) &&
            irqOf(currentPublication) == irq &&
            deliveryOf(currentPublication) == Delivery::Threaded &&
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == handler &&
            __atomic_load_n(&slot.admissionEpoch, __ATOMIC_ACQUIRE) ==
                admissionEpoch;
        if (sameLifetime &&
            (currentMode == SlotMode::Enabled ||
             currentMode == SlotMode::Draining) &&
            threadedGenerationValid(irq, dispatchGeneration))
        {
            if (currentMode == SlotMode::Draining)
            {
                size_t expectedPublication = currentPublication;
                const size_t enabledPublication = makePublication(
                    generationOf(currentPublication), irq, SlotMode::Enabled,
                    Delivery::Threaded);
                beginMutation();
                __atomic_compare_exchange_n(
                    &slot.publication, &expectedPublication,
                    enabledPublication, false, __ATOMIC_SEQ_CST,
                    __ATOMIC_SEQ_CST);
                finishMutation();
                currentPublication = __atomic_load_n(
                    &slot.publication, __ATOMIC_SEQ_CST);
                currentMode = modeOf(currentPublication);
            }

            if (currentMode != SlotMode::Enabled)
            {
                publishSlotQuiesced(slot, irq, dispatchGeneration);
            }

            size_t pending = __atomic_load_n(
                &slot.pendingThreadedGeneration, __ATOMIC_ACQUIRE);
            while (currentMode == SlotMode::Enabled &&
                   (!pending ||
                    generationReached(dispatchGeneration, pending)))
            {
                __atomic_store_n(
                    &slot.previousThreadedGeneration, pending,
                    __ATOMIC_RELEASE);
                __atomic_store_n(
                    &slot.rolledBackThreadedGeneration,
                    static_cast<size_t>(0), __ATOMIC_RELEASE);
                if (__atomic_compare_exchange_n(
                        &slot.pendingThreadedGeneration, &pending,
                        dispatchGeneration, false, __ATOMIC_RELEASE,
                        __ATOMIC_ACQUIRE))
                {
                    if (!threadedGenerationValid(irq, dispatchGeneration))
                    {
                        size_t stale = dispatchGeneration;
                        __atomic_compare_exchange_n(
                            &slot.pendingThreadedGeneration, &stale,
                            static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
                            __ATOMIC_ACQUIRE);
                    }
                    break;
                }
            }

            const size_t finalPublication =
                __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
            const SlotMode finalMode = modeOf(finalPublication);
            if (generationOf(finalPublication) != generationOf(publication) ||
                irqOf(finalPublication) != irq ||
                (finalMode != SlotMode::Enabled &&
                 finalMode != SlotMode::Draining) ||
                __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) != handler ||
                __atomic_load_n(&slot.admissionEpoch, __ATOMIC_ACQUIRE) !=
                    admissionEpoch)
            {
                publishSlotQuiesced(slot, irq, dispatchGeneration);
            }
        }
        else
        {
            // This publication belonged to the occurrence cutoff. If removal
            // closed it before token publication, preserve that membership as
            // a quiesced cancellation for the worker.
            if (occurrencePrecedesRetirement(slot, admissionCutoff))
            {
                publishSlotQuiesced(slot, irq, dispatchGeneration);
            }
        }
        admitted = true;

        unpublishDispatch(&dispatchCleanup, slot, publication, true);
        if (thread)
        {
            thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
        }
    }

    finishAdmissionCutoffCleanup(cutoffCleanup);
    return admitted;
}

bool IrqHandlerRegistry::dispatchThreaded(
    uint8_t irq, size_t dispatchGeneration, ThreadedDispatchResult &result,
    IrqHandler *onlyHandler)
{
    result = {false, false};
    bool admitted = false;
    if (!threadedGenerationValid(irq, dispatchGeneration))
    {
        return false;
    }
    Thread *dispatchThread = Processor::information().getCurrentThread();
    if (!dispatchThread || !Processor::getInterrupts())
    {
        return false;
    }
#if HOSTED
    if (dispatchThread->getHostedSignalDepth())
    {
        return false;
    }
#endif

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    AdmissionCutoff workerCutoff = {};
    const bool cutoffCaptured = captureAdmissionCutoff(irq, workerCutoff);
    AdmissionCutoffCleanup cutoffCleanup(this, workerCutoff);
    if (cutoffCaptured)
    {
        beginAdmissionCutoffCleanup(cutoffCleanup);
    }
    Processor::setInterrupts(interruptsWereEnabled);
    if (!cutoffCaptured)
    {
        return false;
    }

    struct Candidate
    {
        HandlerSlot *slot;
        size_t publication;
        IrqHandlerBase *handler;
    };
    while (true)
    {
        if (__atomic_load_n(
                &m_ThreadedActionMutationWriters, __ATOMIC_SEQ_CST))
        {
            Scheduler::instance().yield();
            continue;
        }
        const size_t actionMutationGeneration = __atomic_load_n(
            &m_ThreadedActionMutationGeneration, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(
                &m_ThreadedActionMutationWriters, __ATOMIC_SEQ_CST))
        {
            Scheduler::instance().yield();
            continue;
        }

        Candidate candidates[MaxHandlerSlots];
        size_t candidateCount = 0;

        // The hard stage marked exact slot publications. A later registration
        // has no token, and a newer occurrence remains pending when an older
        // worker batch is already active.
        for (size_t i = 0; i < MaxHandlerSlots; ++i)
        {
            HandlerSlot &slot = m_Handlers[i];
            const size_t publication =
                __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
            if (irqOf(publication) != irq ||
                deliveryOf(publication) != Delivery::Threaded)
            {
                continue;
            }

            size_t quiesced = __atomic_load_n(
                &slot.quiescedThreadedGeneration, __ATOMIC_ACQUIRE);
            if (quiesced && !threadedGenerationValid(irq, quiesced))
            {
                __atomic_compare_exchange_n(
                    &slot.quiescedThreadedGeneration, &quiesced,
                    static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE);
                quiesced = 0;
            }
            if (quiesced && generationReached(dispatchGeneration, quiesced) &&
                __atomic_compare_exchange_n(
                    &slot.quiescedThreadedGeneration, &quiesced,
                    static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE))
            {
                admitted = true;
                result.allowRearm = true;
            }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            HandlerHazardHook quiescedHook =
                __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
            if (quiescedHook)
            {
                quiescedHook(
                    __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE),
                    HandlerHazardStage::QuiescedObserved);
            }
#endif

            if (modeOf(publication) != SlotMode::Enabled)
            {
                continue;
            }

            const size_t pending = __atomic_load_n(
                &slot.pendingThreadedGeneration, __ATOMIC_ACQUIRE);
            if (!pending || !generationReached(dispatchGeneration, pending))
            {
                continue;
            }
            if (!threadedGenerationValid(irq, pending))
            {
                size_t stale = pending;
                __atomic_compare_exchange_n(
                    &slot.pendingThreadedGeneration, &stale,
                    static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE);
                continue;
            }

            IrqHandlerBase *handler =
                __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
            if (!handler ||
                (onlyHandler &&
                 handler != static_cast<IrqHandlerBase *>(onlyHandler)))
            {
                continue;
            }

            candidates[candidateCount++] = {&slot, publication, handler};
        }

        for (size_t i = 0; i < candidateCount; ++i)
        {
            HandlerSlot &slot = *candidates[i].slot;
            const size_t publication = candidates[i].publication;
            IrqHandlerBase *handler = candidates[i].handler;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            HandlerPrePinHook prePinHook =
                __atomic_load_n(&m_HandlerPrePinHook, __ATOMIC_ACQUIRE);
            if (prePinHook)
            {
                prePinHook(handler);
            }
#endif

            void *owner = currentDispatchOwner();
            Thread *thread = Processor::information().getCurrentThread();
            DispatchCleanup dispatchCleanup(this, &slot, owner, publication);
            if (thread)
            {
                // Publish cleanup before committing the active-dispatch hazard.
                // A nested exception can therefore abandon this stack at every
                // later instruction without leaking callback admission.
                thread->armAtomicStateCleanup(
                    dispatchCleanup.cleanup, abandonDispatch, &dispatchCleanup);
            }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            HandlerHazardHook hazardHook =
                __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
            if (hazardHook)
            {
                hazardHook(handler, HandlerHazardStage::BeforeClaim);
            }
#endif

            if (!publishDispatch(
                    slot, owner, &dispatchCleanup, publication,
                    dispatchGeneration))
            {
                if (thread)
                {
                    thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
                }
                finishAdmissionCutoffCleanup(cutoffCleanup);
                FATAL_NOLOCK("IRQ callback hazard table exhausted.");
                return admitted;
            }

            if (__atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST) !=
                    publication ||
                __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) != handler)
            {
                unpublishDispatch(&dispatchCleanup, slot, publication, true);
                if (thread)
                {
                    thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
                }
                continue;
            }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            HandlerPinHook hook =
                __atomic_load_n(&m_HandlerPinHook, __ATOMIC_ACQUIRE);
            if (hook)
            {
                hook(handler);
            }
#endif

            size_t pending = __atomic_load_n(
                &slot.pendingThreadedGeneration, __ATOMIC_ACQUIRE);
            bool claimed = false;
            size_t claimedGeneration = 0;
            while (pending && generationReached(dispatchGeneration, pending))
            {
                size_t emptyClaim = 0;
                if (!__atomic_compare_exchange_n(
                        &slot.claimedThreadedGeneration, &emptyClaim, pending,
                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                {
                    break;
                }

                claimedGeneration = pending;
                size_t exactPending = pending;
                if (__atomic_compare_exchange_n(
                        &slot.pendingThreadedGeneration, &exactPending,
                        static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
                        __ATOMIC_ACQUIRE))
                {
                    claimed = true;
                    break;
                }
                size_t exactClaim = claimedGeneration;
                __atomic_compare_exchange_n(
                    &slot.claimedThreadedGeneration, &exactClaim,
                    static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE);
                pending = exactPending;
            }
            if (!claimed)
            {
                unpublishDispatch(&dispatchCleanup, slot, publication, true);
                if (thread)
                {
                    thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
                }
                continue;
            }

            admitted = true;

            const IrqDisposition disposition =
                static_cast<IrqHandler *>(handler)->irq(irq);
            if (disposition == IrqDisposition::Handled)
            {
                result.handled = true;
                result.allowRearm = true;
            }
            else if (disposition == IrqDisposition::Quiesced)
            {
                result.allowRearm = true;
            }
            unpublishDispatch(&dispatchCleanup, slot, publication, true);
            if (thread)
            {
                thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
            }

            if (!acquireFinalizationGate(slot, true))
            {
                finishAdmissionCutoffCleanup(cutoffCleanup);
                FATAL("IRQ action finalization gate could not be acquired.");
                return admitted;
            }

            size_t finalPublication = __atomic_load_n(
                &slot.publication, __ATOMIC_SEQ_CST);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            HandlerHazardHook finalizationHook =
                __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
            if (finalizationHook)
            {
                finalizationHook(
                    handler, HandlerHazardStage::BeforeClaimFinalization);
            }
#endif
            if (generationOf(finalPublication) == generationOf(publication) &&
                irqOf(finalPublication) == irq &&
                deliveryOf(finalPublication) == Delivery::Threaded &&
                modeOf(finalPublication) == SlotMode::Draining &&
                __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == handler)
            {
                size_t expectedPublication = finalPublication;
                beginMutation();
                __atomic_compare_exchange_n(
                    &slot.publication, &expectedPublication, publication, false,
                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                finishMutation();
                finalPublication = __atomic_load_n(
                    &slot.publication, __ATOMIC_SEQ_CST);
            }
            if (finalPublication == publication)
            {
                size_t exactClaim = claimedGeneration;
                __atomic_compare_exchange_n(
                    &slot.claimedThreadedGeneration, &exactClaim,
                    static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE);
            }
            else
            {
                publishSlotQuiesced(slot, irq, claimedGeneration);
                size_t exactClaim = claimedGeneration;
                __atomic_compare_exchange_n(
                    &slot.claimedThreadedGeneration, &exactClaim,
                    static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE);
            }
            releaseFinalizationGate(slot);
        }

        bool admissionResolving = false;
        for (size_t i = 0; i < MaxHandlerSlots; ++i)
        {
            HandlerSlot &slot = m_Handlers[i];
            const size_t publication =
                __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
            if (irqOf(publication) != irq ||
                deliveryOf(publication) != Delivery::Threaded)
            {
                continue;
            }

            size_t pending = __atomic_load_n(
                &slot.pendingThreadedGeneration, __ATOMIC_ACQUIRE);
            size_t claimed = __atomic_load_n(
                &slot.claimedThreadedGeneration, __ATOMIC_ACQUIRE);
            const bool pendingReached =
                pending && generationReached(dispatchGeneration, pending);
            const bool claimedReached =
                claimed && generationReached(dispatchGeneration, claimed);
            if (!pendingReached && !claimedReached)
            {
                continue;
            }
            if (pendingReached && !threadedGenerationValid(irq, pending))
            {
                size_t stale = pending;
                __atomic_compare_exchange_n(
                    &slot.pendingThreadedGeneration, &stale,
                    static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE);
                pending = 0;
            }

            const SlotMode mode = modeOf(publication);
            if (mode == SlotMode::Draining || mode == SlotMode::Cancelling ||
                mode == SlotMode::Closed || mode == SlotMode::Retiring)
            {
                // Draining may restore Enabled; committed removal reaches
                // Empty only after publishing cancellation and clearing the
                // token. Do not claim here or retirement could publish a stale
                // cancellation after this worker's final watermark scan.
                admissionResolving = true;
                continue;
            }
        }

        const size_t finalActionMutationWriters = __atomic_load_n(
            &m_ThreadedActionMutationWriters, __ATOMIC_SEQ_CST);
        const size_t finalActionMutationGeneration = __atomic_load_n(
            &m_ThreadedActionMutationGeneration, __ATOMIC_SEQ_CST);
        if (finalActionMutationWriters ||
            finalActionMutationGeneration != actionMutationGeneration)
        {
            continue;
        }

        if (!admissionResolving && !candidateCount)
        {
            break;
        }
        if (admissionResolving)
        {
            Scheduler::instance().yield();
        }
    }

    finishAdmissionCutoffCleanup(cutoffCleanup);
    return admitted;
}

size_t IrqHandlerRegistry::handlerCount(uint8_t irq)
{
    size_t count = 0;
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        const size_t publication =
            __atomic_load_n(&m_Handlers[i].publication, __ATOMIC_SEQ_CST);
        const SlotMode mode = modeOf(publication);
        if ((mode == SlotMode::Enabled || mode == SlotMode::Draining) &&
            irqOf(publication) == irq)
        {
            ++count;
        }
    }
    return count;
}

IrqHandlerRegistry::LineMode IrqHandlerRegistry::lineMode(uint8_t irq)
{
    bool threaded = false;
    bool hard = false;
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        const size_t publication =
            __atomic_load_n(&m_Handlers[i].publication, __ATOMIC_SEQ_CST);
        const SlotMode mode = modeOf(publication);
        if ((mode != SlotMode::Enabled && mode != SlotMode::Draining) ||
            irqOf(publication) != irq)
        {
            continue;
        }

        if (deliveryOf(publication) == Delivery::Threaded)
        {
            threaded = true;
        }
        else
        {
            hard = true;
        }
        if (threaded && hard)
        {
            return LineMode::Mixed;
        }
    }
    return threaded ? LineMode::Threaded :
           hard     ? LineMode::HardOnly :
                      LineMode::Empty;
}

bool IrqHandlerRegistry::snapshotLineConfiguration(
    uint8_t irq, LineConfiguration &configuration) const
{
    for (size_t attempt = 0; attempt < LineSnapshotAttempts; ++attempt)
    {
        if (__atomic_load_n(&m_MutationWriters, __ATOMIC_SEQ_CST))
        {
            continue;
        }
        const size_t generation =
            __atomic_load_n(&m_MutationGeneration, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&m_MutationWriters, __ATOMIC_SEQ_CST))
        {
            continue;
        }

        LineConfiguration observed;
        size_t threadedPolicy = 0;
        size_t hardPolicy = 0;
        bool observedThreaded = false;
        bool observedHard = false;
        bool consistent = true;
        for (size_t i = 0; i < MaxHandlerSlots; ++i)
        {
            const HandlerSlot &slot = m_Handlers[i];
            const size_t publication =
                __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
            if (modeOf(publication) != SlotMode::Enabled ||
                irqOf(publication) != irq)
            {
                continue;
            }

            const size_t policy =
                __atomic_load_n(&slot.policy, __ATOMIC_ACQUIRE);
            if (__atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST) !=
                publication)
            {
                consistent = false;
                break;
            }

            if (deliveryOf(publication) == Delivery::Threaded)
            {
                if (observedThreaded && threadedPolicy != policy)
                {
                    consistent = false;
                    break;
                }
                observedThreaded = true;
                threadedPolicy = policy;
            }
            else
            {
                if (observedHard && hardPolicy != policy)
                {
                    consistent = false;
                    break;
                }
                observedHard = true;
                hardPolicy = policy;
            }
            ++observed.handlerCount;
        }

        size_t observedPolicy = 0;
        if (consistent && observedThreaded && observedHard)
        {
            consistent = mixedPoliciesCompatible(hardPolicy, threadedPolicy);
            observed.mode = LineMode::Mixed;
            observedPolicy = effectiveMixedPolicy(hardPolicy, threadedPolicy);
        }
        else if (consistent && observedThreaded)
        {
            observed.mode = LineMode::Threaded;
            observedPolicy = threadedPolicy;
        }
        else if (consistent && observedHard)
        {
            observed.mode = LineMode::HardOnly;
            observedPolicy = hardPolicy;
        }

        // Writer count must be sampled first: a writer which finishes between
        // these loads changes the following generation and forces a retry.
        const size_t finalWriters =
            __atomic_load_n(&m_MutationWriters, __ATOMIC_SEQ_CST);
        const size_t finalGeneration =
            __atomic_load_n(&m_MutationGeneration, __ATOMIC_SEQ_CST);
        if (finalWriters || generation != finalGeneration)
        {
            continue;
        }
        if (!consistent)
        {
            return false;
        }

        observed.mutationGeneration = finalGeneration;
        decodePolicy(observedPolicy, observed);
        configuration = observed;
        return true;
    }

    return false;
}

size_t IrqHandlerRegistry::hardDispatchState(
    uint8_t irq, size_t &exactGeneration) const
{
    size_t count = 0;
    exactGeneration = 0;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        const ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *token = __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE);
        if (!token)
        {
            continue;
        }

        const size_t generation =
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE);
        HandlerSlot *slot = __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        const size_t publication =
            __atomic_load_n(&dispatch.admittedPublication, __ATOMIC_RELAXED);
        const size_t controllerGeneration =
            __atomic_load_n(&dispatch.controllerGeneration, __ATOMIC_RELAXED);
        if (slot && irqOf(publication) == irq &&
            deliveryOf(publication) == Delivery::HardOnly &&
            __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) == token &&
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) ==
                generation &&
            __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST) == slot)
        {
            ++count;
            exactGeneration = count == 1 ? controllerGeneration : 0;
        }
    }
    return count;
}

size_t IrqHandlerRegistry::threadedDispatchState(
    uint8_t irq, uintptr_t &exactHandlerIdentity) const
{
    size_t count = 0;
    exactHandlerIdentity = 0;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        const ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *token = __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE);
        if (!token)
        {
            continue;
        }

        const size_t generation =
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE);
        HandlerSlot *slot = __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        const size_t publication =
            __atomic_load_n(&dispatch.admittedPublication, __ATOMIC_RELAXED);
        IrqHandlerBase *handler =
            slot ? __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE) : nullptr;
        if (slot && handler && irqOf(publication) == irq &&
            deliveryOf(publication) == Delivery::Threaded &&
            __atomic_load_n(&dispatch.callback, __ATOMIC_RELAXED) &&
            __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) == token &&
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) ==
                generation &&
            __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST) == slot)
        {
            ++count;
            exactHandlerIdentity =
                count == 1 ? reinterpret_cast<uintptr_t>(handler) : 0;
        }
    }
    return count;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void IrqHandlerRegistry::setHandlerPinHook(HandlerPinHook hook)
{
    __atomic_store_n(&m_HandlerPinHook, hook, __ATOMIC_RELEASE);
}

void IrqHandlerRegistry::setHandlerPrePinHook(HandlerPrePinHook hook)
{
    __atomic_store_n(&m_HandlerPrePinHook, hook, __ATOMIC_RELEASE);
}

void IrqHandlerRegistry::setHandlerHazardHook(HandlerHazardHook hook)
{
    __atomic_store_n(&m_HandlerHazardHook, hook, __ATOMIC_RELEASE);
}

void IrqHandlerRegistry::setDispatchAbandonHook(DispatchAbandonHook hook)
{
    __atomic_store_n(&m_DispatchAbandonHook, hook, __ATOMIC_RELEASE);
}

void IrqHandlerRegistry::withMutationLockForTest(MutationLockHook hook)
{
    m_HandlerLock.acquire();
    if (hook)
    {
        hook();
    }
    m_HandlerLock.release();
}

void IrqHandlerRegistry::withMutationEpochForTest(MutationLockHook hook)
{
    beginMutation();
    if (hook)
    {
        hook();
    }
    finishMutation();
}

size_t IrqHandlerRegistry::activeDispatchCountForTest(IrqHandlerBase *handler)
{
    size_t count = 0;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *token = __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE);
        if (!token)
        {
            continue;
        }
        const size_t generation =
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE);
        HandlerSlot *slot = __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        IrqHandlerBase *activeHandler =
            slot ? __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE) : nullptr;
        if (activeHandler == handler &&
            __atomic_load_n(&dispatch.callback, __ATOMIC_RELAXED) &&
            __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) == token &&
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) ==
                generation)
        {
            ++count;
        }
    }
    return count;
}

size_t IrqHandlerRegistry::claimedDispatchCountForOwnerForTest(void *owner)
{
    if (!owner)
    {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *token = __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE);
        if (!token)
        {
            continue;
        }

        const size_t generation =
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE);
        void *dispatchOwner =
            __atomic_load_n(&dispatch.owner, __ATOMIC_RELAXED);
        if (dispatchOwner == owner &&
            __atomic_load_n(&dispatch.callback, __ATOMIC_RELAXED) &&
            __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) == token &&
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) ==
                generation)
        {
            ++count;
        }
    }
    return count;
}

bool IrqHandlerRegistry::containsHandlerForTest(
    uint8_t irq, IrqHandlerBase *handler)
{
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) != SlotMode::Empty &&
            irqOf(publication) == irq &&
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == handler &&
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST) == publication)
        {
            return true;
        }
    }
    return false;
}

size_t IrqHandlerRegistry::tombstoneCountForTest(uint8_t irq) const
{
    size_t count = 0;
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        const size_t publication =
            __atomic_load_n(&m_Handlers[i].publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) == SlotMode::Tombstone &&
            irqOf(publication) == irq)
        {
            ++count;
        }
    }
    return count;
}

size_t IrqHandlerRegistry::threadedActionMutationWriterCountForTest() const
{
    return __atomic_load_n(
        &m_ThreadedActionMutationWriters, __ATOMIC_SEQ_CST);
}

bool IrqHandlerRegistry::setThreadedActionLanesForTest(
    IrqHandlerBase *handler, size_t pendingGeneration,
    size_t previousGeneration, size_t claimedGeneration,
    size_t quiescedGeneration, size_t rolledBackGeneration)
{
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        if (__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) != handler)
        {
            continue;
        }

        __atomic_store_n(
            &slot.pendingThreadedGeneration, pendingGeneration,
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &slot.previousThreadedGeneration, previousGeneration,
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &slot.claimedThreadedGeneration, claimedGeneration,
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &slot.quiescedThreadedGeneration, quiescedGeneration,
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &slot.rolledBackThreadedGeneration, rolledBackGeneration,
            __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

bool IrqHandlerRegistry::consumeThreadedQuiescedForTest(
    IrqHandlerBase *handler, size_t generation)
{
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        if (__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) != handler)
        {
            continue;
        }

        size_t exact = generation;
        return __atomic_compare_exchange_n(
            &slot.quiescedThreadedGeneration, &exact,
            static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE);
    }
    return false;
}
#endif
