/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"

class Process;

RequestQueue::InterruptRequest::InterruptRequest()
    : m_Request(0, true, 0, 0, 0, 0, 0, 0, 0, 0, this), m_State(Idle)
{
}

RequestQueue::InterruptRequest::~InterruptRequest()
{
    if (!isAvailable())
    {
        FATAL("Destroying a published RequestQueue interrupt token.");
    }
}

bool RequestQueue::InterruptRequest::isAvailable() const
{
    return static_cast<size_t>(m_State) == Idle;
}

RequestQueue::RequestQueue(const String &name)
    : m_pActiveRequest(nullptr), m_State(LifecycleState::Stopped),
#if THREADS
      m_LifecycleMutex(), m_RequestQueueWaiters(), m_pThread(nullptr),
      m_bWorkerReady(false), m_pOverrunTimer(nullptr),
#endif
      m_nMaxAsyncRequests(256), m_nAsyncRequests(0), m_nTotalRequests(0),
      m_Name(name.cstr(), name.length())
{
    for (size_t i = 0; i < REQUEST_QUEUE_NUM_PRIORITIES; ++i)
    {
        m_pRequestQueue[i] = nullptr;
        m_pRequestQueueTail[i] = nullptr;
    }

#if THREADS
    m_OverrunChecker.queue = this;
#endif
}

RequestQueue::~RequestQueue()
{
#if THREADS
    bool active = false;
    {
        auto guard = m_RequestQueueWaiters.acquire();
        active = m_State != LifecycleState::Stopped || m_pThread ||
                 m_pOverrunTimer || m_nTotalRequests;
    }
    if (active)
    {
        FATAL(
            "RequestQueue '" << m_Name
                             << "' reached its base destructor while active; "
                                "the most-derived destructor must call "
                                "destroy().");
    }
#endif
}

void RequestQueue::initialise()
{
    resume();
}

#if THREADS
void RequestQueue::startWorker()
{
    {
        auto guard = m_RequestQueueWaiters.acquire();
        if (m_State == LifecycleState::Accepting)
        {
            assert(m_pThread && m_bWorkerReady);
            return;
        }

        if (
            m_State == LifecycleState::Stopping || m_pThread ||
            m_bWorkerReady)
        {
            ERROR(
                "RequestQueue '" << m_Name << "' cannot start while stopping");
            return;
        }
    }

    Process *process = Scheduler::instance().getKernelProcess();
    Thread *worker = new Thread(
        process, &trampoline, reinterpret_cast<void *>(this), nullptr, false,
        false, true);
    worker->setName("RequestQueue worker");

    {
        auto guard = m_RequestQueueWaiters.acquire();
        assert(m_State == LifecycleState::Stopped);
        assert(!m_pThread);
        assert(!m_bWorkerReady);
        m_State = LifecycleState::Accepting;
        m_pThread = worker;
        guard.wakeAll();
    }

    // The delayed worker cannot observe partially published queue state.
    if (!worker->start())
    {
        FATAL("RequestQueue '" << m_Name << "' could not start its worker");
    }

    // A terminal request can retire a delayed Thread before its entry point
    // runs. Do not publish a usable queue until work() has installed the
    // queue-owned lifetime deferral.
    while (true)
    {
        auto guard = m_RequestQueueWaiters.acquire();
        if (m_bWorkerReady)
        {
            break;
        }
        if (
            m_State != LifecycleState::Accepting ||
            m_pThread != worker)
        {
            FATAL(
                "RequestQueue '" << m_Name
                                 << "' lost its worker during startup");
        }
        const WaitQueue::WakeReason reason = guard.waitForCompletion(
            WaitQueue::Channel(this, 2), Thread::CondWait,
            reinterpret_cast<uintptr_t>(this));
        (void) reason;
    }
}

bool RequestQueue::stopWorker()
{
    Thread *worker = nullptr;
    {
        auto guard = m_RequestQueueWaiters.acquire();
        worker = m_pThread;
        if (!worker)
        {
            m_State = LifecycleState::Stopped;
            m_bWorkerReady = false;
            return true;
        }

        if (worker == Processor::information().getCurrentThread())
        {
            ERROR("RequestQueue '" << m_Name << "' worker cannot halt itself");
            return false;
        }

        if (m_State == LifecycleState::Accepting)
        {
            m_State = LifecycleState::Stopping;
            guard.wakeAll();
        }
    }

    if (!worker->joinForCompletion())
    {
        ERROR("RequestQueue '" << m_Name << "' could not join its worker");
        return false;
    }

    {
        auto guard = m_RequestQueueWaiters.acquire();
        m_pThread = nullptr;
        m_State = LifecycleState::Stopped;
        m_bWorkerReady = false;
    }
    return true;
}
#endif

void RequestQueue::destroy()
{
#if THREADS
    TerminationDeferral terminationDeferral;
    LockGuard<Mutex> lifecycleGuard(m_LifecycleMutex);
    if (!stopWorker())
    {
        return;
    }

    if (m_pOverrunTimer)
    {
        if (!m_pOverrunTimer->unregisterHandler(&m_OverrunChecker))
        {
            FATAL(
                "RequestQueue '" << m_Name
                                 << "' could not drain its timer callback");
        }
        m_pOverrunTimer = nullptr;
    }

    Request *cancelled = nullptr;
    {
        auto guard = m_RequestQueueWaiters.acquire();
        assert(!m_pActiveRequest);
        for (size_t priority = 0; priority < REQUEST_QUEUE_NUM_PRIORITIES;
             ++priority)
        {
            Request *request = m_pRequestQueue[priority];
            m_pRequestQueue[priority] = nullptr;
            m_pRequestQueueTail[priority] = nullptr;

            while (request)
            {
                Request *next = request->m_Next;
                request->m_Next = cancelled;
                cancelled = request;
                request = next;
            }
        }
        m_nTotalRequests = 0;
        m_nAsyncRequests = 0;
    }

    while (cancelled)
    {
        Request *next = cancelled->m_Next;
        cancelled->m_Next = nullptr;
        cancelRequest(*cancelled);
        completeRequest(cancelled, 0, true);
        releaseRequest(cancelled);
        cancelled = next;
    }
#endif
}

uint64_t RequestQueue::addRequest(
    size_t priority, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
    uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8)
{
    return addRequest(
        priority, RequestQueue::Block, p1, p2, p3, p4, p5, p6, p7, p8);
}

uint64_t RequestQueue::addRequest(
    size_t priority, ActionOnDuplicate action, uint64_t p1, uint64_t p2,
    uint64_t p3, uint64_t p4, uint64_t p5, uint64_t p6, uint64_t p7,
    uint64_t p8)
{
#if THREADS
    Request *candidate =
        new Request(priority, false, p1, p2, p3, p4, p5, p6, p7, p8);

    if (priority >= REQUEST_QUEUE_NUM_PRIORITIES)
    {
        ERROR(
            "RequestQueue '" << m_Name << "' rejected invalid priority "
                             << priority);
        discardRequest(candidate);
        return 0;
    }

    Request *request = nullptr;
    bool rejected = false;
    bool executeInline = false;
    {
        auto guard = m_RequestQueueWaiters.acquire();
        if (m_State != LifecycleState::Accepting)
        {
            rejected = true;
        }
        else if (m_pThread == Processor::information().getCurrentThread())
        {
            // A worker cannot wait for itself. Execute nested synchronous work
            // inline after dropping the queue guard.
            executeInline = true;
        }
        else
        {
            if (action != NewRequest)
            {
                request = findDuplicate(*candidate);
            }

            if (request)
            {
                if (action == ReturnImmediately)
                {
                    rejected = true;
                }
                else
                {
                    retainRequest(request);
                }
            }
            else
            {
                size_t requestPriority = candidate->m_Priority;
                if (m_pRequestQueueTail[requestPriority])
                {
                    m_pRequestQueueTail[requestPriority]->m_Next = candidate;
                }
                else
                {
                    m_pRequestQueue[requestPriority] = candidate;
                }
                m_pRequestQueueTail[requestPriority] = candidate;
                ++m_nTotalRequests;
                guard.wakeOne();
                request = candidate;
                candidate = nullptr;
            }
        }
    }

    if (executeInline)
    {
        delete candidate;
        return executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
    }
    if (candidate)
    {
        discardRequest(candidate);
    }
    if (rejected)
    {
        return 0;
    }
    assert(request);
    return waitForRequest(request);
#else
    if (priority >= REQUEST_QUEUE_NUM_PRIORITIES)
    {
        ERROR(
            "RequestQueue '" << m_Name << "' rejected invalid priority "
                             << priority);
        Request *candidate =
            new Request(priority, false, p1, p2, p3, p4, p5, p6, p7, p8);
        discardRequest(candidate);
        return 0;
    }
    return executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
#endif
}

uint64_t RequestQueue::addAsyncRequest(
    size_t priority, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
    uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8)
{
    return addAsyncRequestInternal(priority, p1, p2, p3, p4, p5, p6, p7, p8);
}

uint64_t RequestQueue::tryAddAsyncRequest(
    size_t priority, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
    uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8)
{
    return addAsyncRequestInternal(priority, p1, p2, p3, p4, p5, p6, p7, p8);
}

RequestQueue::InterruptEnqueueResult RequestQueue::enqueueFromInterrupt(
    InterruptRequest &token, size_t priority, uint64_t p1, uint64_t p2,
    uint64_t p3, uint64_t p4, uint64_t p5, uint64_t p6, uint64_t p7,
    uint64_t p8)
{
    if (priority >= REQUEST_QUEUE_NUM_PRIORITIES)
    {
        return InterruptEnqueueResult::InvalidPriority;
    }
    if (!token.m_State.compareAndSwap(
            InterruptRequest::Idle, InterruptRequest::Claimed))
    {
        return InterruptEnqueueResult::TokenBusy;
    }

    Request *request = &token.m_Request;
    request->p1 = p1;
    request->p2 = p2;
    request->p3 = p3;
    request->p4 = p4;
    request->p5 = p5;
    request->p6 = p6;
    request->p7 = p7;
    request->p8 = p8;
    request->m_ReturnValue = 0;
#if THREADS
    request->m_References = 1;
#endif
    request->m_Next = nullptr;
    request->m_Priority = priority;
    request->m_Asynchronous = true;
    request->m_Rejected = false;
    request->m_Completed = false;

#if !THREADS
    token.m_State = InterruptRequest::Published;
    executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
    token.m_State = InterruptRequest::Idle;
    return InterruptEnqueueResult::Accepted;
#else
    InterruptEnqueueResult result = InterruptEnqueueResult::Accepted;
    {
        auto guard = m_RequestQueueWaiters.acquire();
        if (m_State != LifecycleState::Accepting)
        {
            result = InterruptEnqueueResult::QueueStopped;
        }
        else if (m_nAsyncRequests >= m_nMaxAsyncRequests)
        {
            result = InterruptEnqueueResult::QueueFull;
        }
        else
        {
            token.m_State = InterruptRequest::Published;
            if (m_pRequestQueueTail[priority])
            {
                m_pRequestQueueTail[priority]->m_Next = request;
            }
            else
            {
                m_pRequestQueue[priority] = request;
            }
            m_pRequestQueueTail[priority] = request;
            ++m_nAsyncRequests;
            ++m_nTotalRequests;
            guard.wakeOne();
        }
    }

    if (result != InterruptEnqueueResult::Accepted)
    {
        token.m_State = InterruptRequest::Idle;
    }
    return result;
#endif
}

uint64_t RequestQueue::addAsyncRequestInternal(
    size_t priority, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
    uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8)
{
#if !THREADS
    Request *request =
        new Request(priority, true, p1, p2, p3, p4, p5, p6, p7, p8);
    if (priority >= REQUEST_QUEUE_NUM_PRIORITIES)
    {
        ERROR(
            "RequestQueue '" << m_Name << "' rejected invalid priority "
                             << priority);
        discardRequest(request);
        return 0;
    }
    executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
    delete request;
    return 1;
#else
    Request *request =
        new Request(priority, true, p1, p2, p3, p4, p5, p6, p7, p8);

    if (priority >= REQUEST_QUEUE_NUM_PRIORITIES)
    {
        ERROR(
            "RequestQueue '" << m_Name << "' rejected invalid priority "
                             << priority);
        discardRequest(request);
        return 0;
    }

    bool rejected = false;
    bool overloaded = false;
    {
        auto guard = m_RequestQueueWaiters.acquire();
        if (m_State != LifecycleState::Accepting)
        {
            rejected = true;
        }
        else if (findDuplicate(*request))
        {
            rejected = true;
        }
        else if (m_nAsyncRequests >= m_nMaxAsyncRequests)
        {
            rejected = true;
            overloaded = true;
        }
        else
        {
            if (m_pRequestQueueTail[priority])
            {
                m_pRequestQueueTail[priority]->m_Next = request;
            }
            else
            {
                m_pRequestQueue[priority] = request;
            }
            m_pRequestQueueTail[priority] = request;
            ++m_nAsyncRequests;
            ++m_nTotalRequests;
            guard.wakeOne();
        }
    }

    if (overloaded)
    {
        ERROR(
            "RequestQueue: '" << m_Name
                              << "' is not keeping up with async requests");
        ERROR(
            " -> priority=" << priority << ", p1=" << Hex << p1 << ", p2=" << p2
                            << ", p3=" << p3 << ", p4=" << p4);
        ERROR(
            " -> p5=" << Hex << p5 << ", p6=" << p6 << ", p7=" << p7
                      << ", p8=" << p8);
    }
    if (rejected)
    {
        discardRequest(request);
        return 0;
    }
    return 1;
#endif
}

void RequestQueue::halt()
{
#if THREADS
    TerminationDeferral terminationDeferral;
    LockGuard<Mutex> lifecycleGuard(m_LifecycleMutex);
    stopWorker();
#endif
}

void RequestQueue::resume()
{
#if THREADS
    TerminationDeferral terminationDeferral;
    LockGuard<Mutex> lifecycleGuard(m_LifecycleMutex);
    startWorker();

    if (!m_pOverrunTimer)
    {
        Timer *timer = Machine::instance().getTimer();
        if (timer && timer->registerHandler(&m_OverrunChecker))
        {
            m_pOverrunTimer = timer;
        }
    }
#endif
}

RequestQueue::LifecycleState RequestQueue::getLifecycleState()
{
#if THREADS
    auto guard = m_RequestQueueWaiters.acquire();
#endif
    return m_State;
}

bool RequestQueue::drain()
{
#if THREADS
    TerminationDeferral terminationDeferral;
    Thread *current = Processor::information().getCurrentThread();
    while (true)
    {
        auto guard = m_RequestQueueWaiters.acquire();
        if (!m_nTotalRequests)
        {
            return true;
        }
        if (m_State != LifecycleState::Accepting)
        {
            ERROR(
                "RequestQueue '" << m_Name
                                 << "' cannot drain while it is stopping");
            return false;
        }
        if (m_pThread == current)
        {
            ERROR("RequestQueue '" << m_Name << "' worker cannot drain itself");
            return false;
        }

        const WaitQueue::WakeReason reason = guard.waitForCompletion(
            WaitQueue::Channel(this, 1), Thread::CallbackDrain,
            reinterpret_cast<uintptr_t>(this));
        (void) reason;
    }
#else
    return true;
#endif
}

int RequestQueue::trampoline(void *p)
{
    RequestQueue *queue = reinterpret_cast<RequestQueue *>(p);
    return queue->work();
}

RequestQueue::Request *RequestQueue::getNextRequest()
{
    for (size_t priority = 0; priority < REQUEST_QUEUE_NUM_PRIORITIES;
         ++priority)
    {
        Request *request = m_pRequestQueue[priority];
        if (!request)
        {
            continue;
        }

        m_pRequestQueue[priority] = request->m_Next;
        if (!m_pRequestQueue[priority])
        {
            m_pRequestQueueTail[priority] = nullptr;
        }
        request->m_Next = nullptr;
        return request;
    }

    return nullptr;
}

RequestQueue::Request *RequestQueue::findDuplicate(const Request &request)
{
    if (m_pActiveRequest &&
        m_pActiveRequest->m_Priority == request.m_Priority &&
        compareRequests(*m_pActiveRequest, request))
    {
        return m_pActiveRequest;
    }

    Request *queued = m_pRequestQueue[request.m_Priority];
    while (queued)
    {
        if (compareRequests(*queued, request))
        {
            return queued;
        }
        queued = queued->m_Next;
    }

    return nullptr;
}

void RequestQueue::completeRequest(
    Request *request, uint64_t returnValue, bool rejected)
{
#if THREADS
    auto guard = request->m_Completion.acquire();
    assert(!request->m_Completed);
    request->m_ReturnValue = returnValue;
    request->m_Rejected = rejected;
    request->m_Completed = true;
    guard.wakeAll();
#else
    request->m_ReturnValue = returnValue;
    request->m_Rejected = rejected;
    request->m_Completed = true;
#endif
}

void RequestQueue::discardRequest(Request *request)
{
    cancelRequest(*request);
    delete request;
}

#if THREADS
void RequestQueue::retainRequest(Request *request)
{
    request->m_References += 1;
}

void RequestQueue::releaseRequest(Request *request)
{
    if (request->m_pInterruptOwner)
    {
        releaseInterruptRequest(request);
        return;
    }

    assert(static_cast<size_t>(request->m_References));
    if ((request->m_References -= 1) == 0)
    {
        delete request;
    }
}

void RequestQueue::releaseInterruptRequest(Request *request)
{
    assert(request);
    InterruptRequest *owner = request->m_pInterruptOwner;
    assert(owner);
    assert(&owner->m_Request == request);
    assert(
        static_cast<size_t>(owner->m_State) ==
        InterruptRequest::Published);
    owner->m_State = InterruptRequest::Idle;
}

uint64_t RequestQueue::waitForRequest(Request *request)
{
    uint64_t result = 0;

    while (true)
    {
        auto guard = request->m_Completion.acquire();
        if (request->m_Completed)
        {
            if (!request->m_Rejected)
            {
                result = request->m_ReturnValue;
            }
            break;
        }

        // A synchronous request transfers payload lifetime to the queue until
        // execution completes. Signals and terminal teardown may wake this
        // thread, but neither can make that completion contract optional.
        WaitQueue::WakeReason reason = guard.waitForCompletion(
            WaitQueue::Channel(), Thread::CondWait,
            reinterpret_cast<uintptr_t>(request));
        (void) reason;
    }

    releaseRequest(request);
    return result;
}
#endif

int RequestQueue::work()
{
#if THREADS
    // The queue, not an unrelated terminal request, owns worker retirement.
    // This prevents an idle death from leaving Accepting with no worker and
    // prevents active executeRequest state from being abandoned.
    TerminationDeferral workerLifetime;
    {
        auto guard = m_RequestQueueWaiters.acquire();
        if (
            m_pThread != Processor::information().getCurrentThread() ||
            m_State != LifecycleState::Accepting || m_bWorkerReady)
        {
            FATAL(
                "RequestQueue '" << m_Name
                                 << "' worker entered with invalid state");
        }
        m_bWorkerReady = true;
        guard.wakeAll(
            WaitQueue::WakeReason::Signalled,
            WaitQueue::Channel(this, 2));
    }

    while (true)
    {
        Request *request = nullptr;
        {
            auto guard = m_RequestQueueWaiters.acquire();
            if (m_State != LifecycleState::Accepting)
            {
                m_State = LifecycleState::Stopped;
                m_bWorkerReady = false;
                return 0;
            }

            request = getNextRequest();
            if (!request)
            {
                WaitQueue::WakeReason reason = guard.waitForCompletion(
                    WaitQueue::Channel(), Thread::CondWait,
                    reinterpret_cast<uintptr_t>(this));
                (void) reason;
                continue;
            }

            assert(!m_pActiveRequest);
            m_pActiveRequest = request;
        }

        assert(request);

        uint64_t result = executeRequest(
            request->p1, request->p2, request->p3, request->p4, request->p5,
            request->p6, request->p7, request->p8);
        completeRequest(request, result, false);

        {
            auto guard = m_RequestQueueWaiters.acquire();
            assert(m_pActiveRequest == request);
            m_pActiveRequest = nullptr;
            assert(m_nTotalRequests);
            --m_nTotalRequests;
            if (request->m_Asynchronous)
            {
                assert(m_nAsyncRequests);
                --m_nAsyncRequests;
            }
            if (!m_nTotalRequests)
            {
                guard.wakeAll(
                    WaitQueue::WakeReason::Signalled,
                    WaitQueue::Channel(this, 1));
            }
        }

        // Drop the queue's ownership after removing the request from every
        // location discoverable by duplicate detection.
        releaseRequest(request);
    }
#else
    return 0;
#endif
}

#if THREADS
void RequestQueue::RequestQueueOverrunChecker::timer(
    uint64_t delta, InterruptState &)
{
    m_Tick += delta;
    if (m_Tick < Time::Multiplier::Second)
    {
        return;
    }
    m_Tick %= Time::Multiplier::Second;

    size_t lastSize = 0;
    size_t currentSize = 0;
    bool growing = false;
    {
        auto guard = queue->m_RequestQueueWaiters.acquire();
        lastSize = m_LastQueueSize;
        currentSize = queue->m_nTotalRequests;
        if (queue->m_pActiveRequest && currentSize)
        {
            // A request already being executed is worker progress, not queue
            // backlog. Long-running I/O must not look like newly accumulating
            // work merely because it crosses a watchdog sample.
            --currentSize;
        }
        bool accepting = queue->m_State == LifecycleState::Accepting;
        m_LastQueueSize = currentSize;
        growing = accepting && lastSize < currentSize;
    }

    if (growing)
    {
        FATAL(
            "RequestQueue '"
            << queue->m_Name
            << "' is NOT keeping up with incoming requests [1s ago we had "
            << lastSize << " queued requests, now have " << currentSize
            << "]!");
    }
}
#endif
