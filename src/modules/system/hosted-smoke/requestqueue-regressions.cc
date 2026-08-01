/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/RoundRobin.h"
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

    ERROR(
        "HOSTED-WAIT-TEST: FAIL requestqueue-lifecycle: " << detail);
    return false;
}

bool waitUntilQueued(Thread *thread, size_t debugState)
{
    for (size_t attempt = 0; attempt < WaitAttempts; ++attempt)
    {
        Thread::WaitDebugInfo info = {};
        uintptr_t debugAddress = 0;
        if (
            thread->getWaitDebugInfo(info) && info.queue && info.queued &&
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
        WakeAtWorkerBlock,
        InterruptHold,
    };

    HostedRequestQueue()
        : RequestQueue(MakeConstantString("Hosted wait regression")),
          executions(0), cancellations(0), queuedCancellations(0),
          wakeAtWorkerBlockExecutions(0), comparisons(0), holdStarted(0),
          releaseHold(0)
    {
    }

    ~HostedRequestQueue() override
    {
        destroy();
    }

    Atomic<size_t> executions;
    Atomic<size_t> cancellations;
    Atomic<size_t> queuedCancellations;
    Atomic<size_t> wakeAtWorkerBlockExecutions;
    Atomic<size_t> comparisons;
    Semaphore holdStarted;
    Semaphore releaseHold;

    WaitQueue *workerWaitQueue()
    {
        return &m_RequestQueueWaiters;
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
            case WakeAtWorkerBlock:
                wakeAtWorkerBlockExecutions += 1;
                return p2 + p3;
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

    bool compareRequests(const Request &, const Request &) override
    {
        comparisons += 1;
        return false;
    }

};

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

    if (
        !queue || channel.owner || channel.value ||
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

bool completionBarrierInterruption(
    HostedRequestQueue &queue, bool terminal)
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

    const bool deferred =
        requeue.requeues == 1 && requeue.hookFailures == 0 &&
        request.finished == 0;
    const bool signalDelivered =
        terminal || g_RequestQueueSignalCalls == 1;

    queue.releaseHold.release();
    const bool joined = caller->join();

    return check(
        workerHeld && initiallyQueued && injected && deferred &&
            signalDelivered && joined && request.finished == 1 &&
            request.result == 42,
        terminal
            ? "terminal teardown escaped a synchronous request completion"
            : "a signal returned a synchronous request before completion");
}

struct WorkerWakeContext
{
    WorkerWakeContext(HostedRequestQueue *queue, WaitQueue *waitQueue)
        : queue(queue), waitQueue(waitQueue), waiter(nullptr), hookCalls(0),
          hookFailures(0), accepted(0)
    {
    }

    HostedRequestQueue *queue;
    WaitQueue *waitQueue;
    Thread *waiter;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookFailures;
    Atomic<size_t> accepted;
};

WorkerWakeContext *g_WorkerWakeContext = nullptr;

void requestQueueWorkerWaitHook(
    WaitQueue *queue, Thread *thread, const WaitQueue::Channel &channel,
    size_t debugState)
{
    WorkerWakeContext *context = g_WorkerWakeContext;
    if (
        !context || queue != context->waitQueue || channel.owner ||
        channel.value || debugState != Thread::CondWait)
    {
        return;
    }

    context->waiter = thread;
    if (!thread)
    {
        context->hookFailures += 1;
    }
    context->hookCalls += 1;

    // Restrict the global seam to this publication. Enqueueing now wakes the
    // visible waiter before the scheduler can commit Sleeping.
    WaitQueue::setBeforeBlockHook(nullptr);
    context->accepted += context->queue->addAsyncRequest(
        0, HostedRequestQueue::WakeAtWorkerBlock, 20, 22);
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

    context->waiter = thread;
    if (
        !queue || channel.owner || channel.value ||
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

    if (
        context->request && context->requeueOnce &&
        context->requeues.compareAndSwap(0, 1))
    {
        if (
            context->queue->republishFromReleaseCallback(
                *context->request, 0, HostedRequestQueue::Sum, 20, 22) !=
            RequestQueue::InterruptEnqueueResult::Accepted)
        {
            context->failures += 1;
        }
    }
}

bool interruptRequestRegressions()
{
    using Result = RequestQueue::InterruptEnqueueResult;

    bool passed = true;
    {
        HostedRequestQueue queue;
        InterruptReleaseContext releaseContext(&queue, true, true);
        RequestQueue::InterruptRequest released(
            interruptRequestReleased, &releaseContext);
        releaseContext.request = &released;
        queue.setMaxAsyncRequests(1);
        queue.initialise();

        passed &= check(
            queue.republishFromReleaseCallback(
                released, 0, HostedRequestQueue::Sum) == Result::TokenBusy &&
                released.isAvailable(),
            "an idle token accepted callback-only republication");
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

        releaseContext.releaseCallback.release();
        passed &= check(
            drainer->join() && drainContext.result && released.isAvailable() &&
                releaseContext.callbacks == 2 &&
                releaseContext.requeues == 1 &&
                releaseContext.failures == 0 && queue.executions == 2,
            "release callback could not safely republish before drain");
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
        Thread *worker = queue.workerThread();
        bool workerSleeping = false;
        for (size_t attempt = 0; attempt < WaitAttempts; ++attempt)
        {
            workerSleeping = worker &&
                             worker->getStatus() == Thread::Sleeping &&
                             waitUntilQueued(worker, Thread::CondWait);
            if (workerSleeping)
            {
                break;
            }
            Scheduler::instance().yield();
        }

        const bool interrupts = Processor::getInterrupts();
        const size_t comparisonsBefore = queue.comparisons;
        Processor::setInterrupts(false);
        const Result accepted = queue.enqueueFromInterrupt(
            request, 0, HostedRequestQueue::Sum, 20, 22);
        const Result busy = queue.enqueueFromInterrupt(
            request, 0, HostedRequestQueue::Sum, 19, 23);
        const bool workerReady = worker && worker->getStatus() == Thread::Ready;
        Processor::setInterrupts(interrupts);

        passed &= check(
            workerSleeping && accepted == Result::Accepted &&
                busy == Result::TokenBusy && workerReady &&
                static_cast<size_t>(queue.comparisons) == comparisonsBefore,
            "preallocated IF=0 enqueue did not wake exactly once or called "
            "virtual duplicate detection");
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
        passed &= check(
            queue.enqueueFromInterrupt(
                capacity, 0, HostedRequestQueue::Sum, 1, 2) ==
                    Result::QueueFull &&
                capacity.isAvailable(),
            "capacity rejection retained its interrupt token");
        queue.setMaxAsyncRequests(256);
        queue.releaseHold.release();
        passed &= check(
            queue.drain() && request.isAvailable(),
            "held interrupt work did not release its token");

        passed &= check(
            queue.enqueueFromInterrupt(
                capacity, 0, HostedRequestQueue::Sum, 19, 23) ==
                Result::Accepted,
            "a rejected interrupt token could not be reused");
        passed &= check(
            queue.drain() && capacity.isAvailable() &&
                queue.executions == 3,
            "a reused interrupt token did not complete once");
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
            queue.addAsyncRequest(
                0, HostedRequestQueue::HoldWorker, 42) == 1 &&
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
        while (
            queue.getLifecycleState() !=
            RequestQueue::LifecycleState::Stopping)
        {
            Scheduler::instance().yield();
        }
        queue.releaseHold.release();

        passed &= check(
            destroyer->join() && destroyContext.finished == 1 &&
                cancelled.isAvailable() && queue.cancellations == 1 &&
                queue.queuedCancellations == 1 &&
                releaseContext.callbacks == 1 &&
                releaseContext.failures == 0,
            "destroy did not cancel and release queued interrupt work");
        passed &= check(
            queue.enqueueFromInterrupt(
                cancelled, 0, HostedRequestQueue::Sum) ==
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

    {
        HostedRequestQueue activeRequestQueue;
        activeRequestQueue.initialise();
        const bool accepted = activeRequestQueue.addAsyncRequest(
                                  0, HostedRequestQueue::HoldWorker, 42) == 1;
        const bool active =
            accepted && activeRequestQueue.holdStarted.acquire();

        const Time::Timestamp deadline =
            Time::getTicks() + Time::Multiplier::Second +
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
                    activeRequestQueue.executions ==
                        static_cast<size_t>(1),
                "an active request was misclassified as queued backlog"))
        {
            return false;
        }
        NOTICE(
            "HOSTED-WAIT-TEST: PASS requestqueue-active-not-backlog");
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
            idleWorker &&
            ownedWorkerQueue.addRequest(
                0, HostedRequestQueue::Sum, 20, 22) == 42;

        ownedWorkerQueue.halt();
        ownedWorkerQueue.resume();
        Thread *activeWorker = ownedWorkerQueue.workerThread();
        const bool held =
            ownedWorkerQueue.addAsyncRequest(
                0, HostedRequestQueue::HoldWorker, 42) == 1 &&
            ownedWorkerQueue.holdStarted.acquire();
        if (activeWorker)
        {
            activeWorker->setUnwindState(Thread::TerminateThread);
        }
        ownedWorkerQueue.releaseHold.release();
        const bool activeTerminalSafe =
            held && activeWorker &&
            ownedWorkerQueue.addRequest(
                0, HostedRequestQueue::Sum, 19, 23) == 42;
        ownedWorkerQueue.destroy();

        if (!check(
                idleTerminalSafe && activeTerminalSafe,
                "queue-owned worker termination orphaned work"))
        {
            return false;
        }
        NOTICE(
            "HOSTED-WAIT-TEST: PASS requestqueue-worker-terminal-ownership");
    }

    HostedRequestQueue queue;
    bool passed = true;

    passed &= check(
        queue.addRequest(0, HostedRequestQueue::Sum, 1, 2) == 0,
        "a stopped queue accepted work before initialise");

    WorkerWakeContext workerWake(&queue, queue.workerWaitQueue());
    g_WorkerWakeContext = &workerWake;
    WaitQueue::setBeforeBlockHook(requestQueueWorkerWaitHook);
    queue.initialise();
    while (queue.wakeAtWorkerBlockExecutions < 1)
    {
        Scheduler::instance().yield();
    }
    WaitQueue::setBeforeBlockHook(nullptr);
    g_WorkerWakeContext = nullptr;

    passed &= check(
        workerWake.hookCalls == 1 && workerWake.hookFailures == 0 &&
            workerWake.waiter != nullptr && workerWake.accepted == 1,
        "the worker pre-block seam did not enqueue exactly one request");
    passed &= check(
        queue.wakeAtWorkerBlockExecutions == 1,
        "the worker lost a request queued between publication and block");
    passed &= check(
        queue.getLifecycleState() ==
            RequestQueue::LifecycleState::Accepting,
        "initialise did not start the worker");
    passed &= check(
        queue.addRequest(
            REQUEST_QUEUE_NUM_PRIORITIES - 1, HostedRequestQueue::Sum, 40,
            2) == 42,
        "synchronous completion returned the wrong result");
    passed &= check(
        queue.addRequest(
            REQUEST_QUEUE_NUM_PRIORITIES, HostedRequestQueue::Sum, 1,
            2) == 0,
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
        queue.addRequest(
            0, HostedRequestQueue::Sum, 20, 22) == 42,
        "the resumed worker did not complete synchronous work");
    passed &= check(
        queue.addRequest(
            0, HostedRequestQueue::SelfSubmit, 19, 23) == 42,
        "worker self-submission did not execute inline");

    passed &= completionBarrierInterruption(queue, false);
    passed &= completionBarrierInterruption(queue, true);

    passed &= check(
        queue.addAsyncRequest(
            0, HostedRequestQueue::HoldWorker, 42) == 1,
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
        queue.addAsyncRequest(
            0, HostedRequestQueue::HoldWorker, 42) == 1,
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

    while (
        queue.getLifecycleState() !=
        RequestQueue::LifecycleState::Stopping)
    {
        Scheduler::instance().yield();
    }
    queue.releaseHold.release();

    passed &= check(
        destroyer->join(),
        "the concurrent destroy worker could not be joined");
    passed &= check(
        submitter->join(),
        "the cancelled synchronous caller could not be joined");
    passed &= check(
        destroyContext.finished == 1, "destroy did not complete exactly once");
    passed &= check(
        queuedContext.published == 1 &&
            queuedContext.hookFailures == 0,
        "the queued synchronous caller did not publish one completion wait");
    passed &= check(
        queuedContext.finished == 1 && queuedContext.result == 0,
        "destroy did not wake and reject the queued synchronous caller");

    queue.destroy();
    passed &= check(
        queue.getLifecycleState() == RequestQueue::LifecycleState::Stopped,
        "destroy was not idempotent");
    passed &= check(
        queue.executions == 9,
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
