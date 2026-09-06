/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/utilities/assert.h"

#include "trace-context.h"

TraceCloneAdmission::~TraceCloneAdmission() {
  if (m_Context)
    m_Context->finishThreadCreation();
}

TraceEnrollmentAdmission::~TraceEnrollmentAdmission() {
  if (m_Context)
    m_Context->cancelEnrollment();
}

TraceStatus PosixTraceContext::reserveThreadCreation(TraceCloneAdmission& admission) {
  assert(!admission.m_Context);
  LockGuard<Mutex> guard(m_AdmissionLock);
  if (!m_Attached || !valid())
    return TraceStatus::Missing;
  if (m_EnrollmentPending || hasIncoming() || m_ThreadCreations == ~size_t(0))
    return TraceStatus::Busy;
  ++m_ThreadCreations;
  admission.m_Context = this;
  return TraceStatus::Success;
}

void PosixTraceContext::finishThreadCreation() {
  LockGuard<Mutex> guard(m_AdmissionLock);
  assert(m_ThreadCreations);
  --m_ThreadCreations;
}

TraceStatus PosixTraceContext::reserveEnrollment(Thread& thread,
                                                 TraceEnrollmentAdmission& admission) {
  assert(!admission.m_Context && !admission.m_Tracer && !admission.m_Tracee);
  LockGuard<Mutex> guard(m_AdmissionLock);
  if (!m_Attached || !valid() || thread.getParent()->getId() != m_ProcessId ||
      thread.getUnwindState() != Thread::Continue)
    return TraceStatus::Missing;
  if (m_EnrollmentPending || m_ThreadCreations)
    return TraceStatus::Busy;
  if (hasIncoming())
    return TraceStatus::Denied;
  if (thread.getTaskId() != m_ProcessId || thread.getParent()->getNumThreads() != 1)
    return TraceStatus::Unsupported;
  TraceTaskRef tracee = tokenUnlocked(thread.getId());
  if (!tracee || !m_Creator || !m_Creator->live())
    return TraceStatus::Missing;
  admission.m_Tracer = m_Creator;
  admission.m_Tracee = tracee;
  admission.m_Context = this;
  m_EnrollmentPending = true;
  return TraceStatus::Success;
}

void PosixTraceContext::cancelEnrollment() {
  LockGuard<Mutex> guard(m_AdmissionLock);
  assert(m_EnrollmentPending);
  m_EnrollmentPending = false;
}

TraceStatus PosixTraceContext::commitEnrollment(TraceEnrollmentAdmission& admission,
                                                const TraceRelationRef& relation) {
  assert(admission.m_Context == this);
  if (!relation || !relation->valid() || relation->tracer() != admission.m_Tracer ||
      relation->tracee() != admission.m_Tracee)
    return TraceStatus::Denied;
  LockGuard<Mutex> guard(m_AdmissionLock);
  assert(m_EnrollmentPending);
  if (!valid() || !ownsTokenUnlocked(admission.m_Tracee) || !admission.m_Tracer->live())
    return TraceStatus::Missing;
  assert(!m_ThreadCreations);
  LockGuard<Spinlock> publication(m_RelationLock);
  if (m_Incoming)
    return TraceStatus::Denied;
  // taskClosed serializes with activate in the relation's short state guard.
  // Any closure callbacks run after that guard and after this publication.
  if (!relation->activate())
    return TraceStatus::Missing;
  m_Incoming = relation;
  m_EnrollmentPending = false;
  admission.m_Context = nullptr;
  return TraceStatus::Success;
}

TraceStatus PosixTraceContext::registerTracee(const TraceTaskRef& tracer,
                                              const TraceRelationRef& relation) {
  if (!tracer || !relation || !relation->valid() || relation->tracer() != tracer)
    return TraceStatus::Denied;
  auto entry = UniquePointer<OwnedRelation>::allocate();
  if (!entry)
    return TraceStatus::NoMemory;
  entry.get()->relation = relation;
  LockGuard<Mutex> guard(m_AdmissionLock);
  if (!m_Attached || !valid() || !ownsTokenUnlocked(tracer))
    return TraceStatus::Missing;
  for (auto* existing = m_Outgoing; existing; existing = existing->next) {
    if (existing->relation == relation)
      return TraceStatus::Success;
  }
  entry.get()->next = m_Outgoing;
  m_Outgoing = entry.releaseOwnership();
  return TraceStatus::Success;
}

void PosixTraceContext::unregisterTracee(const PosixTraceRelation* relation) {
  if (!valid())
    return;
  OwnedRelation* retired = nullptr;
  {
    LockGuard<Mutex> guard(m_AdmissionLock);
    auto** position = &m_Outgoing;
    while (*position && (*position)->relation.get() != relation)
      position = &(*position)->next;
    if (*position) {
      retired = *position;
      *position = retired->next;
    }
  }
  delete retired;
}
