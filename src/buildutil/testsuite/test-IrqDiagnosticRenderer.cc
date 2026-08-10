/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/debugger/commands/IrqDiagnosticRenderer.h"

#include <gtest/gtest.h>

TEST(IrqDiagnosticRenderer, RendersDetachedThreadedState) {
  IrqLineDiagnosticSnapshot snapshot = {};
  snapshot.snapshotGeneration = 9;
  snapshot.observationTimestamp = 1000;
  snapshot.dispatchGeneration = 31;
  snapshot.acknowledgedGeneration = 30;
  snapshot.activeHardDispatchCount = 1;
  snapshot.activeHardDispatchGeneration = 31;
  snapshot.activeThreadedDispatchCount = 1;
  snapshot.publicationCookie = 33;
  snapshot.pendingCookie = 33;
  snapshot.activeCookie = 32;
  snapshot.completedCookie = 31;
  snapshot.completedBatches = 7;
  snapshot.pendingSinceTimestamp = 700;
  snapshot.activeCallbackStartedTimestamp = 900;
  snapshot.lastWakeLatency = 23;
  snapshot.maximumWakeLatency = 45;
  snapshot.lastCallbackRuntime = 67;
  snapshot.maximumCallbackRuntime = 89;
  snapshot.interruptCount = 101;
  snapshot.spuriousCount = 3;
  snapshot.unhandledCount = 2;
  snapshot.publicationFailures = 2;
  snapshot.removalRejections = 5;
  snapshot.controllerContentions = 6;
  snapshot.controllerPromptAttempts = 8;
  snapshot.controllerPromptFailures = 1;
  snapshot.controllerPromptDestination = 0x2a;
  snapshot.diagnosticPublicationFailures = 4;
  snapshot.workerIdentity = 0xfeed;
  snapshot.activeThreadedHandlerIdentity = 0xcafe;
  snapshot.workerDebugAddress = 0xabcd;
  snapshot.workerWaitQueue = 0x111;
  snapshot.workerWaitChannelOwner = 0x222;
  snapshot.workerWaitChannelValue = 0x333;
  snapshot.workerWaitStateLevel = 2;
  snapshot.handlerCount = 2;
  snapshot.maskReasons = IrqMaskAwaitingAcknowledgement | IrqMaskAwaitingThreadedCompletion;
  snapshot.line = 5;
  snapshot.delivery = IrqDelivery::Threaded;
  snapshot.trigger = IrqTrigger::Level;
  snapshot.controllerAck = IrqControllerAck::AfterHardStage;
  snapshot.lineRelease = IrqLineRelease::AfterThreadedCompletion;
  snapshot.workerDebugState = IrqWorkerDebugState::SemaphoreWait;
  snapshot.workerWaitReason = IrqWorkerWaitReason::Waiting;
  snapshot.controllerPromptState = IrqControllerPromptState::Failed;
  snapshot.configured = true;
  snapshot.effectiveMasked = true;
  snapshot.requestedEnabled = true;
  snapshot.acknowledgementPending = true;
  snapshot.threadedPending = true;
  snapshot.dispatcherInitialised = true;
  snapshot.dispatcherActive = true;
  snapshot.hardStageActive = true;
  snapshot.workerDiagnosticAvailable = true;
  snapshot.workerWaitActive = true;
  snapshot.workerWaitQueued = true;

  IrqDiagnosticString line;
  IrqDiagnosticRenderer::render(snapshot, line);

  EXPECT_STREQ(
      "irq 5 snap=9 cfg=yes handlers=2 delivery=threaded "
      "trigger=level ack=after-hard release=after-threaded "
      "req=on masked=yes "
      "mask=awaiting-ack,awaiting-threaded gen=31 acked=30 "
      "count[irq=101 spur=3 unhandled=2] "
      "hard[pins=1 gen=31]\n"
      "  worker=0xfeed handler=0xcafe pins=1 "
      "cookie[pub=33 pending=33 active=32 "
      "done=31] batches=7 fail[publish=2 remove=5 controller=6 diag=4] "
      "prompt[controller attempts=8 failures=1 dest=0x2a state=failed] "
      "state[ack=yes threaded=yes init=yes active=yes closed=no hard=yes]\n"
      "  ns[pending=300 active=100 wake=23/45 "
      "runtime=67/89]\n"
      "  debug[state=sem-wait addr=0xabcd waitq=0x111 "
      "channel=0x222:0x333 reason=waiting level=2 queued=yes]\n",
      static_cast<const char*>(line));
}

TEST(IrqDiagnosticRenderer, DistinguishesSnapshotMiss) {
  IrqLineDiagnosticSnapshot snapshot = {};
  snapshot.line = 14;
  IrqDiagnosticString line;
  IrqDiagnosticRenderer::render(snapshot, line);

  EXPECT_STREQ("irq 14 SNAPSHOT MISS\n", static_cast<const char*>(line));
}

TEST(IrqDiagnosticRenderer, RendersMixedDelivery) {
  IrqLineDiagnosticSnapshot snapshot = {};
  snapshot.snapshotGeneration = 1;
  snapshot.line = 10;
  snapshot.handlerCount = 2;
  snapshot.delivery = IrqDelivery::Mixed;
  snapshot.trigger = IrqTrigger::Level;
  snapshot.controllerAck = IrqControllerAck::AfterHardStage;
  snapshot.lineRelease = IrqLineRelease::AfterThreadedCompletion;
  snapshot.configured = true;

  IrqDiagnosticString line;
  IrqDiagnosticRenderer::render(snapshot, line);

  EXPECT_TRUE(
      line.contains("cfg=yes handlers=2 delivery=mixed trigger=level "
                    "ack=after-hard release=after-threaded"));
}

TEST(IrqDiagnosticRenderer, RendersEveryMaskReasonAndUnknownBits) {
  IrqLineDiagnosticSnapshot snapshot = {};
  snapshot.snapshotGeneration = 1;
  snapshot.line = 3;
  snapshot.maskReasons = IrqMaskNoHandler | IrqMaskAdministrativelyDisabled |
                         IrqMaskAwaitingAcknowledgement | IrqMaskAwaitingThreadedCompletion |
                         IrqMaskMitigated | IrqMaskShuttingDown | IrqMaskControllerContention |
                         0x8000;

  IrqDiagnosticString line;
  IrqDiagnosticRenderer::render(snapshot, line);

  EXPECT_TRUE(
      line.contains("mask=no-handler,admin-disabled,awaiting-ack,awaiting-threaded,"
                    "mitigated,shutting-down,controller-contention,unknown:0x8000"));
  EXPECT_TRUE(
      line.contains("cfg=no handlers=0 delivery=none trigger=n/a ack=n/a "
                    "release=n/a"));
}

TEST(IrqDiagnosticRenderer, RetainsTheCompleteLineAtMaximumValues) {
  IrqLineDiagnosticSnapshot snapshot = {};
  snapshot.snapshotGeneration = ~static_cast<size_t>(0);
  snapshot.observationTimestamp = ~static_cast<size_t>(0);
  snapshot.dispatchGeneration = ~static_cast<size_t>(0);
  snapshot.acknowledgedGeneration = ~static_cast<size_t>(0);
  snapshot.activeHardDispatchCount = ~static_cast<size_t>(0);
  snapshot.activeHardDispatchGeneration = ~static_cast<size_t>(0);
  snapshot.activeThreadedDispatchCount = ~static_cast<size_t>(0);
  snapshot.publicationCookie = ~static_cast<size_t>(0);
  snapshot.pendingCookie = ~static_cast<size_t>(0);
  snapshot.activeCookie = ~static_cast<size_t>(0);
  snapshot.completedCookie = ~static_cast<size_t>(0);
  snapshot.completedBatches = ~static_cast<size_t>(0);
  snapshot.pendingSinceTimestamp = ~static_cast<size_t>(0);
  snapshot.activeCallbackStartedTimestamp = ~static_cast<size_t>(0);
  snapshot.lastWakeLatency = ~static_cast<size_t>(0);
  snapshot.maximumWakeLatency = ~static_cast<size_t>(0);
  snapshot.lastCallbackRuntime = ~static_cast<size_t>(0);
  snapshot.maximumCallbackRuntime = ~static_cast<size_t>(0);
  snapshot.publicationFailures = ~static_cast<size_t>(0);
  snapshot.controllerContentions = ~static_cast<size_t>(0);
  snapshot.controllerPromptAttempts = ~static_cast<size_t>(0);
  snapshot.controllerPromptFailures = ~static_cast<size_t>(0);
  snapshot.controllerPromptDestination = ~static_cast<size_t>(0);
  snapshot.diagnosticPublicationFailures = ~static_cast<size_t>(0);
  snapshot.workerIdentity = ~static_cast<uintptr_t>(0);
  snapshot.activeThreadedHandlerIdentity = ~static_cast<uintptr_t>(0);
  snapshot.workerDebugAddress = ~static_cast<uintptr_t>(0);
  snapshot.workerWaitQueue = ~static_cast<uintptr_t>(0);
  snapshot.workerWaitChannelOwner = ~static_cast<uintptr_t>(0);
  snapshot.workerWaitChannelValue = ~static_cast<uintptr_t>(0);
  snapshot.workerWaitStateLevel = ~static_cast<size_t>(0);
  snapshot.handlerCount = ~static_cast<size_t>(0);
  snapshot.maskReasons = 0xffff;
  snapshot.line = 255;
  snapshot.delivery = IrqDelivery::Threaded;
  snapshot.trigger = IrqTrigger::Synthetic;
  snapshot.controllerAck = IrqControllerAck::BeforeHardStage;
  snapshot.lineRelease = IrqLineRelease::AfterThreadedCompletion;
  snapshot.workerDebugState = IrqWorkerDebugState::CallbackDrain;
  snapshot.workerWaitReason = IrqWorkerWaitReason::Spurious;
  snapshot.controllerPromptState = IrqControllerPromptState::Submitted;
  snapshot.configured = true;
  snapshot.effectiveMasked = true;
  snapshot.requestedEnabled = true;
  snapshot.acknowledgementPending = true;
  snapshot.threadedPending = true;
  snapshot.dispatcherInitialised = true;
  snapshot.dispatcherActive = true;
  snapshot.dispatcherClosed = true;
  snapshot.hardStageActive = true;
  snapshot.workerDiagnosticAvailable = true;
  snapshot.workerWaitActive = true;
  snapshot.workerWaitQueued = true;

  IrqDiagnosticString line;
  IrqDiagnosticRenderer::render(snapshot, line);
  NormalStaticString worker;
  worker += "worker=0x";
  worker.append(snapshot.workerIdentity, 16);

  EXPECT_LT(line.length(), 2047U);
  EXPECT_TRUE(line.contains(static_cast<const char*>(worker)));
  EXPECT_TRUE(line.contains("hard[pins="));
  EXPECT_TRUE(line.contains("gen=ambiguous]"));
  EXPECT_TRUE(line.contains("prompt[controller attempts="));
  EXPECT_TRUE(line.contains("state=submitted]"));
  EXPECT_TRUE(line.contains("hard=yes]\n"));
  EXPECT_TRUE(line.contains("state=callback-drain"));
  EXPECT_TRUE(line.contains("reason=spurious"));
  EXPECT_TRUE(line.contains("queued=yes]\n"));
}
