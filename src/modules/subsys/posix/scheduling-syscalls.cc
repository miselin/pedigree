/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "scheduling-syscalls.h"

namespace {
constexpr int Other = 0, Fifo = 1, RoundRobin = 2, Batch = 3, Idle = 5, Deadline = 6;
constexpr int ResetOnFork = 0x40000000;

class SchedulingResult {
 public:
  SchedulingResult() : m_Thread(*Processor::information().getCurrentThread()) {}
  ~SchedulingResult() {
    m_Thread.setErrno(m_Error);
  }
  int finish(int value) {
    m_Error = value < 0 ? m_Thread.getErrno() : 0;
    return value;
  }

 private:
  Thread& m_Thread;
  size_t m_Error = 0;
};

bool acquireTarget(int pid, Process::ThreadLease& target) {
  Thread* caller = Processor::information().getCurrentThread();
  const bool found = pid == 0 ? caller->getParent()->acquireThread(target, caller)
                              : pid > 0 && Scheduler::instance().acquireThreadByTaskId(target, pid);
  if (found && target->getParent()->getType() == Process::Posix &&
      target->getParent()->getSubsystem() && target->getUnwindState() != Thread::TerminateThread) {
    ThreadPlacement placement;
    target->snapshotPlacement(placement);
    if (placement.migratable)
      return true;
  }
  SYSCALL_ERROR(NoSuchProcess);
  return false;
}

bool authorized(Thread& target) {
  Process* caller = Processor::information().getCurrentThread()->getParent();
  if (caller->getType() == Process::Posix) {
    const auto source = static_cast<PosixProcess*>(caller)->snapshotCredentials();
    const auto destination = static_cast<PosixProcess*>(target.getParent())->snapshotCredentials();
    if (!source.euid || source.euid == destination.ruid || source.euid == destination.euid)
      return true;
  }
  SYSCALL_ERROR(NotEnoughPermissions);
  return false;
}

int priorityBound(int policy, bool maximum) {
  switch (policy) {
    case Fifo:
    case RoundRobin:
      return maximum ? 99 : 1;
    case Other:
    case Batch:
    case Idle:
    case Deadline:
      return 0;
    default:
      SYSCALL_ERROR(InvalidArgument);
      return -1;
  }
}

int setPolicy(int pid, int policy, const void* user) {
  SchedulingResult result;
  Uninterruptible lifetime;
  if (pid < 0 || !user) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  // The Linux kernel ABI has one int; musl reserves additional library fields.
  int priority;
  if (!PosixSubsystem::copyFromUser(&priority, user, sizeof(priority))) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  Process::ThreadLease target;
  if (!acquireTarget(pid, target))
    return result.finish(-1);
  const int basePolicy = policy & ~ResetOnFork;
  const int minimum = priorityBound(basePolicy, false);
  const int maximum = minimum < 0 ? -1 : priorityBound(basePolicy, true);
  if (minimum < 0 || priority < minimum || priority > maximum) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  if (!authorized(*target.get()))
    return result.finish(-1);
  if (policy != Other) {
    SYSCALL_ERROR(OperationNotSupported);
    return result.finish(-1);
  }
  // Ordinary tasks already use the sole admitted policy with RT priority zero.
  return result.finish(0);
}
}  // namespace

int posix_sched_setparam(int pid, const void* parameter) {
  return setPolicy(pid, Other, parameter);
}

int posix_sched_setscheduler(int pid, int policy, const void* parameter) {
  if (policy < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return setPolicy(pid, policy, parameter);
}

int posix_sched_getparam(int pid, void* parameter) {
  SchedulingResult result;
  Uninterruptible lifetime;
  if (pid < 0 || !parameter) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  Process::ThreadLease target;
  if (!acquireTarget(pid, target))
    return result.finish(-1);
  const int priority = 0;
  target.reset();
  if (!PosixSubsystem::copyToUser(parameter, &priority, sizeof(priority))) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  return result.finish(0);
}

int posix_sched_getscheduler(int pid) {
  SchedulingResult result;
  Uninterruptible lifetime;
  if (pid < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  Process::ThreadLease target;
  return result.finish(acquireTarget(pid, target) ? Other : -1);
}

int posix_sched_get_priority_max(int policy) {
  return priorityBound(policy, true);
}

int posix_sched_get_priority_min(int policy) {
  return priorityBound(policy, false);
}

int posix_sched_rr_get_interval(int pid, void* interval) {
  SchedulingResult result;
  Uninterruptible lifetime;
  if (pid < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  Process::ThreadLease target;
  if (!acquireTarget(pid, target))
    return result.finish(-1);
  uint64_t quantum;
  {
    LockGuard<Spinlock> guard(target->getLock());
    quantum = target->getScheduler()->nominalQuantumNs();
  }
  target.reset();
  if (!quantum) {
    SYSCALL_ERROR(OperationNotSupported);
    return result.finish(-1);
  }
  const int64_t timespec[2] = {static_cast<int64_t>(quantum / 1000000000),
                               static_cast<int64_t>(quantum % 1000000000)};
  if (!PosixSubsystem::copyToUser(interval, timespec, sizeof(timespec))) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  return result.finish(0);
}

int posix_sched_setaffinity(int pid, unsigned int length, const void* user) {
  SchedulingResult result;
  Uninterruptible lifetime;
  CpuAffinityMask mask;
  size_t copied = length;
  const size_t bytes = Scheduler::affinityBytes();
  if (copied > bytes)
    copied = bytes;
  if (copied && !PosixSubsystem::copyFromUser(mask.data(), user, copied)) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  mask.intersect(Scheduler::onlineAffinity());
  Process::ThreadLease target;
  if (!acquireTarget(pid, target))
    return result.finish(-1);
  for (;;) {
    uint64_t generation = 0;
    AffinityResult admitted;
    {
      MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
      if (!authorized(*target.get()))
        return result.finish(-1);
      auto* subsystem = static_cast<PosixSubsystem*>(target->getParent()->getSubsystem());
      if (!subsystem->affinityPolicyAllowed()) {
        SYSCALL_ERROR(OperationNotSupported);
        return result.finish(-1);
      }
      admitted = target->requestAffinity(mask, generation);
      if (admitted == AffinityResult::Success || admitted == AffinityResult::Busy)
        subsystem->recordAffinityPolicyUse();
    }
    if (admitted == AffinityResult::Invalid || admitted == AffinityResult::Pinned) {
      SYSCALL_ERROR(InvalidArgument);
      return result.finish(-1);
    }
    if (admitted == AffinityResult::Unsupported) {
      SYSCALL_ERROR(OperationNotSupported);
      return result.finish(-1);
    }
    if (admitted == AffinityResult::Terminal) {
      SYSCALL_ERROR(NoSuchProcess);
      return result.finish(-1);
    }
    // No VM/credential gate survives this wait. Exec can cancel the request
    // before it drains the external target lease retained by this caller.
    if (target->waitAffinity(generation) != AffinityResult::Success) {
      SYSCALL_ERROR(NoSuchProcess);
      return result.finish(-1);
    }
    if (admitted != AffinityResult::Busy)
      return result.finish(0);
  }
}

int posix_sched_getaffinity(int pid, unsigned int length, void* user) {
  SchedulingResult result;
  Uninterruptible lifetime;
  const size_t bytes = Scheduler::affinityBytes();
  if (length < bytes || length % sizeof(unsigned long)) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  Process::ThreadLease target;
  if (!acquireTarget(pid, target))
    return result.finish(-1);
  ThreadPlacement placement;
  target->snapshotPlacement(placement);
  target.reset();
  if (!PosixSubsystem::copyToUser(user, placement.allowed.data(), bytes)) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  return result.finish(static_cast<int>(bytes));
}

int posix_getcpu(unsigned int* cpu, unsigned int* node) {
  SchedulingResult result;
  Uninterruptible lifetime;
  const unsigned int current = Processor::index(), locality = 0;
  bool copied = true;
  if (cpu && !PosixSubsystem::copyToUser(cpu, &current, sizeof(current)))
    copied = false;
  if (node && !PosixSubsystem::copyToUser(node, &locality, sizeof(locality)))
    copied = false;
  if (!copied) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  return result.finish(0);
}
