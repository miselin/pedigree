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

#ifndef REQUEST_QUEUE_H
#define REQUEST_QUEUE_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/IntrusiveMpscQueue.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#if THREADS
#include "pedigree/kernel/machine/TimerHandler.h"
#endif

class Thread;
class Timer;
class PerProcessorScheduler;

#define REQUEST_QUEUE_NUM_PRIORITIES 4

/**
 * Implements a request queue with one worker thread.
 *
 * Synchronous callers wait on a request completion owned by the queue.
 * Asynchronous requests are placed directly on the same queue and never need
 * a wrapper thread.
 */
class EXPORTED_PUBLIC RequestQueue
{
  public:
    class PreallocatedRequest;

    enum class OverrunStatus
    {
        Clear,
        Armed,
        Stalled,
        Overloaded,
    };

#if THREADS && HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    /** Takes one watchdog snapshot without reporting it as fatal. */
    OverrunStatus sampleOverrunForTest();

    using HostedSmokeHook = void (*)(void *);

    void setAfterPreallocatedAdmissionHookForTest(
        HostedSmokeHook hook, void *context)
    {
        m_AfterPreallocatedAdmissionHook = hook;
        m_AfterPreallocatedAdmissionContext = context;
    }

    void setAfterIntakeExchangeHookForTest(HostedSmokeHook hook, void *context)
    {
        m_AfterIntakeExchangeHook = hook;
        m_AfterIntakeExchangeContext = context;
    }

    size_t workerTransientRetriesForTest()
    {
        return m_WorkerTransientRetries.value();
    }

    size_t guardedTransientRetriesForTest()
    {
        return m_GuardedTransientRetries.value();
    }

    size_t publisherDrainRetriesForTest()
    {
        return m_PublisherDrainRetries.value();
    }
#endif

#if THREADS
  private:
    class RequestQueueOverrunChecker : public TimerHandler
    {
        friend class RequestQueue;

        RequestQueueOverrunChecker()
            : m_LastQueueSize(0), m_LastProgressGeneration(0), m_Tick(0),
              m_HasBacklogBaseline(false), queue(0)
        {
        }

      private:
        virtual void timer(uint64_t delta);
        OverrunStatus sample(size_t &lastSize, size_t &currentSize);
        void resetBaselineLocked();

        size_t m_LastQueueSize;
        size_t m_LastProgressGeneration;
        uint64_t m_Tick;
        bool m_HasBacklogBaseline;

        RequestQueue *queue;
    };
#endif

  protected:
    class Request;

    struct IntakeNode
    {
        explicit IntakeNode(Request *request = nullptr)
            : next(nullptr), owner(request)
        {
        }

        IntakeNode *next;
        Request *owner;
    };

    /** Request structure shared by allocated and preallocated publications. */
    class Request
    {
      public:
        Request(
            size_t requestPriority, bool asynchronous, uint64_t requestP1,
            uint64_t requestP2, uint64_t requestP3, uint64_t requestP4,
            uint64_t requestP5, uint64_t requestP6, uint64_t requestP7,
            uint64_t requestP8,
            PreallocatedRequest *preallocatedOwner = nullptr)
            : p1(requestP1), p2(requestP2), p3(requestP3), p4(requestP4),
              p5(requestP5), p6(requestP6), p7(requestP7), p8(requestP8),
              m_ReturnValue(0),
#if THREADS
              m_Completion(), m_References(asynchronous ? 1 : 2),
#endif
              m_Next(nullptr), m_Intake(this), m_Priority(requestPriority),
              m_Asynchronous(asynchronous), m_Rejected(false),
              m_Completed(false), m_pPreallocatedOwner(preallocatedOwner)
        {
        }

        uint64_t p1, p2, p3, p4, p5, p6, p7, p8;

      private:
        friend class PreallocatedRequest;
        friend class RequestQueue;

        ~Request() = default;

        uint64_t m_ReturnValue;
#if THREADS
        WaitQueue m_Completion;
        Atomic<size_t> m_References;
#endif
        Request *m_Next;
        IntakeNode m_Intake;
        size_t m_Priority;
        bool m_Asynchronous;
        bool m_Rejected;
        bool m_Completed;
        PreallocatedRequest *m_pPreallocatedOwner;

        Request(const Request &);
        void operator=(const Request &);
    };

    class IntakeLane
    {
      public:
        IntakeLane() : m_Stub(), m_Queue(m_Stub)
        {
        }

        IntakeNode m_Stub;
        IntrusiveMpscQueue<IntakeNode, &IntakeNode::next> m_Queue;

      private:
        IntakeLane(const IntakeLane &) = delete;
        IntakeLane &operator=(const IntakeLane &) = delete;
    };

  public:
    /**
     * Single preallocated publication slot for allocation-free producers.
     *
     * Use one token per logical source. Republishing the same token coalesces
     * work until the queued request has executed or been cancelled.
     *
     * The owner must keep this object alive until isAvailable() is true. A
     * successful publication transfers payload ownership to the queue; every
     * rejection leaves ownership with the caller. An optional release
     * callback runs after execution or cancellation, outside the request-list
     * and waiter guard. The token remains unavailable until it returns; it may
     * republish allocation-free dependent work through
     * republishPreallocatedWhileReleasing().
     * The callback can run on the queue worker, a teardown thread, or inline
     * when threading is disabled. It must be bounded and nonblocking, and must
     * not enter queue lifecycle operations or destroy the queue or token.
     */
    class EXPORTED_PUBLIC PreallocatedRequest
    {
      public:
        using ReleaseCallback = void (*)(void *);

        PreallocatedRequest();
        PreallocatedRequest(
            ReleaseCallback releaseCallback, void *releaseContext);
        ~PreallocatedRequest();

        bool isAvailable() const;

      private:
        friend class RequestQueue;
        NOT_COPYABLE_OR_ASSIGNABLE(PreallocatedRequest);

        enum State
        {
            Idle,
            Claimed,
            Published,
            Releasing,
        };

        Request m_Request;
        Atomic<size_t> m_State;
        Atomic<size_t> m_ReleaseDepth;
        ReleaseCallback m_ReleaseCallback;
        void *m_ReleaseContext;
    };

    enum class PreallocatedPublishResult
    {
        Accepted,
        // Another accepted publication, or a claim that cannot roll back,
        // already owns this token.
        TokenBusy,
        QueueStopped,
        // Retained for source and module ABI compatibility. Preallocated
        // publications do not consume allocation admission.
        QueueFull,
        InvalidPriority,
    };

    /** Creates a new RequestQueue. */
    RequestQueue(const String &name);
    virtual ~RequestQueue();

    // Action to perform when a duplicate request is found in the queue.
    enum ActionOnDuplicate
    {
        // Block waiting for it to complete, and return its return value.
        Block,
        // Ignore the duplicate and create a new request.
        NewRequest,
        // Return immediately, ignoring the result from the request.
        ReturnImmediately
    };

    enum class LifecycleState
    {
        Stopped,
        Accepting,
        Stopping,
    };

    /** Initialises the queue, spawning the worker thread. */
    virtual void initialise();

    /**
     * Destroys the queue and joins its worker.
     *
     * Every external IRQ, timer, and thread producer must already be quiesced.
     * Closing admission drains callers which already entered the publication
     * gate; it cannot extend this object's lifetime around future callers.
     */
    virtual void destroy();

    /**
     * Adds a request to the queue. Blocks until it finishes and returns the
     * result, deferring signal and terminal interruption until completion.
     *
     * \param priority The priority to attach to this request. Lower number is
     * higher priority.
     */
    MUST_USE_RESULT uint64_t addRequest(
        size_t priority, uint64_t p1 = 0, uint64_t p2 = 0, uint64_t p3 = 0,
        uint64_t p4 = 0, uint64_t p5 = 0, uint64_t p6 = 0, uint64_t p7 = 0,
        uint64_t p8 = 0);

    /** Adds a request to the queue with optional behavior on duplicate
     * detection. */
    MUST_USE_RESULT uint64_t addRequest(
        size_t priority, ActionOnDuplicate action, uint64_t p1 = 0,
        uint64_t p2 = 0, uint64_t p3 = 0, uint64_t p4 = 0, uint64_t p5 = 0,
        uint64_t p6 = 0, uint64_t p7 = 0, uint64_t p8 = 0);

    /**
     * Adds an asynchronous request to the queue without waiting for execution.
     *
     * \return One if the request was accepted, zero if it was rejected or
     * deduplicated.
     */
    uint64_t addAsyncRequest(
        size_t priority, uint64_t p1 = 0, uint64_t p2 = 0, uint64_t p3 = 0,
        uint64_t p4 = 0, uint64_t p5 = 0, uint64_t p6 = 0, uint64_t p7 = 0,
        uint64_t p8 = 0);

    /**
     * Publishes allocation-free work using a preallocated token.
     *
     * This path performs no allocation, deallocation, logging, or blocking.
     * It does not participate in compareRequests(): token identity provides
     * coalescing, and allocation-backed requests are always a distinct
     * duplicate domain. Preallocated work bypasses the allocation-backed
     * asynchronous backlog limit. The same token cannot be republished until
     * execution or cancellation has returned it to Idle. TokenBusy is
     * returned only for work whose queue admission can no longer fail. The
     * queue and token owner must outlive this call; destroy() requires every
     * producer to be quiesced first.
     *
     * Raw hardware handlers must defer through the typed IRQ dispatcher, not
     * call RequestQueue publication directly.
     */
    MUST_USE_RESULT PreallocatedPublishResult publishPreallocated(
        PreallocatedRequest &request, size_t priority, uint64_t p1 = 0,
        uint64_t p2 = 0, uint64_t p3 = 0, uint64_t p4 = 0, uint64_t p5 = 0,
        uint64_t p6 = 0, uint64_t p7 = 0, uint64_t p8 = 0);

    /**
     * Republishes a token while its release callback is retiring it.
     *
     * This is accepted only from the Releasing state. Besides callback-driven
     * work, an owner may use it to close the final callback-versus-producer
     * race before falling back to publishPreallocated().
     */
    MUST_USE_RESULT PreallocatedPublishResult
    republishPreallocatedWhileReleasing(
        PreallocatedRequest &request, size_t priority, uint64_t p1 = 0,
        uint64_t p2 = 0, uint64_t p3 = 0, uint64_t p4 = 0, uint64_t p5 = 0,
        uint64_t p6 = 0, uint64_t p7 = 0, uint64_t p8 = 0);

    /**
     * Stop and join the worker, retaining queued requests for resume().
     */
    void halt();

    /**
     * Resume RequestQueue operations.
     */
    void resume();

    /** Returns the current worker lifecycle state. */
    LifecycleState getLifecycleState();

    /**
     * Waits until every request published before and during the wait has
     * completed. Producers must already be quiesced; the queue remains
     * accepting so callbacks may publish dependent work while draining.
     */
    bool drain();

  protected:
    /** Callback - classes are expected to inherit and override this function.
       It's called when a request needs to be executed (by the worker thread).
     */
    virtual uint64_t executeRequest(
        uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
        uint64_t p6, uint64_t p7, uint64_t p8) = 0;

    RequestQueue(const RequestQueue &);
    void operator=(const RequestQueue &);

    /**
     * Defaults to never comparing as equal. Used to determine duplicates
     * among allocation-backed synchronous and asynchronous requests.
     * Preallocated tokens use identity instead and are never passed here.
     * This runs under the queue's non-sleeping guard and therefore must not
     * block.
     */
    virtual bool compareRequests(const Request &a, const Request &b)
    {
        return false;
    }

    /**
     * Releases payload ownership for a request that will not execute.
     *
     * This runs without the queue guard for rejected candidates (including
     * stopped queues, duplicates and capacity limits) and queued requests
     * cancelled by destroy(). Implementations used by allocation-free
     * producers must not sleep. Derived destructors must call destroy() while
     * their override and member state are still alive.
     */
    virtual void cancelRequest(const Request &request)
    {
    }

    /** Thread trampoline */
    static int trampoline(void *p);

    /** Thread worker function */
    int work();

    enum class NextRequestResult
    {
        Item,
        Empty,
        Retry,
    };

    /** Get the next request without bypassing a transient higher priority. */
    NextRequestResult getNextRequest(Request *&request);

    /** Move one priority's accepted intake into its guarded ready list. */
    bool drainIntakeLocked(size_t priority);

    /** Publish an accepted request through its priority's MPSC intake. */
    void publishRequest(Request *request);

    /** Find an equivalent queued or executing request. */
    Request *findDuplicate(const Request &request);

    /** Complete a request and wake every synchronous caller. */
    static void
    completeRequest(Request *request, uint64_t returnValue, bool rejected);

    /** Reject and delete a candidate that was never published. */
    void discardRequest(Request *request);

    uint64_t addAsyncRequestInternal(
        size_t priority, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
        uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8);

    PreallocatedPublishResult publishPreallocatedRequest(
        PreallocatedRequest &request, PreallocatedRequest::State availableState,
        size_t priority, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
        uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8);

    /** Returns an executed or cancelled preallocated request to its owner. */
    static void releasePreallocatedRequest(Request *request);

#if THREADS
    /** Reference management for requests shared by worker and callers. */
    static void retainRequest(Request *request);
    static void releaseRequest(Request *request);
    static uint64_t waitForRequest(Request *request);

    /** Start/stop helpers called with m_LifecycleMutex held. */
    void startWorker();
    bool stopWorker();

    static bool workerReady(void *context);
    void closePreallocatedAdmission();
    void waitForPreallocatedPublishers();
#endif

    static constexpr size_t PublicationClosed = static_cast<size_t>(1)
                                                << ((sizeof(size_t) * 8) - 1);
    static constexpr size_t PublicationCountMask = ~PublicationClosed;

    /** Lock-free intake shared by allocated and preallocated producers. */
    IntakeLane m_IntakeLanes[REQUEST_QUEUE_NUM_PRIORITIES];

    /** The request queue */
    Request *m_pRequestQueue[REQUEST_QUEUE_NUM_PRIORITIES];
    Request *m_pRequestQueueTail[REQUEST_QUEUE_NUM_PRIORITIES];

    /** The request currently being executed by the worker. */
    Request *m_pActiveRequest;

    /** Worker lifecycle, atomically visible to the ready predicate. */
    Atomic<size_t> m_State;

#if THREADS
    /** Serialises initialise/halt/resume/destroy, including worker joins. */
    Mutex m_LifecycleMutex;

    /** Non-sleeping request-list lock and worker predicate wait queue. */
    WaitQueue m_RequestQueueWaiters;

    Thread *m_pThread;

    /** Scheduler which owns the predicate-backed worker and its IRQ doorbell.
     */
    Atomic<PerProcessorScheduler *> m_pWorkerScheduler;

    /** The worker has entered work() and installed its lifetime deferral. */
    Atomic<size_t> m_bWorkerReady;

    /** Keeps a preempted worker eligible inside queue critical sections. */
    Atomic<size_t> m_bWorkerActive;

    /** Changes whenever the worker claims another queued request. */
    size_t m_WorkerProgressGeneration;

    RequestQueueOverrunChecker m_OverrunChecker;
    Timer *m_pOverrunTimer;

    /** High bit closes preallocated publication; low bits count publishers. */
    Atomic<size_t> m_PublicationState;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HostedSmokeHook m_AfterPreallocatedAdmissionHook;
    void *m_AfterPreallocatedAdmissionContext;
    HostedSmokeHook m_AfterIntakeExchangeHook;
    void *m_AfterIntakeExchangeContext;
    Atomic<size_t> m_WorkerTransientRetries;
    Atomic<size_t> m_GuardedTransientRetries;
    Atomic<size_t> m_PublisherDrainRetries;
#endif
#endif

    /** Allocation admission limit and total active async request count. */
    size_t m_nMaxAsyncRequests;
    Atomic<size_t> m_nAsyncRequests;

    /** Number of queued or executing requests. */
    Atomic<size_t> m_nTotalRequests;

    /** One while the single worker is executing or releasing a request. */
    Atomic<size_t> m_nActiveRequests;

    NormalStaticString m_Name;
};

#endif
