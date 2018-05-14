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
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

#include "pedigree/kernel/utilities/assert.h"

RequestQueue::RequestQueue(const String &name)
    : m_Stop(false),
#ifdef THREADS
      m_RequestQueueMutex(false), m_pThread(0), m_Halted(false),
#endif
      m_nMaxAsyncRequests(256), m_nAsyncRequests(0), m_Name(name)
{
    for (size_t i = 0; i < REQUEST_QUEUE_NUM_PRIORITIES; i++)
        m_pRequestQueue[i] = 0;

#ifdef THREADS
    m_OverrunChecker.queue = this;
#endif
}

RequestQueue::~RequestQueue()
{
    destroy();
}

void RequestQueue::initialise()
{
// Start the worker thread.
#ifdef THREADS
    if (m_pThread)
    {
        WARNING("RequestQueue initialised multiple times - don't do this.");
        return;
    }

    // Start RequestQueue workers in the kernel process only.
    Process *pProcess = Scheduler::instance().getKernelProcess();

    m_Stop = false;
    m_pThread =
        new Thread(pProcess, &trampoline, reinterpret_cast<void *>(this));
    m_Halted = false;

    // Add our timer so we can figure out if we're not keeping up with
    // synchronous requests
    Timer *t = Machine::instance().getTimer();
    if (t)
    {
        t->registerHandler(&m_OverrunChecker);
    }
#else
    WARNING("RequestQueue: This build does not support threads");
#endif
}

void RequestQueue::destroy()
{
#ifdef THREADS
    // Halt the queue - we're done.
    halt();

    // Clean up the queue in full.
    m_RequestQueueMutex.acquire();
    for (size_t i = 0; i < REQUEST_QUEUE_NUM_PRIORITIES; ++i)
    {
        Request *pRequest = m_pRequestQueue[i];

        // No more requests at this priority, we're cleaning up.
        m_pRequestQueue[i] = 0;

        while (pRequest)
        {
            // Cancel the request, let the owner clean up.
            pRequest->bReject = true;
            pRequest->mutex.release();
            pRequest = pRequest->next;
        }
    }
    m_RequestQueueMutex.release();

    Timer *t = Machine::instance().getTimer();
    if (t)
    {
        t->unregisterHandler(&m_OverrunChecker);
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
#ifdef THREADS
    // Create a new request object.
    Request *pReq = new Request();
    pReq->p1 = p1;
    pReq->p2 = p2;
    pReq->p3 = p3;
    pReq->p4 = p4;
    pReq->p5 = p5;
    pReq->p6 = p6;
    pReq->p7 = p7;
    pReq->p8 = p8;
    pReq->next = 0;
    pReq->bReject = false;
    pReq->refcnt = 1;
    pReq->owner = this;
    pReq->priority = priority;

    // Do we own pReq?
    bool bOwnRequest = true;

    // Add to the request queue.
    m_RequestQueueMutex.acquire();

    if (m_pRequestQueue[priority] == 0)
        m_pRequestQueue[priority] = pReq;
    else
    {
        Request *p = m_pRequestQueue[priority];
        while (p->next != 0)
        {
            // Wait for duplicates instead of re-inserting, if the compare
            // function is defined.
            if (compareRequests(*p, *pReq) && action != NewRequest)
            {
                bOwnRequest = false;
                delete pReq;
                pReq = p;
                break;
            }
            p = p->next;
        }

        if (bOwnRequest && compareRequests(*p, *pReq) && action != NewRequest)
        {
            bOwnRequest = false;
            delete pReq;
            pReq = p;
        }
        else if (bOwnRequest)
            p->next = pReq;
    }

    if (!bOwnRequest)
    {
        if (action == ReturnImmediately)
        {
            m_RequestQueueMutex.release();
            return 0;
        }
        ++pReq->refcnt;
    }
    else
    {
        pReq->pThread = Processor::information().getCurrentThread();
        pReq->pThread->addRequest(pReq);
    }

    ++m_nTotalRequests;

    // One more item now available.
    m_RequestQueueCondition.signal();
    m_RequestQueueMutex.release();

    // We are waiting on the worker thread - mark the thread as such.
    Thread *pThread = Processor::information().getCurrentThread();
    pThread->setBlockingThread(m_pThread);

    if (pReq->bReject)
    {
        // Hmm, in the time the RequestQueueMutex was being acquired, we got
        // pre-empted, and then an unexpected exit event happened. The request
        // is to be rejected, so don't acquire the mutex at all.
        if (!--pReq->refcnt)
            delete pReq;
        return 0;
    }

    // Wait for the request to be satisfied. This should sleep the thread.
    pReq->mutex.acquire();

    m_RequestQueueMutex.acquire();
    --m_nTotalRequests;
    m_RequestQueueMutex.release();

    // Don't use the Thread object if it may be already freed
    if (!pReq->bReject)
        pThread->setBlockingThread(0);

    if (pReq->bReject || pThread->wasInterrupted() ||
        pThread->getUnwindState() == Thread::Exit)
    {
        // The request was interrupted somehow. We cannot assume that pReq's
        // contents are valid, so just return zero. The caller may have to redo
        // their request.
        // By releasing here, the worker thread can detect that the request was
        // interrupted and clean up by itself.
        NOTICE("RequestQueue::addRequest - interrupted");
        if (pReq->bReject && !--pReq->refcnt)
            delete pReq;  // Safe to delete, unexpected exit condition
        else
            pReq->mutex.release();
        return 0;
    }

    // Grab the result.
    uintptr_t ret = pReq->ret;

    // Delete the request structure.
    if (bOwnRequest && pReq->pThread)
        pReq->pThread->removeRequest(pReq);
    if (!--pReq->refcnt)
        delete pReq;
    else
        pReq->mutex.release();

    return ret;
#else
    return executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
#endif
}

int RequestQueue::doAsync(void *p)
{
    RequestQueue::Request *pReq = reinterpret_cast<RequestQueue::Request *>(p);

    ++(pReq->owner->m_nAsyncRequests);

    // Just return if the request is a duplicate, as our caller doesn't care and
    // we're otherwise just using up another thread stack and burning time for
    // no real reason.
    uint64_t result = pReq->owner->addRequest(
        pReq->priority, ReturnImmediately, pReq->p1, pReq->p2, pReq->p3,
        pReq->p4, pReq->p5, pReq->p6, pReq->p7, pReq->p8);

    --(pReq->owner->m_nAsyncRequests);

    delete pReq;

    return 0;
}

uint64_t RequestQueue::addAsyncRequest(
    size_t priority, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
    uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8)
{
#ifndef THREADS
    return addRequest(priority, p1, p2, p3, p4, p5, p6, p7, p8);
#else
    // Create a new request object.
    Request *pReq = new Request();
    pReq->p1 = p1;
    pReq->p2 = p2;
    pReq->p3 = p3;
    pReq->p4 = p4;
    pReq->p5 = p5;
    pReq->p6 = p6;
    pReq->p7 = p7;
    pReq->p8 = p8;
    pReq->next = 0;
    pReq->bReject = false;
    pReq->refcnt = 0;
    pReq->owner = this;
    pReq->priority = priority;

    // We cannot block, so we just have to drop the request if the queue is
    // already overloaded with async requests.
    if (m_nAsyncRequests >= m_nMaxAsyncRequests)
    {
        ERROR(
            "RequestQueue: '" << m_Name
                              << "' is not keeping up with demand for "
                                 "async requests");
        ERROR(
            " -> priority=" << priority << ", p1=" << Hex << p1 << ", p2=" << p2
                            << ", p3=" << p3 << ", p4=" << p4);
        ERROR(
            " -> p5=" << Hex << p5 << ", p6=" << p6 << ", p7=" << p7
                      << ", p8=" << p8);
        delete pReq;
        return 0;
    }

    // Add to RequestQueue.
    Process *pProcess = Scheduler::instance().getKernelProcess();
    Thread *pThread =
        new Thread(pProcess, &doAsync, reinterpret_cast<void *>(pReq));
    pThread->detach();
#endif

    return 0;
}

void RequestQueue::halt()
{
#ifdef THREADS
    m_RequestQueueMutex.acquire();
    if (!m_Halted)
    {
        m_Stop = true;
        m_RequestQueueCondition.broadcast();

        // Join now - we need to release the mutex so the worker thread can keep
        // going, as it could be blocked on trying to acquire it right now.
        m_RequestQueueMutex.release();
        m_pThread->join();
        m_RequestQueueMutex.acquire();

        m_pThread = 0;
        m_Halted = true;
    }
    m_RequestQueueMutex.release();
#endif
}

void RequestQueue::resume()
{
#ifdef THREADS
    LockGuard<Mutex> guard(m_RequestQueueMutex);

    if (m_Halted)
    {
        initialise();
    }
#endif
}

int RequestQueue::trampoline(void *p)
{
    RequestQueue *pRQ = reinterpret_cast<RequestQueue *>(p);
    return pRQ->work();
}

RequestQueue::Request *RequestQueue::getNextRequest()
{
#ifdef THREADS
    // Must have the lock to be here.
    assert(!m_RequestQueueMutex.getValue());
#endif

    // Get the most important queue with data in.
    /// \todo Stop possible starvation here.
    size_t priority = 0;
    bool bFound = false;
    for (priority = 0; priority < REQUEST_QUEUE_NUM_PRIORITIES - 1; priority++)
    {
        if (m_pRequestQueue[priority])
        {
            bFound = true;
            break;
        }
    }

    if (!bFound)
    {
        return 0;
    }

    Request *pReq = m_pRequestQueue[priority];
    if (pReq != 0)
    {
        m_pRequestQueue[priority] = pReq->next;
    }

    return pReq;
}

int RequestQueue::work()
{
#ifdef THREADS
    // Hold from the start - this will be released by the condition variable
    // wait for us, and re-acquired on return, so we'll always have the lock
    // until we explicitly release it.
    m_RequestQueueMutex.acquire();
    while (true)
    {
        // Do we need to stop?
        if (m_Stop)
        {
            m_RequestQueueMutex.release();
            return 0;
        }

        Request *pReq = getNextRequest();
        if (!pReq)
        {
            // Need to wait for another request.
            /// \todo should handle errors properly here
            m_RequestQueueCondition.wait(m_RequestQueueMutex);
            continue;
        }

        // We have a request! We don't need to use the queue anymore.
        m_RequestQueueMutex.release();

        // Verify that it's still valid to run the request
        if (!pReq->bReject)
        {
            // Perform the request.
            bool finished = true;
            pReq->ret = executeRequest(
                pReq->p1, pReq->p2, pReq->p3, pReq->p4, pReq->p5, pReq->p6,
                pReq->p7, pReq->p8);
            if (pReq->mutex.tryAcquire())
            {
                // Something's gone wrong - the calling thread has released the
                // Mutex. Destroy the request and grab the next request from the
                // queue. The calling thread has long since stopped caring about
                // whether we're done or not.
                NOTICE("RequestQueue::work - caller interrupted");
                if (pReq->pThread)
                    pReq->pThread->removeRequest(pReq);
                finished = false;

                m_RequestQueueMutex.acquire();
                continue;
            }
            switch (
                Processor::information().getCurrentThread()->getUnwindState())
            {
                case Thread::Continue:
                    break;
                case Thread::Exit:
                    WARNING("RequestQueue: unwind state is Exit, request not "
                            "cleaned up. Leak?");
                    return 0;
                case Thread::ReleaseBlockingThread:
                    Processor::information().getCurrentThread()->setUnwindState(
                        Thread::Continue);
                    break;
            }

            // Request finished - post the request's mutex to wake the calling
            // thread.
            if (finished)
            {
                pReq->bCompleted = true;
                pReq->mutex.release();
            }
        }

        // Acquire mutex ready to re-check condition.
        // We do this here as the head of the loop must have the lock (to allow
        // the condition variable to work with our lock correctly).
        m_RequestQueueMutex.acquire();
    }
#else
    return 0;
#endif
}

#ifdef THREADS
void RequestQueue::RequestQueueOverrunChecker::timer(
    uint64_t delta, InterruptState &)
{
    m_Tick += delta;
    if (delta < Time::Multiplier::Second)
    {
        return;
    }

    m_Tick -= Time::Multiplier::Second;

    queue->m_RequestQueueMutex.acquire();
    size_t lastSize = m_LastQueueSize;
    size_t currentSize = queue->m_nTotalRequests;
    m_LastQueueSize = currentSize;
    queue->m_RequestQueueMutex.release();

    if (lastSize < currentSize)
    {
        FATAL(
            "RequestQueue '"
            << queue->m_Name
            << "' is NOT keeping up with incoming requests [1s ago we had "
            << lastSize << " requests, now have " << currentSize << "]!");
    }
}
#endif

bool RequestQueue::isRequestValid(const Request *r)
{
#ifdef THREADS
    // Halted RequestQueue already has the RequestQueue mutex held.
    LockGuard<Mutex> guard(m_RequestQueueMutex);
#endif

    for (size_t priority = 0; priority < REQUEST_QUEUE_NUM_PRIORITIES - 1;
         ++priority)
    {
        Request *pReq = m_pRequestQueue[priority];
        while (pReq)
        {
            if (pReq == r)
                return true;

            pReq = pReq->next;
        }
    }

    return false;
}
