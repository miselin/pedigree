/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_POSIX_TRACE_TASK_TOKEN_H
#define PEDIGREE_POSIX_TRACE_TASK_TOKEN_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

class PosixTraceContext;
class Thread;
class ParentDeathSignals;

enum class TraceStatus {
  Success,
  Missing,
  Denied,
  Busy,
  Unsupported,
  NoMemory,
  Invalid,
  Full,
  NotStopped
};

class EXPORTED_PUBLIC TraceTaskToken {
 public:
  enum class State { Staged, Live, Closed };
  struct Snapshot {
    State state = State::Staged;
    size_t processId = 0, localThreadId = 0, linuxTaskId = 0;
  };
  TraceTaskToken() = default;
  Snapshot snapshot() const;
  bool live() const;
  int parentDeathSignal() const {
    return __atomic_load_n(&m_ParentDeathSignal, __ATOMIC_ACQUIRE);
  }
  void clearParentDeathSignal() {
    __atomic_store_n(&m_ParentDeathSignal, 0, __ATOMIC_RELEASE);
  }

 private:
  friend class PosixTraceContext;
  friend class ParentDeathSignals;
  TraceTaskToken(const TraceTaskToken&) = delete;
  TraceTaskToken& operator=(const TraceTaskToken&) = delete;
  void publish(const Thread&);
  void promote(const Thread&);
  void close();
  mutable Spinlock m_Lock;
  Snapshot m_Identity;
  // Registry membership and creator admission use the parent-death policy mutex.
  // Credential commits can clear the signal without taking a sleeping lock.
  int m_ParentDeathSignal = 0;
  bool m_ParentDeathArmed = false;
  SharedPointer<TraceTaskToken> m_ParentDeathCreator;
};
using TraceTaskRef = SharedPointer<TraceTaskToken>;
#endif
