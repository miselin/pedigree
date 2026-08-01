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
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#if THREADS
#include "pedigree/kernel/machine/TimerHandler.h"
#endif

class Thread;
class Timer;

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
    class InterruptRequest;

#if THREADS
  private:
    class RequestQueueOverrunChecker : public TimerHandler
    {
        friend class RequestQueue;

        RequestQueueOverrunChecker() : m_LastQueueSize(0), m_Tick(0), queue(0)
        {
        }

      private:
        virtual void timer(uint64_t delta, InterruptState &state);

        size_t m_LastQueueSize;
        uint64_t m_Tick;

        RequestQueue *queue;
    };
#endif

  protected:
    /** Request structure shared by allocated and preallocated publications. */
    class Request
    {
      public:
        Request(
            size_t requestPriority, bool asynchronous, uint64_t requestP1,
            uint64_t requestP2, uint64_t requestP3, uint64_t requestP4,
            uint64_t requestP5, uint64_t requestP6, uint64_t requestP7,
            uint64_t requestP8, InterruptRequest *interruptOwner = nullptr)
            : p1(requestP1), p2(requestP2), p3(requestP3), p4(requestP4),
              p5(requestP5), p6(requestP6), p7(requestP7), p8(requestP8),
              m_ReturnValue(0),
#if THREADS
              m_Completion(), m_References(asynchronous ? 1 : 2),
#endif
              m_Next(nullptr), m_Priority(requestPriority),
              m_Asynchronous(asynchronous), m_Rejected(false),
              m_Completed(false), m_pInterruptOwner(interruptOwner)
        {
        }

        uint64_t p1, p2, p3, p4, p5, p6, p7, p8;

      private:
        friend class InterruptRequest;
        friend class RequestQueue;

        ~Request() = default;

        uint64_t m_ReturnValue;
#if THREADS
        WaitQueue m_Completion;
        Atomic<size_t> m_References;
#endif
        Request *m_Next;
        size_t m_Priority;
        bool m_Asynchronous;
        bool m_Rejected;
        bool m_Completed;
        InterruptRequest *m_pInterruptOwner;

        Request(const Request &);
        void operator=(const Request &);
    };

  public:
    /**
     * Single preallocated publication slot for IRQ and timer callbacks.
     *
     * Use one token per logical interrupt source. Republishing the same token
     * coalesces work until the queued request has executed or been cancelled.
     *
     * The owner must keep this object alive until isAvailable() is true. A
     * successful enqueue transfers payload ownership to the queue; every
     * rejection leaves ownership with the caller.
     */
    class EXPORTED_PUBLIC InterruptRequest
    {
      public:
        InterruptRequest();
        ~InterruptRequest();

        bool isAvailable() const;

      private:
        friend class RequestQueue;
        NOT_COPYABLE_OR_ASSIGNABLE(InterruptRequest);

        enum State
        {
            Idle,
            Claimed,
            Published,
        };

        Request m_Request;
        Atomic<size_t> m_State;
    };

    enum class InterruptEnqueueResult
    {
        Accepted,
        TokenBusy,
        QueueStopped,
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

    /** Destroys the queue, killing the worker thread (safely) */
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
     * Publishes preallocated work from an IRQ or timer callback.
     *
     * This path performs no allocation, deallocation, logging, or blocking.
     * It does not call compareRequests(): token identity is the interrupt-side
     * coalescing mechanism. The same token cannot be republished until
     * execution or cancellation has returned it to Idle.
     */
    MUST_USE_RESULT InterruptEnqueueResult enqueueFromInterrupt(
        InterruptRequest &request, size_t priority, uint64_t p1 = 0,
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
     * for synchronous and asynchronous requests. This runs under the queue's
     * non-sleeping guard and therefore must not block.
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
     * cancelled by destroy(). Implementations used by interrupt-side queues
     * must not sleep. Derived destructors must call destroy() while their
     * override and member state are still alive.
     */
    virtual void cancelRequest(const Request &request)
    {
    }

    /** Thread trampoline */
    static int trampoline(void *p);

    /** Thread worker function */
    int work();

    /** Get the next Request, or NULL if no available requests. */
    Request *getNextRequest();

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

    /** Returns an executed or cancelled preallocated request to its owner. */
    static void releaseInterruptRequest(Request *request);

#if THREADS
    /** Reference management for requests shared by worker and callers. */
    static void retainRequest(Request *request);
    static void releaseRequest(Request *request);
    static uint64_t waitForRequest(Request *request);

    /** Start/stop helpers called with m_LifecycleMutex held. */
    void startWorker();
    bool stopWorker();
#endif

    /** The request queue */
    Request *m_pRequestQueue[REQUEST_QUEUE_NUM_PRIORITIES];
    Request *m_pRequestQueueTail[REQUEST_QUEUE_NUM_PRIORITIES];

    /** The request currently being executed by the worker. */
    Request *m_pActiveRequest;

    /** Worker lifecycle, protected by m_RequestQueueWaiters. */
    LifecycleState m_State;

#if THREADS
    /** Serialises initialise/halt/resume/destroy, including worker joins. */
    Mutex m_LifecycleMutex;

    /** Non-sleeping request-list lock and worker predicate wait queue. */
    WaitQueue m_RequestQueueWaiters;

    Thread *m_pThread;

    /** The worker has entered work() and installed its lifetime deferral. */
    bool m_bWorkerReady;

    RequestQueueOverrunChecker m_OverrunChecker;
    Timer *m_pOverrunTimer;
#endif

    /** Maximum and current number of queued or executing async requests. */
    size_t m_nMaxAsyncRequests;
    size_t m_nAsyncRequests;

    /** Number of queued or executing requests. */
    size_t m_nTotalRequests;

    NormalStaticString m_Name;
};

#endif
