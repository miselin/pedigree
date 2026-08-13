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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/ProcessorThreadAllocator.h"
#include "pedigree/kernel/process/RoundRobinCoreAllocator.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

Scheduler Scheduler::m_Instance;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
Scheduler::GenericThreadStatusHook g_GenericThreadStatusHook = nullptr;
}
#endif

// Scheduler can be used at times where it is not yet safe to do the useful
// "safer" Spinlock deadlock detection.
#define SCHEDULER_HAS_SAFE_SPINLOCKS true

// Do we allow recursing in the Scheduler lock? Note that the lock surrounds
// memory operations (editing a List<T>), so if e.g. VirtualAddressSpace depends
// on Scheduler, you need to recurse.
#define SCHEDULER_HAS_RECURSIVE_SPINLOCKS true

Scheduler::ProcessLease::ProcessLease() : m_pProcess(nullptr), m_TerminationDeferral(false) {}

Scheduler::ProcessLease::ProcessLease(Process* process)
    : m_pProcess(process), m_TerminationDeferral(process != nullptr) {}

Scheduler::ProcessLease::ProcessLease(ProcessLease&& other)
    : m_pProcess(other.m_pProcess),
      m_TerminationDeferral(pedigree_std::move(other.m_TerminationDeferral)) {
  other.m_pProcess = nullptr;
}

Scheduler::ProcessLease::~ProcessLease() {
  reset();
}

Scheduler::ProcessLease& Scheduler::ProcessLease::operator=(ProcessLease&& other) {
  if (this != &other) {
    if (other.m_pProcess) {
      m_TerminationDeferral = pedigree_std::move(other.m_TerminationDeferral);

      Process* previous = m_pProcess;
      m_pProcess = other.m_pProcess;
      other.m_pProcess = nullptr;
      if (previous) {
        Scheduler::instance().releaseProcessLease(previous);
      }
    } else {
      reset();
    }
  }
  return *this;
}

void Scheduler::ProcessLease::reset() {
  Process* process = m_pProcess;
  m_pProcess = nullptr;
  if (process) {
    Scheduler::instance().releaseProcessLease(process);
  }
  m_TerminationDeferral = TerminationDeferral(false);
}

Scheduler::Scheduler()
    : m_Processes(),
      m_NextPid(0),
      m_PTMap(),
      m_TPMap(),
      m_pKernelProcess(0),
      m_pBspScheduler(0),
      m_SchedulerLock(false),
      m_ProcessRemovalWaiters() {}

bool Scheduler::initialise(Process* pKernelProcess) {
  RoundRobinCoreAllocator* pRoundRobin = new RoundRobinCoreAllocator();
  ProcessorThreadAllocator::instance().setAlgorithm(pRoundRobin);

  m_pKernelProcess = pKernelProcess;

  List<PerProcessorScheduler*> procList;

  m_pBspScheduler = &Processor::information().getScheduler();
  procList.pushBack(m_pBspScheduler);

  size_t i = 0;
  for (Vector<ProcessorInformation*>::Iterator it = Processor::m_ProcessorInformation.begin();
       it != Processor::m_ProcessorInformation.end(); it++, i += 2) {
    auto thisScheduler = &((*it)->getScheduler());
    if (thisScheduler != m_pBspScheduler) {
      procList.pushBack(thisScheduler);
    }
  }

  pRoundRobin->initialise(procList);

  return true;
}

void Scheduler::addThread(Thread* pThread, PerProcessorScheduler& PPSched) {
  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  m_TPMap.insert(pThread, &PPSched);
  m_SchedulerLock.release();
}

void Scheduler::removeThread(Thread* pThread) {
  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  PerProcessorScheduler* pPpSched = m_TPMap.lookup(pThread);
  if (pPpSched) {
    pPpSched->removeThread(pThread);
    m_TPMap.remove(pThread);
  }
  m_SchedulerLock.release();
}

bool Scheduler::threadInSchedule(Thread* pThread) {
  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  PerProcessorScheduler* pPpSched = m_TPMap.lookup(pThread);
  m_SchedulerLock.release();
  return pPpSched != 0;
}

size_t Scheduler::reserveProcessId() {
  return (m_NextPid += 1) - 1;  // little dance for Atomic
}

void Scheduler::releaseProcessLease(Process* process) {
  process->endExternalLease();
}

void Scheduler::drainDeferredTimeAccounting() {
  // Reserve outside m_SchedulerLock. A worker generation is snapshotted
  // before this call, so a Process added after the count can only publish a
  // later generation and will be covered by the next pass.
  Vector<Process*> processes(getNumProcesses());

  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  for (List<Process*>::Iterator it = m_Processes.begin();
       it != m_Processes.end() && processes.count() < processes.size(); ++it) {
    Process* process = *it;
    if (process->beginExternalLease()) {
      processes.pushBack(process);
    }
  }
  m_SchedulerLock.release();

  for (Vector<Process*>::Iterator it = processes.begin(); it != processes.end(); ++it) {
    Process* process = *it;
    process->drainDeferredTimeAccounting();
    process->endExternalLease();
  }
}

void Scheduler::addProcess(Process* pProcess) {
  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  m_Processes.pushBack(pProcess);
  m_SchedulerLock.release();
}

void Scheduler::removeProcess(Process* pProcess) {
  bool removed = false;
  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  pProcess->closeExternalLeaseAdmission();
  for (List<Process*>::Iterator it = m_Processes.begin(); it != m_Processes.end(); it++) {
    if (*it == pProcess) {
      m_Processes.erase(it);
      removed = true;
      break;
    }
  }
  m_SchedulerLock.release();
  if (removed) {
    m_ProcessRemovalWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(pProcess));
  }
}

void Scheduler::yield() {
  Processor::information().getScheduler().schedule();
}

size_t Scheduler::getNumProcesses() {
  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  size_t result = m_Processes.count();
  m_SchedulerLock.release();
  return result;
}

bool Scheduler::acquireProcess(ProcessLease& lease, size_t n) {
  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  if (n >= m_Processes.count()) {
    m_SchedulerLock.release();
    lease.reset();
    return false;
  }

  size_t i = 0;
  Process* pResult = 0;
  for (List<Process*>::Iterator it = m_Processes.begin(); it != m_Processes.end(); it++) {
    if (i == n) {
      pResult = *it;
      break;
    }
    i++;
  }

  if (pResult && !pResult->beginExternalLease()) {
    pResult = nullptr;
  }
  m_SchedulerLock.release();
  lease = ProcessLease(pResult);
  return pResult != nullptr;
}

bool Scheduler::acquireFirstProcessOfType(ProcessLease& lease, Process::ProcessType type) {
  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  Process* pResult = nullptr;
  for (List<Process*>::Iterator it = m_Processes.begin(); it != m_Processes.end(); ++it) {
    Process* candidate = *it;
    if (candidate->getType() == type && candidate->beginExternalLease()) {
      pResult = candidate;
      break;
    }
  }
  m_SchedulerLock.release();

  lease = ProcessLease(pResult);
  return pResult != nullptr;
}

bool Scheduler::acquireProcess(ProcessLease& lease, Process* expected) {
  if (!expected) {
    lease.reset();
    return false;
  }

  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  Process* pResult = nullptr;
  for (List<Process*>::Iterator it = m_Processes.begin(); it != m_Processes.end(); ++it) {
    if (*it == expected) {
      pResult = *it;
      break;
    }
  }

  if (pResult && !pResult->beginExternalLease()) {
    pResult = nullptr;
  }
  m_SchedulerLock.release();
  lease = ProcessLease(pResult);
  return pResult != nullptr;
}

bool Scheduler::acquireProcessById(ProcessLease& lease, size_t id) {
  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  Process* pResult = nullptr;
  for (List<Process*>::Iterator it = m_Processes.begin(); it != m_Processes.end(); ++it) {
    Process* candidate = *it;
    if (candidate->getId() == id) {
      pResult = candidate;
      break;
    }
  }

  if (pResult && !pResult->beginExternalLease()) {
    pResult = nullptr;
  }
  m_SchedulerLock.release();
  lease = ProcessLease(pResult);
  return pResult != nullptr;
}

void Scheduler::waitUntilProcessRemoved(Process* expected) {
  TerminationDeferral terminationDeferral;
  while (true) {
    auto guard = m_ProcessRemovalWaiters.acquire();
    bool present = false;
    m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
    for (List<Process*>::Iterator it = m_Processes.begin(); it != m_Processes.end(); ++it) {
      if (*it == expected) {
        present = true;
        break;
      }
    }
    m_SchedulerLock.release();
    if (!present) {
      return;
    }

    const WaitQueue::WakeReason reason = guard.waitForCompletion(
        WaitQueue::Channel(expected), Thread::ProcessWait, reinterpret_cast<uintptr_t>(expected));
    (void)reason;
  }
}

Process* Scheduler::getChildProcess(Process* pParent, size_t n) {
  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  size_t childIndex = 0;
  Process* pResult = 0;
  for (List<Process*>::Iterator it = m_Processes.begin(); it != m_Processes.end(); ++it) {
    Process* pProcess = *it;
    if (pProcess && pProcess->getParent() == pParent) {
      if (childIndex == n) {
        pResult = pProcess;
        break;
      }
      ++childIndex;
    }
  }
  m_SchedulerLock.release();
  return pResult;
}

void Scheduler::threadStatusChanged(Thread* pThread) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  GenericThreadStatusHook hook = __atomic_load_n(&g_GenericThreadStatusHook, __ATOMIC_ACQUIRE);
  if (hook) {
    hook(pThread);
  }
#endif
  m_SchedulerLock.acquire(SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
  PerProcessorScheduler* pSched = m_TPMap.lookup(pThread);
  assert(pSched);
  m_SchedulerLock.release();

  pSched->threadStatusChanged(pThread);
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Scheduler::setGenericThreadStatusHook(GenericThreadStatusHook hook) {
  __atomic_store_n(&g_GenericThreadStatusHook, hook, __ATOMIC_RELEASE);
}
#endif

#endif
