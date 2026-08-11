/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "system/kernel/machine/mach_pc/LocalApicTlbShootdown.h"
#include <gtest/gtest.h>

class LocalApicTlbShootdownTestPeer {
 public:
  static bool closeClaimBeforeRevalidation(LocalApicTlbShootdown& shootdown, size_t processor,
                                           LocalApicTlbShootdown::Service& service) {
    const size_t generation = shootdown.m_Generation.value();
    const size_t serving = shootdown.servingToken(generation);
    const size_t acknowledged = shootdown.acknowledgedToken(generation);
    const size_t previous = shootdown.m_ProcessorState[processor].value();
    if (!generation || shootdown.isServing(previous) || previous == acknowledged ||
        !shootdown.m_ProcessorState[processor].compareAndSwap(previous, serving)) {
      return false;
    }

    const uintptr_t address = shootdown.m_Address.value();
    shootdown.close();
    return shootdown.revalidateServiceClaim(processor, generation, address, serving, acknowledged,
                                            service);
  }

  static void seedRolloverCollision(LocalApicTlbShootdown& shootdown, size_t processor) {
    shootdown.m_NextGeneration = shootdown.MaxGeneration;
    shootdown.m_ProcessorState[processor] = shootdown.acknowledgedToken(1);
  }

  static bool processorAcknowledged(LocalApicTlbShootdown& shootdown, size_t processor,
                                    size_t generation) {
    return shootdown.m_ProcessorState[processor].value() == shootdown.acknowledgedToken(generation);
  }
};

TEST(LocalApicProcessorControlOwner, SerialisesControlAndRequiresOwningProcessor) {
  LocalApicProcessorControlOwner owner;
  EXPECT_FALSE(owner.owned());
  ASSERT_TRUE(owner.tryAcquire(0));
  EXPECT_TRUE(owner.owned());
  EXPECT_TRUE(owner.ownedBy(0));
  EXPECT_FALSE(owner.quiescedBy(0));
  EXPECT_FALSE(owner.ownedBy(1));
  EXPECT_FALSE(owner.tryAcquire(0));
  EXPECT_FALSE(owner.claimQuiesced(0));
  EXPECT_FALSE(owner.tryAcquire(1));
  EXPECT_FALSE(owner.release(1));
  ASSERT_TRUE(owner.release(0));
  EXPECT_FALSE(owner.owned());
  EXPECT_TRUE(owner.tryAcquire(1));
  EXPECT_TRUE(owner.release(1));
}

TEST(LocalApicProcessorControlOwner, RetainedQuiesceOwnsClosedGateUntilResume) {
  LocalApicProcessorControlOwner owner;
  LocalApicTlbMutationGate gate;

  ASSERT_TRUE(owner.tryAcquire(0));
  ASSERT_TRUE(gate.closeReversible());
  ASSERT_TRUE(owner.markQuiesced(0));
  EXPECT_TRUE(owner.quiescedBy(0));
  EXPECT_FALSE(owner.tryAcquire(1));
  EXPECT_FALSE(owner.claimQuiesced(1));
  EXPECT_FALSE(owner.release(0));

  // Resume claims the retained operation, restores mapping admission, and
  // only then publishes the owner as free.
  ASSERT_TRUE(owner.claimQuiesced(0));
  ASSERT_TRUE(gate.reopen());
  EXPECT_FALSE(owner.tryAcquire(1));
  ASSERT_TRUE(owner.release(0));
  EXPECT_TRUE(owner.tryAcquire(1));
  EXPECT_TRUE(owner.release(1));
}

TEST(LocalApicProcessorControlOwner, TerminalRetryCannotStealLiveHalt) {
  LocalApicProcessorControlOwner owner;

  ASSERT_TRUE(owner.tryAcquire(0));
  EXPECT_TRUE(owner.activeBy(0));
  EXPECT_FALSE(owner.claimTerminal(0));
  EXPECT_FALSE(owner.tryAcquire(1));

  // Only the invocation which is about to return marks the terminal
  // operation retained. A later same-CPU retry can then claim and finish it.
  ASSERT_TRUE(owner.markTerminal(0));
  EXPECT_TRUE(owner.terminalBy(0));
  EXPECT_TRUE(owner.ownedBy(0));
  EXPECT_FALSE(owner.activeBy(0));
  EXPECT_FALSE(owner.release(0));
  EXPECT_FALSE(owner.claimTerminal(1));
  ASSERT_TRUE(owner.claimTerminal(0));
  EXPECT_TRUE(owner.activeBy(0));
  EXPECT_TRUE(owner.release(0));
}

TEST(LocalApicProcessorControlOwner, TerminalControlCanAdoptRetainedRemoteQuiesce) {
  LocalApicProcessorControlOwner owner;
  ASSERT_TRUE(owner.tryAcquire(1));
  ASSERT_TRUE(owner.markQuiesced(1));

  EXPECT_TRUE(owner.claimAnyQuiesced(3));
  EXPECT_TRUE(owner.activeBy(3));
  EXPECT_FALSE(owner.ownedBy(1));
  EXPECT_TRUE(owner.markTerminal(3));
}

TEST(LocalApicProcessorControlOwner, TerminalControlCanAdoptOnlyCompletedRemoteFailure) {
  LocalApicProcessorControlOwner owner;
  ASSERT_TRUE(owner.tryAcquire(1));

  EXPECT_FALSE(owner.claimAnyFailed(3));
  ASSERT_TRUE(owner.markFailed(1));
  EXPECT_TRUE(owner.failedBy(1));
  EXPECT_TRUE(owner.claimAnyFailed(3));
  EXPECT_TRUE(owner.activeBy(3));
  EXPECT_FALSE(owner.ownedBy(1));
  EXPECT_TRUE(owner.markTerminal(3));
}

TEST(LocalApicTlbMutationGate, CloseDrainsAdmittedMutationAndRejectsNewOnes) {
  LocalApicTlbMutationGate gate;
  ASSERT_TRUE(gate.tryEnter());
  EXPECT_EQ(gate.active(), 1U);

  ASSERT_TRUE(gate.closeReversible());
  EXPECT_TRUE(gate.closed());
  EXPECT_FALSE(gate.drained());
  EXPECT_FALSE(gate.tryEnter());

  EXPECT_TRUE(gate.leave());
  EXPECT_TRUE(gate.drained());
  EXPECT_TRUE(gate.reopen());
  EXPECT_FALSE(gate.closed());
  EXPECT_TRUE(gate.tryEnter());
  EXPECT_TRUE(gate.leave());
}

TEST(LocalApicTlbMutationGate, CancelledCloseRestoresAdmissionWithSuspendedLease) {
  LocalApicProcessorControlOwner owner;
  LocalApicTlbMutationGate gate;
  ASSERT_TRUE(owner.tryAcquire(0));
  ASSERT_TRUE(gate.tryEnter());
  ASSERT_TRUE(gate.closeReversible());
  ASSERT_FALSE(gate.drained());

  ASSERT_TRUE(gate.cancelClose());
  EXPECT_FALSE(gate.closed());
  EXPECT_FALSE(owner.tryAcquire(1));
  EXPECT_TRUE(gate.tryEnter());
  EXPECT_TRUE(gate.leave());
  EXPECT_TRUE(gate.leave());
  EXPECT_TRUE(gate.drained());
  EXPECT_TRUE(owner.release(0));
}

TEST(LocalApicTlbMutationGate, RetirementWaitsAcrossMutationAndShootdownPublication) {
  LocalApicTlbMutationGate gate;
  LocalApicTlbShootdown shootdown;

  // The mapper is admitted before it changes the PTE. Retirement closes the
  // gate in the exact window after that write but before publish.
  ASSERT_TRUE(gate.tryEnter());
  ASSERT_TRUE(gate.closeReversible());
  EXPECT_FALSE(gate.drained());
  EXPECT_FALSE(gate.tryEnter());

  // Because the admitted mapper remains runnable, it can publish and finish
  // the remote invalidation even though later mapping admission is closed.
  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000001000ULL, 0, 2));
  LocalApicTlbShootdown::Service remote;
  ASSERT_TRUE(shootdown.beginService(1, remote));
  ASSERT_TRUE(shootdown.finishService(remote));
  ASSERT_TRUE(shootdown.complete());
  shootdown.close();
  ASSERT_TRUE(shootdown.release());
  ASSERT_TRUE(gate.leave());

  // Only now can retirement own the empty shootdown barrier and select the
  // Paused state; there is no mapper or published generation to strand.
  ASSERT_TRUE(gate.drained());
  ASSERT_TRUE(shootdown.tryAcquire());
  EXPECT_TRUE(shootdown.drained());
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbMutationGate, TerminalCloseCannotBeCancelledOrReopened) {
  LocalApicTlbMutationGate gate;
  ASSERT_TRUE(gate.tryEnter());
  ASSERT_TRUE(gate.closeReversible());
  ASSERT_TRUE(gate.closeTerminal());
  EXPECT_TRUE(gate.terminalClosed());
  EXPECT_FALSE(gate.cancelClose());
  EXPECT_FALSE(gate.reopen());
  EXPECT_FALSE(gate.tryEnter());
  EXPECT_TRUE(gate.leave());
  EXPECT_TRUE(gate.drained());
  EXPECT_TRUE(gate.closeTerminal());
  EXPECT_TRUE(gate.terminalClosed());
}

TEST(LocalApicTlbTerminalFailure, FirstFailureElectsStableCoordinatorAndReason) {
  LocalApicTlbTerminalFailure failure;
  EXPECT_FALSE(failure.active());
  ASSERT_TRUE(failure.elect(2, 5));
  EXPECT_TRUE(failure.active());
  EXPECT_TRUE(failure.coordinator(2));
  EXPECT_FALSE(failure.coordinator(1));
  EXPECT_EQ(failure.reason(), 5U);
  EXPECT_FALSE(failure.elect(1, 3));
  EXPECT_EQ(failure.reason(), 5U);
}

TEST(LocalApicTlbShootdown, RejectsExplicitInterruptAndDebuggerContexts) {
  EXPECT_TRUE(LocalApicTlbShootdown::supportsContext(ExecutionContext::WaitableThread));
  EXPECT_TRUE(LocalApicTlbShootdown::supportsContext(ExecutionContext::AtomicThread));
  EXPECT_FALSE(LocalApicTlbShootdown::supportsContext(ExecutionContext::HardDeviceIrq));
  EXPECT_FALSE(LocalApicTlbShootdown::supportsContext(ExecutionContext::SchedulerIrq));
  EXPECT_FALSE(LocalApicTlbShootdown::supportsContext(ExecutionContext::HostedSyntheticIrq));
  EXPECT_FALSE(LocalApicTlbShootdown::supportsContext(ExecutionContext::DebuggerTrap));
}

TEST(LocalApicTlbShootdown, UsesLocalInvalidationOnlyAfterEveryPeerIsTerminal) {
  EXPECT_FALSE(LocalApicTlbShootdown::onlyCurrentProcessorServiceable(0, 0));
  EXPECT_TRUE(LocalApicTlbShootdown::onlyCurrentProcessorServiceable(1, 0));
  EXPECT_FALSE(LocalApicTlbShootdown::onlyCurrentProcessorServiceable(4, 2));
  EXPECT_TRUE(LocalApicTlbShootdown::onlyCurrentProcessorServiceable(4, 3));
}

TEST(LocalApicTlbShootdown, RejectedServiceNeverAcquiresReaderLease) {
  LocalApicTlbShootdown shootdown;
  LocalApicTlbShootdown::Service service;

  EXPECT_FALSE(shootdown.beginService(0, service));
  EXPECT_TRUE(shootdown.drained());
  EXPECT_EQ(shootdown.servicing(), 0U);

  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000111000ULL, 0, 2));
  EXPECT_FALSE(shootdown.beginService(0, service));
  EXPECT_TRUE(shootdown.drained());
  EXPECT_EQ(shootdown.servicing(), 0U);
  shootdown.close();
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbShootdown, RejectsDuplicateAndAlreadyAcknowledgedProcessorLeases) {
  LocalApicTlbShootdown shootdown;
  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000112000ULL, 0, 3));

  LocalApicTlbShootdown::Service primary;
  LocalApicTlbShootdown::Service duplicate;
  ASSERT_TRUE(shootdown.beginService(1, primary));
  EXPECT_FALSE(shootdown.beginService(1, duplicate));
  EXPECT_EQ(shootdown.servicing(), 1U);
  EXPECT_EQ(shootdown.acknowledgedMask(), 0x1ULL);

  EXPECT_TRUE(shootdown.finishService(primary));
  EXPECT_EQ(shootdown.servicing(), 0U);
  EXPECT_EQ(shootdown.acknowledgedMask(), 0x3ULL);
  EXPECT_FALSE(shootdown.complete());

  // An observer arriving after this CPU acknowledged is rejected without
  // creating a second lease for the same generation.
  EXPECT_FALSE(shootdown.beginService(1, duplicate));
  EXPECT_EQ(shootdown.servicing(), 0U);

  LocalApicTlbShootdown::Service final;
  ASSERT_TRUE(shootdown.beginService(2, final));
  EXPECT_EQ(shootdown.servicing(), 1U);
  EXPECT_FALSE(shootdown.complete());
  EXPECT_TRUE(shootdown.finishService(final));
  EXPECT_EQ(shootdown.servicing(), 0U);
  EXPECT_TRUE(shootdown.complete());

  shootdown.close();
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbShootdown, RejectsRetiringReaderAsNextGenerationInitiator) {
  LocalApicTlbShootdown shootdown;
  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000113000ULL, 0, 2));

  LocalApicTlbShootdown::Service retiring;
  ASSERT_TRUE(shootdown.beginService(1, retiring));
  shootdown.close();

  EXPECT_FALSE(shootdown.publish(0xFFFF900000114000ULL, 1, 2));
  EXPECT_FALSE(shootdown.finishService(retiring));
  ASSERT_TRUE(shootdown.publish(0xFFFF900000114000ULL, 1, 2));

  LocalApicTlbShootdown::Service remote;
  ASSERT_TRUE(shootdown.beginService(0, remote));
  EXPECT_TRUE(shootdown.finishService(remote));
  EXPECT_TRUE(shootdown.complete());
  shootdown.close();
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbShootdown, GenerationMismatchRetiresClaimWithoutAcknowledgingNewWork) {
  LocalApicTlbShootdown shootdown;
  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000115000ULL, 0, 2));
  const size_t generation = shootdown.generation();

  LocalApicTlbShootdown::Service rejected;
  EXPECT_FALSE(LocalApicTlbShootdownTestPeer::closeClaimBeforeRevalidation(shootdown, 1, rejected));
  EXPECT_EQ(rejected.generation, 0U);
  EXPECT_TRUE(LocalApicTlbShootdownTestPeer::processorAcknowledged(shootdown, 1, generation));
  EXPECT_TRUE(shootdown.drained());
  EXPECT_FALSE(shootdown.complete());
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbShootdown, RolloverSkipsGenerationTokenStillHeldByProcessor) {
  LocalApicTlbShootdown shootdown;
  ASSERT_TRUE(shootdown.tryAcquire());
  LocalApicTlbShootdownTestPeer::seedRolloverCollision(shootdown, 1);

  ASSERT_TRUE(shootdown.publish(0xFFFF900000116000ULL, 0, 2));
  EXPECT_EQ(shootdown.generation(), 2U);
  EXPECT_TRUE(LocalApicTlbShootdownTestPeer::processorAcknowledged(shootdown, 1, 1));

  LocalApicTlbShootdown::Service remote;
  ASSERT_TRUE(shootdown.beginService(1, remote));
  EXPECT_TRUE(shootdown.finishService(remote));
  EXPECT_TRUE(shootdown.complete());
  shootdown.close();
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbShootdown, PublishesAddressAndWaitsForEveryProcessor) {
  LocalApicTlbShootdown shootdown;
  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000123000ULL, 1, 4));

  EXPECT_NE(shootdown.generation(), 0U);
  EXPECT_EQ(shootdown.address(), 0xFFFF900000123000ULL);
  EXPECT_EQ(shootdown.expectedMask(), 0xFULL);
  EXPECT_EQ(shootdown.acknowledgedMask(), 0x2ULL);
  EXPECT_FALSE(shootdown.complete());
  LocalApicTlbShootdown::Service initiatorService;
  EXPECT_FALSE(shootdown.beginService(1, initiatorService));

  for (size_t processor : {0U, 2U, 3U}) {
    LocalApicTlbShootdown::Service service;
    ASSERT_TRUE(shootdown.beginService(processor, service));
    EXPECT_EQ(service.address, 0xFFFF900000123000ULL);
    EXPECT_TRUE(shootdown.finishService(service));
    EXPECT_FALSE(shootdown.beginService(processor, service));
  }

  EXPECT_TRUE(shootdown.complete());
  shootdown.close();
  EXPECT_TRUE(shootdown.drained());
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbShootdown, HoldsGenerationUntilServiceLeaseDrains) {
  LocalApicTlbShootdown shootdown;
  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000456000ULL, 0, 2));

  LocalApicTlbShootdown::Service service;
  ASSERT_TRUE(shootdown.beginService(1, service));
  EXPECT_EQ(shootdown.servicing(), 1U);

  shootdown.close();
  EXPECT_FALSE(shootdown.drained());
  EXPECT_FALSE(shootdown.release());
  EXPECT_FALSE(shootdown.finishService(service));
  EXPECT_EQ(shootdown.acknowledgedMask(), 0U);
  EXPECT_TRUE(shootdown.drained());
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbShootdown, NewGenerationIsolatedFromRetiringReader) {
  LocalApicTlbShootdown shootdown;
  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000456000ULL, 0, 2));

  LocalApicTlbShootdown::Service retiring;
  ASSERT_TRUE(shootdown.beginService(1, retiring));
  const size_t retiredGeneration = retiring.generation;
  shootdown.close();

  // A reader which speculated on the prior request can become visible after
  // the next owner is ready to publish. The generation tag, not a pre-publish
  // reader drain, prevents its acknowledgement from crossing transactions.
  ASSERT_TRUE(shootdown.publish(0xFFFF900000457000ULL, 0, 2));
  EXPECT_NE(shootdown.generation(), retiredGeneration);
  EXPECT_FALSE(shootdown.finishService(retiring));
  EXPECT_EQ(shootdown.acknowledgedMask(), 0x1ULL);

  LocalApicTlbShootdown::Service current;
  ASSERT_TRUE(shootdown.beginService(1, current));
  EXPECT_EQ(current.address, 0xFFFF900000457000ULL);
  EXPECT_TRUE(shootdown.finishService(current));
  EXPECT_TRUE(shootdown.complete());
  shootdown.close();
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbShootdown, ClosedFailedGenerationCanBeRetainedForTerminalAdoption) {
  LocalApicTlbShootdown shootdown;
  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000654000ULL, 0, 2));

  shootdown.close();
  EXPECT_TRUE(shootdown.retainedClosed());
  EXPECT_FALSE(shootdown.tryAcquire());
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbShootdown, SerialisesConcurrentPublishers) {
  LocalApicTlbShootdown shootdown;
  ASSERT_TRUE(shootdown.tryAcquire());
  EXPECT_FALSE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000789000ULL, 0, 1));
  EXPECT_TRUE(shootdown.complete());
  shootdown.close();
  ASSERT_TRUE(shootdown.release());

  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000ABC000ULL, 0, 1));
  EXPECT_EQ(shootdown.address(), 0xFFFF900000ABC000ULL);
  shootdown.close();
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbShootdown, TerminalBarrierCannotOvertakePublishedGeneration) {
  LocalApicTlbShootdown shootdown;

  // Model a remote processor which owns a published shootdown immediately
  // before shutdown starts closing the processor-control state.
  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0xFFFF900000DEF000ULL, 2, 4));
  EXPECT_FALSE(shootdown.tryAcquire());

  for (size_t processor : {0U, 1U, 3U}) {
    LocalApicTlbShootdown::Service service;
    ASSERT_TRUE(shootdown.beginService(processor, service));
    EXPECT_TRUE(shootdown.finishService(service));
  }
  ASSERT_TRUE(shootdown.complete());
  shootdown.close();
  ASSERT_TRUE(shootdown.release());

  // The terminal transition can now own an empty barrier. Keeping it held
  // across Paused -> Terminal excludes every later publisher until all peers
  // have committed to their permanent halt loops.
  ASSERT_TRUE(shootdown.tryAcquire());
  EXPECT_EQ(shootdown.generation(), 0U);
  EXPECT_TRUE(shootdown.drained());
  EXPECT_FALSE(shootdown.tryAcquire());
  EXPECT_TRUE(shootdown.release());
}

TEST(LocalApicTlbShootdown, RejectsInvalidTopologyWithoutPublishing) {
  LocalApicTlbShootdown shootdown;
  ASSERT_TRUE(shootdown.tryAcquire());
  EXPECT_FALSE(shootdown.publish(0x1000, 0, 0));
  EXPECT_FALSE(shootdown.publish(0x1000, 64, 64));
  EXPECT_FALSE(shootdown.publish(0x1000, 0, 65));
  EXPECT_EQ(shootdown.generation(), 0U);
  EXPECT_TRUE(shootdown.release());

  ASSERT_TRUE(shootdown.tryAcquire());
  ASSERT_TRUE(shootdown.publish(0x1000, 63, 64));
  EXPECT_EQ(shootdown.expectedMask(), ~uint64_t(0));
  shootdown.close();
  EXPECT_TRUE(shootdown.release());
}
