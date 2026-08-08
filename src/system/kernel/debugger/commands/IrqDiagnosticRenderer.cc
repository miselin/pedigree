/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/debugger/commands/IrqDiagnosticRenderer.h"

namespace
{
constexpr uint16_t KnownMaskReasons =
    IrqMaskNoHandler | IrqMaskAdministrativelyDisabled |
    IrqMaskAwaitingAcknowledgement | IrqMaskAwaitingThreadedCompletion |
    IrqMaskMitigated | IrqMaskShuttingDown | IrqMaskControllerContention;

const char *yesNo(bool value)
{
    return value ? "yes" : "no";
}

const char *deliveryName(IrqDelivery delivery)
{
    switch (delivery)
    {
        case IrqDelivery::None:
            return "none";
        case IrqDelivery::Hard:
            return "hard";
        case IrqDelivery::Threaded:
            return "threaded";
        case IrqDelivery::Mixed:
            return "mixed";
    }
    return "unknown";
}

const char *triggerName(IrqTrigger trigger)
{
    switch (trigger)
    {
        case IrqTrigger::Edge:
            return "edge";
        case IrqTrigger::Level:
            return "level";
        case IrqTrigger::Synthetic:
            return "synthetic";
    }
    return "unknown";
}

const char *controllerAckName(IrqControllerAck ack)
{
    switch (ack)
    {
        case IrqControllerAck::None:
            return "none";
        case IrqControllerAck::BeforeHardStage:
            return "before-hard";
        case IrqControllerAck::AfterHardStage:
            return "after-hard";
    }
    return "unknown";
}

const char *lineReleaseName(IrqLineRelease release)
{
    switch (release)
    {
        case IrqLineRelease::AfterHardStage:
            return "after-hard";
        case IrqLineRelease::AfterThreadedCompletion:
            return "after-threaded";
    }
    return "unknown";
}

const char *workerDebugStateName(IrqWorkerDebugState state)
{
    switch (state)
    {
        case IrqWorkerDebugState::Unavailable:
            return "unavailable";
        case IrqWorkerDebugState::None:
            return "none";
        case IrqWorkerDebugState::SemaphoreWait:
            return "sem-wait";
        case IrqWorkerDebugState::ConditionWait:
            return "cond-wait";
        case IrqWorkerDebugState::Joining:
            return "joining";
        case IrqWorkerDebugState::FutexWait:
            return "futex-wait";
        case IrqWorkerDebugState::EventWait:
            return "event-wait";
        case IrqWorkerDebugState::ProcessWait:
            return "process-wait";
        case IrqWorkerDebugState::CallbackDrain:
            return "callback-drain";
    }
    return "unknown";
}

const char *workerWaitReasonName(IrqWorkerWaitReason reason)
{
    switch (reason)
    {
        case IrqWorkerWaitReason::Unavailable:
            return "unavailable";
        case IrqWorkerWaitReason::Waiting:
            return "waiting";
        case IrqWorkerWaitReason::Signalled:
            return "signalled";
        case IrqWorkerWaitReason::Event:
            return "event";
        case IrqWorkerWaitReason::Unwinding:
            return "unwinding";
        case IrqWorkerWaitReason::Terminating:
            return "terminating";
        case IrqWorkerWaitReason::Spurious:
            return "spurious";
    }
    return "unknown";
}

const char *controllerPromptStateName(IrqControllerPromptState state)
{
    switch (state)
    {
        case IrqControllerPromptState::NotRequired:
            return "none";
        case IrqControllerPromptState::Submitted:
            return "submitted";
        case IrqControllerPromptState::Failed:
            return "failed";
    }
    return "unknown";
}

void appendMaskReason(
    IrqDiagnosticString &line, bool &first, uint16_t reasons, uint16_t reason,
    const char *name)
{
    if (!(reasons & reason))
    {
        return;
    }

    if (!first)
    {
        line += ',';
    }
    line += name;
    first = false;
}

void appendMaskReasons(IrqDiagnosticString &line, uint16_t reasons)
{
    if (reasons == IrqMaskNone)
    {
        line += "none";
        return;
    }

    bool first = true;
    appendMaskReason(line, first, reasons, IrqMaskNoHandler, "no-handler");
    appendMaskReason(
        line, first, reasons, IrqMaskAdministrativelyDisabled,
        "admin-disabled");
    appendMaskReason(
        line, first, reasons, IrqMaskAwaitingAcknowledgement, "awaiting-ack");
    appendMaskReason(
        line, first, reasons, IrqMaskAwaitingThreadedCompletion,
        "awaiting-threaded");
    appendMaskReason(line, first, reasons, IrqMaskMitigated, "mitigated");
    appendMaskReason(
        line, first, reasons, IrqMaskShuttingDown, "shutting-down");
    appendMaskReason(
        line, first, reasons, IrqMaskControllerContention,
        "controller-contention");

    const uint16_t unknown = reasons & ~KnownMaskReasons;
    if (unknown)
    {
        if (!first)
        {
            line += ',';
        }
        line += "unknown:0x";
        line.append(unknown, 16);
    }
}

void appendHardDispatchState(
    IrqDiagnosticString &line, const IrqLineDiagnosticSnapshot &snapshot)
{
    line += " hard[pins=";
    line.append(snapshot.activeHardDispatchCount);
    line += " gen=";
    if (!snapshot.activeHardDispatchCount)
    {
        line += "none";
    }
    else if (
        snapshot.activeHardDispatchCount == 1 &&
        snapshot.activeHardDispatchGeneration)
    {
        line.append(snapshot.activeHardDispatchGeneration);
    }
    else
    {
        line += "ambiguous";
    }
    line += ']';
}

void appendAge(
    IrqDiagnosticString &line, bool present, size_t observed, size_t started)
{
    if (!present || !started || observed < started)
    {
        line += "none";
    }
    else
    {
        line.append(observed - started);
    }
}

void appendWorkerState(
    IrqDiagnosticString &line, const IrqLineDiagnosticSnapshot &snapshot)
{
    line += "\n  worker=0x";
    line.append(snapshot.workerIdentity, 16);
    line += " handler=";
    if (!snapshot.activeThreadedDispatchCount)
    {
        line += "none";
    }
    else if (
        snapshot.activeThreadedDispatchCount == 1 &&
        snapshot.activeThreadedHandlerIdentity)
    {
        line += "0x";
        line.append(snapshot.activeThreadedHandlerIdentity, 16);
    }
    else
    {
        line += "ambiguous";
    }
    line += " pins=";
    line.append(snapshot.activeThreadedDispatchCount);
    line += " cookie[pub=";
    line.append(snapshot.publicationCookie);
    line += " pending=";
    line.append(snapshot.pendingCookie);
    line += " active=";
    line.append(snapshot.activeCookie);
    line += " done=";
    line.append(snapshot.completedCookie);
    line += "] batches=";
    line.append(snapshot.completedBatches);
    line += " fail[publish=";
    line.append(snapshot.publicationFailures);
    line += " remove=";
    line.append(snapshot.removalRejections);
    line += " controller=";
    line.append(snapshot.controllerContentions);
    line += " diag=";
    line.append(snapshot.diagnosticPublicationFailures);
    line += "] prompt[controller attempts=";
    line.append(snapshot.controllerPromptAttempts);
    line += " failures=";
    line.append(snapshot.controllerPromptFailures);
    line += " dest=0x";
    line.append(snapshot.controllerPromptDestination, 16);
    line += " state=";
    line += controllerPromptStateName(snapshot.controllerPromptState);
    line += "] state[ack=";
    line += yesNo(snapshot.acknowledgementPending);
    line += " threaded=";
    line += yesNo(snapshot.threadedPending);
    line += " init=";
    line += yesNo(snapshot.dispatcherInitialised);
    line += " active=";
    line += yesNo(snapshot.dispatcherActive);
    line += " closed=";
    line += yesNo(snapshot.dispatcherClosed);
    line += " hard=";
    line += yesNo(snapshot.hardStageActive);
    line += "]\n";
}

void appendTimingState(
    IrqDiagnosticString &line, const IrqLineDiagnosticSnapshot &snapshot)
{
    line += "  ns[pending=";
    appendAge(
        line, snapshot.pendingCookie != 0, snapshot.observationTimestamp,
        snapshot.pendingSinceTimestamp);
    line += " active=";
    appendAge(
        line, snapshot.activeCookie != 0, snapshot.observationTimestamp,
        snapshot.activeCallbackStartedTimestamp);
    line += " wake=";
    line.append(snapshot.lastWakeLatency);
    line += '/';
    line.append(snapshot.maximumWakeLatency);
    line += " runtime=";
    line.append(snapshot.lastCallbackRuntime);
    line += '/';
    line.append(snapshot.maximumCallbackRuntime);
    line += "]\n";
}

void appendWorkerDebugState(
    IrqDiagnosticString &line, const IrqLineDiagnosticSnapshot &snapshot)
{
    line += "  debug[";
    if (!snapshot.workerDiagnosticAvailable)
    {
        line += "unavailable]\n";
        return;
    }

    line += "state=";
    line += workerDebugStateName(snapshot.workerDebugState);
    line += " addr=0x";
    line.append(snapshot.workerDebugAddress, 16);
    if (!snapshot.workerWaitActive)
    {
        line += " wait=none]\n";
        return;
    }

    line += " waitq=0x";
    line.append(snapshot.workerWaitQueue, 16);
    line += " channel=0x";
    line.append(snapshot.workerWaitChannelOwner, 16);
    line += ":0x";
    line.append(snapshot.workerWaitChannelValue, 16);
    line += " reason=";
    line += workerWaitReasonName(snapshot.workerWaitReason);
    line += " level=";
    line.append(snapshot.workerWaitStateLevel);
    line += " queued=";
    line += yesNo(snapshot.workerWaitQueued);
    line += "]\n";
}
}  // namespace

void IrqDiagnosticRenderer::render(
    const IrqLineDiagnosticSnapshot &snapshot, IrqDiagnosticString &line)
{
    line.clear();
    line += "irq ";
    line.append(snapshot.line);

    if (!snapshot.snapshotGeneration)
    {
        line += " SNAPSHOT MISS\n";
        return;
    }
    else
    {
        line += " snap=";
        line.append(snapshot.snapshotGeneration);
        line += " cfg=";
        line += yesNo(snapshot.configured);
        line += " handlers=";
        line.append(snapshot.handlerCount);
        line += " delivery=";
        line += deliveryName(snapshot.delivery);
        line += " trigger=";
        line += snapshot.configured ? triggerName(snapshot.trigger) : "n/a";
        line += " ack=";
        line += snapshot.configured ?
                    controllerAckName(snapshot.controllerAck) :
                    "n/a";
        line += " release=";
        line +=
            snapshot.configured ? lineReleaseName(snapshot.lineRelease) : "n/a";
        line += " req=";
        line += snapshot.requestedEnabled ? "on" : "off";
        line += " masked=";
        line += yesNo(snapshot.effectiveMasked);
        line += " mask=";
        appendMaskReasons(line, snapshot.maskReasons);
    }

    line += " gen=";
    line.append(snapshot.dispatchGeneration);
    line += " acked=";
    line.append(snapshot.acknowledgedGeneration);
    line += " count[irq=";
    line.append(snapshot.interruptCount);
    line += " spur=";
    line.append(snapshot.spuriousCount);
    line += " unhandled=";
    line.append(snapshot.unhandledCount);
    line += ']';
    appendHardDispatchState(line, snapshot);
    appendWorkerState(line, snapshot);
    appendTimingState(line, snapshot);
    appendWorkerDebugState(line, snapshot);
}
