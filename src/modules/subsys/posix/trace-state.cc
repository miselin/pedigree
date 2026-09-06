/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/utility.h"

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "trace-context.h"
#include "trace-state.h"
#include "wait-state.h"

class PosixTraceRelation::State {
 public:
  mutable WaitQueue waiters;
  bool active = false, closed = false, stopped = false, report = false;
  bool resumed = false, detached = false, execTrap = false;
  bool ownsReservation = false;
  uint64_t generation = 0;
  UniquePointer<SignalEvent> prepared;
  bool jobStopped = false, continueReport = false;
  StopKind kind = StopKind::Signal;
  int signal = 0, replacement = 0;
  int32_t senderPid = 0;
  uint32_t senderUid = 0;
  Amd64UserRegisters registers{};
  TraceSignalInfo info;
  PosixWait::Report waitReport, continuationReport;
};

PosixTraceRelation::PosixTraceRelation(const TraceTaskRef& tracer, const TraceTaskRef& tracee)
    : m_Tracer(tracer), m_Tracee(tracee), m_State(UniquePointer<State>::allocate()) {}
PosixTraceRelation::~PosixTraceRelation() = default;
bool PosixTraceRelation::valid() const {
  return bool(m_State);
}
bool PosixTraceRelation::activate() {
  if (!valid())
    return false;
  auto guard = m_State.get()->waiters.acquire();
  if (m_State.get()->closed || m_State.get()->active)
    return false;
  m_State.get()->active = true;
  return true;
}
bool PosixTraceRelation::stopped() const {
  if (!valid())
    return false;
  auto guard = m_State.get()->waiters.acquire();
  return m_State.get()->stopped && !m_State.get()->closed && !m_State.get()->resumed;
}
TraceStatus PosixTraceRelation::snapshotRegisters(Amd64UserRegisters& output) const {
  auto guard = m_State.get()->waiters.acquire();
  if (!m_State.get()->stopped || m_State.get()->closed || m_State.get()->resumed)
    return TraceStatus::NotStopped;
  output = m_State.get()->registers;
  return TraceStatus::Success;
}
TraceStatus PosixTraceRelation::snapshotSignalInfo(TraceSignalInfo& output) const {
  auto guard = m_State.get()->waiters.acquire();
  if (!m_State.get()->stopped || m_State.get()->closed || m_State.get()->resumed)
    return TraceStatus::NotStopped;
  if (m_State.get()->kind == StopKind::Group)
    return TraceStatus::Invalid;
  output = m_State.get()->info;
  return TraceStatus::Success;
}
bool PosixTraceRelation::selectStop(bool stopped, bool continued, bool consume,
                                    PosixWait::Report& output) {
  auto guard = m_State.get()->waiters.acquire();
  if (m_State.get()->closed)
    return false;
  if (stopped && m_State.get()->stopped && m_State.get()->report) {
    output = m_State.get()->waitReport;
    if (consume)
      m_State.get()->report = false;
    return true;
  }
  if (continued && m_State.get()->continueReport) {
    output = m_State.get()->continuationReport;
    if (consume)
      m_State.get()->continueReport = false;
    return true;
  }
  return false;
}
TraceStatus PosixTraceRelation::resume(int signal, bool detach) {
  if (signal < 0 || signal > 64)
    return TraceStatus::Invalid;
  uint64_t generation;
  bool prepare = false, inheritReservation = false;
  {
    auto guard = m_State.get()->waiters.acquire();
    if (!m_State.get()->stopped || m_State.get()->closed || m_State.get()->resumed)
      return TraceStatus::NotStopped;
    generation = m_State.get()->generation;
    prepare = signal && m_State.get()->kind != StopKind::Group &&
              (m_State.get()->kind == StopKind::Exec || signal != m_State.get()->signal);
    inheritReservation = m_State.get()->ownsReservation;
  }
  auto* caller =
      static_cast<PosixProcess*>(Processor::information().getCurrentThread()->getParent());
  const auto credentials = caller->snapshotCredentials();
  UniquePointer<SignalEvent> prepared;
  if (prepare) {
    const auto status = posix_trace_prepare_resume(m_Tracee, signal, inheritReservation,
                                                   static_cast<int32_t>(caller->getId()),
                                                   credentials.ruid, prepared);
    if (status != TraceStatus::Success)
      return status;
  }
  {
    auto guard = m_State.get()->waiters.acquire();
    if (!m_State.get()->stopped || m_State.get()->closed || m_State.get()->resumed ||
        m_State.get()->generation != generation)
      return TraceStatus::NotStopped;
    m_State.get()->prepared = pedigree_std::move(prepared);
    m_State.get()->senderPid = static_cast<int32_t>(caller->getId());
    m_State.get()->senderUid = credentials.ruid;
    m_State.get()->replacement = signal;
    m_State.get()->detached = detach;
    if (m_State.get()->kind == StopKind::Group && !detach)
      m_State.get()->jobStopped = false;
    m_State.get()->resumed = true;
    m_State.get()->report = false;
    guard.wakeAll();
  }
  if (detach)
    unlink();
  return TraceStatus::Success;
}
void PosixTraceRelation::taskClosed(const TraceTaskRef& task) {
  if (!valid() || (task != m_Tracer && task != m_Tracee))
    return;
  UniquePointer<SignalEvent> retired;
  {
    auto guard = m_State.get()->waiters.acquire();
    if (m_State.get()->closed)
      return;
    m_State.get()->closed = true;
    m_State.get()->active = false;
    m_State.get()->detached = true;
    if (!m_State.get()->resumed)
      m_State.get()->replacement =
          task == m_Tracer && m_State.get()->kind != StopKind::Exec ? m_State.get()->signal : 0;
    m_State.get()->resumed = true;
    if (task == m_Tracee)
      retired = pedigree_std::move(m_State.get()->prepared);
    m_State.get()->report = false;
    guard.wakeAll();
  }
  unlink();
}
void PosixTraceRelation::unlink() {
  // Each lookup is lexical. Neither tokens nor the relation pin a task while
  // it is stopped, and a retiring context already detached its own reference.
  {
    Process::ThreadLease task;
    if (posix_trace_acquire_task(m_Tracee, task) == TraceStatus::Success) {
      auto* subsystem = static_cast<PosixSubsystem*>(task->getParent()->getSubsystem());
      subsystem->traceContext().clearIncoming(this);
      task->clearSignalFrameRequirement();
    }
  }
  Process::ThreadLease tracer;
  if (posix_trace_acquire_task(m_Tracer, tracer) == TraceStatus::Success) {
    auto* subsystem = static_cast<PosixSubsystem*>(tracer->getParent()->getSubsystem());
    subsystem->traceContext().unregisterTracee(this);
  }
}
void PosixTraceRelation::continued(Process& process) {
  Scheduler::ProcessLease parent;
  if (!Scheduler::instance().acquireProcess(parent, process.getParent())) {
    auto guard = m_State.get()->waiters.acquire();
    guard.wakeAll();
    return;
  }
  PosixWait::Report report;
  report.pid = static_cast<int32_t>(process.getId());
  report.uid = static_cast<PosixProcess&>(process).snapshotCredentials().ruid;
  report.cause = PosixWait::Continue;
  report.status = 18;
  report.userNanoseconds = process.getUserTime() + process.getReapedChildrenUserTime();
  report.kernelNanoseconds = process.getKernelTime() + process.getReapedChildrenKernelTime();
  auto parentGuard = parent->acquireChildStateWait();
  auto guard = m_State.get()->waiters.acquire();
  if (m_State.get()->jobStopped && !m_State.get()->closed) {
    m_State.get()->continuationReport = report;
    m_State.get()->continueReport = true;
    m_State.get()->jobStopped = false;
    parentGuard.wakeAll();
  }
  guard.wakeAll();
}
void PosixTraceRelation::imageCommitted() {
  auto guard = m_State.get()->waiters.acquire();
  if (!m_State.get()->closed && m_State.get()->active) {
    m_State.get()->execTrap = true;
    m_State.get()->report = false;
    m_State.get()->stopped = false;
  }
}
bool PosixTraceRelation::takeExecTrap() {
  auto guard = m_State.get()->waiters.acquire();
  const bool pending = m_State.get()->execTrap && !m_State.get()->closed;
  m_State.get()->execTrap = false;
  return pending;
}
PosixTraceRelation::Resume PosixTraceRelation::stop(Thread& thread, UserReturnFrame& frame,
                                                    StopKind kind, int signal,
                                                    const TraceSignalInfo& info, size_t epoch,
                                                    bool ownsReservation) {
  UserRegisterSnapshot copy;
  if (!frame.snapshot(copy) || copy.architecture != UserRegisterSnapshot::Architecture::Amd64)
    return {0, false, true};
  Process* process = thread.getParent();
  PosixWait::Report report;
  report.pid = static_cast<int32_t>(process->getId());
  report.uid = static_cast<PosixProcess*>(process)->snapshotCredentials().ruid;
  report.cause = 4;  // CLD_TRAPPED.
  report.status = signal;
  report.userNanoseconds = process->getUserTime() + process->getReapedChildrenUserTime();
  report.kernelNanoseconds = process->getKernelTime() + process->getReapedChildrenKernelTime();
  thread.setUserReturnSignalParked(true);
  struct ParkReset {
    Thread& thread;
    ~ParkReset() {
      thread.setUserReturnSignalParked(false);
    }
  } parkReset{thread};
  Thread::StackDiscardScope park(
      [](void* value) { static_cast<Thread*>(value)->setUserReturnSignalParked(false); }, &thread);
  {
    Scheduler::ProcessLease parent;
    if (!Scheduler::instance().acquireProcess(parent, process->getParent()))
      return {kind == StopKind::Exec ? 0 : signal, true, false};
    auto parentGuard = parent->acquireChildStateWait();
    if (kind == StopKind::Group && process->getContinuationEpoch() != epoch)
      return {0, false, false};
    auto guard = m_State.get()->waiters.acquire();
    if (m_State.get()->closed || !m_State.get()->active)
      return {kind == StopKind::Exec ? 0 : signal, true, false};
    ++m_State.get()->generation;
    m_State.get()->ownsReservation = ownsReservation;
    m_State.get()->kind = kind;
    if (kind == StopKind::Group)
      m_State.get()->jobStopped = true;
    m_State.get()->signal = signal;
    m_State.get()->registers = copy.amd64;
    m_State.get()->info = info;
    m_State.get()->waitReport = report;
    m_State.get()->stopped = true;
    m_State.get()->report = true;
    m_State.get()->resumed = false;
    parentGuard.wakeAll();
  }
  {
    Scheduler::ProcessLease parent;
    Process::ThreadLease recipient;
    if (Scheduler::instance().acquireProcess(parent, process->getParent()) &&
        parent->getType() == Process::Posix && parent->acquireProcessSignalThread(recipient)) {
      auto* subsystem = static_cast<PosixSubsystem*>(parent->getSubsystem());
      PosixSubsystem::SignalDisposition disposition;
      if (subsystem && subsystem->getSignalDisposition(17, disposition) &&
          !(disposition.flags & 1U /* Linux SA_NOCLDSTOP */))
        subsystem->queueSignalDelivery(recipient.get(), 17, nullptr, 4, true, signal);
    }
  }
  Resume result;
  for (;;) {
    auto guard = m_State.get()->waiters.acquire();
    if (thread.getUnwindState() != Thread::Continue) {
      result.terminal = true;
      break;
    }
    if (m_State.get()->resumed) {
      result.signal = m_State.get()->replacement;
      result.detached = m_State.get()->detached;
      result.pid = m_State.get()->senderPid;
      result.uid = m_State.get()->senderUid;
      result.prepared = pedigree_std::move(m_State.get()->prepared);
      break;
    }
    const auto wake = guard.wait(WaitQueue::Channel(), Thread::ProcessWait);
    if (wake == WaitQueue::WakeReason::Terminating || wake == WaitQueue::WakeReason::Unwinding) {
      result.terminal = true;
      break;
    }
  }
  {
    auto guard = m_State.get()->waiters.acquire();
    m_State.get()->stopped = false;
    m_State.get()->report = false;
  }
  thread.setUserReturnSignalParked(false);
  if (kind == StopKind::Group && result.detached && !result.terminal)
    process->suspendIfContinuationEpoch(signal, epoch);
  return result;
}
