/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "modules/drivers/common/usb-hcd/PortChangeRequest.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"

namespace
{
bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL usb-hcd-port-change-publication: " << detail);
    return false;
}

class HostedUsbPortQueue final : public RequestQueue
{
  public:
    enum Controller
    {
        Ehci,
        Ohci,
        Uhci,
    };

    HostedUsbPortQueue()
        : RequestQueue(MakeConstantString("Hosted USB port changes")),
          workerEntered(0), holdEntered(0), releaseHold(0), executions(0),
          suppressed(0), failures(0), cancellations(0)
    {
        seen[Ehci] = 0;
        seen[Ohci] = 0;
        seen[Uhci] = 0;
    }

    ~HostedUsbPortQueue() override
    {
        destroy();
    }

    void setMaximum(size_t maximum)
    {
        m_nMaxAsyncRequests = maximum;
    }

    Thread *workerThread() const
    {
        return m_pThread;
    }

    Semaphore workerEntered;
    Semaphore holdEntered;
    Semaphore releaseHold;
    Atomic<size_t> executions;
    Atomic<size_t> suppressed;
    Atomic<size_t> failures;
    Atomic<size_t> cancellations;
    Atomic<uint64_t> seen[3];

  protected:
    uint64_t executeRequest(
        uint64_t controller, uint64_t port, uint64_t publication,
        uint64_t signalWorker, uint64_t holdWorker, uint64_t returnEarly,
        uint64_t,
        uint64_t generation) override
    {
        auto *request =
            reinterpret_cast<UsbHcd::PortChangeRequest *>(publication);
        if (signalWorker)
        {
            workerEntered.release();
        }

        PublicationCompletion completion(*request, generation);
        if (!completion)
        {
            suppressed += 1;
            return 0;
        }
        if (holdWorker && generation == 1)
        {
            holdEntered.release();
            const bool released = releaseHold.acquireForCompletion();
            if (!released)
            {
                failures += 1;
            }
        }
        if (returnEarly)
        {
            return 0;
        }
        if (controller > Uhci || port >= 64)
        {
            failures += 1;
        }
        else
        {
            const uint64_t bit = 1ULL << port;
            seen[controller] |= bit;
        }
        executions += 1;
        return 0;
    }

    void cancelRequest(const Request &request) override
    {
        auto *publication = reinterpret_cast<UsbHcd::PortChangeRequest *>(
            request.p3);
        publication->cancel(request.p8);
        cancellations += 1;
    }

  private:
    using PublicationCompletion = UsbHcd::PortChangeRequest::Completion;
};

struct AcknowledgeWaitContext
{
    AcknowledgeWaitContext(
        UsbHcd::PortChangeRequest *request, Thread *worker, size_t generation)
        : request(request), worker(worker), generation(generation), hookCalls(0),
          hookFailures(0), wakeBeforeBlock(0)
    {
    }

    UsbHcd::PortChangeRequest *request;
    Thread *worker;
    size_t generation;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookFailures;
    Atomic<size_t> wakeBeforeBlock;
};

AcknowledgeWaitContext *g_AcknowledgeWaitContext = nullptr;

void acknowledgeBeforeBlockHook(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    AcknowledgeWaitContext *context = g_AcknowledgeWaitContext;
    if (
        !context || thread != context->worker ||
        channel.owner != context->request)
    {
        return;
    }

    context->hookCalls += 1;
    Thread::WaitDebugInfo wait = {};
    uintptr_t debugAddress = 0;
    const bool validPublication =
        queue && !channel.value && debugState == Thread::CallbackDrain &&
        thread->getWaitDebugInfo(wait) && wait.queue == queue &&
        wait.channelOwner == channel.owner &&
        wait.channelValue == channel.value && wait.queued &&
        wait.reason == WaitQueue::WakeReason::Waiting &&
        thread->getDebugState(debugAddress) == Thread::CallbackDrain &&
        debugAddress == reinterpret_cast<uintptr_t>(context->request);

    // Always release the worker so a failed assertion cannot strand the
    // RequestQueue thread inside the test.
    context->request->acknowledge(context->generation);

    Thread::WaitDebugInfo signalledWait = {};
    if (
        validPublication && thread->getWaitDebugInfo(signalledWait) &&
        signalledWait.queue == queue &&
        signalledWait.channelOwner == channel.owner &&
        signalledWait.channelValue == channel.value && signalledWait.queued &&
        signalledWait.reason == WaitQueue::WakeReason::Signalled)
    {
        context->wakeBeforeBlock += 1;
    }
    else
    {
        context->hookFailures += 1;
    }
}

struct DestroyContext
{
    explicit DestroyContext(HostedUsbPortQueue *queue)
        : queue(queue), finished(0)
    {
    }

    HostedUsbPortQueue *queue;
    Atomic<size_t> finished;
};

int destroyQueue(void *parameter)
{
    auto *context = reinterpret_cast<DestroyContext *>(parameter);
    context->queue->destroy();
    context->finished += 1;
    return 0;
}

bool allIdle(UsbHcd::PortChangeRequest *requests, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (!requests[i].isIdle())
        {
            return false;
        }
    }
    return true;
}
}  // namespace

bool runHostedUsbHcdPortChangeRegressions()
{
    using Publication = UsbHcd::PortChangeRequest;
    using Result = Publication::Result;

    bool passed = true;
    passed &= check(
        UsbHcd::EhciRootPortCount == 15 &&
            UsbHcd::OhciRootPortCount == 15 &&
            UsbHcd::UhciRootPortCount == 8 &&
            UsbHcd::validEhciRootPortCount(15) &&
            !UsbHcd::validEhciRootPortCount(16) &&
            !UsbHcd::validOhciRootPortCount(0) &&
            UsbHcd::validOhciRootPortCount(15) &&
            !UsbHcd::validOhciRootPortCount(16) &&
            UsbHcd::validUhciRootPortCount(8) &&
            !UsbHcd::validUhciRootPortCount(9),
        "root-port token bounds do not match controller limits");

    constexpr uint16_t Status = 0x123f;
    constexpr uint16_t ChangeMask = 0x2a;
    constexpr uint16_t Acknowledge = 0x02;
    const uint16_t w1c =
        UsbHcd::selectiveW1cValue(Status, ChangeMask, Acknowledge);
    passed &= check(
        (w1c & ChangeMask) == Acknowledge &&
            (w1c & ~ChangeMask) == (Status & ~ChangeMask),
        "selective W1C echoed an unrelated change bit");
    passed &= check(
        Publication::canAcknowledge(Result::Accepted) &&
            Publication::canAcknowledge(Result::Coalesced) &&
            !Publication::canAcknowledge(Result::TokenBusy) &&
            !Publication::canAcknowledge(Result::QueueFull) &&
            !Publication::canAcknowledge(Result::QueueStopped) &&
            !Publication::canAcknowledge(Result::InvalidPriority),
        "a rejected publication was treated as safe to ACK");

    UsbHcd::DeferredPortChanges deferred;
    deferred.defer(0, 1);
    deferred.defer(2, 4);
    deferred.defer(0, 3);
    passed &= check(
        !deferred.empty() && deferred.release(0) == 3 &&
            deferred.release(2) == 4 && deferred.empty(),
        "mixed port publication passes lost a deferred generation");

    HostedUsbPortQueue queue;
    Publication recovered;
    passed &= check(
        recovered.configure(
            queue, 0, HostedUsbPortQueue::Ehci, 0,
            reinterpret_cast<uintptr_t>(&recovered)),
        "port token configuration was rejected");
    const Publication::Observation stopped = recovered.observe();
    passed &= check(
        stopped.result == Result::QueueStopped && stopped.generation == 1 &&
            !recovered.isIdle(),
        "stopped queue discarded the pending hardware observation");

    queue.initialise();
    queue.setMaximum(0);
    const Publication::Observation reservedAdmission = recovered.observe();
    passed &= check(
        reservedAdmission.result == Result::Accepted &&
            reservedAdmission.generation == 2,
        "preallocated port token depended on allocation admission");
    if (Publication::canAcknowledge(reservedAdmission.result))
    {
        recovered.acknowledge(reservedAdmission.generation);
    }
    passed &= check(
        queue.drain() && recovered.isIdle() &&
            !recovered.hasPublicationFailure(),
        "over-capacity port token did not drain");
    queue.setMaximum(256);
    queue.executions = 0;
    queue.seen[HostedUsbPortQueue::Ehci] = 0;

    Publication invalid;
    passed &= check(
        !invalid.configure(queue, REQUEST_QUEUE_NUM_PRIORITIES),
        "invalid priority configured a port token");

    Publication ordered;
    passed &= check(
        ordered.configure(
            queue, 0, HostedUsbPortQueue::Ehci, 0,
            reinterpret_cast<uintptr_t>(&ordered), 1, 1),
        "ordered port token configuration was rejected");

    AcknowledgeWaitContext acknowledgeWait(
        &ordered, queue.workerThread(), 1);
    g_AcknowledgeWaitContext = &acknowledgeWait;
    WaitQueue::setBeforeBlockHook(acknowledgeBeforeBlockHook);
    const Publication::Observation first = ordered.observe();
    const bool workerEntered =
        first.result == Result::Accepted && queue.workerEntered.acquire();
    constexpr size_t AcknowledgeWaitAttempts = 10000;
    for (size_t i = 0;
         i < AcknowledgeWaitAttempts && !acknowledgeWait.wakeBeforeBlock &&
         !acknowledgeWait.hookFailures;
         ++i)
    {
        Scheduler::instance().yield();
    }
    if (!acknowledgeWait.wakeBeforeBlock)
    {
        ordered.acknowledge(first.generation);
    }
    WaitQueue::setBeforeBlockHook(nullptr);
    g_AcknowledgeWaitContext = nullptr;

    passed &= check(
        workerEntered,
        "worker did not reach the pre-ACK publication barrier");
    const bool waitQueueAcknowledgementPassed =
        first.generation == 1 && acknowledgeWait.hookCalls == 1 &&
        acknowledgeWait.hookFailures == 0 &&
        acknowledgeWait.wakeBeforeBlock == 1;
    passed &= check(
        waitQueueAcknowledgementPassed,
        "hardware ACK did not signal the published pre-block waiter");
    if (waitQueueAcknowledgementPassed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS usb-hcd-port-change-waitqueue-ack");
    }
    passed &= check(
        queue.holdEntered.acquire(),
        "acknowledged worker did not reach the execution hold");
    const Publication::Observation second = ordered.observe();
    const Publication::Observation third = ordered.observe();
    passed &= check(
        second.result == Result::Coalesced &&
            third.result == Result::Coalesced && queue.executions == 0,
        "active port observations did not coalesce behind the worker");
    queue.releaseHold.release();
    passed &= check(
        queue.workerEntered.acquire() && queue.executions == 1,
        "follow-up worker crossed a deferred controller ACK");
    ordered.acknowledge(third.generation);
    passed &= check(
        queue.drain() && ordered.isIdle() && queue.executions == 2 &&
            !ordered.hasPublicationFailure(),
        "latest coalesced generation was not consumed exactly once");
    queue.seen[HostedUsbPortQueue::Ehci] = 0;
    queue.executions = 0;

    Publication earlyReturn;
    passed &= check(
        earlyReturn.configure(
            queue, 0, HostedUsbPortQueue::Uhci, 0,
            reinterpret_cast<uintptr_t>(&earlyReturn), 0, 0, 1),
        "early-return port token configuration was rejected");
    const Publication::Observation early = earlyReturn.observe();
    passed &= check(
        early.result == Result::Accepted,
        "early-return publication was rejected");
    earlyReturn.acknowledge(early.generation);
    passed &= check(
        queue.drain() && earlyReturn.isIdle(),
        "early worker return retained its port token");

    Publication ehci[UsbHcd::EhciRootPortCount];
    Publication ohci[UsbHcd::OhciRootPortCount];
    Publication uhci[UsbHcd::UhciRootPortCount];

    for (size_t i = 0; i < UsbHcd::EhciRootPortCount; ++i)
    {
        passed &= check(
            ehci[i].configure(
                queue, 0, HostedUsbPortQueue::Ehci, i,
                reinterpret_cast<uintptr_t>(&ehci[i])),
            "EHCI token configuration was rejected");
        const Publication::Observation observation = ehci[i].observe();
        passed &= check(
            observation.result == Result::Accepted,
            "EHCI token was rejected");
        if (Publication::canAcknowledge(observation.result))
        {
            ehci[i].acknowledge(observation.generation);
        }
    }
    for (size_t i = 0; i < UsbHcd::OhciRootPortCount; ++i)
    {
        passed &= check(
            ohci[i].configure(
                queue, 0, HostedUsbPortQueue::Ohci, i,
                reinterpret_cast<uintptr_t>(&ohci[i])),
            "OHCI token configuration was rejected");
        const Publication::Observation observation = ohci[i].observe();
        passed &= check(
            observation.result == Result::Accepted,
            "OHCI token was rejected");
        if (Publication::canAcknowledge(observation.result))
        {
            ohci[i].acknowledge(observation.generation);
        }
    }
    for (size_t i = 0; i < UsbHcd::UhciRootPortCount; ++i)
    {
        passed &= check(
            uhci[i].configure(
                queue, 0, HostedUsbPortQueue::Uhci, i,
                reinterpret_cast<uintptr_t>(&uhci[i])),
            "UHCI token configuration was rejected");
        const Publication::Observation observation = uhci[i].observe();
        passed &= check(
            observation.result == Result::Accepted,
            "UHCI token was rejected");
        if (Publication::canAcknowledge(observation.result))
        {
            uhci[i].acknowledge(observation.generation);
        }
    }

    const uint64_t ehciMask = (1ULL << UsbHcd::EhciRootPortCount) - 1;
    const uint64_t ohciMask = (1ULL << UsbHcd::OhciRootPortCount) - 1;
    const uint64_t uhciMask = (1ULL << UsbHcd::UhciRootPortCount) - 1;
    passed &= check(
        queue.drain() && queue.seen[HostedUsbPortQueue::Ehci] == ehciMask &&
            queue.seen[HostedUsbPortQueue::Ohci] == ohciMask &&
            queue.seen[HostedUsbPortQueue::Uhci] == uhciMask &&
            queue.failures == 0 &&
            queue.executions ==
                UsbHcd::EhciRootPortCount + UsbHcd::OhciRootPortCount +
                    UsbHcd::UhciRootPortCount &&
            allIdle(ehci, UsbHcd::EhciRootPortCount) &&
            allIdle(ohci, UsbHcd::OhciRootPortCount) &&
            allIdle(uhci, UsbHcd::UhciRootPortCount),
        "controller port tokens did not map and complete exactly once");
    queue.destroy();

    HostedUsbPortQueue stopQueue;
    stopQueue.initialise();
    Publication unacknowledged;
    passed &= check(
        unacknowledged.configure(
            stopQueue, 0, HostedUsbPortQueue::Ehci, 0,
            reinterpret_cast<uintptr_t>(&unacknowledged), 1),
        "stop-wait port token configuration was rejected");
    const Publication::Observation waiting = unacknowledged.observe();
    const bool stopWorkerEntered =
        waiting.result == Result::Accepted &&
        stopQueue.workerEntered.acquire();
    bool stopWaitPublished = false;
    for (size_t i = 0; i < 10000 && !stopWaitPublished; ++i)
    {
        Thread::WaitDebugInfo wait = {};
        uintptr_t debugAddress = 0;
        stopWaitPublished =
            stopQueue.workerThread()->getWaitDebugInfo(wait) && wait.queue &&
            wait.channelOwner == &unacknowledged && !wait.channelValue &&
            wait.queued && wait.reason == WaitQueue::WakeReason::Waiting &&
            stopQueue.workerThread()->getDebugState(debugAddress) ==
                Thread::CallbackDrain &&
            debugAddress == reinterpret_cast<uintptr_t>(&unacknowledged);
        if (!stopWaitPublished)
        {
            Scheduler::instance().yield();
        }
    }
    passed &= check(
        stopWorkerEntered && stopWaitPublished,
        "worker did not enter its unacknowledged generation wait");
    const Publication::Observation stopFollowUp = unacknowledged.observe();
    const bool stopFollowUpRetained =
        stopFollowUp.result == Result::Coalesced &&
        stopFollowUp.generation == 2;
    passed &= check(
        stopFollowUpRetained,
        "stop-wait port token did not retain its pending follow-up");
    unacknowledged.stopAfterQuiesce();
    const bool stopWakePassed =
        stopQueue.drain() && unacknowledged.isIdle() &&
        stopQueue.suppressed == 1 && !stopQueue.workerEntered.tryAcquire() &&
        !unacknowledged.hasPublicationFailure();
    passed &= check(
        stopWakePassed,
        "stop did not release an unacknowledged active worker");
    if (
        stopWorkerEntered && stopWaitPublished && stopFollowUpRetained &&
        stopWakePassed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS usb-hcd-port-change-waitqueue-stop");
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "usb-hcd-port-change-stop-suppresses-republish");
    }
    stopQueue.destroy();

    HostedUsbPortQueue cancellationQueue;
    cancellationQueue.initialise();
    Publication blocker;
    Publication cancelled;
    passed &= check(
        blocker.configure(
            cancellationQueue, 0, HostedUsbPortQueue::Ehci, 0,
            reinterpret_cast<uintptr_t>(&blocker), 0, 1) &&
            cancelled.configure(
                cancellationQueue, 0, HostedUsbPortQueue::Ohci, 0,
                reinterpret_cast<uintptr_t>(&cancelled)),
        "cancellation port token configuration was rejected");
    const Publication::Observation blocking = blocker.observe();
    passed &= check(
        blocking.result == Result::Accepted,
        "cancellation blocker was rejected");
    blocker.acknowledge(blocking.generation);
    passed &= check(
        cancellationQueue.holdEntered.acquire(),
        "cancellation blocker did not enter the worker");
    const Publication::Observation cancellation = cancelled.observe();
    passed &= check(
        cancellation.result == Result::Accepted,
        "cancellation target was rejected");
    cancelled.acknowledge(cancellation.generation);
    const Publication::Observation cancellationFollowUp = cancelled.observe();
    passed &= check(
        cancellationFollowUp.result == Result::Coalesced,
        "cancellation target did not retain a coalesced follow-up");

    blocker.stopAfterQuiesce();
    cancelled.stopAfterQuiesce();
    DestroyContext destroyContext(&cancellationQueue);
    Thread *destroyer = new Thread(
        Scheduler::instance().getKernelProcess(), destroyQueue,
        &destroyContext, nullptr, false, true);
    destroyer->setName("hosted USB port-change destroy regression");
    while (
        cancellationQueue.getLifecycleState() !=
        RequestQueue::LifecycleState::Stopping)
    {
        Scheduler::instance().yield();
    }
    cancellationQueue.releaseHold.release();
    passed &= check(
        destroyer->join() && destroyContext.finished == 1 &&
            blocker.isIdle() && cancelled.isIdle() &&
            cancellationQueue.cancellations == 1 &&
            !cancelled.hasPublicationFailure(),
        "destroy did not cancel and release a queued port token");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS usb-hcd-port-change-publication");
    }
    return passed;
}
