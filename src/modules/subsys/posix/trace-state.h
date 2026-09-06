/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_POSIX_TRACE_STATE_H
#define PEDIGREE_POSIX_TRACE_STATE_H
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/processor/UserReturnFrame.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include "trace-task-token.h"

class Thread;
class Process;
namespace PosixWait {
struct Report;
}
struct TraceSignalInfo {
  uint64_t words[16]{};
};
static_assert(sizeof(TraceSignalInfo) == 128, "Linux siginfo size");
class EXPORTED_PUBLIC PosixTraceRelation {
 public:
  enum class StopKind { Signal, Group, Exec };
  struct Resume {
    Resume() = default;
    Resume(int value, bool detach, bool end) : signal(value), detached(detach), terminal(end) {}
    int signal = 0;
    bool detached = false;
    bool terminal = false;
    int32_t pid = 0;
    uint32_t uid = 0;
    UniquePointer<SignalEvent> prepared;
  };
  PosixTraceRelation(const TraceTaskRef& tracer, const TraceTaskRef& tracee);
  ~PosixTraceRelation();
  bool valid() const;
  bool activate();
  void taskClosed(const TraceTaskRef& task);
  const TraceTaskRef& tracer() const {
    return m_Tracer;
  }
  const TraceTaskRef& tracee() const {
    return m_Tracee;
  }
  TraceStatus snapshotRegisters(Amd64UserRegisters&) const;
  TraceStatus snapshotSignalInfo(TraceSignalInfo&) const;
  TraceStatus resume(int signal, bool detach);
  bool stopped() const;
  bool selectStop(bool stopped, bool continued, bool consume, PosixWait::Report&);
  Resume stop(Thread&, UserReturnFrame&, StopKind, int signal, const TraceSignalInfo&,
              size_t continuationEpoch = 0, bool ownsReservation = false);
  void continued(Process&);
  void imageCommitted();
  bool takeExecTrap();

 private:
  PosixTraceRelation(const PosixTraceRelation&) = delete;
  PosixTraceRelation& operator=(const PosixTraceRelation&) = delete;
  void unlink();
  class State;
  const TraceTaskRef m_Tracer, m_Tracee;
  UniquePointer<State> m_State;
};
using TraceRelationRef = SharedPointer<PosixTraceRelation>;
EXPORTED_PUBLIC TraceStatus posix_trace_traceme();
EXPORTED_PUBLIC TraceStatus posix_trace_lookup(int32_t tid, TraceRelationRef& empty);
TraceStatus posix_trace_prepare_resume(const TraceTaskRef&, int signal, bool inheritReservation,
                                       int32_t pid, uint32_t uid, UniquePointer<SignalEvent>&);
#endif
