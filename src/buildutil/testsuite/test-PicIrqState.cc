/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <atomic>
#include <gtest/gtest.h>
#include <thread>

#include "system/kernel/machine/mach_pc/PicIrqState.h"

namespace
{
struct RecordedPicWrite
{
    PicControllerWriteTarget target;
    uint8_t value;
};

class PicWriteRecorder
{
  public:
    static constexpr size_t Capacity = 16;

    PicWriteRecorder() : count(0), overflow(false), writes()
    {
    }

    void operator()(PicControllerWriteTarget target, uint8_t value)
    {
        if (count == Capacity)
        {
            overflow = true;
            return;
        }

        writes[count].target = target;
        writes[count].value = value;
        ++count;
    }

    size_t count;
    bool overflow;
    RecordedPicWrite writes[Capacity];
};

void expectWrite(
    const PicWriteRecorder &recorder, size_t index,
    PicControllerWriteTarget target, uint8_t value)
{
    ASSERT_LT(index, recorder.count);
    EXPECT_EQ(recorder.writes[index].target, target);
    EXPECT_EQ(recorder.writes[index].value, value);
}
}  // namespace

TEST(PicIrqState, SchedulerRouteExcludesGenericRegistration)
{
    PicIrqState state;
    const IrqPolicy scheduler = IrqPolicy::edgeHard();

    ASSERT_TRUE(state.canRegisterScheduler(0, scheduler));
    state.schedulerRegistered(0, scheduler);

    EXPECT_TRUE(state.schedulerRegistered(0));
    EXPECT_FALSE(state.canRegister(0, scheduler, IrqDelivery::Hard));
    EXPECT_FALSE(
        state.canRegister(0, IrqPolicy::edgeThreaded(), IrqDelivery::Threaded));
    EXPECT_FALSE(
        state.canRegister(0, IrqPolicy::levelHard(), IrqDelivery::Hard));
    EXPECT_FALSE(state.canRegister(
        0, IrqPolicy::levelThreaded(), IrqDelivery::Threaded));

    state.schedulerUnregistered(0);
    EXPECT_FALSE(state.schedulerRegistered(0));
    EXPECT_TRUE(state.canRegister(0, scheduler, IrqDelivery::Hard));
}

TEST(PicIrqState, GenericRegistrationExcludesSchedulerRoute)
{
    PicIrqState state;
    const IrqPolicy generic = IrqPolicy::edgeHard();

    EXPECT_FALSE(state.canRegisterScheduler(1, IrqPolicy::edgeHard()));
    EXPECT_FALSE(state.canRegisterScheduler(0, IrqPolicy::levelHard()));

    ASSERT_TRUE(state.canRegister(0, generic, IrqDelivery::Hard));
    state.handlerRegistered(0, generic, IrqDelivery::Hard);

    EXPECT_FALSE(state.canRegisterScheduler(0, IrqPolicy::edgeHard()));

    state.handlerUnregistered(0, IrqDelivery::Hard);
    EXPECT_TRUE(state.canRegisterScheduler(0, IrqPolicy::edgeHard()));
}

TEST(PicIrqState, CascadeInputIsReservedFromHandlers)
{
    PicIrqState state;

    EXPECT_FALSE(state.canRegister(
        PicIrqState::CascadeIrq, IrqPolicy::edgeHard(), IrqDelivery::Hard));
    EXPECT_FALSE(state.canRegister(
        PicIrqState::CascadeIrq, IrqPolicy::edgeThreaded(),
        IrqDelivery::Threaded));
    EXPECT_FALSE(state.canRegister(
        PicIrqState::CascadeIrq, IrqPolicy::levelHard(), IrqDelivery::Hard));
    EXPECT_FALSE(state.canRegister(
        PicIrqState::CascadeIrq, IrqPolicy::levelThreaded(),
        IrqDelivery::Threaded));

    state.setAllEnabled(false);
    EXPECT_EQ(
        state.masterMask() & (1U << PicIrqState::CascadeIrq),
        static_cast<uint8_t>(0));
}

TEST(PicIrqState, SchedulerOccurrenceIsTerminallyAccounted)
{
    PicIrqState state;
    state.schedulerRegistered(0, IrqPolicy::edgeHard());

    const size_t generation = state.beginDispatch(0);
    ASSERT_NE(generation, static_cast<size_t>(0));
    EXPECT_TRUE(state.acknowledge(0));
    state.completeDispatch(0, generation, false);

    EXPECT_EQ(state.dispatchGeneration(0), generation);
    EXPECT_EQ(state.acknowledgedGeneration(0), generation);
    EXPECT_FALSE(state.acknowledgementPending(0));
    EXPECT_TRUE(state.enabled(0));

    state.schedulerUnregistered(0);
    EXPECT_EQ(state.handlerCount(0), static_cast<size_t>(0));
    EXPECT_FALSE(state.enabled(0));
}

TEST(PicIrqState, LineTransitionMasksUntilPublicationCompletes)
{
    constexpr size_t Irq = 5;
    PicIrqState state;
    state.setAllEnabled(false);
    state.handlerRegistered(Irq, IrqPolicy::edgeHard(), IrqDelivery::Hard);
    ASSERT_TRUE(state.enabled(Irq));

    state.beginLineTransition(Irq);
    EXPECT_TRUE(state.lineTransitionPending(Irq));
    EXPECT_FALSE(state.enabled(Irq));

    state.finishLineTransition(Irq);
    EXPECT_FALSE(state.lineTransitionPending(Irq));
    EXPECT_TRUE(state.enabled(Irq));
}

TEST(PicIrqState, PhysicalOverrideCannotUnmaskCanonicalLines)
{
    const uint16_t canonical = 0x00F0;
    const uint16_t temporary = 0x9002;

    EXPECT_EQ(effectivePicMask(canonical, temporary), 0x90F2);
}

TEST(PicIrqState, ReplacementLifetimeWaitsForDeliveryProcessorRelease)
{
    constexpr uint16_t Irq4 = 1U << 4;
    const uint16_t applied = 0xFFFF;
    const uint16_t canonical = static_cast<uint16_t>(applied & ~Irq4);

    const uint16_t deferredTemporary =
        preserveUnsafePicUnmasks(applied, canonical, 0, Irq4, false);
    EXPECT_NE(deferredTemporary & Irq4, static_cast<uint16_t>(0));
    EXPECT_NE(
        effectivePicMask(canonical, deferredTemporary) & Irq4,
        static_cast<uint16_t>(0));

    const uint16_t releaseTemporary =
        preserveUnsafePicUnmasks(applied, canonical, 0, Irq4, true);
    EXPECT_EQ(releaseTemporary & Irq4, static_cast<uint16_t>(0));
    EXPECT_EQ(
        effectivePicMask(canonical, releaseTemporary) & Irq4,
        static_cast<uint16_t>(0));
}

TEST(PicIrqState, StableLifetimeCanRestoreWhileRemoteOwnerIsHeld)
{
    constexpr uint16_t Irq0 = 1U << 0;
    constexpr uint16_t Irq4 = 1U << 4;
    const uint16_t temporary = Irq0 | Irq4;

    EXPECT_EQ(
        restorablePicTemporaryMask(temporary, Irq4), Irq0);
    EXPECT_EQ(
        preserveUnsafePicUnmasks(0xFFFF, 0xFFEE, 0, Irq4, false),
        Irq4);
}

TEST(PicHardTailQueue, FinalHardSelfRemovalRetainsThreadedTerminalRecord)
{
    constexpr uint8_t Irq = 5;
    PicIrqState state;
    PicControllerStateGate gate;
    PicHardTailQueue tails;
    state.setAllEnabled(false);
    state.handlerRegistered(
        Irq, IrqPolicy::levelThreaded(), IrqDelivery::Threaded);
    state.handlerRegistered(Irq, IrqPolicy::levelHard(), IrqDelivery::Hard);
    ASSERT_EQ(state.delivery(Irq), IrqDelivery::Mixed);
    const size_t dispatchGeneration = state.beginDispatch(Irq);
    state.beginThreadedDispatch(Irq);

    PicHardTailRecord published = {};
    published.irq = Irq;
    published.controllerLifetime = gate.currentLifetime(Irq);
    published.dispatchGeneration = dispatchGeneration;
    published.hardStageGeneration = 7;
    published.threadedCookie = 11;
    published.controllerAck = IrqControllerAck::AfterHardStage;
    published.hardDisposition = HardIrqDisposition::Handled;
    published.hasThreadedStage = true;
    published.threadedPublished = true;
    published.admitted = true;

    gate.beginLineTransition(Irq);
    state.handlerUnregistered(Irq, IrqDelivery::Hard);
    ASSERT_EQ(state.delivery(Irq), IrqDelivery::Threaded);
    ASSERT_NE(
        gate.finishLineTransition(Irq), published.controllerLifetime);

    ASSERT_TRUE(tails.publish(Irq, published));
    PicHardTailRecord consumed = {};
    ASSERT_TRUE(tails.take(Irq, consumed));
    EXPECT_EQ(consumed.threadedCookie, published.threadedCookie);
    EXPECT_EQ(consumed.dispatchGeneration, dispatchGeneration);
    EXPECT_TRUE(consumed.hasThreadedStage);
    EXPECT_TRUE(consumed.threadedPublished);
    EXPECT_TRUE(consumed.admitted);
    EXPECT_EQ(consumed.hardDisposition, HardIrqDisposition::Handled);

    // The slot remains unavailable until the consumer has completed software
    // state, physical masking, and EOI for this exact record.
    EXPECT_FALSE(tails.publish(Irq, published));
    tails.complete(Irq);
    ASSERT_TRUE(tails.publish(Irq, published));
    ASSERT_TRUE(tails.take(Irq, consumed));
    tails.complete(Irq);
}

TEST(PicHardTailQueue, LifetimeMutationDoesNotDemoteHardOutcome)
{
    constexpr uint8_t Irq = 5;
    PicControllerStateGate gate;
    PicHardTailQueue tails;
    PicHardTailRecord published = {};
    published.irq = Irq;
    published.controllerLifetime = gate.currentLifetime(Irq);
    published.dispatchGeneration = 19;
    published.hardStageGeneration = 23;
    published.threadedCookie = 29;
    published.controllerAck = IrqControllerAck::BeforeHardStage;
    published.hardDisposition = HardIrqDisposition::KeepMasked;
    published.hasThreadedStage = true;
    published.threadedPublished = true;
    published.admitted = false;

    ASSERT_TRUE(gate.tryClaim());
    gate.beginLineTransition(Irq);
    const size_t newLifetime = gate.finishLineTransition(Irq);
    ASSERT_NE(newLifetime, published.controllerLifetime);
    ASSERT_TRUE(gate.releaseIfIdle());

    ASSERT_TRUE(tails.publish(Irq, published));
    ASSERT_TRUE(gate.queueTailRecord(Irq));
    PicControllerStateGate::PendingActions actions;
    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_TRUE(actions.empty());

    PicHardTailRecord consumed = {};
    ASSERT_TRUE(tails.take(Irq, consumed));
    EXPECT_EQ(consumed.controllerLifetime, published.controllerLifetime);
    EXPECT_EQ(consumed.hardDisposition, HardIrqDisposition::KeepMasked);
    EXPECT_FALSE(consumed.admitted);

    PicHardTailCurrentState current = {};
    current.controllerLifetime = newLifetime;
    current.dispatchGeneration = consumed.dispatchGeneration;
    current.hardStageGeneration = consumed.hardStageGeneration;
    current.threadedCookie = consumed.threadedCookie;
    current.threadedDispatchGeneration = consumed.dispatchGeneration;
    current.hardHandlerCount = 1;
    current.delivery = IrqDelivery::Mixed;
    current.hardLineQuarantined = true;
    const PicHardTailPlan plan = resolvePicHardTail(consumed, current);
    EXPECT_FALSE(plan.controllerLifetimeCurrent);
    EXPECT_TRUE(plan.hardStageLifetimeCurrent);
    EXPECT_TRUE(plan.threadedLifetimeCurrent);
    EXPECT_EQ(
        plan.effectiveHardDisposition, HardIrqDisposition::KeepMasked);
    EXPECT_EQ(plan.threadedAction, PicHardTailThreadedAction::Publish);
    tails.complete(Irq);
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicHardTailQueue, MixedPeerMutationRetainsBothStageOutcomes)
{
    constexpr uint8_t Irq = 10;
    PicIrqState state;
    PicControllerStateGate gate;
    PicHardTailQueue tails;
    state.setAllEnabled(false);
    state.handlerRegistered(
        Irq, IrqPolicy::levelThreaded(), IrqDelivery::Threaded);
    state.handlerRegistered(Irq, IrqPolicy::levelHard(), IrqDelivery::Hard);
    state.handlerRegistered(Irq, IrqPolicy::levelHard(), IrqDelivery::Hard);
    const size_t dispatchGeneration = state.beginDispatch(Irq);
    state.beginThreadedDispatch(Irq);

    PicHardTailRecord published = {};
    published.irq = Irq;
    published.controllerLifetime = gate.currentLifetime(Irq);
    published.dispatchGeneration = dispatchGeneration;
    published.hardStageGeneration = 3;
    published.threadedCookie = 5;
    published.controllerAck = IrqControllerAck::AfterHardStage;
    published.hardDisposition = HardIrqDisposition::Handled;
    published.hasThreadedStage = true;
    published.threadedPublished = true;
    published.admitted = true;

    gate.beginLineTransition(Irq);
    state.handlerUnregistered(Irq, IrqDelivery::Hard);
    ASSERT_EQ(state.delivery(Irq), IrqDelivery::Mixed);
    ASSERT_NE(
        gate.finishLineTransition(Irq), published.controllerLifetime);

    ASSERT_TRUE(tails.publish(Irq, published));
    PicHardTailRecord consumed = {};
    ASSERT_TRUE(tails.take(Irq, consumed));
    EXPECT_EQ(consumed.dispatchGeneration, dispatchGeneration);
    EXPECT_EQ(consumed.threadedCookie, published.threadedCookie);
    EXPECT_TRUE(consumed.threadedPublished);
    EXPECT_TRUE(consumed.admitted);

    PicHardTailCurrentState current = {};
    current.controllerLifetime = gate.currentLifetime(Irq);
    current.dispatchGeneration = state.dispatchGeneration(Irq);
    current.hardStageGeneration = consumed.hardStageGeneration;
    current.threadedCookie = consumed.threadedCookie;
    current.threadedDispatchGeneration = consumed.dispatchGeneration;
    current.hardHandlerCount = state.hardHandlerCount(Irq);
    current.delivery = state.delivery(Irq);
    const PicHardTailPlan plan = resolvePicHardTail(consumed, current);
    EXPECT_FALSE(plan.controllerLifetimeCurrent);
    EXPECT_TRUE(plan.hardStageLifetimeCurrent);
    EXPECT_TRUE(plan.threadedLifetimeCurrent);
    EXPECT_EQ(
        plan.effectiveHardDisposition, HardIrqDisposition::Handled);
    EXPECT_EQ(plan.threadedAction, PicHardTailThreadedAction::Publish);
    tails.complete(Irq);
}

TEST(PicHardTailQueue, FailedMutationStillRetainsTerminalRecord)
{
    constexpr uint8_t Irq = 4;
    PicControllerStateGate gate;
    PicHardTailQueue tails;
    PicHardTailRecord published = {};
    published.irq = Irq;
    published.controllerLifetime = gate.currentLifetime(Irq);
    published.dispatchGeneration = 31;
    published.hardStageGeneration = 37;
    published.hardDisposition = HardIrqDisposition::NotHandled;
    published.admitted = true;

    // A registry mutation can fail after the line has already entered and
    // exited a controller transition. The callback outcome still belongs to
    // the occurrence which captured the old lifetime.
    gate.beginLineTransition(Irq);
    ASSERT_NE(
        gate.finishLineTransition(Irq), published.controllerLifetime);
    ASSERT_TRUE(tails.publish(Irq, published));

    PicHardTailRecord consumed = {};
    ASSERT_TRUE(tails.take(Irq, consumed));
    EXPECT_EQ(consumed.dispatchGeneration, published.dispatchGeneration);
    EXPECT_EQ(consumed.hardStageGeneration, published.hardStageGeneration);
    EXPECT_EQ(consumed.hardDisposition, HardIrqDisposition::NotHandled);
    EXPECT_TRUE(consumed.admitted);
    tails.complete(Irq);
}

TEST(
    PicHardTailFinalizer,
    StaleMixedTailMasksBeforeDoorbellAndEoiAndThenReleasesSlot)
{
    constexpr uint8_t Irq = 5;
    enum Step : size_t
    {
        TemporaryMask = 1,
        ThreadedDoorbell,
        PublicationFailureMask,
        ControllerEoi,
    };

    PicHardTailRecord record = {};
    record.irq = Irq;
    record.controllerLifetime = 11;
    record.dispatchGeneration = 13;
    record.hardStageGeneration = 17;
    record.threadedCookie = 19;
    record.controllerAck = IrqControllerAck::AfterHardStage;
    record.hardDisposition = HardIrqDisposition::KeepMasked;
    record.hasThreadedStage = true;
    record.threadedPublished = true;
    record.admitted = true;

    PicHardTailCurrentState current = {};
    current.controllerLifetime = 12;
    current.dispatchGeneration = record.dispatchGeneration;
    current.hardStageGeneration = 18;
    current.threadedCookie = record.threadedCookie;
    current.threadedDispatchGeneration = record.dispatchGeneration;
    current.delivery = IrqDelivery::Threaded;

    const PicHardTailPlan plan = resolvePicHardTail(record, current);
    EXPECT_FALSE(plan.controllerLifetimeCurrent);
    EXPECT_FALSE(plan.hardStageLifetimeCurrent);
    EXPECT_TRUE(plan.threadedLifetimeCurrent);
    EXPECT_EQ(
        plan.effectiveHardDisposition, HardIrqDisposition::Handled);
    EXPECT_EQ(plan.threadedAction, PicHardTailThreadedAction::Publish);

    PicHardTailCurrentState sameLifetime = current;
    sameLifetime.controllerLifetime = record.controllerLifetime;
    EXPECT_TRUE(
        resolvePicHardTail(record, sameLifetime).controllerLifetimeCurrent);

    const PicHardTailDoorbellResult missing =
        resolvePicHardTailDoorbell(false, false);
    EXPECT_FALSE(missing.published);
    EXPECT_FALSE(missing.invalidateStagedDispatch);
    EXPECT_FALSE(missing.quarantine);
    EXPECT_TRUE(missing.completeDispatch);

    for (size_t attempt = 0; attempt < 2; ++attempt)
    {
        const bool doorbellAccepted = attempt == 0;
        PicHardTailQueue tails;
        ASSERT_TRUE(tails.publish(Irq, record));

        size_t steps[4] = {};
        size_t stepCount = 0;
        ASSERT_TRUE(tails.consume(
            Irq, [&](const PicHardTailRecord &consumed) {
                // Claimed storage cannot be reused until mask, publication,
                // and controller acknowledgement have all returned.
                EXPECT_FALSE(tails.publish(Irq, consumed));

                PicHardTailTerminalSequence terminal(
                    !plan.controllerLifetimeCurrent, consumed.controllerAck);
                terminal.applyTemporaryMask(
                    [&]() { steps[stepCount++] = TemporaryMask; });
                steps[stepCount++] = ThreadedDoorbell;
                const PicHardTailDoorbellResult doorbell =
                    resolvePicHardTailDoorbell(
                        consumed.threadedPublished, doorbellAccepted);
                if (doorbell.quarantine)
                {
                    steps[stepCount++] = PublicationFailureMask;
                }
                terminal.acknowledge(
                    [&]() { steps[stepCount++] = ControllerEoi; });
            }));

        ASSERT_EQ(stepCount, doorbellAccepted ? 3U : 4U);
        EXPECT_EQ(steps[0], TemporaryMask);
        EXPECT_EQ(steps[1], ThreadedDoorbell);
        if (doorbellAccepted)
        {
            EXPECT_EQ(steps[2], ControllerEoi);
        }
        else
        {
            EXPECT_EQ(steps[2], PublicationFailureMask);
            EXPECT_EQ(steps[3], ControllerEoi);
        }

        // consume() releases the slot only after the finalizer has returned.
        ASSERT_TRUE(tails.publish(Irq, record));
        ASSERT_TRUE(tails.consume(Irq, [](const PicHardTailRecord &) {}));
    }
}

TEST(PicHardTailQueue, ReadyPublicationCannotBeStrandedByOlderPendingDrain)
{
    constexpr uint8_t Irq = 9;
    PicControllerStateGate gate;
    PicHardTailQueue tails;
    PicControllerStateGate::PendingActions actions;

    ASSERT_TRUE(gate.tryAcquireClean());
    EXPECT_FALSE(gate.queueEntry(Irq, gate.currentLifetime(Irq)));
    ASSERT_TRUE(gate.takePending(actions));

    PicHardTailRecord published = {};
    published.irq = Irq;
    published.controllerLifetime = gate.currentLifetime(Irq);
    ASSERT_TRUE(tails.publish(Irq, published));

    // Model the old owner reaching idle between Ready and the producer's
    // fresh Pending publication. The producer must become the new owner.
    ASSERT_TRUE(gate.releaseIfIdle());
    ASSERT_TRUE(gate.queueTailRecord(Irq));
    ASSERT_TRUE(gate.takePending(actions));
    PicHardTailRecord consumed = {};
    ASSERT_TRUE(tails.take(Irq, consumed));
    EXPECT_EQ(consumed.irq, Irq);
    tails.complete(Irq);
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicHardTailQueue, ConcurrentHandoffsPublishCompleteRecords)
{
    constexpr uint8_t Irq = 6;
    constexpr size_t Iterations = 10000;
    PicHardTailQueue tails;
    std::atomic<size_t> failures(0);

    std::thread producer([&]() {
        for (size_t sequence = 1; sequence <= Iterations; ++sequence)
        {
            PicHardTailRecord record = {};
            record.irq = Irq;
            record.controllerLifetime = sequence;
            record.dispatchGeneration = sequence * 3;
            record.hardStageGeneration = sequence * 5;
            record.threadedCookie = sequence * 7;
            record.controllerAck = IrqControllerAck::AfterHardStage;
            record.hardDisposition = HardIrqDisposition::Handled;
            record.hasThreadedStage = true;
            record.threadedPublished = true;
            record.admitted = true;
            while (!tails.publish(Irq, record))
            {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        for (size_t sequence = 1; sequence <= Iterations; ++sequence)
        {
            PicHardTailRecord record = {};
            while (!tails.take(Irq, record))
            {
                std::this_thread::yield();
            }
            if (record.controllerLifetime != sequence ||
                record.dispatchGeneration != sequence * 3 ||
                record.hardStageGeneration != sequence * 5 ||
                record.threadedCookie != sequence * 7 ||
                record.controllerAck != IrqControllerAck::AfterHardStage ||
                record.hardDisposition != HardIrqDisposition::Handled ||
                !record.hasThreadedStage || !record.threadedPublished ||
                !record.admitted)
            {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
            tails.complete(Irq);
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(failures.load(std::memory_order_relaxed), static_cast<size_t>(0));
}

TEST(PicControllerStateGate, ExistingOwnerReceivesPublishedHandoff)
{
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    const size_t lifetime = gate.currentLifetime(4);

    ASSERT_TRUE(gate.tryAcquireClean());
    EXPECT_FALSE(gate.tryAcquireClean());
    EXPECT_FALSE(gate.queueEntry(4, lifetime));

    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[4], static_cast<size_t>(1));
    EXPECT_EQ(actions.tail[4], static_cast<size_t>(0));
    EXPECT_EQ(actions.tailEoi[4], static_cast<size_t>(0));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, ReleasedOwnerIsRecoveredByProducer)
{
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    const size_t lifetime = gate.currentLifetime(7);

    ASSERT_TRUE(gate.tryAcquireClean());
    ASSERT_TRUE(gate.releaseIfIdle());

    ASSERT_TRUE(gate.queueEntry(7, lifetime));
    EXPECT_FALSE(gate.tryClaim());
    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[7], static_cast<size_t>(1));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, TailWorkPreservesEoiObligations)
{
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    const size_t lifetime3 = gate.currentLifetime(3);
    const size_t lifetime12 = gate.currentLifetime(12);

    ASSERT_TRUE(gate.tryAcquireClean());
    EXPECT_FALSE(gate.queueTail(3, lifetime3, false));
    EXPECT_FALSE(gate.queueTail(3, lifetime3, true));
    EXPECT_FALSE(gate.queueTail(12, lifetime12, true));

    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.tail[3], static_cast<size_t>(2));
    EXPECT_EQ(actions.tailEoi[3], static_cast<size_t>(1));
    EXPECT_EQ(actions.tail[12], static_cast<size_t>(1));
    EXPECT_EQ(actions.tailEoi[12], static_cast<size_t>(1));
    EXPECT_EQ(actions.entry[3], static_cast<size_t>(0));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, CoalescedWorkRetainsEveryCount)
{
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    const size_t lifetime = gate.currentLifetime(9);

    ASSERT_TRUE(gate.tryAcquireClean());
    for (size_t i = 0; i < 5; ++i)
    {
        EXPECT_FALSE(gate.queueEntry(9, lifetime));
    }
    for (size_t i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(gate.queueTail(9, lifetime, i != 0));
    }

    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[9], static_cast<size_t>(5));
    EXPECT_EQ(actions.tail[9], static_cast<size_t>(3));
    EXPECT_EQ(actions.tailEoi[9], static_cast<size_t>(2));
    EXPECT_FALSE(actions.empty());

    EXPECT_FALSE(gate.takePending(actions));
    EXPECT_TRUE(actions.empty());
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, PendingPublicationDefeatsIdleReleaseCas)
{
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    const size_t lifetime = gate.currentLifetime(1);

    ASSERT_TRUE(gate.tryAcquireClean());
    ASSERT_FALSE(gate.takePending(actions));
    ASSERT_TRUE(actions.empty());

    // This models publication between the owner's idle check and release CAS.
    EXPECT_FALSE(gate.queueEntry(1, lifetime));
    EXPECT_FALSE(gate.releaseIfIdle());

    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[1], static_cast<size_t>(1));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, LifetimeAdvanceDemotesQueuedEntry)
{
    constexpr size_t Irq = 4;
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    const size_t oldLifetime = gate.currentLifetime(Irq);

    ASSERT_TRUE(gate.tryAcquireClean());
    EXPECT_FALSE(gate.queueEntry(Irq, oldLifetime));
    gate.beginLineTransition(Irq);
    const size_t newLifetime = gate.finishLineTransition(Irq);
    ASSERT_NE(newLifetime, oldLifetime);

    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[Irq], static_cast<size_t>(0));
    EXPECT_EQ(actions.staleEntry[Irq], static_cast<size_t>(1));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, OldTailCannotQualifyAgainstReplacement)
{
    constexpr size_t Irq = 11;
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    const size_t oldLifetime = gate.currentLifetime(Irq);

    ASSERT_TRUE(gate.tryAcquireClean());
    gate.beginLineTransition(Irq);
    const size_t newLifetime = gate.finishLineTransition(Irq);
    ASSERT_NE(newLifetime, oldLifetime);
    EXPECT_FALSE(gate.queueTail(Irq, oldLifetime, true));
    EXPECT_FALSE(gate.queueTail(Irq, newLifetime, false));

    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.tail[Irq], static_cast<size_t>(1));
    EXPECT_EQ(actions.tailEoi[Irq], static_cast<size_t>(0));
    EXPECT_EQ(actions.staleTail[Irq], static_cast<size_t>(1));
    EXPECT_EQ(actions.staleTailEoi[Irq], static_cast<size_t>(1));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, TransitionPublicationIsAlwaysStale)
{
    constexpr size_t Irq = 6;
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;

    ASSERT_TRUE(gate.tryAcquireClean());
    gate.beginLineTransition(Irq);
    const size_t transition = gate.currentLifetime(Irq);
    ASSERT_EQ(transition, PicControllerStateGate::TransitionLifetime);
    EXPECT_FALSE(gate.queueEntry(Irq, transition));
    gate.finishLineTransition(Irq);

    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[Irq], static_cast<size_t>(0));
    EXPECT_EQ(actions.staleEntry[Irq], static_cast<size_t>(1));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, SafeReleasePublishesFreshLifetimeBeforeUnmask)
{
    constexpr size_t Irq = 6;
    PicIrqState state;
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;

    state.setAllEnabled(false);
    state.handlerRegistered(Irq, IrqPolicy::edgeHard(), IrqDelivery::Hard);
    ASSERT_TRUE(gate.tryAcquireClean());

    state.beginLineTransition(Irq);
    gate.beginLineTransition(Irq);
    const size_t lifetime = gate.finishLineTransition(Irq);
    ASSERT_NE(lifetime, PicControllerStateGate::TransitionLifetime);
    ASSERT_FALSE(state.enabled(Irq));

    EXPECT_FALSE(gate.queueEntry(Irq, lifetime));
    state.finishLineTransition(Irq);
    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[Irq], static_cast<size_t>(1));
    EXPECT_EQ(actions.staleEntry[Irq], static_cast<size_t>(0));
    EXPECT_TRUE(state.enabled(Irq));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, RemoteTransitionStaysStaleUntilSafeRelease)
{
    constexpr size_t Irq = 5;
    PicIrqState state;
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;

    state.setAllEnabled(false);
    state.handlerRegistered(Irq, IrqPolicy::edgeHard(), IrqDelivery::Hard);
    ASSERT_TRUE(gate.tryAcquireClean());

    state.beginLineTransition(Irq);
    gate.beginLineTransition(Irq);
    state.finishLineTransition(Irq);
    uint16_t temporary = preserveUnsafePicUnmasks(
        static_cast<uint16_t>(1U << Irq), state.mask(), 0,
        static_cast<uint16_t>(1U << Irq), false);

    EXPECT_EQ(
        gate.currentLifetime(Irq),
        PicControllerStateGate::TransitionLifetime);
    EXPECT_NE(
        effectivePicMask(state.mask(), temporary) & (1U << Irq),
        static_cast<uint16_t>(0));
    EXPECT_FALSE(gate.queueEntry(Irq, gate.currentLifetime(Irq)));
    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[Irq], static_cast<size_t>(0));
    EXPECT_EQ(actions.staleEntry[Irq], static_cast<size_t>(1));

    const size_t lifetime = gate.finishLineTransition(Irq);
    temporary = 0;
    EXPECT_NE(lifetime, PicControllerStateGate::TransitionLifetime);
    EXPECT_EQ(
        effectivePicMask(state.mask(), temporary) & (1U << Irq),
        static_cast<uint16_t>(0));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, RetransitionCancelsDeferredPublication)
{
    constexpr size_t Irq = 10;
    PicIrqState state;
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    uint16_t deferred = 0;

    state.setAllEnabled(false);
    state.handlerRegistered(Irq, IrqPolicy::edgeHard(), IrqDelivery::Hard);
    ASSERT_TRUE(gate.tryAcquireClean());

    state.beginLineTransition(Irq);
    gate.beginLineTransition(Irq);
    state.finishLineTransition(Irq);
    deferred |= static_cast<uint16_t>(1U << Irq);

    deferred = cancelDeferredPicLineTransition(deferred, Irq);
    state.beginLineTransition(Irq);
    gate.beginLineTransition(Irq);
    EXPECT_EQ(deferred & (1U << Irq), static_cast<uint16_t>(0));
    EXPECT_EQ(
        gate.currentLifetime(Irq),
        PicControllerStateGate::TransitionLifetime);

    EXPECT_FALSE(gate.queueEntry(Irq, gate.currentLifetime(Irq)));
    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[Irq], static_cast<size_t>(0));
    EXPECT_EQ(actions.staleEntry[Irq], static_cast<size_t>(1));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, ConcurrentLifetimeChangesLoseNoPublications)
{
    constexpr size_t Irq = 11;
    constexpr size_t ProducerCount = 4;
    constexpr size_t PublicationsPerProducer = 1024;
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    std::atomic<bool> start(false);
    std::thread producers[ProducerCount];

    ASSERT_TRUE(gate.tryAcquireClean());
    for (size_t producer = 0; producer < ProducerCount; ++producer)
    {
        producers[producer] = std::thread([&gate, &start]() {
            while (!start.load(std::memory_order_acquire))
            {
            }
            for (size_t i = 0; i < PublicationsPerProducer; ++i)
            {
                gate.queueEntry(Irq, gate.currentLifetime(Irq));
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (size_t lifetime = 0; lifetime < 128; ++lifetime)
    {
        gate.beginLineTransition(Irq);
        gate.finishLineTransition(Irq);
    }
    for (size_t producer = 0; producer < ProducerCount; ++producer)
    {
        producers[producer].join();
    }

    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(
        actions.entry[Irq] + actions.staleEntry[Irq],
        ProducerCount * PublicationsPerProducer);
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, NonUrgentWorkCanTransferToContinuation)
{
    constexpr size_t Irq = 5;
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    const size_t lifetime = gate.currentLifetime(Irq);

    ASSERT_TRUE(gate.tryAcquireClean());
    EXPECT_FALSE(gate.queueEntry(Irq, lifetime));
    EXPECT_TRUE(gate.hasPending());
    EXPECT_FALSE(gate.urgentPending());
    ASSERT_TRUE(gate.relinquishOwnerForContinuation());

    ASSERT_TRUE(gate.tryClaim());
    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[Irq], static_cast<size_t>(1));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, SchedulerWorkCannotBeStrandedOnWorkerWake)
{
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    const size_t lifetime = gate.currentLifetime(0);

    ASSERT_TRUE(gate.tryAcquireClean());
    EXPECT_FALSE(gate.queueEntry(0, lifetime));
    EXPECT_TRUE(gate.hasPending());
    EXPECT_TRUE(gate.urgentPending());
    EXPECT_FALSE(gate.relinquishOwnerForContinuation());

    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[0], static_cast<size_t>(1));
    EXPECT_FALSE(gate.urgentPending());
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicControllerStateGate, SchedulerDropRestoresBeforeContinuationHandoff)
{
    PicIrqState state;
    PicControllerStateGate gate;
    PicControllerStateGate::PendingActions actions;
    size_t realEntries[PicIrqState::LineCount] = {};
    size_t spuriousEntries[PicIrqState::LineCount] = {};

    state.setAllEnabled(false);
    state.schedulerRegistered(0, IrqPolicy::edgeHard());
    ASSERT_TRUE(gate.tryAcquireClean());
    ASSERT_FALSE(gate.queueEntry(0, gate.currentLifetime(0)));
    ASSERT_TRUE(gate.takePending(actions));
    realEntries[0] = actions.entry[0];

    const PicContentionLineResult first = resolvePicContentionLine(
        state, 0, realEntries[0], actions.tail[0], actions.tailEoi[0]);
    ASSERT_TRUE(first.schedulerDrop);
    uint16_t temporary = temporaryPicMaskForDeferredWork(
        0, realEntries, actions, spuriousEntries);
    ASSERT_EQ(temporary, static_cast<uint16_t>(1));

    const uint16_t restored = restorablePicTemporaryMask(temporary, 0);
    temporary &= static_cast<uint16_t>(~restored);
    EXPECT_EQ(effectivePicMask(state.mask(), temporary) & 1U, 0U);

    // Model a PIT edge arriving after the remote physical unmask but before
    // Owner can become Clean. Its publication must defeat release and force
    // the current owner to account another terminal batch.
    EXPECT_FALSE(gate.queueEntry(0, gate.currentLifetime(0)));
    EXPECT_FALSE(gate.releaseIfIdle());
    ASSERT_TRUE(gate.takePending(actions));
    EXPECT_EQ(actions.entry[0], static_cast<size_t>(1));
    EXPECT_TRUE(gate.releaseIfIdle());
}

TEST(PicContentionActions, DeviceEntryMasksBeforeMasterSpecificEoi)
{
    constexpr size_t Irq = 4;
    PicIrqState state;
    state.setAllEnabled(false);
    state.handlerRegistered(Irq, IrqPolicy::edgeHard(), IrqDelivery::Hard);

    PicControllerStateGate::PendingActions pending;
    size_t realEntries[PicIrqState::LineCount] = {};
    pending.entry[Irq] = realEntries[Irq] = 1;
    const PicContentionLineResult result = resolvePicContentionLine(
        state, Irq, realEntries[Irq], pending.tail[Irq], pending.tailEoi[Irq]);

    PicWriteRecorder recorder;
    emitPicContentionWrites(
        state, result.maskChanged, realEntries, pending, 0, recorder);

    EXPECT_TRUE(result.terminalWork);
    EXPECT_FALSE(result.schedulerDrop);
    EXPECT_TRUE(result.quarantine);
    EXPECT_FALSE(result.invalidateThreaded);
    EXPECT_EQ(result.unhandledOccurrences, static_cast<size_t>(1));
    EXPECT_TRUE(state.acknowledgementPending(Irq));
    EXPECT_FALSE(state.enabled(Irq));
    ASSERT_FALSE(recorder.overflow);
    ASSERT_EQ(recorder.count, static_cast<size_t>(3));
    expectWrite(
        recorder, 0, PicControllerWriteTarget::MasterMask,
        state.masterMask());
    expectWrite(
        recorder, 1, PicControllerWriteTarget::SlaveMask, state.slaveMask());
    expectWrite(
        recorder, 2, PicControllerWriteTarget::MasterCommand, 0x64);
}

TEST(PicContentionActions, CurrentRtcThreadedEntrySurvivesControllerOwner)
{
    constexpr size_t Irq = 8;
    PicIrqState state;
    state.setAllEnabled(false);
    state.handlerRegistered(
        Irq, IrqPolicy::levelThreaded(), IrqDelivery::Threaded);

    PicControllerStateGate gate;
    ASSERT_TRUE(gate.tryAcquireClean());
    const size_t lifetime = gate.currentLifetime(Irq);

    // Model IRQ8 arriving while an ordinary thread owns the controller gate.
    // Hard entry cannot wait, but the current threaded-only lifetime does not
    // need the interrupted register frame and must survive for normal worker
    // admission by the eventual owner.
    EXPECT_FALSE(gate.queueEntry(Irq, lifetime));
    PicControllerStateGate::PendingActions pending;
    ASSERT_TRUE(gate.takePending(pending));
    ASSERT_EQ(pending.entry[Irq], static_cast<size_t>(1));

    const PicContentionLineResult result = resolvePicContentionLine(
        state, Irq, pending.entry[Irq], pending.tail[Irq],
        pending.tailEoi[Irq]);

    EXPECT_TRUE(result.terminalWork);
    EXPECT_FALSE(result.schedulerDrop);
    EXPECT_FALSE(result.quarantine);
    EXPECT_FALSE(result.invalidateThreaded);
    EXPECT_EQ(result.threadedOccurrences, static_cast<size_t>(1));
    EXPECT_EQ(result.unhandledOccurrences, static_cast<size_t>(0));
    EXPECT_TRUE(state.enabled(Irq));
    EXPECT_FALSE(state.acknowledgementPending(Irq));
    EXPECT_EQ(state.dispatchGeneration(Irq), static_cast<size_t>(0));

    size_t remainingEntries[PicIrqState::LineCount] = {};
    remainingEntries[Irq] = pending.entry[Irq];
    remainingEntries[Irq] -= result.threadedOccurrences;
    pending.entry[Irq] -= result.threadedOccurrences;
    PicWriteRecorder recorder;
    emitPicContentionWrites(
        state, false, remainingEntries, pending, 0, recorder);
    EXPECT_EQ(recorder.count, static_cast<size_t>(0));

    // The retained occurrence now enters the ordinary threaded state path:
    // the level line closes before its EOI and reopens only after its worker.
    const size_t generation = state.beginDispatch(Irq);
    state.beginThreadedDispatch(Irq);
    EXPECT_FALSE(state.enabled(Irq));
    EXPECT_TRUE(state.threadedPending(Irq));
    EXPECT_TRUE(state.completeThreadedDispatch(Irq, generation, true));
    EXPECT_TRUE(state.enabled(Irq));
}

TEST(PicContentionActions, DeferredWorkUsesOnlyPhysicalOverride)
{
    PicControllerStateGate::PendingActions stale;
    size_t realEntries[PicIrqState::LineCount] = {};
    size_t spuriousEntries[PicIrqState::LineCount] = {};
    realEntries[4] = 1;
    stale.tail[5] = 1;
    stale.tailEoi[12] = 1;
    spuriousEntries[7] = 1;
    spuriousEntries[15] = 1;

    const uint16_t existing = static_cast<uint16_t>(1U << 9);
    const uint16_t temporary = temporaryPicMaskForDeferredWork(
        existing, realEntries, stale, spuriousEntries);

    EXPECT_NE(temporary & (1U << 4), static_cast<uint16_t>(0));
    EXPECT_EQ(temporary & (1U << 5), static_cast<uint16_t>(0));
    EXPECT_NE(temporary & (1U << 7), static_cast<uint16_t>(0));
    EXPECT_NE(temporary & (1U << 9), static_cast<uint16_t>(0));
    EXPECT_NE(temporary & (1U << 12), static_cast<uint16_t>(0));
    EXPECT_NE(temporary & (1U << 15), static_cast<uint16_t>(0));
}

TEST(PicContentionActions, SpuriousSlaveMasksBeforeCascadeEoiAndRestore)
{
    constexpr size_t Irq = 15;
    PicIrqState state;
    state.setAllEnabled(false);
    state.handlerRegistered(Irq, IrqPolicy::edgeHard(), IrqDelivery::Hard);
    const uint16_t canonical = state.mask();

    PicControllerStateGate::PendingActions pending;
    size_t realEntries[PicIrqState::LineCount] = {};
    size_t spuriousEntries[PicIrqState::LineCount] = {};
    pending.entry[Irq] = 1;
    spuriousEntries[Irq] = 1;
    const uint16_t temporary = temporaryPicMaskForDeferredWork(
        0, realEntries, pending, spuriousEntries);

    PicWriteRecorder recorder;
    emitPicMaskWrites(effectivePicMask(canonical, temporary), recorder);
    emitPicContentionWrites(state, false, realEntries, pending, 1, recorder);
    emitPicMaskWrites(canonical, recorder);

    ASSERT_FALSE(recorder.overflow);
    ASSERT_EQ(recorder.count, static_cast<size_t>(5));
    expectWrite(
        recorder, 0, PicControllerWriteTarget::MasterMask,
        static_cast<uint8_t>(effectivePicMask(canonical, temporary) & 0xFF));
    expectWrite(
        recorder, 1, PicControllerWriteTarget::SlaveMask,
        static_cast<uint8_t>(effectivePicMask(canonical, temporary) >> 8));
    expectWrite(
        recorder, 2, PicControllerWriteTarget::MasterCommand, 0x62);
    expectWrite(
        recorder, 3, PicControllerWriteTarget::MasterMask,
        static_cast<uint8_t>(canonical & 0xFF));
    expectWrite(
        recorder, 4, PicControllerWriteTarget::SlaveMask,
        static_cast<uint8_t>(canonical >> 8));
}

TEST(PicContentionActions, StaleWorkDoesNotMutateReplacementState)
{
    constexpr size_t Irq = 4;
    PicIrqState state;
    state.setAllEnabled(false);
    state.handlerRegistered(Irq, IrqPolicy::edgeHard(), IrqDelivery::Hard);
    const size_t dispatchGeneration = state.dispatchGeneration(Irq);
    const size_t acknowledgedGeneration = state.acknowledgedGeneration(Irq);
    const uint16_t canonical = state.mask();

    PicControllerStateGate::PendingActions stale;
    size_t realEntries[PicIrqState::LineCount] = {};
    size_t spuriousEntries[PicIrqState::LineCount] = {};
    realEntries[Irq] = 1;
    stale.tail[Irq] = 1;
    stale.tailEoi[Irq] = 1;
    const uint16_t temporary = temporaryPicMaskForDeferredWork(
        0, realEntries, stale, spuriousEntries);
    PicWriteRecorder recorder;
    emitPicMaskWrites(effectivePicMask(canonical, temporary), recorder);
    emitPicContentionWrites(state, false, realEntries, stale, 0, recorder);
    emitPicMaskWrites(canonical, recorder);

    EXPECT_EQ(state.dispatchGeneration(Irq), dispatchGeneration);
    EXPECT_EQ(state.acknowledgedGeneration(Irq), acknowledgedGeneration);
    EXPECT_FALSE(state.acknowledgementPending(Irq));
    EXPECT_EQ(state.mask(), canonical);
}

TEST(PicContentionActions, SlaveEntryMasksBeforeSlaveAndCascadeEois)
{
    constexpr size_t Irq = 12;
    PicIrqState state;
    state.setAllEnabled(false);
    state.handlerRegistered(Irq, IrqPolicy::edgeHard(), IrqDelivery::Hard);

    PicControllerStateGate::PendingActions pending;
    size_t realEntries[PicIrqState::LineCount] = {};
    pending.entry[Irq] = realEntries[Irq] = 1;
    const PicContentionLineResult result = resolvePicContentionLine(
        state, Irq, realEntries[Irq], pending.tail[Irq], pending.tailEoi[Irq]);

    PicWriteRecorder recorder;
    emitPicContentionWrites(
        state, result.maskChanged, realEntries, pending, 0, recorder);

    EXPECT_TRUE(result.quarantine);
    EXPECT_TRUE(state.acknowledgementPending(Irq));
    EXPECT_FALSE(state.enabled(Irq));
    ASSERT_FALSE(recorder.overflow);
    ASSERT_EQ(recorder.count, static_cast<size_t>(4));
    expectWrite(
        recorder, 0, PicControllerWriteTarget::MasterMask,
        state.masterMask());
    expectWrite(
        recorder, 1, PicControllerWriteTarget::SlaveMask, state.slaveMask());
    expectWrite(
        recorder, 2, PicControllerWriteTarget::SlaveCommand, 0x64);
    expectWrite(
        recorder, 3, PicControllerWriteTarget::MasterCommand, 0x62);
}

TEST(PicContentionActions, SchedulerEntryDropsWithoutStickyMask)
{
    PicIrqState state;
    state.setAllEnabled(false);
    state.schedulerRegistered(0, IrqPolicy::edgeHard());

    PicControllerStateGate::PendingActions pending;
    size_t realEntries[PicIrqState::LineCount] = {};
    pending.entry[0] = realEntries[0] = 1;
    const PicContentionLineResult result = resolvePicContentionLine(
        state, 0, realEntries[0], pending.tail[0], pending.tailEoi[0]);

    PicWriteRecorder recorder;
    emitPicContentionWrites(
        state, result.maskChanged, realEntries, pending, 0, recorder);

    EXPECT_TRUE(result.terminalWork);
    EXPECT_TRUE(result.schedulerDrop);
    EXPECT_FALSE(result.quarantine);
    EXPECT_FALSE(result.invalidateThreaded);
    EXPECT_FALSE(result.maskChanged);
    EXPECT_EQ(result.unhandledOccurrences, static_cast<size_t>(1));
    EXPECT_TRUE(state.enabled(0));
    EXPECT_FALSE(state.acknowledgementPending(0));
    EXPECT_EQ(state.dispatchGeneration(0), static_cast<size_t>(1));
    EXPECT_EQ(state.acknowledgedGeneration(0), static_cast<size_t>(1));
    ASSERT_FALSE(recorder.overflow);
    ASSERT_EQ(recorder.count, static_cast<size_t>(1));
    expectWrite(
        recorder, 0, PicControllerWriteTarget::MasterCommand, 0x60);
}

TEST(PicContentionActions, SchedulerTailStillFailsClosed)
{
    PicIrqState state;
    state.setAllEnabled(false);
    state.schedulerRegistered(0, IrqPolicy::edgeHard());

    PicControllerStateGate::PendingActions pending;
    size_t realEntries[PicIrqState::LineCount] = {};
    pending.tail[0] = 1;
    pending.tailEoi[0] = 1;
    const PicContentionLineResult result = resolvePicContentionLine(
        state, 0, realEntries[0], pending.tail[0], pending.tailEoi[0]);

    PicWriteRecorder recorder;
    emitPicContentionWrites(
        state, result.maskChanged, realEntries, pending, 0, recorder);

    EXPECT_TRUE(result.terminalWork);
    EXPECT_FALSE(result.schedulerDrop);
    EXPECT_TRUE(result.quarantine);
    EXPECT_EQ(result.unhandledOccurrences, static_cast<size_t>(1));
    EXPECT_FALSE(state.enabled(0));
    EXPECT_TRUE(state.acknowledgementPending(0));
    ASSERT_FALSE(recorder.overflow);
    ASSERT_EQ(recorder.count, static_cast<size_t>(3));
    expectWrite(
        recorder, 0, PicControllerWriteTarget::MasterMask,
        state.masterMask());
    expectWrite(
        recorder, 1, PicControllerWriteTarget::SlaveMask, state.slaveMask());
    expectWrite(
        recorder, 2, PicControllerWriteTarget::MasterCommand, 0x60);
}

TEST(PicContentionActions, TailEoiOnlyBatchMasksBeforeEoi)
{
    constexpr size_t Irq = 3;
    PicIrqState state;
    state.setAllEnabled(false);
    state.handlerRegistered(Irq, IrqPolicy::edgeHard(), IrqDelivery::Hard);

    PicControllerStateGate::PendingActions pending;
    size_t realEntries[PicIrqState::LineCount] = {};
    pending.tailEoi[Irq] = 1;
    const PicContentionLineResult result = resolvePicContentionLine(
        state, Irq, realEntries[Irq], pending.tail[Irq], pending.tailEoi[Irq]);

    PicWriteRecorder recorder;
    emitPicContentionWrites(
        state, result.maskChanged, realEntries, pending, 0, recorder);

    EXPECT_TRUE(result.terminalWork);
    EXPECT_TRUE(result.quarantine);
    EXPECT_EQ(result.unhandledOccurrences, static_cast<size_t>(0));
    EXPECT_FALSE(state.enabled(Irq));
    EXPECT_TRUE(state.acknowledgementPending(Irq));
    ASSERT_FALSE(recorder.overflow);
    ASSERT_EQ(recorder.count, static_cast<size_t>(3));
    expectWrite(
        recorder, 0, PicControllerWriteTarget::MasterMask,
        state.masterMask());
    expectWrite(
        recorder, 1, PicControllerWriteTarget::SlaveMask, state.slaveMask());
    expectWrite(
        recorder, 2, PicControllerWriteTarget::MasterCommand, 0x63);
}

TEST(PicContentionActions, SchedulerDropReconcilesAnExistingMask)
{
    PicIrqState state;
    state.setAllEnabled(false);
    state.schedulerRegistered(0, IrqPolicy::edgeHard());
    const size_t priorGeneration = state.beginDispatch(0);
    state.completeDispatch(0, priorGeneration, true);
    ASSERT_TRUE(state.acknowledgementPending(0));
    ASSERT_FALSE(state.enabled(0));

    PicControllerStateGate::PendingActions pending;
    size_t realEntries[PicIrqState::LineCount] = {};
    pending.entry[0] = realEntries[0] = 1;
    const PicContentionLineResult result = resolvePicContentionLine(
        state, 0, realEntries[0], pending.tail[0], pending.tailEoi[0]);

    PicWriteRecorder recorder;
    emitPicContentionWrites(
        state, result.maskChanged, realEntries, pending, 0, recorder);

    EXPECT_TRUE(result.schedulerDrop);
    EXPECT_TRUE(result.maskChanged);
    EXPECT_FALSE(state.acknowledgementPending(0));
    EXPECT_TRUE(state.enabled(0));
    ASSERT_FALSE(recorder.overflow);
    ASSERT_EQ(recorder.count, static_cast<size_t>(3));
    expectWrite(
        recorder, 0, PicControllerWriteTarget::MasterMask,
        state.masterMask());
    expectWrite(
        recorder, 1, PicControllerWriteTarget::SlaveMask, state.slaveMask());
    expectWrite(
        recorder, 2, PicControllerWriteTarget::MasterCommand, 0x60);
}

TEST(PicContentionActions, TailWithoutEoiMasksButDoesNotAcknowledge)
{
    constexpr size_t Irq = 3;
    PicIrqState state;
    state.setAllEnabled(false);
    state.handlerRegistered(Irq, IrqPolicy::edgeHard(), IrqDelivery::Hard);

    PicControllerStateGate::PendingActions pending;
    size_t realEntries[PicIrqState::LineCount] = {};
    pending.tail[Irq] = 1;
    const PicContentionLineResult result = resolvePicContentionLine(
        state, Irq, realEntries[Irq], pending.tail[Irq], pending.tailEoi[Irq]);

    PicWriteRecorder recorder;
    emitPicContentionWrites(
        state, result.maskChanged, realEntries, pending, 0, recorder);

    EXPECT_TRUE(result.quarantine);
    EXPECT_TRUE(result.maskChanged);
    EXPECT_TRUE(state.acknowledgementPending(Irq));
    ASSERT_FALSE(recorder.overflow);
    ASSERT_EQ(recorder.count, static_cast<size_t>(2));
    expectWrite(
        recorder, 0, PicControllerWriteTarget::MasterMask,
        state.masterMask());
    expectWrite(
        recorder, 1, PicControllerWriteTarget::SlaveMask, state.slaveMask());
}

TEST(PicContentionActions, SpuriousIrq7EmitsNoEoi)
{
    PicIrqState state;
    PicControllerStateGate::PendingActions pending;
    size_t realEntries[PicIrqState::LineCount] = {};
    pending.entry[7] = 1;

    PicWriteRecorder recorder;
    emitPicContentionWrites(state, false, realEntries, pending, 0, recorder);

    EXPECT_FALSE(recorder.overflow);
    EXPECT_EQ(recorder.count, static_cast<size_t>(0));
}

TEST(PicContentionActions, SpuriousIrq15EoisCascadeOnly)
{
    PicIrqState state;
    PicControllerStateGate::PendingActions pending;
    size_t realEntries[PicIrqState::LineCount] = {};
    pending.entry[15] = 1;

    PicWriteRecorder recorder;
    emitPicContentionWrites(state, false, realEntries, pending, 1, recorder);

    ASSERT_FALSE(recorder.overflow);
    ASSERT_EQ(recorder.count, static_cast<size_t>(1));
    expectWrite(
        recorder, 0, PicControllerWriteTarget::MasterCommand, 0x62);
}

TEST(PicContentionActions, RealSlaveEoiPrecedesSpuriousCascadeEoi)
{
    constexpr size_t Irq = 12;
    PicIrqState state;
    state.setAllEnabled(false);
    state.handlerRegistered(Irq, IrqPolicy::edgeHard(), IrqDelivery::Hard);

    PicControllerStateGate::PendingActions pending;
    size_t realEntries[PicIrqState::LineCount] = {};
    pending.entry[Irq] = realEntries[Irq] = 1;
    pending.entry[15] = 1;
    const PicContentionLineResult result = resolvePicContentionLine(
        state, Irq, realEntries[Irq], pending.tail[Irq], pending.tailEoi[Irq]);

    PicWriteRecorder recorder;
    emitPicContentionWrites(
        state, result.maskChanged, realEntries, pending, 1, recorder);

    ASSERT_FALSE(recorder.overflow);
    ASSERT_EQ(recorder.count, static_cast<size_t>(5));
    expectWrite(
        recorder, 0, PicControllerWriteTarget::MasterMask,
        state.masterMask());
    expectWrite(
        recorder, 1, PicControllerWriteTarget::SlaveMask, state.slaveMask());
    expectWrite(
        recorder, 2, PicControllerWriteTarget::SlaveCommand, 0x64);
    expectWrite(
        recorder, 3, PicControllerWriteTarget::MasterCommand, 0x62);
    expectWrite(
        recorder, 4, PicControllerWriteTarget::MasterCommand, 0x62);
}
