/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "PosixSubsystem.h"
#include "trace-context.h"

namespace {
size_t localId(const Thread& thread) {
  return const_cast<Thread&>(thread).getId();
}
}  // namespace

// Reuse the task tokens' existing lifetime and creating-thread identity. All
// fanout is pinned under this policy lock and delivered after releasing it.
class ParentDeathSignals {
 public:
  static void publish(PreparedTraceTask& task, const TraceTaskRef& creator) {
    LockGuard<Mutex> guard(s_Lock);
    task.m_Token->m_ParentDeathCreator = creator;
    task.m_ParentDeathNext = s_Tasks;
    if (s_Tasks) {
      s_Tasks->m_ParentDeathPrevious = &task;
    }
    s_Tasks = &task;
  }

  static bool arm(const TraceTaskRef& task, int signal) {
    LockGuard<Mutex> guard(s_Lock);
    if (!task->live()) {
      return false;
    }
    const bool armed = signal && task->m_ParentDeathCreator && task->m_ParentDeathCreator->live();
    if (armed != task->m_ParentDeathArmed) {
      if (armed) {
        ++s_ArmedCount;
      } else {
        --s_ArmedCount;
      }
    }
    task->m_ParentDeathArmed = armed;
    __atomic_store_n(&task->m_ParentDeathSignal, signal, __ATOMIC_RELEASE);
    return true;
  }

  static void retire(PreparedTraceTask& node) {
    const TraceTaskRef& task = node.m_Token;
    struct Delivery {
      TraceTaskRef task;
      int signal;
    };
    Vector<Delivery> deliveries;
    {
      LockGuard<Mutex> guard(s_Lock);
      if (node.m_ParentDeathPrevious) {
        node.m_ParentDeathPrevious->m_ParentDeathNext = node.m_ParentDeathNext;
      } else {
        s_Tasks = node.m_ParentDeathNext;
      }
      if (node.m_ParentDeathNext) {
        node.m_ParentDeathNext->m_ParentDeathPrevious = node.m_ParentDeathPrevious;
      }
      node.m_ParentDeathNext = node.m_ParentDeathPrevious = nullptr;
      if (task->m_ParentDeathArmed) {
        task->m_ParentDeathArmed = false;
        --s_ArmedCount;
      }
      // Most tasks never request this feature. Their exit is constant-time.
      if (!s_ArmedCount) {
        return;
      }
      for (auto* child = s_Tasks; child; child = child->m_ParentDeathNext) {
        if (child->m_Token->m_ParentDeathCreator != task || !child->m_Token->m_ParentDeathArmed) {
          continue;
        }
        child->m_Token->m_ParentDeathArmed = false;
        --s_ArmedCount;
        const int signal = child->m_Token->parentDeathSignal();
        if (signal) {
          // The owning task node cannot release its token until retirement
          // removes it under this same policy lock.
          deliveries.pushBack({child->m_Token, signal});
        }
      }
    }
    for (const auto& delivery : deliveries) {
      Process::ThreadLease target;
      if (posix_trace_acquire_task(delivery.task, target) != TraceStatus::Success) {
        continue;
      }
      auto* subsystem = static_cast<PosixSubsystem*>(target->getParent()->getSubsystem());
      subsystem->sendSignal(target.get(), delivery.signal, false, true);
    }
  }

 private:
  static Mutex s_Lock;
  static PreparedTraceTask* s_Tasks;
  static size_t s_ArmedCount;
};
Mutex ParentDeathSignals::s_Lock;
PreparedTraceTask* ParentDeathSignals::s_Tasks = nullptr;
size_t ParentDeathSignals::s_ArmedCount = 0;

TraceTaskToken::Snapshot TraceTaskToken::snapshot() const {
  LockGuard<Spinlock> guard(m_Lock);
  return m_Identity;
}

bool TraceTaskToken::live() const {
  return snapshot().state == State::Live;
}

void TraceTaskToken::publish(const Thread& thread) {
  LockGuard<Spinlock> guard(m_Lock);
  assert(m_Identity.state == State::Staged);
  m_Identity.processId = thread.getParent()->getId();
  m_Identity.localThreadId = localId(thread);
  m_Identity.linuxTaskId = thread.getTaskId();
  m_Identity.state = State::Live;
}

void TraceTaskToken::promote(const Thread& thread) {
  LockGuard<Spinlock> guard(m_Lock);
  if (m_Identity.state != State::Live)
    return;
  assert(m_Identity.processId == thread.getParent()->getId());
  assert(m_Identity.localThreadId == localId(thread));
  m_Identity.linuxTaskId = thread.getTaskId();
}

void TraceTaskToken::close() {
  LockGuard<Spinlock> guard(m_Lock);
  m_Identity.state = State::Closed;
}

PosixTraceContext::~PosixTraceContext() {
  close();
  // The core creation scope and current syscall retain the owning subsystem.
  assert(!m_ThreadCreations && !m_EnrollmentPending);
}

bool PosixTraceContext::valid() const {
  return !__atomic_load_n(&m_Closed, __ATOMIC_ACQUIRE);
}

void PosixTraceContext::attach(Process& process) {
  LockGuard<Mutex> guard(m_AdmissionLock);
  assert(valid());
  assert(!m_Attached || m_ProcessId == process.getId());
  m_ProcessId = process.getId();
  m_Attached = true;
}

TraceStatus PosixTraceContext::prepareTask(UniquePointer<PreparedTraceTask>& result) {
  assert(!result);
  {
    LockGuard<Mutex> guard(m_AdmissionLock);
    if (!m_Attached || !valid())
      return TraceStatus::Missing;
  }
  auto prepared = UniquePointer<PreparedTraceTask>::allocate();
  if (!prepared)
    return TraceStatus::NoMemory;
  prepared.get()->m_Token = TraceTaskRef::tryAllocate();
  if (!prepared.get()->m_Token)
    return TraceStatus::NoMemory;
  result = pedigree_std::move(prepared);
  return TraceStatus::Success;
}

TraceStatus PosixTraceContext::publishTask(UniquePointer<PreparedTraceTask>& prepared,
                                           Thread& thread) {
  assert(prepared && prepared.get()->m_Token);
  LockGuard<Mutex> guard(m_AdmissionLock);
  if (!m_Attached || !valid() || thread.getParent()->getId() != m_ProcessId ||
      thread.getUnwindState() == Thread::TerminateThread)
    return TraceStatus::Missing;
  // Shutdown publishes TerminateThread before calling the retirement hook.
  // Its earlier hook therefore cannot be undone by delayed publication.
  if (tokenUnlocked(localId(thread)))
    return TraceStatus::Denied;
  prepared.get()->m_Token->publish(thread);
  ParentDeathSignals::publish(*prepared.get(), m_Creator);
  auto* node = prepared.releaseOwnership();
  node->m_Next = m_Tasks;
  m_Tasks = node;
  return TraceStatus::Success;
}

TraceTaskRef PosixTraceContext::tokenUnlocked(size_t id) const {
  for (auto* task = m_Tasks; task; task = task->m_Next) {
    const auto identity = task->m_Token->snapshot();
    if (identity.state == TraceTaskToken::State::Live && identity.localThreadId == id)
      return task->m_Token;
  }
  return {};
}

bool PosixTraceContext::ownsTokenUnlocked(const TraceTaskRef& token) const {
  for (auto* task = m_Tasks; task; task = task->m_Next) {
    if (task->m_Token == token)
      return token->live();
  }
  return false;
}

bool PosixTraceContext::taskToken(const Thread& thread, TraceTaskRef& result) const {
  assert(!result);
  TraceTaskRef acquired;
  {
    LockGuard<Mutex> guard(m_AdmissionLock);
    if (!m_Attached || !valid() || thread.getParent()->getId() != m_ProcessId)
      return false;
    acquired = tokenUnlocked(localId(thread));
  }
  result = pedigree_std::move(acquired);
  return bool(result);
}

void PosixTraceContext::setCreator(const TraceTaskRef& token) {
  LockGuard<Mutex> guard(m_AdmissionLock);
  assert(m_Attached && valid() && !m_Tasks && !m_Creator);
  m_Creator = token;
}

TraceTaskRef PosixTraceContext::creator() const {
  LockGuard<Mutex> guard(m_AdmissionLock);
  return valid() ? m_Creator : TraceTaskRef{};
}

bool PosixTraceContext::setParentDeathSignal(const Thread& thread, int signal) {
  TraceTaskRef task;
  if (!taskToken(thread, task)) {
    return false;
  }
  return ParentDeathSignals::arm(task, signal);
}

void PosixTraceContext::promoteExec(const Thread& thread) {
  TraceTaskRef token;
  {
    LockGuard<Mutex> guard(m_AdmissionLock);
    if (!valid())
      return;
    assert(thread.getParent()->getId() == m_ProcessId);
    token = tokenUnlocked(localId(thread));
    assert(bool(token));
    token->promote(thread);
  }
}

bool PosixTraceContext::acquireIncoming(TraceRelationRef& result) const {
  assert(!result);
  LockGuard<Spinlock> guard(m_RelationLock);
  if (!valid())
    return false;
  result = m_Incoming;
  return bool(result);
}

bool PosixTraceContext::selectStop(size_t parentPid, bool stopped, bool continued, bool consume,
                                   PosixWait::Report& report) {
  LockGuard<Spinlock> guard(m_RelationLock);
  if (!valid() || !m_Incoming)
    return false;
  const auto owner = m_Incoming->tracer()->snapshot();
  return owner.state == TraceTaskToken::State::Live && owner.processId == parentPid &&
         m_Incoming->selectStop(stopped, continued, consume, report);
}

bool PosixTraceContext::hasIncoming() const {
  LockGuard<Spinlock> guard(m_RelationLock);
  return bool(m_Incoming);
}

void PosixTraceContext::clearIncoming(const PosixTraceRelation* relation) {
  TraceRelationRef retired;
  {
    LockGuard<Spinlock> guard(m_RelationLock);
    if (m_Incoming.get() == relation)
      retired = pedigree_std::move(m_Incoming);
  }
}

void PosixTraceContext::retireTask(const Thread& thread) {
  if (!valid())
    return;
  PreparedTraceTask* retired = nullptr;
  TraceRelationRef incoming;
  {
    LockGuard<Mutex> guard(m_AdmissionLock);
    if (!valid() || thread.getParent()->getId() != m_ProcessId)
      return;
    auto** position = &m_Tasks;
    while (*position && (*position)->m_Token->snapshot().localThreadId != localId(thread))
      position = &(*position)->m_Next;
    if (!*position)
      return;
    retired = *position;
    *position = retired->m_Next;
    retired->m_Token->close();
    LockGuard<Spinlock> publication(m_RelationLock);
    if (m_Incoming && m_Incoming->tracee() == retired->m_Token)
      incoming = pedigree_std::move(m_Incoming);
  }
  ParentDeathSignals::retire(*retired);
  if (incoming)
    incoming->taskClosed(retired->m_Token);
  for (;;) {
    OwnedRelation* outgoing = nullptr;
    {
      LockGuard<Mutex> guard(m_AdmissionLock);
      auto** position = &m_Outgoing;
      while (*position && (*position)->relation->tracer() != retired->m_Token)
        position = &(*position)->next;
      if (*position) {
        outgoing = *position;
        *position = outgoing->next;
      }
    }
    if (!outgoing)
      break;
    outgoing->relation->taskClosed(retired->m_Token);
    delete outgoing;
  }
  delete retired;
}

void PosixTraceContext::close() {
  // The once-only, IRQ-enabled subsystem exit boundary performs first close.
  // Later terminal destructor fallbacks must not acquire a sleeping lock.
  if (!valid())
    return;
  PreparedTraceTask* tasks = nullptr;
  OwnedRelation* outgoing = nullptr;
  TraceRelationRef incoming;
  TraceTaskRef creator;
  {
    LockGuard<Mutex> guard(m_AdmissionLock);
    if (!valid())
      return;
    __atomic_store_n(&m_Closed, true, __ATOMIC_RELEASE);
    tasks = m_Tasks;
    m_Tasks = nullptr;
    outgoing = m_Outgoing;
    m_Outgoing = nullptr;
    for (auto* task = tasks; task; task = task->m_Next)
      task->m_Token->close();
    creator = pedigree_std::move(m_Creator);
    LockGuard<Spinlock> publication(m_RelationLock);
    incoming = pedigree_std::move(m_Incoming);
  }
  if (incoming)
    incoming->taskClosed(incoming->tracee());
  while (outgoing) {
    auto* next = outgoing->next;
    outgoing->relation->taskClosed(outgoing->relation->tracer());
    delete outgoing;
    outgoing = next;
  }
  while (tasks) {
    auto* next = tasks->m_Next;
    ParentDeathSignals::retire(*tasks);
    delete tasks;
    tasks = next;
  }
}

TraceStatus posix_trace_acquire_task(const TraceTaskRef& token, Process::ThreadLease& result) {
  assert(!result);
  if (!token)
    return TraceStatus::Missing;
  const auto identity = token->snapshot();
  if (identity.state != TraceTaskToken::State::Live)
    return TraceStatus::Missing;
  Scheduler::ProcessLease process;
  if (!Scheduler::instance().acquireProcessById(process, identity.processId) ||
      process->getType() != Process::Posix)
    return TraceStatus::Missing;
  Process::ThreadLease task;
  if (!process->acquireThreadById(task, identity.localThreadId) || !task->acceptingEvents() ||
      task->getUnwindState() != Thread::Continue)
    return TraceStatus::Missing;
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  TraceTaskRef actual;
  if (!subsystem || !subsystem->traceContext().taskToken(*task.get(), actual) || actual != token ||
      !token->live())
    return TraceStatus::Missing;
  result = pedigree_std::move(task);
  return TraceStatus::Success;
}
