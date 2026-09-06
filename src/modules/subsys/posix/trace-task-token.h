/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_POSIX_TRACE_TASK_TOKEN_H
#define PEDIGREE_POSIX_TRACE_TASK_TOKEN_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

class PosixTraceContext;
class Thread;

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

 private:
  friend class PosixTraceContext;
  TraceTaskToken(const TraceTaskToken&) = delete;
  TraceTaskToken& operator=(const TraceTaskToken&) = delete;
  void publish(const Thread&);
  void promote(const Thread&);
  void close();
  mutable Spinlock m_Lock;
  Snapshot m_Identity;
};
using TraceTaskRef = SharedPointer<TraceTaskToken>;
#endif
