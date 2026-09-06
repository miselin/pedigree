/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Pointers.h"

#if THREADS
class Scheduler::ActivitySample {
 public:
  struct Entry {
    Process* process = nullptr;
    Thread* thread = nullptr;
  };
  ~ActivitySample() {
    for (size_t i = 0; i < count; ++i)
      Scheduler::releaseActivityEntry(entries.get()[i].process, entries.get()[i].thread);
  }
  TerminationDeferral lifetime;
  UniqueArray<Entry> entries;
  size_t count = 0;
};

void Scheduler::releaseActivityEntry(Process* process, Thread* thread) {
  if (thread)
    thread->endExternalLease();
  if (process)
    process->endExternalLease();
}

Scheduler::SystemActivity Scheduler::systemActivity() {
  SystemActivity result;
  {
    LockGuard<Spinlock> registry(m_SchedulerLock);
    for (auto it = m_TPMap.begin(); it != m_TPMap.end(); ++it) {
      if (it.key() != __atomic_load_n(&it.value()->m_pIdleThread, __ATOMIC_ACQUIRE))
        ++result.tasks;
    }
  }
  {
    LockGuard<Mutex> averages(m_ActivityLock);
    m_LoadAverage.snapshot(result.loads);
  }
  return result;
}

void Scheduler::sampleLoadAverage() {
  const uint64_t now = Time::getTicks();
  if (now < __atomic_load_n(&m_NextActivityAttempt, __ATOMIC_ACQUIRE))
    return;
  LockGuard<Mutex> averages(m_ActivityLock);
  if (now < m_NextActivityAttempt)
    return;
  const uint64_t delay = LoadAverage::PeriodNanoseconds;
  __atomic_store_n(&m_NextActivityAttempt, now > ~uint64_t(0) - delay ? ~uint64_t(0) : now + delay,
                   __ATOMIC_RELEASE);

  ActivitySample sample;
  bool prepared = false;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    size_t required;
    {
      LockGuard<Spinlock> registry(m_SchedulerLock);
      required = m_TPMap.count();
    }
    sample.entries = UniqueArray<ActivitySample::Entry>::allocate(required);
    if (required && !sample.entries)
      return;
    {
      LockGuard<Spinlock> registry(m_SchedulerLock);
      if (m_TPMap.count() > required)
        continue;
      for (auto it = m_TPMap.begin(); it != m_TPMap.end(); ++it) {
        Thread* thread = it.key();
        if (thread == __atomic_load_n(&it.value()->m_pIdleThread, __ATOMIC_ACQUIRE))
          continue;
        Process* process = thread->getParent();
        if (!process->beginExternalLease())
          continue;
        auto& entry = sample.entries.get()[sample.count++];
        entry.process = process;
        if (thread->beginExternalLease())
          entry.thread = thread;
      }
      prepared = true;
    }
    break;
  }
  if (!prepared)
    return;
  uint32_t active = 0;
  Thread* observer = Processor::information().getCurrentThread();
  for (size_t i = 0; i < sample.count; ++i) {
    Thread* thread = sample.entries.get()[i].thread;
    if (!thread || thread == observer)
      continue;
    LockGuard<Spinlock> state(thread->m_Lock);
    const auto status = thread->getStatus();
    // Eligibility predicates are the same nonblocking predicates used by the
    // run queue. Evaluate them only after dropping the global registry lock.
    // Exclude this observer, whose eligibility protects the sampling work.
    if ((status == Thread::Ready || status == Thread::Running) &&
        !__atomic_load_n(&thread->m_ReadyPublicationPending, __ATOMIC_ACQUIRE) &&
        (!thread->m_SchedulerReadyPredicate ||
         thread->m_SchedulerReadyPredicate(thread->m_SchedulerReadyContext)))
      if (active != ~uint32_t(0))
        ++active;
  }
  // Sleeping tasks have no distinct uninterruptible-I/O classification yet.
  m_LoadAverage.update(now, active);
}
#endif
