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

#include <utilities/RequestQueue.h>
#include <processor/Processor.h>
#include <process/Scheduler.h>
#include <panic.h>
#include <Log.h>

#include <utilities/assert.h>

RequestQueue::RequestQueue() :
  m_RequestQueue(), m_Stop(false), m_RequestQueueMutex(false)
#ifdef THREADS
  , m_pThread(0)
#endif
  , m_Halted(false)
{
}

RequestQueue::~RequestQueue()
{
  destroy();
}

void RequestQueue::initialise()
{
  // Start the worker thread.
#ifdef THREADS
  if(m_pThread)
  {
    WARNING("RequestQueue initialised multiple times - don't do this.");
    return;
  }

  // Start RequestQueue workers in the kernel process only.
  Process *pProcess = Scheduler::instance().getKernelProcess();

  m_Stop = false;
  m_pThread = new Thread(pProcess, &trampoline, reinterpret_cast<void*>(this));
  m_Halted = false;
#else
  WARNING("RequestQueue: This build does not support threads");
#endif
}

void RequestQueue::destroy()
{
#ifdef THREADS
  // Halt the queue - we're done.
  halt();

  /// \todo We really should clean up the queue.
#endif
}

uint64_t RequestQueue::addRequest(size_t priority, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
                                  uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8)
{
#ifdef THREADS
  Thread *pThread = Processor::information().getCurrentThread();

  // Create a new request object.
  SharedPointer<Request> pReq(new Request);
  pReq->p1 = p1; pReq->p2 = p2; pReq->p3 = p3; pReq->p4 = p4; pReq->p5 = p5; pReq->p6 = p6; pReq->p7 = p7; pReq->p8 = p8;
  pReq->owner = this;
  pReq->priority = priority;

  // Do we own pReq?
  bool bOwnRequest = true;

  bool bSharedRequest = false;

  // Add to the request queue.
  m_RequestQueueMutex.acquire();

  for (auto it : m_RequestQueue[priority])
  {
    if (compareRequests(*(it.get()), *pReq))
    {
      // We're going to share.
      // Note: this will destroy the memory pointed to by pReq and add another
      // reference to the other Request object.
      bSharedRequest = true;
      pReq = it;
      break;
    }
  }

  if (!bSharedRequest)
  {
    m_RequestQueue[priority].pushBack(pReq);

    pReq->pThread = pThread;
    pReq->pThread->addRequest(pReq);
  }

  // One more item now available.
  m_RequestQueueCondition.signal();
  m_RequestQueueMutex.release();

  // We are waiting on the worker thread - mark the thread as such.
  pThread->setBlockingThread(m_pThread);

  // Mutual exclusion while we check the status of the request.
  pReq->mutex.acquire();
  if (pReq->isRejected())
  {
    // Something else marked this finished while we were acquiring, so we
    // should abort the request.
    return 0;
  }

  while (!pReq->hasFinished())
  {
    while (!pReq->condition.wait(pReq->mutex))
      ;
  }

  // No longer waiting for the request to complete.
  pThread->setBlockingThread(0);

  uintptr_t ret = pReq->ret;

  // Delete the request structure.
  if(pReq->pThread && !bSharedRequest)
  {
    Thread *pRemovalThread = pReq->pThread;
    pReq->pThread = 0;
    pRemovalThread->removeRequest(pReq);
  }
  pReq->mutex.release();

  return ret;
#else
  return executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
#endif
}

int RequestQueue::doAsync(void *p)
{
  RequestQueue::Request *pReq = reinterpret_cast<RequestQueue::Request *>(p);
  // Sync point with addAsyncRequest.
  pReq->mutex.acquire();
  pReq->mutex.release();

  // Perform the request proper.
  pReq->owner->addRequest(pReq->priority, pReq->p1, pReq->p2, pReq->p3,
                          pReq->p4, pReq->p5, pReq->p6, pReq->p7, pReq->p8);
  delete pReq;
  return 0;
}

uint64_t RequestQueue::addAsyncRequest(size_t priority, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
                                       uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8)
{
#ifndef THREADS
  return addRequest(priority, p1, p2, p3, p4, p5, p6, p7, p8);
#else
  // Create a new request object.
  Request *pReq = new Request();
  pReq->p1 = p1; pReq->p2 = p2; pReq->p3 = p3; pReq->p4 = p4; pReq->p5 = p5; pReq->p6 = p6; pReq->p7 = p7; pReq->p8 = p8;
  pReq->owner = this;
  pReq->priority = priority;

  // Block the new thread until we've fully detached.
  pReq->mutex.acquire();

  // Add to RequestQueue.
  Process *pProcess = Scheduler::instance().getKernelProcess();
  Thread *pThread = new Thread(pProcess, &doAsync,
      reinterpret_cast<void *>(pReq));
  pThread->detach();

  // Now OK for the thread to start working.
  pReq->mutex.release();
#endif

  return 0;
}

void RequestQueue::halt()
{
#ifdef THREADS
  m_RequestQueueMutex.acquire();
  if(!m_Halted)
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

  if(m_Halted)
  {
    initialise();
  }
#endif
}

int RequestQueue::trampoline(void *p)
{
  RequestQueue *pRQ = reinterpret_cast<RequestQueue*> (p);
  return pRQ->work();
}

SharedPointer<RequestQueue::Request> RequestQueue::getNextRequest()
{
    // Must have the lock to be here.
    assert(!m_RequestQueueMutex.getValue());

    for (size_t priority = 0; priority < REQUEST_QUEUE_NUM_PRIORITIES; ++priority)
    {
        if (m_RequestQueue[priority].count())
        {
            return m_RequestQueue[priority].popFront();
        }
    }

    return SharedPointer<RequestQueue::Request>();
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

    SharedPointer<Request> pReq = getNextRequest();
    if (!pReq)
    {
      // Need to wait for another request.
      while (!m_RequestQueueCondition.wait(m_RequestQueueMutex))
        ;
      continue;
    }

    // We have a request! We don't need to use the queue anymore.
    m_RequestQueueMutex.release();

    // Verify that it's still valid to run the request
    pReq->mutex.acquire();
    if (!pReq->hasFinished())
    {
      // Perform the request.
      pReq->status = Request::Active;

      // Avoid using pReq until we can get the mutex again. Don't hold the
      // mutex during the actual request execution.
      uint64_t p1 = pReq->p1;
      uint64_t p2 = pReq->p2;
      uint64_t p3 = pReq->p3;
      uint64_t p4 = pReq->p4;
      uint64_t p5 = pReq->p5;
      uint64_t p6 = pReq->p6;
      uint64_t p7 = pReq->p7;
      uint64_t p8 = pReq->p8;
      pReq->mutex.release();

      uint64_t result = executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);

      switch (Processor::information().getCurrentThread()->getUnwindState())
      {
        case Thread::Continue:
          break;
        case Thread::Exit:
          WARNING("RequestQueue: unwind state is Exit, request not cleaned up. Leak?");
          return 0;
        case Thread::ReleaseBlockingThread:
          Processor::information().getCurrentThread()->setUnwindState(Thread::Continue);
          break;
      }

      // We want ALL threads waiting to be woken to see that this request is
      // now complete.
      pReq->mutex.acquire();
      pReq->ret = result;
      pReq->status = Request::Completed;
      pReq->condition.broadcast();
    }
    pReq->mutex.release();

    // Acquire mutex ready to re-check condition.
    // We do this here as the head of the loop must have the lock (to allow the
    // condition variable to work with our lock correctly).
    m_RequestQueueMutex.acquire();
  }
#else
  return 0;
#endif
}

RequestQueue::Request::Request() : p1(0), p2(0), p3(0), p4(0), p5(0), p6(0), p7(0), p8(0), ret(0),
#ifdef THREADS
                    mutex(false), condition(), pThread(0),
#endif
                    status(Pending), owner(0), priority(0)
{
}

RequestQueue::Request::~Request()
{
}
