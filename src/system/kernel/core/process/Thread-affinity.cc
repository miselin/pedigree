/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <config.h>

#if THREADS

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/SchedulingAlgorithm.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#if X86_COMMON && MULTIPROCESSOR
#include <machine/mach_pc/LocalApic.h>
#include <machine/mach_pc/Pc.h>
#endif

#if PEDIGREE_AFFINITY_TESTS
namespace {
Thread* g_AffinityCommitTarget = nullptr;
Thread::AffinityCommitHook g_AffinityCommitHook = nullptr;
}  // namespace

void Thread::setAffinityCommitHookForTest(Thread* target, AffinityCommitHook hook) {
  __atomic_store_n(&g_AffinityCommitTarget, target, __ATOMIC_RELEASE);
  __atomic_store_n(&g_AffinityCommitHook, hook, __ATOMIC_RELEASE);
}
#endif

ThreadPlacement ThreadPlacement::initialUser() {
  ThreadPlacement placement;
  placement.allowed = Scheduler::onlineAffinity();
  placement.migratable = true;
  return placement;
}

ThreadPlacement ThreadPlacement::inherit(Thread& creator) {
  ThreadPlacement placement;
  creator.snapshotPlacement(placement);
  return placement.migratable ? placement : initialUser();
}

void Thread::initialisePlacement(const ThreadPlacement* placement) {
  if (placement) {
    m_Placement = *placement;
    m_Placement.allowed.intersect(Scheduler::onlineAffinity());
    if (m_Placement.allowed.empty())
      FATAL("Thread construction has no allowed online processor.");
  }
}

void Thread::snapshotPlacement(ThreadPlacement& placement) {
  LockGuard<Spinlock> guard(m_Lock);
  snapshotPlacementLocked(placement);
}

void Thread::snapshotPlacementLocked(ThreadPlacement& placement) const {
  placement = m_Placement;
}

bool Thread::tryPinLegacyUserCallbacks() {
  LockGuard<Spinlock> guard(m_Lock);
  PerProcessorScheduler* owner = getScheduler();
  if (!owner || m_bShutdown || m_SignalFramesRequired || getUnwindState() != Continue ||
      m_LegacyUserCallbackPins == ~size_t(0) ||
      !m_Placement.allowed.contains(owner->logicalCpu()) ||
      (m_AffinityPending && !m_RequestedAffinity.contains(owner->logicalCpu())))
    return false;
  ++m_LegacyUserCallbackPins;
  return true;
}

void Thread::unpinLegacyUserCallbacks() {
  LockGuard<Spinlock> guard(m_Lock);
  assert(m_LegacyUserCallbackPins);
  --m_LegacyUserCallbackPins;
}

AffinityResult Thread::requestAffinity(const CpuAffinityMask& requested, uint64_t& generation) {
  CpuAffinityMask effective = requested;
  effective.intersect(Scheduler::onlineAffinity());
  generation = 0;
  if (effective.empty())
    return AffinityResult::Invalid;

  PerProcessorScheduler* owner = nullptr;
  Process* parent = m_pParent;
  bool rejected = false;
  bool parentPinned = false;
  bool threadPinned = false;
  {
    auto progress = m_AffinityWaiters.acquire();
    LockGuard<Spinlock> guard(m_Lock);
    if (m_bShutdown || getUnwindState() != Continue || m_Status == AwaitingJoin ||
        m_Status == Zombie)
      return AffinityResult::Terminal;
    if (!m_Placement.migratable)
      return AffinityResult::Pinned;
    if (m_AffinityPending) {
      generation = m_AffinityGeneration;
      return AffinityResult::Busy;
    }
    owner = getScheduler();
    assert(owner);
    if (m_LegacyUserCallbackPins && !effective.contains(owner->logicalCpu()))
      return AffinityResult::Unsupported;
    if (m_AffinityGeneration == ~uint64_t(0))
      return AffinityResult::Invalid;

    if (!m_AffinityWorkQueued) {
      if (!parent->beginExternalLease())
        return AffinityResult::Terminal;
      parentPinned = true;
      threadPinned = beginExternalLease();
      rejected = !threadPinned;
    }
    if (!rejected) {
      generation = ++m_AffinityGeneration;
      m_RequestedAffinity = effective;
      m_AffinityPending = true;
      if (!m_AffinityWorkQueued) {
        m_AffinityWorkQueued = true;
        if (!owner->enqueueAffinity(this)) {
          m_AffinityWorkQueued = false;
          m_AffinityPending = false;
          generation = 0;
          rejected = true;
        }
      }
    }
  }
  if (rejected) {
    if (threadPinned)
      endExternalLease();
    if (parentPinned)
      parent->endExternalLease();
    return AffinityResult::Terminal;
  }
  owner->prompt();
  return AffinityResult::Success;
}

AffinityResult Thread::waitAffinity(uint64_t generation) {
  if (!generation)
    return AffinityResult::Invalid;
  TerminationDeferral lifetime;
  while (true) {
    auto progress = m_AffinityWaiters.acquire();
    {
      LockGuard<Spinlock> guard(m_Lock);
      if (generation > m_AffinityGeneration)
        return AffinityResult::Invalid;
      if (generation <= m_AffinityCompleted)
        return AffinityResult::Success;
      if (!m_AffinityPending)
        return AffinityResult::Terminal;
    }
    const auto reason = progress.waitForCompletion(WaitQueue::Channel(this), Thread::ProcessWait,
                                                   reinterpret_cast<uintptr_t>(this));
    (void)reason;
  }
}

AffinityResult Thread::completeAffinityAtSafePoint(bool* waited) {
  Processor::setInterrupts(false);
  if (waited)
    *waited = false;
  if (Processor::information().getCurrentThread() != this)
    FATAL("Affinity gate entered for a non-current Thread.");

  // Only this audited wait continuation may move. Suppress callbacks across
  // its entire lifetime, including preemption before waiter publication.
  Uninterruptible gateScope;
  while (true) {
    bool rejected = false;
    bool parentPinned = false;
    bool threadPinned = false;
    bool finished = false;
    AffinityResult result = AffinityResult::Success;
    Process* parent = m_pParent;
    PerProcessorScheduler* owner = nullptr;
    {
      auto progress = m_AffinityWaiters.acquire();
      {
        LockGuard<Spinlock> guard(m_Lock);
        if (!m_AffinityGatePending) {
          owner = getScheduler();
          assert(owner);
          if (m_bShutdown || getUnwindState() != Continue) {
            result = AffinityResult::Terminal;
            finished = true;
          } else if (m_Placement.allowed.contains(owner->logicalCpu())) {
            finished = true;
          }
          if (!finished)
            assert(!m_LegacyUserCallbackPins);
          if (!finished && !m_AffinityWorkQueued) {
            parentPinned = parent->beginExternalLease();
            if (parentPinned)
              threadPinned = beginExternalLease();
            rejected = !threadPinned;
          }
          if (!finished && !rejected) {
            m_AffinityGatePending = true;
            if (!m_AffinityWorkQueued) {
              m_AffinityWorkQueued = true;
              if (!owner->enqueueAffinity(this)) {
                m_AffinityWorkQueued = false;
                m_AffinityGatePending = false;
                rejected = true;
              }
            }
          }
        } else {
          owner = getScheduler();
        }
      }
      if (!finished && !rejected) {
        owner->prompt();
        // The guard prevents completion from racing enrollment. Terminal
        // wakeups cannot abandon an admitted node; the source worker cancels
        // it and owns the final lease release.
        if (waited)
          *waited = true;
        const auto reason =
            m_AffinityWaiters.wait(progress, nullptr, WaitQueue::Channel(this), ProcessWait,
                                   reinterpret_cast<uintptr_t>(this), true, false);
        (void)reason;
      }
    }
    if (finished) {
      Processor::setInterrupts(false);
      return result;
    }
    if (rejected) {
      if (threadPinned)
        endExternalLease();
      if (parentPinned)
        parent->endExternalLease();
      Processor::setInterrupts(false);
      return AffinityResult::Terminal;
    }
    Processor::setInterrupts(false);
  }
}

void Thread::publishReadyNotification() {
  LockGuard<Spinlock> guard(m_Lock);
  assert(m_ReadyPublicationPending);
  __atomic_store_n(&m_ReadyPublicationPending, false, __ATOMIC_RELEASE);
  if (m_Status == Ready) {
    PerProcessorScheduler* owner = getScheduler();
    assert(owner);
    owner->m_pSchedulingAlgorithm->threadStatusChanged(this);
  }
}

bool PerProcessorScheduler::enqueueAffinity(Thread* thread, bool accepted) {
  LockGuard<Spinlock> guard(m_AffinityQueueLock);
  if (!m_AffinityAdmissionOpen && !accepted)
    return false;
  assert(!thread->m_AffinityNext);
  if (m_AffinityTail)
    m_AffinityTail->m_AffinityNext = thread;
  else
    m_AffinityHead = thread;
  m_AffinityTail = thread;
  m_AffinityRequests += 1;
  ringIrqWorkDoorbell();
  return true;
}

void PerProcessorScheduler::prompt() {
  ringIrqWorkDoorbell();
#if X86_COMMON && MULTIPROCESSOR
  if (this != &Processor::information().getScheduler()) {
    ProcessorInformation* information = Processor::informationAt(m_LogicalCpu);
    assert(information);
    const uint8_t apicId = information->localApicId();
    // Failed prompts leave accepted work visible to the periodic tick.
    const bool submitted = Pc::instance().getLocalApic().interProcessorInterrupt(
        apicId, IPI_RESCHEDULE_VECTOR, LocalApic::deliveryModeFixed, true, false);
    (void)submitted;
  }
#endif
}

void PerProcessorScheduler::drainAffinityRequests() {
  assert(this == &Processor::information().getScheduler());
  // Both delayed and immediate constructors may still be waiting for their
  // add worker to install the first context. Let that worker make progress.
  const size_t batch = m_AffinityRequests.value();
  for (size_t i = 0; i < batch; ++i) {
    Thread* thread = nullptr;
    {
      LockGuard<Spinlock> guard(m_AffinityQueueLock);
      thread = m_AffinityHead;
      if (!thread)
        break;
      m_AffinityHead = thread->m_AffinityNext;
      if (!m_AffinityHead)
        m_AffinityTail = nullptr;
      thread->m_AffinityNext = nullptr;
    }
#if PEDIGREE_AFFINITY_TESTS
    const auto hook = __atomic_load_n(&g_AffinityCommitHook, __ATOMIC_ACQUIRE);
    if (hook && thread == __atomic_load_n(&g_AffinityCommitTarget, __ATOMIC_ACQUIRE))
      hook(thread);
#endif
    bool retry = false;
    PerProcessorScheduler* destination = this;
    {
      auto progress = thread->m_AffinityWaiters.acquire();
      {
        LockGuard<Spinlock> guard(thread->m_Lock);
        assert(thread->m_AffinityWorkQueued && thread->getScheduler() == this);
        assert(thread != Processor::information().getCurrentThread());
        const bool terminal = m_StopTimeAccountingWorker.value() || thread->m_bShutdown ||
                              thread->getUnwindState() != Thread::Continue ||
                              thread->m_Status == Thread::AwaitingJoin ||
                              thread->m_Status == Thread::Zombie;
        if (!terminal && !thread->m_HasSchedulerContext) {
          retry = true;
        } else {
          if (!terminal) {
            assert(thread->m_Status != Thread::Running);
            if (thread->m_AffinityPending) {
              assert(!thread->m_LegacyUserCallbackPins ||
                     thread->m_RequestedAffinity.contains(m_LogicalCpu));
              thread->m_Placement.allowed = thread->m_RequestedAffinity;
              thread->m_AffinityCompleted = thread->m_AffinityGeneration;
            }
            if (thread->m_AffinityGatePending) {
              if (!thread->m_Placement.allowed.contains(m_LogicalCpu)) {
                assert(!thread->m_LegacyUserCallbackPins);
                for (size_t cpu = 0; cpu < CpuAffinityMask::MaximumCpus; ++cpu) {
                  if (thread->m_Placement.allowed.contains(cpu)) {
                    destination = Scheduler::schedulerForCpu(cpu);
                    break;
                  }
                }
              }
              assert(destination);
              m_pSchedulingAlgorithm->removeThread(thread);
              Scheduler::instance().rebindThread(thread, *destination);
              destination->m_pSchedulingAlgorithm->threadStatusChanged(thread);
            }
          }
          thread->m_AffinityPending = false;
          thread->m_AffinityGatePending = false;
          thread->m_AffinityWorkQueued = false;
        }
      }
      if (!retry)
        progress.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(thread));
    }
    m_AffinityRequests -= 1;
    if (retry) {
      const bool queued = enqueueAffinity(thread, true);
      assert(queued);
    } else {
      destination->prompt();
      // The node owns a lease independently of the syscall's caller. The
      // final release may destroy a terminal detached target.
      Process* parent = thread->m_pParent;
      thread->endExternalLease();
      parent->endExternalLease();
    }
  }
}

#endif
