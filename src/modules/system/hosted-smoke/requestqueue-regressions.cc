/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/RoundRobin.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/RequestQueue.h"

namespace
{
constexpr size_t RequestQueueSignalNumber = 11;
constexpr size_t WaitAttempts = 10000;

Atomic<size_t> g_RequestQueueSignalCalls(0);

void hostedRequestQueueSignalHandler(size_t)
{
    g_RequestQueueSignalCalls += 1;
}

bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL requestqueue-lifecycle: " << detail);
    return false;
}

bool waitUntilQueued(Thread *thread, size_t debugState)
{
    for (size_t attempt = 0; attempt < WaitAttempts; ++attempt)
    {
        Thread::WaitDebugInfo info = {};
        uintptr_t debugAddress = 0;
        if (thread->getWaitDebugInfo(info) && info.queue && info.queued &&
            thread->getDebugState(debugAddress) == debugState)
        {
            return true;
        }
        Scheduler::instance().yield();
    }
    return false;
}

class HostedRequestQueue : public RequestQueue
{
  public:
    enum Operation
    {
        Sum = 1,
        SelfSubmit,
        SelfSubmitInner,
        HoldWorker,
        CancelQueued,
        InterruptHold,
        Record,
        RecordHold,
    };

    HostedRequestQueue()
        : RequestQueue(MakeConstantString("Hosted wait regression")),
          executions(0), cancellations(0), queuedCancellations(0),
          comparisons(0), recordedCount(0), recordFailures(0), holdStarted(0),
          releaseHold(0), matchEqualPayload(false)
    {
    }

    ~HostedRequestQueue() override
    {
        destroy();
    }

    Atomic<size_t> executions;
    Atomic<size_t> cancellations;
    Atomic<size_t> queuedCancellations;
    Atomic<size_t> comparisons;
    Atomic<size_t> recordedCount;
    Atomic<size_t> recordFailures;
    uint64_t recorded[16] = {};
    Semaphore holdStarted;
    Semaphore releaseHold;

    size_t requestWaiterCount()
    {
        return m_RequestQueueWaiters.waiterCount();
    }

    Thread *workerThread()
    {
        auto guard = m_RequestQueueWaiters.acquire();
        return m_pThread;
    }

    void setMaxAsyncRequests(size_t maximum)
    {
        m_nMaxAsyncRequests = maximum;
    }

    void setMatchEqualPayload(bool match)
    {
        matchEqualPayload = match;
    }

  protected:
    uint64_t executeRequest(
        uint64_t p1, uint64_t p2, uint64_t p3, uint64_t, uint64_t, uint64_t,
        uint64_t, uint64_t) override
    {
        executions += 1;

        switch (p1)
        {
            case Sum:
            case SelfSubmitInner:
                return p2 + p3;
            case SelfSubmit:
                return addRequest(0, SelfSubmitInner, p2, p3);
            case HoldWorker:
            case InterruptHold:
                holdStarted.release();
                return releaseHold.acquire() ? p2 : 0;
            case Record:
            case RecordHold: {
                const size_t index = (recordedCount += 1) - 1;
                if (index < (sizeof(recorded) / sizeof(recorded[0])))
                {
                    recorded[index] = p2;
                }
                else
                {
                    recordFailures += 1;
                }
                if (p1 == RecordHold)
                {
                    holdStarted.release();
                    if (!releaseHold.acquireForCompletion())
                    {
                        return 0;
                    }
                }
                return p2;
            }
            default:
                return 0;
        }
    }

    void cancelRequest(const Request &request) override
    {
        cancellations += 1;
        if (request.p1 == CancelQueued)
        {
            queuedCancellations += 1;
        }
    }

    bool compareRequests(const Request &a, const Request &b) override
    {
        comparisons += 1;
        return matchEqualPayload && a.p1 == b.p1 && a.p2 == b.p2 &&
               a.p3 == b.p3 && a.p4 == b.p4 && a.p5 == b.p5 && a.p6 == b.p6 &&
               a.p7 == b.p7 && a.p8 == b.p8;
    }

  private:
    bool matchEqualPayload;
};

bool watchdogProgressRegression()
{
    HostedRequestQueue queue;
    queue.initialise();

    bool passed =
        queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
        queue.holdStarted.acquire();
    passed &= queue.addAsyncRequest(0, HostedRequestQueue::Sum, 20, 22) == 1;
    const RequestQueue::OverrunStatus transient = queue.sampleOverrunForTest();

    queue.releaseHold.release();
    const bool firstDrained = queue.drain();
    const RequestQueue::OverrunStatus cleared = queue.sampleOverrunForTest();

    passed &=
        queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
        queue.holdStarted.acquire();
    passed &= queue.addAsyncRequest(0, HostedRequestQueue::Sum, 19, 23) == 1;
    const RequestQueue::OverrunStatus baseline = queue.sampleOverrunForTest();
    const RequestQueue::OverrunStatus stalled = queue.sampleOverrunForTest();

    queue.releaseHold.release();
    const bool secondDrained = queue.drain();
    queue.destroy();

    passed &= transient == RequestQueue::OverrunStatus::Armed && firstDrained &&
              cleared == RequestQueue::OverrunStatus::Clear &&
              baseline == RequestQueue::OverrunStatus::Armed &&
              stalled == RequestQueue::OverrunStatus::Stalled && secondDrained;
    if (!check(
            passed, "the watchdog confused transient admission with a full "
                    "no-progress interval"))
    {
        return false;
    }

    NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-watchdog-progress");
    return true;
}

struct HeldRequestContext
{
    explicit HeldRequestContext(HostedRequestQueue *queue)
        : queue(queue), finished(0), result(~0ULL)
    {
    }

    HostedRequestQueue *queue;
    Atomic<size_t> finished;
    uint64_t result;
};

struct CompletionRequeueContext
{
    explicit CompletionRequeueContext(Thread *target)
        : target(target), requeues(0), hookFailures(0)
    {
    }

    Thread *target;
    Atomic<size_t> requeues;
    Atomic<size_t> hookFailures;
};

CompletionRequeueContext *g_CompletionRequeueContext = nullptr;

void completionRequeueHook(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    CompletionRequeueContext *context = g_CompletionRequeueContext;
    if (!context || thread != context->target)
    {
        return;
    }

    if (!queue || channel.owner || channel.value ||
        debugState != Thread::CondWait)
    {
        context->hookFailures += 1;
    }
    context->requeues += 1;
}

int submitHeldRequest(void *parameter)
{
    HeldRequestContext *context =
        reinterpret_cast<HeldRequestContext *>(parameter);
    context->result =
        context->queue->addRequest(0, HostedRequestQueue::HoldWorker, 42);
    context->finished += 1;
    return 0;
}

bool completionBarrierInterruption(HostedRequestQueue &queue, bool terminal)
{
    HeldRequestContext request(&queue);
    Thread *caller = new Thread(
        Scheduler::instance().getKernelProcess(), submitHeldRequest, &request,
        nullptr, false, true);
    if (terminal)
    {
        caller->setName("hosted RequestQueue terminal caller");
    }
    else
    {
        caller->setName("hosted RequestQueue signal caller");
    }

    const bool workerHeld = queue.holdStarted.acquire();
    const bool initiallyQueued = waitUntilQueued(caller, Thread::CondWait);

    CompletionRequeueContext requeue(caller);
    g_CompletionRequeueContext = &requeue;
    WaitQueue::setBeforeBlockHook(completionRequeueHook);

    bool injected = true;
    if (terminal)
    {
        caller->setUnwindState(Thread::TerminateThread);
    }
    else
    {
        g_RequestQueueSignalCalls = 0;
        SignalEvent *event = new SignalEvent(
            reinterpret_cast<uintptr_t>(&hostedRequestQueueSignalHandler),
            RequestQueueSignalNumber, ~0UL, 0, true, true);
        injected = caller->sendEvent(event);
        if (!injected)
        {
            delete event;
        }
    }

    for (size_t attempt = 0;
         attempt < WaitAttempts && !requeue.requeues && !request.finished;
         ++attempt)
    {
        Scheduler::instance().yield();
    }

    WaitQueue::setBeforeBlockHook(nullptr);
    g_CompletionRequeueContext = nullptr;

    const bool deferred = requeue.requeues == 1 && requeue.hookFailures == 0 &&
                          request.finished == 0;
    const bool signalDelivered = terminal || g_RequestQueueSignalCalls == 1;

    queue.releaseHold.release();
    const bool joined = caller->join();

    return check(
        workerHeld && initiallyQueued && injected && deferred &&
            signalDelivered && joined && request.finished == 1 &&
            request.result == 42,
        terminal ?
            "terminal teardown escaped a synchronous request completion" :
            "a signal returned a synchronous request before completion");
}

struct RequestQueueDestroyContext
{
    explicit RequestQueueDestroyContext(HostedRequestQueue *queue)
        : queue(queue), finished(0)
    {
    }

    HostedRequestQueue *queue;
    Atomic<size_t> finished;
};

struct RequestQueueDrainContext
{
    explicit RequestQueueDrainContext(HostedRequestQueue *queue)
        : queue(queue), finished(0), result(false)
    {
    }

    HostedRequestQueue *queue;
    Atomic<size_t> finished;
    bool result;
};

struct QueuedRequestContext
{
    explicit QueuedRequestContext(HostedRequestQueue *queue)
        : queue(queue), waiter(nullptr), published(0), hookFailures(0),
          finished(0), result(~0ULL)
    {
    }

    HostedRequestQueue *queue;
    Thread *waiter;
    Atomic<size_t> published;
    Atomic<size_t> hookFailures;
    Atomic<size_t> finished;
    uint64_t result;
};

QueuedRequestContext *g_QueuedRequestContext = nullptr;

void queuedRequestWaitHook(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    QueuedRequestContext *context = g_QueuedRequestContext;
    if (!context)
    {
        return;
    }

    if (thread != context->waiter)
    {
        return;
    }

    if (!queue || channel.owner || channel.value ||
        debugState != Thread::CondWait)
    {
        context->hookFailures += 1;
    }
    context->published += 1;
}

int submitQueuedRequest(void *parameter)
{
    QueuedRequestContext *context =
        reinterpret_cast<QueuedRequestContext *>(parameter);
    context->waiter = Processor::information().getCurrentThread();
    context->result =
        context->queue->addRequest(0, HostedRequestQueue::CancelQueued);
    context->finished += 1;
    return 0;
}

int destroyRequestQueue(void *parameter)
{
    RequestQueueDestroyContext *context =
        reinterpret_cast<RequestQueueDestroyContext *>(parameter);
    context->queue->destroy();
    context->finished += 1;
    return 0;
}

int drainRequestQueue(void *parameter)
{
    RequestQueueDrainContext *context =
        reinterpret_cast<RequestQueueDrainContext *>(parameter);
    context->result = context->queue->drain();
    context->finished += 1;
    return 0;
}

struct RequestQueueHaltContext
{
    explicit RequestQueueHaltContext(HostedRequestQueue *queue)
        : queue(queue), finished(0)
    {
    }

    HostedRequestQueue *queue;
    Atomic<size_t> finished;
};

int haltRequestQueue(void *parameter)
{
    auto *context = reinterpret_cast<RequestQueueHaltContext *>(parameter);
    context->queue->halt();
    context->finished += 1;
    return 0;
}

struct PublicationPauseContext
{
    PublicationPauseContext() : calls(0), failures(0), entered(0), release(0)
    {
    }

    Atomic<size_t> calls;
    Atomic<size_t> failures;
    Semaphore entered;
    Semaphore release;
};

void pauseFirstPublication(void *parameter)
{
    auto *context = reinterpret_cast<PublicationPauseContext *>(parameter);
    if ((context->calls += 1) != 1)
    {
        return;
    }

    context->entered.release();
    if (!context->release.acquireForCompletion())
    {
        context->failures += 1;
    }
}

struct InterruptPublicationContext
{
    InterruptPublicationContext(
        HostedRequestQueue *queue, RequestQueue::InterruptRequest *request,
        size_t priority, uint64_t operation, uint64_t value)
        : queue(queue), request(request), priority(priority),
          operation(operation), value(value),
          result(RequestQueue::InterruptEnqueueResult::QueueStopped),
          finished(0)
    {
    }

    HostedRequestQueue *queue;
    RequestQueue::InterruptRequest *request;
    size_t priority;
    uint64_t operation;
    uint64_t value;
    RequestQueue::InterruptEnqueueResult result;
    Atomic<size_t> finished;
};

int publishInterruptRequest(void *parameter)
{
    auto *context = reinterpret_cast<InterruptPublicationContext *>(parameter);
    context->result = context->queue->enqueueFromInterrupt(
        *context->request, context->priority, context->operation,
        context->value);
    context->finished += 1;
    return 0;
}

struct AsyncPublicationContext
{
    AsyncPublicationContext(
        HostedRequestQueue *queue, size_t priority, uint64_t operation,
        uint64_t value)
        : queue(queue), priority(priority), operation(operation), value(value),
          result(0), finished(0)
    {
    }

    HostedRequestQueue *queue;
    size_t priority;
    uint64_t operation;
    uint64_t value;
    uint64_t result;
    Atomic<size_t> finished;
};

int publishAsyncRequest(void *parameter)
{
    auto *context = reinterpret_cast<AsyncPublicationContext *>(parameter);
    context->result = context->queue->addAsyncRequest(
        context->priority, context->operation, context->value);
    context->finished += 1;
    return 0;
}

struct InterruptReleaseContext
{
    InterruptReleaseContext(
        HostedRequestQueue *queue, bool requeueOnce, bool holdFirst = false)
        : queue(queue), request(nullptr), requeueOnce(requeueOnce),
          holdFirst(holdFirst), callbacks(0), requeues(0), failures(0),
          callbackEntered(0), releaseCallback(0)
    {
    }

    HostedRequestQueue *queue;
    RequestQueue::InterruptRequest *request;
    bool requeueOnce;
    bool holdFirst;
    Atomic<size_t> callbacks;
    Atomic<size_t> requeues;
    Atomic<size_t> failures;
    Semaphore callbackEntered;
    Semaphore releaseCallback;
};

void interruptRequestReleased(void *parameter)
{
    auto *context = reinterpret_cast<InterruptReleaseContext *>(parameter);
    const size_t callback = (context->callbacks += 1);
    if (!context->request || context->request->isAvailable())
    {
        context->failures += 1;
    }

    if (context->holdFirst && callback == 1)
    {
        context->callbackEntered.release();
        if (!context->releaseCallback.acquireForCompletion())
        {
            context->failures += 1;
            return;
        }
    }

    if (context->request && context->requeueOnce &&
        context->requeues.compareAndSwap(0, 1))
    {
        if (context->queue->republishWhileReleasing(
                *context->request, 0, HostedRequestQueue::Sum, 20, 22) !=
            RequestQueue::InterruptEnqueueResult::Accepted)
        {
            context->failures += 1;
        }
    }
}

bool predicateDoorbellRegression()
{
    using Result = RequestQueue::InterruptEnqueueResult;

    HostedRequestQueue queue;
    RequestQueue::InterruptRequest request;
    queue.initialise();

    // Consume initialise()'s readiness notification so the publication below
    // is the only possible source of the observed doorbell.
    PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();
    const size_t waitersBefore = queue.requestWaiterCount();
    const bool interrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);
    const Result accepted =
        queue.enqueueFromInterrupt(request, 0, HostedRequestQueue::Sum, 20, 22);
    const Result busy =
        queue.enqueueFromInterrupt(request, 0, HostedRequestQueue::Sum, 19, 23);
    const bool doorbellPending =
        PerProcessorScheduler::currentIrqWorkDoorbellPendingForTest();
    const bool deferred = queue.executions == 0;
    const size_t waitersAfterPublication = queue.requestWaiterCount();
    Processor::setInterrupts(interrupts);

    PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();
    const bool drained = queue.drain();
    const size_t executions = queue.executions;
    const bool available = request.isAvailable();
    queue.destroy();

    const bool passed = check(
        waitersBefore == 0 && waitersAfterPublication == 0 &&
            accepted == Result::Accepted && busy == Result::TokenBusy &&
            doorbellPending && deferred && drained && executions == 1 &&
            available,
        "IF=0 publication touched a WaitQueue or escaped the predicate "
        "doorbell");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-predicate-doorbell");
    }
    return passed;
}

bool interruptDuplicateDomainRegression()
{
    using Result = RequestQueue::InterruptEnqueueResult;

    HostedRequestQueue queue;
    RequestQueue::InterruptRequest interrupt;
    queue.setMatchEqualPayload(true);
    queue.initialise();

    bool passed =
        queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
        queue.holdStarted.acquire();

    const bool interrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);
    const Result interruptAccepted = queue.enqueueFromInterrupt(
        interrupt, 1, HostedRequestQueue::Record, 77, 88);
    Processor::setInterrupts(interrupts);

    const size_t comparisonsBefore = queue.comparisons;
    const bool allocatedAccepted =
        queue.addAsyncRequest(1, HostedRequestQueue::Record, 77, 88) == 1;
    const size_t comparisonsAfter = queue.comparisons;

    queue.releaseHold.release();
    const bool drained = queue.drain();
    const bool executedBoth = queue.recordedCount == 2 &&
                              queue.recorded[0] == 77 &&
                              queue.recorded[1] == 77;
    queue.destroy();

    passed &= interruptAccepted == Result::Accepted && allocatedAccepted &&
              comparisonsBefore == 0 && comparisonsAfter == 0 && drained &&
              executedBoth && interrupt.isAvailable() &&
              queue.cancellations == 0 && queue.executions == 3;
    passed = check(
        passed,
        "allocation-backed duplicate comparison entered the interrupt-token "
        "coalescing domain");
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "requestqueue-interrupt-duplicate-domain");
    }
    return passed;
}

bool intakeOrderingRegression()
{
    using Result = RequestQueue::InterruptEnqueueResult;

    HostedRequestQueue queue;
    RequestQueue::InterruptRequest firstInterrupt;
    RequestQueue::InterruptRequest secondInterrupt;
    queue.initialise();

    bool passed =
        queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
        queue.holdStarted.acquire();
    passed &= queue.addAsyncRequest(3, HostedRequestQueue::Record, 30) == 1;
    passed &= queue.addAsyncRequest(1, HostedRequestQueue::Record, 10) == 1;

    const bool interrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);
    const Result firstAccepted = queue.enqueueFromInterrupt(
        firstInterrupt, 1, HostedRequestQueue::Record, 11);
    Processor::setInterrupts(interrupts);

    passed &= queue.addAsyncRequest(1, HostedRequestQueue::Record, 12) == 1;
    Processor::setInterrupts(false);
    const Result secondAccepted = queue.enqueueFromInterrupt(
        secondInterrupt, 1, HostedRequestQueue::Record, 13);
    Processor::setInterrupts(interrupts);

    passed &= queue.addAsyncRequest(2, HostedRequestQueue::Record, 20) == 1;
    passed &= queue.addAsyncRequest(0, HostedRequestQueue::Record, 1) == 1;

    queue.releaseHold.release();
    const bool drained = queue.drain();
    const uint64_t expected[] = {1, 10, 11, 12, 13, 20, 30};
    bool orderMatches =
        queue.recordedCount == (sizeof(expected) / sizeof(expected[0]));
    if (orderMatches)
    {
        for (size_t i = 0; i < (sizeof(expected) / sizeof(expected[0])); ++i)
        {
            orderMatches &= queue.recorded[i] == expected[i];
        }
    }

    passed &= firstAccepted == Result::Accepted &&
              secondAccepted == Result::Accepted && drained && orderMatches &&
              queue.recordFailures == 0 && queue.executions == 8 &&
              firstInterrupt.isAvailable() && secondInterrupt.isAvailable();
    queue.destroy();

    passed = check(
        passed,
        "the shared intake lost same-priority FIFO or strict priority order");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-intake-ordering");
    }
    return passed;
}

bool haltRetentionRegression()
{
    using Result = RequestQueue::InterruptEnqueueResult;

    HostedRequestQueue queue;
    RequestQueue::InterruptRequest retained;
    RequestQueue::InterruptRequest rejected;
    queue.initialise();

    bool passed =
        queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
        queue.holdStarted.acquire();
    passed &= queue.addAsyncRequest(1, HostedRequestQueue::Record, 40) == 1;

    const bool interrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);
    const Result retainedResult =
        queue.enqueueFromInterrupt(retained, 1, HostedRequestQueue::Record, 41);
    Processor::setInterrupts(interrupts);

    RequestQueueHaltContext haltContext(&queue);
    Thread *halter = new Thread(
        Scheduler::instance().getKernelProcess(), haltRequestQueue,
        &haltContext, nullptr, false, true);
    halter->setName("hosted RequestQueue halt retention regression");

    bool stopping = false;
    for (size_t attempt = 0; attempt < WaitAttempts; ++attempt)
    {
        stopping =
            queue.getLifecycleState() == RequestQueue::LifecycleState::Stopping;
        if (stopping)
        {
            break;
        }
        Scheduler::instance().yield();
    }

    Result rejectedResult = Result::Accepted;
    if (stopping)
    {
        Processor::setInterrupts(false);
        rejectedResult = queue.enqueueFromInterrupt(
            rejected, 1, HostedRequestQueue::Record, 42);
        Processor::setInterrupts(interrupts);
    }
    const bool retainedBeforeRelease =
        queue.recordedCount == 0 && !retained.isAvailable();
    queue.releaseHold.release();
    const bool halted =
        halter->join() && haltContext.finished == 1 &&
        queue.getLifecycleState() == RequestQueue::LifecycleState::Stopped;
    const bool retainedWhileStopped =
        queue.recordedCount == 0 && !retained.isAvailable();

    queue.resume();
    const bool drained = queue.drain();
    const bool resumedInOrder = queue.recordedCount == 2 &&
                                queue.recorded[0] == 40 &&
                                queue.recorded[1] == 41;
    queue.destroy();

    passed &= retainedResult == Result::Accepted && stopping &&
              rejectedResult == Result::QueueStopped &&
              rejected.isAvailable() && retainedBeforeRelease && halted &&
              retainedWhileStopped && drained && resumedInOrder &&
              retained.isAvailable() && queue.recordFailures == 0 &&
              queue.executions == 3;
    passed = check(
        passed,
        "halt did not close IRQ admission while retaining accepted work");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-halt-retention");
    }
    return passed;
}

bool rejectedPublisherDestroyWaitRegression()
{
    using Result = RequestQueue::InterruptEnqueueResult;

    HostedRequestQueue queue;
    RequestQueue::InterruptRequest request;
    PublicationPauseContext pause;
    queue.setAfterInterruptAdmissionHookForTest(pauseFirstPublication, &pause);

    InterruptPublicationContext publication(
        &queue, &request, 1, HostedRequestQueue::Record, 16);
    Thread *publisher = new Thread(
        Scheduler::instance().getKernelProcess(), publishInterruptRequest,
        &publication, nullptr, false, true);
    publisher->setName("hosted RequestQueue rejected publication pause");
    const bool paused = pause.entered.acquire();

    RequestQueueDestroyContext destroyContext(&queue);
    Thread *destroyer = new Thread(
        Scheduler::instance().getKernelProcess(), destroyRequestQueue,
        &destroyContext, nullptr, false, true);
    destroyer->setName(
        "hosted RequestQueue stopped publication drain regression");

    bool drainWaitObserved = false;
    for (size_t attempt = 0; attempt < WaitAttempts; ++attempt)
    {
        drainWaitObserved = queue.publisherDrainRetriesForTest() != 0;
        if (drainWaitObserved)
        {
            break;
        }
        Scheduler::instance().yield();
    }

    const bool destroyWaited = drainWaitObserved && !publication.finished &&
                               !destroyContext.finished &&
                               request.isAvailable();
    pause.release.release();
    const bool publisherJoined = publisher->join();
    const bool destroyerJoined = destroyer->join();
    queue.setAfterInterruptAdmissionHookForTest(nullptr, nullptr);

    const bool passed = check(
        paused && destroyWaited && publisherJoined && destroyerJoined &&
            publication.finished == 1 &&
            publication.result == Result::QueueStopped &&
            destroyContext.finished == 1 && request.isAvailable() &&
            pause.calls == 1 && pause.failures == 0 && queue.executions == 0 &&
            queue.cancellations == 0 && queue.recordedCount == 0,
        "a never-started queue outran a rejected publisher in its closed-gate "
        "lifetime count");
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "requestqueue-rejected-publication-close-wait");
    }
    return passed;
}

bool interruptAdmissionCloseWaitRegression()
{
    using Result = RequestQueue::InterruptEnqueueResult;

    HostedRequestQueue queue;
    RequestQueue::InterruptRequest request;
    PublicationPauseContext pause;
    queue.initialise();
    queue.setAfterInterruptAdmissionHookForTest(pauseFirstPublication, &pause);

    InterruptPublicationContext publication(
        &queue, &request, 1, HostedRequestQueue::Record, 17);
    Thread *publisher = new Thread(
        Scheduler::instance().getKernelProcess(), publishInterruptRequest,
        &publication, nullptr, false, true);
    publisher->setName("hosted RequestQueue admitted publication pause");
    const bool paused = pause.entered.acquire();

    RequestQueueDestroyContext destroyContext(&queue);
    Thread *destroyer = new Thread(
        Scheduler::instance().getKernelProcess(), destroyRequestQueue,
        &destroyContext, nullptr, false, true);
    destroyer->setName("hosted RequestQueue publication drain regression");

    bool stopping = false;
    for (size_t attempt = 0; attempt < WaitAttempts; ++attempt)
    {
        stopping =
            queue.getLifecycleState() == RequestQueue::LifecycleState::Stopping;
        if (stopping)
        {
            break;
        }
        Scheduler::instance().yield();
    }

    const bool destroyWaited = stopping && !publication.finished &&
                               !destroyContext.finished &&
                               request.isAvailable();
    pause.release.release();
    const bool publisherJoined = publisher->join();
    const bool destroyerJoined = destroyer->join();
    queue.setAfterInterruptAdmissionHookForTest(nullptr, nullptr);

    const bool passed = check(
        paused && destroyWaited && publisherJoined && destroyerJoined &&
            publication.finished == 1 &&
            publication.result == Result::Accepted &&
            destroyContext.finished == 1 && request.isAvailable() &&
            pause.calls == 1 && pause.failures == 0 && queue.executions == 0 &&
            queue.cancellations == 1 && queue.recordedCount == 0,
        "destroy outran an admitted interrupt publisher paused before its "
        "closed-gate observation");
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "requestqueue-interrupt-admission-close-wait");
    }
    return passed;
}

bool intakeTransientPublicationRegression()
{
    using Result = RequestQueue::InterruptEnqueueResult;

    HostedRequestQueue queue;
    RequestQueue::InterruptRequest interrupt;
    PublicationPauseContext pause;
    queue.initialise();

    bool passed =
        queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
        queue.holdStarted.acquire();
    passed &= queue.addAsyncRequest(3, HostedRequestQueue::Record, 30) == 1;
    queue.setAfterIntakeExchangeHookForTest(pauseFirstPublication, &pause);

    InterruptPublicationContext first(
        &queue, &interrupt, 1, HostedRequestQueue::RecordHold, 10);
    Thread *interruptPublisher = new Thread(
        Scheduler::instance().getKernelProcess(), publishInterruptRequest,
        &first, nullptr, false, true);
    interruptPublisher->setName(
        "hosted RequestQueue incomplete MPSC publisher");
    const bool paused = pause.entered.acquire();

    AsyncPublicationContext second(&queue, 1, HostedRequestQueue::Record, 11);
    Thread *ordinaryPublisher = new Thread(
        Scheduler::instance().getKernelProcess(), publishAsyncRequest, &second,
        nullptr, false, true);
    ordinaryPublisher->setName(
        "hosted RequestQueue transient duplicate scanner");

    queue.releaseHold.release();
    bool bothRetried = false;
    for (size_t attempt = 0; attempt < WaitAttempts; ++attempt)
    {
        bothRetried = queue.workerTransientRetriesForTest() &&
                      queue.guardedTransientRetriesForTest();
        if (bothRetried)
        {
            break;
        }
        Scheduler::instance().yield();
    }

    const bool noOvertake =
        queue.recordedCount == 0 && !first.finished && !second.finished;
    pause.release.release();
    const bool interruptJoined = interruptPublisher->join();
    const bool firstHeld = queue.holdStarted.acquire();
    const bool ordinaryJoined = ordinaryPublisher->join();
    queue.setAfterIntakeExchangeHookForTest(nullptr, nullptr);

    const bool firstStillLeads =
        firstHeld && queue.recordedCount == 1 && queue.recorded[0] == 10;
    queue.releaseHold.release();
    const bool drained = queue.drain();
    const uint64_t expected[] = {10, 11, 30};
    bool orderMatches =
        queue.recordedCount == (sizeof(expected) / sizeof(expected[0]));
    if (orderMatches)
    {
        for (size_t i = 0; i < (sizeof(expected) / sizeof(expected[0])); ++i)
        {
            orderMatches &= queue.recorded[i] == expected[i];
        }
    }
    queue.destroy();

    passed &= paused && bothRetried && noOvertake && interruptJoined &&
              ordinaryJoined && first.result == Result::Accepted &&
              first.finished == 1 && second.result == 1 &&
              second.finished == 1 && firstStillLeads && drained &&
              orderMatches && interrupt.isAvailable() && pause.calls == 2 &&
              pause.failures == 0 && queue.executions == 4 &&
              queue.cancellations == 0 && queue.recordFailures == 0;
    passed = check(
        passed,
        "an incomplete MPSC link was treated as empty or lost priority/FIFO "
        "ordering");
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "requestqueue-intake-transient-publication");
    }
    return passed;
}

bool interruptRequestRegressions()
{
    using Result = RequestQueue::InterruptEnqueueResult;

    bool passed = true;
    {
        HostedRequestQueue queue;
        InterruptReleaseContext releaseContext(&queue, false, true);
        RequestQueue::InterruptRequest released(
            interruptRequestReleased, &releaseContext);
        releaseContext.request = &released;
        queue.setMaxAsyncRequests(0);
        queue.initialise();

        passed &= check(
            queue.republishWhileReleasing(
                released, 0, HostedRequestQueue::Sum) == Result::TokenBusy &&
                released.isAvailable(),
            "an idle token accepted release-only republication");
        passed &= check(
            queue.enqueueFromInterrupt(
                released, 0, HostedRequestQueue::Sum, 19, 23) ==
                Result::Accepted,
            "release-callback request was not admitted");
        passed &= check(
            releaseContext.callbackEntered.acquire(),
            "release callback did not enter its drain handoff");

        RequestQueueDrainContext drainContext(&queue);
        Thread *drainer = new Thread(
            Scheduler::instance().getKernelProcess(), drainRequestQueue,
            &drainContext, nullptr, false, true);
        drainer->setName("hosted interrupt release drain regression");
        const bool drainWaitPublished =
            waitUntilQueued(drainer, Thread::CallbackDrain);
        passed &= check(
            drainWaitPublished && drainContext.finished == 0,
            "drain observed a transient empty queue during release");

        passed &= check(
            queue.republishWhileReleasing(
                released, 0, HostedRequestQueue::Sum, 20, 22) ==
                    Result::Accepted &&
                !released.isAvailable(),
            "a producer could not win the final release handoff");
        releaseContext.releaseCallback.release();
        passed &= check(
            drainer->join() && drainContext.result && released.isAvailable() &&
                releaseContext.callbacks == 2 && releaseContext.requeues == 0 &&
                releaseContext.failures == 0 && queue.executions == 2,
            "producer-assisted release work escaped drain");
        queue.destroy();
    }

    {
        HostedRequestQueue queue;
        InterruptReleaseContext releaseContext(&queue, true);
        RequestQueue::InterruptRequest released(
            interruptRequestReleased, &releaseContext);
        releaseContext.request = &released;
        queue.setMaxAsyncRequests(0);
        queue.initialise();

        passed &= check(
            queue.enqueueFromInterrupt(
                released, 0, HostedRequestQueue::Sum, 19, 23) ==
                    Result::Accepted &&
                queue.drain() && released.isAvailable() &&
                releaseContext.callbacks == 2 && releaseContext.requeues == 1 &&
                releaseContext.failures == 0 && queue.executions == 2,
            "release callback could not safely republish at capacity");
        queue.destroy();
    }

    {
        HostedRequestQueue queue;
        RequestQueue::InterruptRequest request;
        RequestQueue::InterruptRequest capacity;

        passed &= check(
            queue.enqueueFromInterrupt(
                request, 0, HostedRequestQueue::Sum, 20, 22) ==
                    Result::QueueStopped &&
                request.isAvailable(),
            "a stopped queue claimed an interrupt token");
        passed &= check(
            queue.enqueueFromInterrupt(
                request, REQUEST_QUEUE_NUM_PRIORITIES,
                HostedRequestQueue::Sum) == Result::InvalidPriority &&
                request.isAvailable(),
            "an invalid interrupt priority claimed its token");

        queue.initialise();
        const bool interrupts = Processor::getInterrupts();
        const size_t comparisonsBefore = queue.comparisons;
        Processor::setInterrupts(false);
        const Result accepted = queue.enqueueFromInterrupt(
            request, 0, HostedRequestQueue::Sum, 20, 22);
        const Result busy = queue.enqueueFromInterrupt(
            request, 0, HostedRequestQueue::Sum, 19, 23);
        const bool deferred = queue.executions == 0;
        Processor::setInterrupts(interrupts);
        PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();

        passed &= check(
            accepted == Result::Accepted && busy == Result::TokenBusy &&
                deferred &&
                static_cast<size_t>(queue.comparisons) == comparisonsBefore,
            "preallocated IF=0 enqueue ran early or called virtual duplicate "
            "detection");
        passed &= check(
            queue.drain() && request.isAvailable() && queue.executions == 1,
            "executed interrupt work did not release its token");

        passed &= check(
            queue.enqueueFromInterrupt(
                request, 0, HostedRequestQueue::InterruptHold, 42) ==
                    Result::Accepted &&
                queue.holdStarted.acquire(),
            "interrupt hold request was not admitted");
        passed &= check(
            queue.enqueueFromInterrupt(
                request, 0, HostedRequestQueue::InterruptHold, 42) ==
                Result::TokenBusy,
            "a published interrupt token was admitted twice");
        queue.setMaxAsyncRequests(1);
        const size_t cancellationsBeforeCapacity = queue.cancellations;
        passed &= check(
            queue.enqueueFromInterrupt(
                capacity, 0, HostedRequestQueue::Sum, 1, 2) ==
                    Result::Accepted &&
                !capacity.isAvailable() &&
                queue.addAsyncRequest(0, HostedRequestQueue::Sum, 3, 4) == 0 &&
                queue.cancellations == cancellationsBeforeCapacity + 1,
            "preallocated work depended on allocation backlog capacity");
        queue.setMaxAsyncRequests(256);
        queue.releaseHold.release();
        passed &= check(
            queue.drain() && request.isAvailable() && capacity.isAvailable() &&
                queue.executions == 3,
            "over-capacity interrupt work did not drain exactly once");

        passed &= check(
            queue.enqueueFromInterrupt(
                capacity, 0, HostedRequestQueue::Sum, 19, 23) ==
                Result::Accepted,
            "a drained interrupt token was not admitted again");
        passed &= check(
            queue.drain() && capacity.isAvailable() && queue.executions == 4,
            "a drained interrupt token could not be reused");
        queue.destroy();
    }

    {
        HostedRequestQueue queue;
        InterruptReleaseContext releaseContext(&queue, false);
        RequestQueue::InterruptRequest cancelled(
            interruptRequestReleased, &releaseContext);
        releaseContext.request = &cancelled;
        queue.initialise();
        passed &= check(
            queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
                queue.holdStarted.acquire(),
            "teardown setup did not hold the worker");
        passed &= check(
            queue.enqueueFromInterrupt(
                cancelled, 0, HostedRequestQueue::CancelQueued) ==
                Result::Accepted,
            "teardown setup did not queue the interrupt token");

        RequestQueueDestroyContext destroyContext(&queue);
        Thread *destroyer = new Thread(
            Scheduler::instance().getKernelProcess(), destroyRequestQueue,
            &destroyContext, nullptr, false, true);
        destroyer->setName("hosted interrupt request destroy regression");
        while (queue.getLifecycleState() !=
               RequestQueue::LifecycleState::Stopping)
        {
            Scheduler::instance().yield();
        }
        queue.releaseHold.release();

        passed &= check(
            destroyer->join() && destroyContext.finished == 1 &&
                cancelled.isAvailable() && queue.cancellations == 1 &&
                queue.queuedCancellations == 1 &&
                releaseContext.callbacks == 1 && releaseContext.failures == 0,
            "destroy did not cancel and release queued interrupt work");
        passed &= check(
            queue.enqueueFromInterrupt(cancelled, 0, HostedRequestQueue::Sum) ==
                    Result::QueueStopped &&
                cancelled.isAvailable(),
            "a stopped queue retained a reused interrupt token");
    }

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-interrupt-token");
    }
    return passed;
}
}  // namespace

bool runHostedRequestQueueRegressions()
{
    Thread *current = Processor::information().getCurrentThread();
    if (!check(
            RoundRobin::runHostedIntrusiveQueueRegressions(current),
            "intrusive scheduler ready-queue invariants failed"))
    {
        return false;
    }
    NOTICE("HOSTED-WAIT-TEST: PASS scheduler-intrusive-ready-queue");

    if (!interruptRequestRegressions())
    {
        return false;
    }

    if (!predicateDoorbellRegression())
    {
        return false;
    }

    if (!interruptDuplicateDomainRegression())
    {
        return false;
    }

    if (!intakeOrderingRegression())
    {
        return false;
    }

    if (!haltRetentionRegression())
    {
        return false;
    }

    if (!rejectedPublisherDestroyWaitRegression())
    {
        return false;
    }

    if (!interruptAdmissionCloseWaitRegression())
    {
        return false;
    }

    if (!intakeTransientPublicationRegression())
    {
        return false;
    }

    if (!watchdogProgressRegression())
    {
        return false;
    }

    {
        HostedRequestQueue activeRequestQueue;
        activeRequestQueue.initialise();
        const bool accepted = activeRequestQueue.addAsyncRequest(
                                  0, HostedRequestQueue::HoldWorker, 42) == 1;
        const bool active =
            accepted && activeRequestQueue.holdStarted.acquire();

        const Time::Timestamp deadline = Time::getTicks() +
                                         Time::Multiplier::Second +
                                         (100 * Time::Multiplier::Millisecond);
        while (Time::getTicks() < deadline)
        {
            Scheduler::instance().yield();
        }

        activeRequestQueue.releaseHold.release();
        const bool drained = activeRequestQueue.drain();
        activeRequestQueue.destroy();
        if (!check(
                accepted && active && drained &&
                    activeRequestQueue.executions == static_cast<size_t>(1),
                "an active request was misclassified as queued backlog"))
        {
            return false;
        }
        NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-active-not-backlog");
    }

    {
        HostedRequestQueue ownedWorkerQueue;
        ownedWorkerQueue.initialise();
        Thread *idleWorker = ownedWorkerQueue.workerThread();
        if (idleWorker)
        {
            idleWorker->setUnwindState(Thread::TerminateThread);
        }
        const bool idleTerminalSafe =
            idleWorker && ownedWorkerQueue.addRequest(
                              0, HostedRequestQueue::Sum, 20, 22) == 42;

        ownedWorkerQueue.halt();
        ownedWorkerQueue.resume();
        Thread *activeWorker = ownedWorkerQueue.workerThread();
        const bool held = ownedWorkerQueue.addAsyncRequest(
                              0, HostedRequestQueue::HoldWorker, 42) == 1 &&
                          ownedWorkerQueue.holdStarted.acquire();
        if (activeWorker)
        {
            activeWorker->setUnwindState(Thread::TerminateThread);
        }
        ownedWorkerQueue.releaseHold.release();
        const bool activeTerminalSafe =
            held && activeWorker &&
            ownedWorkerQueue.addRequest(0, HostedRequestQueue::Sum, 19, 23) ==
                42;
        ownedWorkerQueue.destroy();

        if (!check(
                idleTerminalSafe && activeTerminalSafe,
                "queue-owned worker termination orphaned work"))
        {
            return false;
        }
        NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-worker-terminal-ownership");
    }

    HostedRequestQueue queue;
    bool passed = true;

    passed &= check(
        queue.addRequest(0, HostedRequestQueue::Sum, 1, 2) == 0,
        "a stopped queue accepted work before initialise");

    queue.initialise();
    passed &= check(
        queue.getLifecycleState() == RequestQueue::LifecycleState::Accepting,
        "initialise did not start the worker");
    passed &= check(
        queue.addRequest(
            REQUEST_QUEUE_NUM_PRIORITIES - 1, HostedRequestQueue::Sum, 40, 2) ==
            42,
        "synchronous completion returned the wrong result");
    passed &= check(
        queue.addRequest(
            REQUEST_QUEUE_NUM_PRIORITIES, HostedRequestQueue::Sum, 1, 2) == 0,
        "an invalid priority was accepted");

    queue.halt();
    passed &= check(
        queue.getLifecycleState() == RequestQueue::LifecycleState::Stopped,
        "halt did not join the worker");
    passed &= check(
        queue.addRequest(0, HostedRequestQueue::Sum, 3, 4) == 0,
        "a halted queue accepted work");

    queue.resume();
    passed &= check(
        queue.addRequest(0, HostedRequestQueue::Sum, 20, 22) == 42,
        "the resumed worker did not complete synchronous work");
    passed &= check(
        queue.addRequest(0, HostedRequestQueue::SelfSubmit, 19, 23) == 42,
        "worker self-submission did not execute inline");

    passed &= completionBarrierInterruption(queue, false);
    passed &= completionBarrierInterruption(queue, true);

    passed &= check(
        queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1,
        "the drain test request was not accepted");
    passed &= check(
        queue.holdStarted.acquire(),
        "the drain test worker did not enter its request");
    RequestQueueDrainContext drainContext(&queue);
    Thread *drainer = new Thread(
        Scheduler::instance().getKernelProcess(), drainRequestQueue,
        &drainContext, nullptr, false, true);
    drainer->setName("hosted RequestQueue drain regression");
    const bool drainWaitPublished =
        waitUntilQueued(drainer, Thread::CallbackDrain);
    const bool drainBlocked = drainContext.finished == 0;
    queue.releaseHold.release();
    passed &= check(
        drainer->join() && drainWaitPublished && drainBlocked &&
            drainContext.finished == 1 && drainContext.result,
        "drain returned before all published work completed");

    passed &= check(
        queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1,
        "the blocking worker request was not accepted");
    passed &= check(
        queue.holdStarted.acquire(),
        "the worker did not enter its held request");

    QueuedRequestContext queuedContext(&queue);
    g_QueuedRequestContext = &queuedContext;
    WaitQueue::setBeforeBlockHook(queuedRequestWaitHook);
    Thread *submitter = new Thread(
        Scheduler::instance().getKernelProcess(), submitQueuedRequest,
        &queuedContext, nullptr, false, true);
    submitter->setName("hosted RequestQueue queued caller");

    while (queuedContext.published < 1)
    {
        Scheduler::instance().yield();
    }
    WaitQueue::setBeforeBlockHook(nullptr);
    g_QueuedRequestContext = nullptr;

    RequestQueueDestroyContext destroyContext(&queue);
    Thread *destroyer = new Thread(
        Scheduler::instance().getKernelProcess(), destroyRequestQueue,
        &destroyContext, nullptr, false, true);
    destroyer->setName("hosted RequestQueue destroy regression");

    while (queue.getLifecycleState() != RequestQueue::LifecycleState::Stopping)
    {
        Scheduler::instance().yield();
    }
    queue.releaseHold.release();

    passed &= check(
        destroyer->join(), "the concurrent destroy worker could not be joined");
    passed &= check(
        submitter->join(),
        "the cancelled synchronous caller could not be joined");
    passed &= check(
        destroyContext.finished == 1, "destroy did not complete exactly once");
    passed &= check(
        queuedContext.published == 1 && queuedContext.hookFailures == 0,
        "the queued synchronous caller did not publish one completion wait");
    passed &= check(
        queuedContext.finished == 1 && queuedContext.result == 0,
        "destroy did not wake and reject the queued synchronous caller");

    queue.destroy();
    passed &= check(
        queue.getLifecycleState() == RequestQueue::LifecycleState::Stopped,
        "destroy was not idempotent");
    passed &= check(
        queue.executions == 8,
        "the worker executed an unexpected number of requests");
    passed &= check(
        queue.cancellations == 4,
        "rejected requests did not release their payloads");
    passed &= check(
        queue.queuedCancellations == 1,
        "destroy did not cancel the queued synchronous request");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-lifecycle");
    }
    return passed;
}
