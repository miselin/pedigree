/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "pedigree/kernel/debugger/commands/IrqDiagnosticRenderer.h"

TEST(IrqDiagnosticRenderer, RendersDetachedThreadedState)
{
    IrqLineDiagnosticSnapshot snapshot = {};
    snapshot.snapshotGeneration = 9;
    snapshot.dispatchGeneration = 31;
    snapshot.acknowledgedGeneration = 30;
    snapshot.activeHardDispatchCount = 1;
    snapshot.activeHardDispatchGeneration = 31;
    snapshot.publicationCookie = 33;
    snapshot.pendingCookie = 33;
    snapshot.activeCookie = 32;
    snapshot.completedCookie = 31;
    snapshot.completedBatches = 7;
    snapshot.publicationFailures = 2;
    snapshot.diagnosticPublicationFailures = 4;
    snapshot.workerIdentity = 0xfeed;
    snapshot.handlerCount = 2;
    snapshot.maskReasons =
        IrqMaskAwaitingAcknowledgement | IrqMaskAwaitingThreadedCompletion;
    snapshot.line = 5;
    snapshot.delivery = IrqDelivery::Threaded;
    snapshot.trigger = IrqTrigger::Level;
    snapshot.controllerAck = IrqControllerAck::AfterHardStage;
    snapshot.lineRelease = IrqLineRelease::AfterThreadedCompletion;
    snapshot.configured = true;
    snapshot.effectiveMasked = true;
    snapshot.requestedEnabled = true;
    snapshot.acknowledgementPending = true;
    snapshot.threadedPending = true;
    snapshot.dispatcherInitialised = true;
    snapshot.dispatcherActive = true;
    snapshot.hardStageActive = true;

    HugeStaticString line;
    IrqDiagnosticRenderer::render(snapshot, line);

    EXPECT_STREQ(
        "irq 5 snapshot=9 configured=yes handlers=2 delivery=threaded "
        "trigger=level ack=after-hard release=after-threaded "
        "requested=enabled masked=yes "
        "mask=awaiting-ack,awaiting-threaded dispatch-gen=31 ack-gen=30 "
        "hard-count=1 hard-gen=31\n"
        "  worker=0xfeed cookies[published=33 pending=33 active=32 "
        "completed=31] batches=7 failures[worker-publish=2 diag-publish=4] "
        "state[ack-pending=yes threaded-pending=yes worker-initialised=yes "
        "worker-active=yes worker-closed=no hard-stage-active=yes]\n",
        static_cast<const char *>(line));
}

TEST(IrqDiagnosticRenderer, DistinguishesSnapshotMiss)
{
    IrqLineDiagnosticSnapshot snapshot = {};
    snapshot.line = 14;
    HugeStaticString line;
    IrqDiagnosticRenderer::render(snapshot, line);

    EXPECT_STREQ("irq 14 SNAPSHOT MISS\n", static_cast<const char *>(line));
}

TEST(IrqDiagnosticRenderer, RendersEveryMaskReasonAndUnknownBits)
{
    IrqLineDiagnosticSnapshot snapshot = {};
    snapshot.snapshotGeneration = 1;
    snapshot.line = 3;
    snapshot.maskReasons = IrqMaskNoHandler | IrqMaskAdministrativelyDisabled |
                           IrqMaskAwaitingAcknowledgement |
                           IrqMaskAwaitingThreadedCompletion |
                           IrqMaskMitigated | IrqMaskShuttingDown | 0x8000;

    HugeStaticString line;
    IrqDiagnosticRenderer::render(snapshot, line);

    EXPECT_TRUE(line.contains(
        "mask=no-handler,admin-disabled,awaiting-ack,awaiting-threaded,"
        "mitigated,shutting-down,unknown:0x8000"));
    EXPECT_TRUE(line.contains(
        "configured=no handlers=0 delivery=none trigger=n/a ack=n/a "
        "release=n/a"));
}

TEST(IrqDiagnosticRenderer, RetainsTheCompleteLineAtMaximumValues)
{
    IrqLineDiagnosticSnapshot snapshot = {};
    snapshot.snapshotGeneration = ~static_cast<size_t>(0);
    snapshot.dispatchGeneration = ~static_cast<size_t>(0);
    snapshot.acknowledgedGeneration = ~static_cast<size_t>(0);
    snapshot.activeHardDispatchCount = ~static_cast<size_t>(0);
    snapshot.activeHardDispatchGeneration = ~static_cast<size_t>(0);
    snapshot.publicationCookie = ~static_cast<size_t>(0);
    snapshot.pendingCookie = ~static_cast<size_t>(0);
    snapshot.activeCookie = ~static_cast<size_t>(0);
    snapshot.completedCookie = ~static_cast<size_t>(0);
    snapshot.completedBatches = ~static_cast<size_t>(0);
    snapshot.publicationFailures = ~static_cast<size_t>(0);
    snapshot.diagnosticPublicationFailures = ~static_cast<size_t>(0);
    snapshot.workerIdentity = ~static_cast<uintptr_t>(0);
    snapshot.handlerCount = ~static_cast<size_t>(0);
    snapshot.maskReasons = 0xffff;
    snapshot.line = 255;
    snapshot.delivery = IrqDelivery::Threaded;
    snapshot.trigger = IrqTrigger::Synthetic;
    snapshot.controllerAck = IrqControllerAck::BeforeHardStage;
    snapshot.lineRelease = IrqLineRelease::AfterThreadedCompletion;
    snapshot.configured = true;
    snapshot.effectiveMasked = true;
    snapshot.requestedEnabled = true;
    snapshot.acknowledgementPending = true;
    snapshot.threadedPending = true;
    snapshot.dispatcherInitialised = true;
    snapshot.dispatcherActive = true;
    snapshot.dispatcherClosed = true;
    snapshot.hardStageActive = true;

    HugeStaticString line;
    IrqDiagnosticRenderer::render(snapshot, line);
    NormalStaticString worker;
    worker += "worker=0x";
    worker.append(snapshot.workerIdentity, 16);

    EXPECT_LT(line.length(), 1023U);
    EXPECT_TRUE(line.contains(static_cast<const char *>(worker)));
    EXPECT_TRUE(line.contains("hard-gen=ambiguous"));
    EXPECT_TRUE(line.contains("hard-stage-active=yes]\n"));
}
