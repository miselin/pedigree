/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_POSIX_TRACE_CONTEXT_H
#define PEDIGREE_POSIX_TRACE_CONTEXT_H

#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Process.h"

#include "trace-state.h"

class EXPORTED_PUBLIC PreparedTraceTask {
 public:
  PreparedTraceTask() = default;
  ~PreparedTraceTask() = default;

 private:
  friend class PosixTraceContext;
  PreparedTraceTask(const PreparedTraceTask&) = delete;
  PreparedTraceTask& operator=(const PreparedTraceTask&) = delete;
  TraceTaskRef m_Token;
  PreparedTraceTask* m_Next = nullptr;
};

// Declare inside the existing Process::ThreadCreationScope lifetime.
class EXPORTED_PUBLIC TraceCloneAdmission {
 public:
  TraceCloneAdmission() = default;
  ~TraceCloneAdmission();

 private:
  friend class PosixTraceContext;
  TraceCloneAdmission(const TraceCloneAdmission&) = delete;
  TraceCloneAdmission& operator=(const TraceCloneAdmission&) = delete;
  PosixTraceContext* m_Context = nullptr;
};

// Caller defers events through enrollment and core frame-flag publication.
class EXPORTED_PUBLIC TraceEnrollmentAdmission {
 public:
  TraceEnrollmentAdmission() = default;
  ~TraceEnrollmentAdmission();
  const TraceTaskRef& tracer() const {
    return m_Tracer;
  }
  const TraceTaskRef& tracee() const {
    return m_Tracee;
  }

 private:
  friend class PosixTraceContext;
  TraceEnrollmentAdmission(const TraceEnrollmentAdmission&) = delete;
  TraceEnrollmentAdmission& operator=(const TraceEnrollmentAdmission&) = delete;
  PosixTraceContext* m_Context = nullptr;
  TraceTaskRef m_Tracer, m_Tracee;
};

class EXPORTED_PUBLIC PosixTraceContext {
 public:
  PosixTraceContext() = default;
  ~PosixTraceContext();
  bool valid() const;
  void attach(Process&);
  TraceStatus prepareTask(UniquePointer<PreparedTraceTask>&);
  TraceStatus publishTask(UniquePointer<PreparedTraceTask>&, Thread&);
  bool taskToken(const Thread&, TraceTaskRef&) const;
  void setCreator(const TraceTaskRef&);
  TraceTaskRef creator() const;
  void promoteExec(const Thread&);
  void retireTask(const Thread&);
  void close();

  TraceStatus reserveThreadCreation(TraceCloneAdmission&);
  TraceStatus reserveEnrollment(Thread&, TraceEnrollmentAdmission&);
  TraceStatus commitEnrollment(TraceEnrollmentAdmission&, const TraceRelationRef&);
  TraceStatus registerTracee(const TraceTaskRef& tracer, const TraceRelationRef&);
  void unregisterTracee(const PosixTraceRelation*);
  void clearIncoming(const PosixTraceRelation*);
  // Output must be empty and declared outside any parent child-state guard.
  // Only a short spin guard is taken; release the reference after that guard.
  bool acquireIncoming(TraceRelationRef& empty) const;
  bool selectStop(size_t parentPid, bool stopped, bool continued, bool consume, PosixWait::Report&);

 private:
  friend class TraceCloneAdmission;
  friend class TraceEnrollmentAdmission;
  PosixTraceContext(const PosixTraceContext&) = delete;
  PosixTraceContext& operator=(const PosixTraceContext&) = delete;
  struct OwnedRelation {
    TraceRelationRef relation;
    OwnedRelation* next = nullptr;
  };
  TraceTaskRef tokenUnlocked(size_t localId) const;
  bool ownsTokenUnlocked(const TraceTaskRef&) const;
  void finishThreadCreation();
  void cancelEnrollment();
  bool hasIncoming() const;
  mutable Mutex m_AdmissionLock;
  mutable Spinlock m_RelationLock;
  bool m_Attached = false, m_Closed = false;
  size_t m_ProcessId = 0, m_ThreadCreations = 0;
  bool m_EnrollmentPending = false;
  PreparedTraceTask* m_Tasks = nullptr;
  OwnedRelation* m_Outgoing = nullptr;
  TraceTaskRef m_Creator;
  TraceRelationRef m_Incoming;
};

// No lock from either context/token may be held by callers. Output starts empty.
EXPORTED_PUBLIC TraceStatus posix_trace_acquire_task(const TraceTaskRef&, Process::ThreadLease&);
#endif
