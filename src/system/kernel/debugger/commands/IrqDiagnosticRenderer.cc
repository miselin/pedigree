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
    IrqMaskMitigated | IrqMaskShuttingDown;

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

void appendMaskReason(
    HugeStaticString &line, bool &first, uint16_t reasons, uint16_t reason,
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

void appendMaskReasons(HugeStaticString &line, uint16_t reasons)
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
    HugeStaticString &line, const IrqLineDiagnosticSnapshot &snapshot)
{
    line += " hard-count=";
    line.append(snapshot.activeHardDispatchCount);
    line += " hard-gen=";
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
}

void appendWorkerState(
    HugeStaticString &line, const IrqLineDiagnosticSnapshot &snapshot)
{
    line += "\n  worker=0x";
    line.append(snapshot.workerIdentity, 16);
    line += " cookies[published=";
    line.append(snapshot.publicationCookie);
    line += " pending=";
    line.append(snapshot.pendingCookie);
    line += " active=";
    line.append(snapshot.activeCookie);
    line += " completed=";
    line.append(snapshot.completedCookie);
    line += "] batches=";
    line.append(snapshot.completedBatches);
    line += " failures[worker-publish=";
    line.append(snapshot.publicationFailures);
    line += " diag-publish=";
    line.append(snapshot.diagnosticPublicationFailures);
    line += "] state[ack-pending=";
    line += yesNo(snapshot.acknowledgementPending);
    line += " threaded-pending=";
    line += yesNo(snapshot.threadedPending);
    line += " worker-initialised=";
    line += yesNo(snapshot.dispatcherInitialised);
    line += " worker-active=";
    line += yesNo(snapshot.dispatcherActive);
    line += " worker-closed=";
    line += yesNo(snapshot.dispatcherClosed);
    line += " hard-stage-active=";
    line += yesNo(snapshot.hardStageActive);
    line += "]\n";
}
}  // namespace

void IrqDiagnosticRenderer::render(
    const IrqLineDiagnosticSnapshot &snapshot, HugeStaticString &line)
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
        line += " snapshot=";
        line.append(snapshot.snapshotGeneration);
        line += " configured=";
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
        line += " requested=";
        line += snapshot.requestedEnabled ? "enabled" : "disabled";
        line += " masked=";
        line += yesNo(snapshot.effectiveMasked);
        line += " mask=";
        appendMaskReasons(line, snapshot.maskReasons);
    }

    line += " dispatch-gen=";
    line.append(snapshot.dispatchGeneration);
    line += " ack-gen=";
    line.append(snapshot.acknowledgedGeneration);
    line += " counts[total=";
    line.append(snapshot.interruptCount);
    line += " spurious=";
    line.append(snapshot.spuriousCount);
    line += " unhandled=";
    line.append(snapshot.unhandledCount);
    line += ']';
    appendHardDispatchState(line, snapshot);
    appendWorkerState(line, snapshot);
}
