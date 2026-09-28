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

#include <config.h>

#if THREADS
#include "pedigree/kernel/ActivityDiagnostics.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/RoundRobin.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/assert.h"

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
#include "pedigree/kernel/process/OwnedThread.h"
#endif

RoundRobin::RoundRobin() : m_Lock(false) {
  for (size_t i = 0; i < MAX_PRIORITIES; ++i) {
    m_pReadyQueueHeads[i] = nullptr;
    m_pReadyQueueTails[i] = nullptr;
    m_ReadyQueueAges[i] = 0;
#if PEDIGREE_READY_QUEUE_COUNTS
    m_ReadyQueueCounts[i] = 0;
#endif
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
  assert(isReady(pThread));
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
#if PEDIGREE_READY_QUEUE_COUNTS
  ++m_ReadyQueueCounts[priority];
#endif
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
#if PEDIGREE_READY_QUEUE_COUNTS
  assert(m_ReadyQueueCounts[priority]);
  --m_ReadyQueueCounts[priority];
#endif
}

Thread* RoundRobin::getNext(Thread* pCurrentThread, bool currentRunnable) {
  ActivityDiagnostics::ReadyQueueSelectionScope selectionScope;
  LockGuard<Spinlock> guard(m_Lock);

  for (size_t i = 0; i < MAX_PRIORITIES; ++i) {
    Thread* last = m_pReadyQueueTails[i];
    while (Thread* pThread = m_pReadyQueueHeads[i]) {
      ActivityDiagnostics::recordReadyQueueCandidateVisit();
      if (pThread == pCurrentThread || !isReady(pThread)) {
        unlink(pThread);
      } else if (pThread->getPriority() != i) {
        unlink(pThread);
        enqueue(pThread);
      } else {
        break;
      }
      // Do not revisit entries requeued during a concurrent priority change.
      if (pThread == last) {
        break;
      }
    }
  }

  const size_t currentPriority =
      currentRunnable && pCurrentThread ? pCurrentThread->getPriority() : MAX_PRIORITIES;
  assert(!currentRunnable || !pCurrentThread || currentPriority < MAX_PRIORITIES);
  size_t selected = MAX_PRIORITIES;
  size_t oldest = MAX_PRIORITIES;
  for (size_t i = 0; i < MAX_PRIORITIES; ++i) {
    if (!m_pReadyQueueHeads[i] && i != currentPriority) {
      m_ReadyQueueAges[i] = 0;
      continue;
    }
    if (selected == MAX_PRIORITIES) {
      selected = i;
    }
    if (m_ReadyQueueAges[i] >= AgingSelections &&
        (oldest == MAX_PRIORITIES || m_ReadyQueueAges[i] > m_ReadyQueueAges[oldest])) {
      oldest = i;
    }
  }

  // Maintenance must still run behind a continuously runnable application.
  // Aging by selections bounds starvation without another periodic timer.
  if (oldest != MAX_PRIORITIES) {
    selected = oldest;
  }
  for (size_t i = 0; i < MAX_PRIORITIES; ++i) {
    if (i == selected) {
      m_ReadyQueueAges[i] = 0;
    } else if ((m_pReadyQueueHeads[i] || i == currentPriority) &&
               m_ReadyQueueAges[i] < AgingSelections + MAX_PRIORITIES) {
      ++m_ReadyQueueAges[i];
    }
  }

  if (selected != MAX_PRIORITIES) {
    if (Thread* pThread = m_pReadyQueueHeads[selected]) {
      unlink(pThread);
      return pThread;
    }
    return pCurrentThread;
  }
  ActivityDiagnostics::recordSchedulerNoEligibleSelection();
  return 0;
}

bool RoundRobin::hasReady() {
  LockGuard<Spinlock> guard(m_Lock);
  for (size_t i = 0; i < MAX_PRIORITIES; ++i) {
    if (m_pReadyQueueHeads[i]) {
      return true;
    }
  }
  return false;
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
  return pThread->getStatus() == Thread::Ready &&
         !__atomic_load_n(&pThread->m_ReadyPublicationPending, __ATOMIC_ACQUIRE);
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
bool RoundRobin::runHostedIntrusiveQueueRegressions(Thread* pThread) {
  if (!pThread || pThread->m_bReadyQueued || pThread->m_pReadyPrevious || pThread->m_pReadyNext) {
    return false;
  }

  auto entry = [](void*) { return 0; };
  OwnedThread first(new Thread(pThread->getParent(), entry, nullptr, nullptr, false, true, true));
  OwnedThread second(new Thread(pThread->getParent(), entry, nullptr, nullptr, false, true, true));
  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  first->m_Lock.acquire();
  second->m_Lock.acquire();
  const Thread::Status status = pThread->m_Status;
  const size_t priority = pThread->m_Priority;
  const Thread::Status firstStatus = first->m_Status;
  const Thread::Status secondStatus = second->m_Status;
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

  pThread->m_Status = Thread::Running;
  pThread->m_Priority = DEFAULT_PRIORITY;
  first->m_Status = Thread::Ready;
  second->m_Status = Thread::Ready;
  {
    RoundRobin queue;
    first->m_Priority = MAINTENANCE_PRIORITY;
    queue.threadStatusChanged(first.get());
    for (size_t i = 0; i < AgingSelections; ++i) {
      passed &= queue.getNext(pThread, true) == pThread;
    }
    passed &= queue.getNext(pThread, true) == first.get();

    // A blocked current thread cannot suppress maintenance work.
    queue.threadStatusChanged(first.get());
    passed &= queue.getNext(pThread, false) == first.get();

    first->m_Priority = DEFAULT_PRIORITY;
    second->m_Priority = DEFAULT_PRIORITY;
    queue.threadStatusChanged(first.get());
    queue.threadStatusChanged(second.get());
    passed &= queue.getNext(pThread, true) == first.get();
    passed &= queue.getNext(pThread, true) == second.get();
    passed &= queue.getNext(pThread, true) == pThread;

    queue.threadStatusChanged(first.get());
    first->m_Priority = 0;
    passed &= queue.getNext(pThread, true) == first.get();
  }
  {
    RoundRobin queue;
    pThread->m_Priority = 0;
    first->m_Priority = DEFAULT_PRIORITY;
    second->m_Priority = MAX_PRIORITIES - 1;
    queue.threadStatusChanged(first.get());
    queue.threadStatusChanged(second.get());
    size_t skipped[3] = {};
    size_t served[3] = {};
    for (size_t i = 0; i < 4 * (AgingSelections + MAX_PRIORITIES); ++i) {
      Thread* selected = queue.getNext(pThread, true);
      const size_t index = selected == pThread ? 0 : selected == first.get() ? 1 : 2;
      passed &= selected == pThread || selected == first.get() || selected == second.get();
      ++served[index];
      for (size_t j = 0; j < 3; ++j) {
        skipped[j] = j == index ? 0 : skipped[j] + 1;
        passed &= skipped[j] < AgingSelections + MAX_PRIORITIES;
      }
      if (selected && selected != pThread) {
        queue.threadStatusChanged(selected);
      }
    }
    passed &= served[0] > served[1] && served[1] && served[2];
  }

  first->m_Status = firstStatus;
  second->m_Status = secondStatus;
  second->m_Lock.release();
  first->m_Lock.release();
  pThread->m_Status = status;
  pThread->m_Priority = priority;
  Processor::setInterrupts(interrupts);
  return passed;
}
#endif

#endif
