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

#if THREADS
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/RoundRobin.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/assert.h"

RoundRobin::RoundRobin() : m_Lock(false) {
  for (size_t i = 0; i < MAX_PRIORITIES; ++i) {
    m_pReadyQueueHeads[i] = nullptr;
    m_pReadyQueueTails[i] = nullptr;
  }
}

RoundRobin::~RoundRobin() {
  for (size_t i = 0; i < MAX_PRIORITIES; ++i) {
    while (m_pReadyQueueHeads[i]) {
      unlink(m_pReadyQueueHeads[i]);
    }
    assert(!m_pReadyQueueTails[i]);
  }
}

void RoundRobin::addThread(Thread* pThread) {}

void RoundRobin::removeThread(Thread* pThread) {
  LockGuard<Spinlock> guard(m_Lock);
  unlink(pThread);
}

void RoundRobin::enqueue(Thread* pThread) {
  assert(pThread);
  assert(!pThread->m_bReadyQueued);
  assert(!pThread->m_pReadyPrevious);
  assert(!pThread->m_pReadyNext);
  assert(pThread->getPriority() < MAX_PRIORITIES);

  const size_t priority = pThread->getPriority();
  pThread->m_pReadyPrevious = m_pReadyQueueTails[priority];
  pThread->m_ReadyQueuePriority = priority;
  pThread->m_bReadyQueued = true;
  if (m_pReadyQueueTails[priority]) {
    m_pReadyQueueTails[priority]->m_pReadyNext = pThread;
  } else {
    m_pReadyQueueHeads[priority] = pThread;
  }
  m_pReadyQueueTails[priority] = pThread;
}

void RoundRobin::unlink(Thread* pThread) {
  if (!pThread || !pThread->m_bReadyQueued) {
    return;
  }

  const size_t priority = pThread->m_ReadyQueuePriority;
  assert(priority < MAX_PRIORITIES);
  if (pThread->m_pReadyPrevious) {
    pThread->m_pReadyPrevious->m_pReadyNext = pThread->m_pReadyNext;
  } else {
    assert(m_pReadyQueueHeads[priority] == pThread);
    m_pReadyQueueHeads[priority] = pThread->m_pReadyNext;
  }
  if (pThread->m_pReadyNext) {
    pThread->m_pReadyNext->m_pReadyPrevious = pThread->m_pReadyPrevious;
  } else {
    assert(m_pReadyQueueTails[priority] == pThread);
    m_pReadyQueueTails[priority] = pThread->m_pReadyPrevious;
  }

  pThread->m_pReadyPrevious = nullptr;
  pThread->m_pReadyNext = nullptr;
  pThread->m_ReadyQueuePriority = MAX_PRIORITIES;
  pThread->m_bReadyQueued = false;
}

Thread* RoundRobin::getNext(Thread* pCurrentThread) {
  LockGuard<Spinlock> guard(m_Lock);

  Thread* pThread = 0;
  for (size_t i = 0; i < MAX_PRIORITIES; i++) {
    // Bound the scan so a stale entry whose priority changes cannot be
    // requeued forever in the same selection pass.
    size_t candidates = 0;
    for (pThread = m_pReadyQueueHeads[i]; pThread; pThread = pThread->m_pReadyNext) {
      ++candidates;
    }
    while (candidates--) {
      pThread = m_pReadyQueueHeads[i];
      assert(pThread);
      unlink(pThread);

      if (pThread == pCurrentThread || !isReady(pThread)) {
        continue;
      }

      if (pThread->getPriority() != i) {
        enqueue(pThread);
        continue;
      }

      if (!isEligible(pThread)) {
        // Predicate-backed workers stay published without making the
        // hard producer touch this queue when work arrives.
        enqueue(pThread);
        continue;
      }

      return pThread;
    }
  }
  return 0;
}

void RoundRobin::threadStatusChanged(Thread* pThread) {
  LockGuard<Spinlock> guard(m_Lock);

  if (pThread->m_bReadyQueued) {
    if (!RoundRobin::isReady(pThread) || pThread->m_ReadyQueuePriority != pThread->getPriority()) {
      unlink(pThread);
    } else {
      return;
    }
  }

  if (RoundRobin::isReady(pThread)) {
    enqueue(pThread);
  }
}

bool RoundRobin::isReady(Thread* pThread) {
  return pThread->getStatus() == Thread::Ready;
}

bool RoundRobin::isEligible(Thread* pThread) {
  return isReady(pThread) && (!pThread->m_SchedulerReadyPredicate ||
                              pThread->m_SchedulerReadyPredicate(pThread->m_SchedulerReadyContext));
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
bool g_HostedSchedulerPredicateReady = false;

bool hostedSchedulerPredicate(void*) {
  return g_HostedSchedulerPredicateReady;
}
}  // namespace

bool RoundRobin::runHostedIntrusiveQueueRegressions(Thread* pThread) {
  if (!pThread || pThread->m_bReadyQueued || pThread->m_pReadyPrevious || pThread->m_pReadyNext) {
    return false;
  }

  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  const Thread::Status status = pThread->m_Status;
  const size_t priority = pThread->m_Priority;
  const Thread::SchedulerReadyPredicate predicate = pThread->m_SchedulerReadyPredicate;
  void* predicateContext = pThread->m_SchedulerReadyContext;
  bool passed = true;

  {
    RoundRobin queue;
    pThread->m_Status = Thread::Ready;
    pThread->m_Priority = 1;
    queue.threadStatusChanged(pThread);
    queue.threadStatusChanged(pThread);
    passed &= pThread->m_bReadyQueued && pThread->m_ReadyQueuePriority == 1 &&
              !pThread->m_pReadyPrevious && !pThread->m_pReadyNext;

    pThread->m_Priority = 2;
    queue.threadStatusChanged(pThread);
    passed &= pThread->m_bReadyQueued && pThread->m_ReadyQueuePriority == 2 &&
              !pThread->m_pReadyPrevious && !pThread->m_pReadyNext;

    pThread->m_Status = Thread::AwaitingJoin;
    passed &= !queue.getNext(nullptr) && !pThread->m_bReadyQueued;

    pThread->m_Status = Thread::Ready;
    queue.threadStatusChanged(pThread);
    passed &= !queue.getNext(pThread) && !pThread->m_bReadyQueued;

    pThread->m_Status = Thread::Ready;
    queue.threadStatusChanged(pThread);
    passed &= pThread->m_bReadyQueued;
  }

  passed &= !pThread->m_bReadyQueued && !pThread->m_pReadyPrevious && !pThread->m_pReadyNext &&
            pThread->m_ReadyQueuePriority == MAX_PRIORITIES;
  {
    RoundRobin reused;
    pThread->m_Status = Thread::Ready;
    pThread->m_Priority = 0;
    reused.threadStatusChanged(pThread);
    passed &= reused.getNext(nullptr) == pThread && !pThread->m_bReadyQueued;
  }

  {
    RoundRobin predicateQueue;
    g_HostedSchedulerPredicateReady = false;
    pThread->m_Status = Thread::Ready;
    pThread->m_Priority = 0;
    pThread->m_SchedulerReadyPredicate = hostedSchedulerPredicate;
    pThread->m_SchedulerReadyContext = nullptr;
    predicateQueue.threadStatusChanged(pThread);
    passed &= !predicateQueue.getNext(nullptr) && pThread->m_bReadyQueued;
    pThread->m_Status = Thread::AwaitingJoin;
    predicateQueue.threadStatusChanged(pThread);
    passed &= !pThread->m_bReadyQueued && !pThread->m_pReadyPrevious && !pThread->m_pReadyNext;
    pThread->m_Status = Thread::Ready;
    predicateQueue.threadStatusChanged(pThread);
    g_HostedSchedulerPredicateReady = true;
    passed &= predicateQueue.getNext(nullptr) == pThread && !pThread->m_bReadyQueued;
  }

  pThread->m_Status = status;
  pThread->m_Priority = priority;
  pThread->m_SchedulerReadyPredicate = predicate;
  pThread->m_SchedulerReadyContext = predicateContext;
  Processor::setInterrupts(interrupts);
  return passed;
}
#endif

#endif
