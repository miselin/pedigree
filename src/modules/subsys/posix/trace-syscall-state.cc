/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "trace-context.h"

namespace {
bool permitted(PosixProcess& source, PosixProcess& destination) {
  if (&source == &destination)
    return true;
  const auto caller = source.snapshotCredentials();
  const auto target = destination.snapshotCredentials();
  return target.dumpable && caller.ruid == target.ruid && caller.ruid == target.euid &&
         caller.ruid == target.suid && caller.rgid == target.rgid && caller.rgid == target.egid &&
         caller.rgid == target.sgid;
}
}  // namespace
TraceStatus posix_trace_traceme() {
#if !X64
  return TraceStatus::Unsupported;
#else
  Thread* current = Processor::information().getCurrentThread();
  auto* process = static_cast<PosixProcess*>(current->getParent());
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  Uninterruptible enrollmentWindow;
  TraceEnrollmentAdmission admission;
  auto status = subsystem->traceContext().reserveEnrollment(*current, admission);
  if (status != TraceStatus::Success)
    return status;
  Scheduler::ProcessLease creator;
  {
    Process::ThreadLease task;
    status = posix_trace_acquire_task(admission.tracer(), task);
    if (status != TraceStatus::Success ||
        !Scheduler::instance().acquireProcess(creator, task->getParent()))
      return TraceStatus::Missing;
  }
  MemoryMapManager::OperationGuard authority(MemoryMapManager::instance());
  PosixSubsystem::UserImageToken image;
  if (!subsystem->snapshotUserImage(image))
    return TraceStatus::Missing;
  if (!subsystem->tracePolicyAllowed())
    return TraceStatus::Unsupported;
  if (process->getParent() != creator.get() || creator->getType() != Process::Posix)
    return TraceStatus::Missing;
  if (!permitted(*static_cast<PosixProcess*>(creator.get()), *process))
    return TraceStatus::Denied;
  auto* tracerSubsystem = static_cast<PosixSubsystem*>(creator->getSubsystem());
  auto relation = TraceRelationRef::tryAllocate(admission.tracer(), admission.tracee());
  if (!relation || !relation->valid())
    return TraceStatus::NoMemory;
  status = tracerSubsystem->traceContext().registerTracee(admission.tracer(), relation);
  if (status != TraceStatus::Success)
    return status;
  if (!current->tryRequireSignalFrames()) {
    tracerSubsystem->traceContext().unregisterTracee(relation.get());
    return TraceStatus::Unsupported;
  }
  status = subsystem->traceContext().commitEnrollment(admission, relation);
  if (status != TraceStatus::Success) {
    current->clearSignalFrameRequirement();
    tracerSubsystem->traceContext().unregisterTracee(relation.get());
  }
  return status;
#endif
}
TraceStatus posix_trace_lookup(int32_t tid, TraceRelationRef& result) {
  assert(!result);
  if (tid <= 0)
    return TraceStatus::Missing;
  Thread* current = Processor::information().getCurrentThread();
  auto* caller = static_cast<PosixProcess*>(current->getParent());
  auto* callerSubsystem = static_cast<PosixSubsystem*>(caller->getSubsystem());
  TraceTaskRef owner;
  if (!callerSubsystem->traceContext().taskToken(*current, owner))
    return TraceStatus::Missing;
  Scheduler::ProcessLease process;
  TraceTaskRef selected;
  {
    Process::ThreadLease task;
    if (!Scheduler::instance().acquireThreadByTaskId(task, static_cast<size_t>(tid)) ||
        task->getParent()->getType() != Process::Posix || !task->acceptingEvents() ||
        task->getUnwindState() != Thread::Continue)
      return TraceStatus::Missing;
    auto* subsystem = static_cast<PosixSubsystem*>(task->getParent()->getSubsystem());
    if (!subsystem || !subsystem->traceContext().taskToken(*task.get(), selected) ||
        !Scheduler::instance().acquireProcess(process, task->getParent()))
      return TraceStatus::Missing;
  }
  TraceRelationRef relation;
  {
    MemoryMapManager::OperationGuard authority(MemoryMapManager::instance());
    auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
    PosixSubsystem::UserImageToken image;
    if (!subsystem || !subsystem->snapshotUserImage(image) || !selected->live() ||
        !subsystem->traceContext().acquireIncoming(relation) || relation->tracer() != owner ||
        relation->tracee() != selected)
      return TraceStatus::Missing;
    if (!permitted(*caller, *static_cast<PosixProcess*>(process.get())))
      return TraceStatus::Denied;
    if (!relation->stopped())
      return TraceStatus::NotStopped;
  }
  result = pedigree_std::move(relation);
  return TraceStatus::Success;
}

TraceStatus posix_trace_prepare_resume(const TraceTaskRef& target, int signal,
                                       bool inheritReservation, int32_t pid, uint32_t uid,
                                       UniquePointer<SignalEvent>& result) {
  assert(!result);
  const auto identity = target->snapshot();
  if (identity.state != TraceTaskToken::State::Live)
    return TraceStatus::Missing;
  Scheduler::ProcessLease process;
  if (!Scheduler::instance().acquireProcessById(process, identity.processId) ||
      process->getType() != Process::Posix)
    return TraceStatus::Missing;
  MemoryMapManager::OperationGuard authority(MemoryMapManager::instance());
  auto* caller =
      static_cast<PosixProcess*>(Processor::information().getCurrentThread()->getParent());
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  PosixSubsystem::UserImageToken image;
  if (!subsystem || !subsystem->snapshotUserImage(image))
    return TraceStatus::Missing;
  if (!permitted(*caller, *static_cast<PosixProcess*>(process.get())))
    return TraceStatus::Denied;
  // Acquire only after the possibly blocking authority gate, and release
  // before that gate. A tracee exec never waits for a lease held behind it.
  Process::ThreadLease task;
  if (posix_trace_acquire_task(target, task) != TraceStatus::Success)
    return TraceStatus::Missing;
  return subsystem->prepareTraceSignal(*task.get(), signal, pid, uid, inheritReservation, result);
}
