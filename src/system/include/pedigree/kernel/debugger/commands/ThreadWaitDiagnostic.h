/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef THREADWAITDIAGNOSTIC_H
#define THREADWAITDIAGNOSTIC_H

#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/utilities/StaticString.h"

/** A snapshot detached from live Thread/Scheduler state for safe rendering. */
struct ThreadWaitDiagnostic {
  ThreadWaitDiagnostic()
      : sleeping(false),
        hasWait(false),
        queue(0),
        channelOwner(0),
        channelValue(0),
        reason(WaitQueue::WakeReason::Waiting),
        stateLevel(0),
        mutexOwner(0),
        queued(false) {}

  bool sleeping;
  bool hasWait;
  uintptr_t queue;
  uintptr_t channelOwner;
  uintptr_t channelValue;
  WaitQueue::WakeReason reason;
  size_t stateLevel;
  uintptr_t mutexOwner;
  bool queued;
};

inline const char* threadWaitReasonName(WaitQueue::WakeReason reason) {
  switch (reason) {
    case WaitQueue::WakeReason::Waiting:
      return "waiting";
    case WaitQueue::WakeReason::Signalled:
      return "signalled";
    case WaitQueue::WakeReason::Event:
      return "event";
    case WaitQueue::WakeReason::Unwinding:
      return "unwinding";
    case WaitQueue::WakeReason::Terminating:
      return "terminating";
    case WaitQueue::WakeReason::Spurious:
      return "spurious";
  }
  return "unknown";
}

inline void appendThreadWaitDiagnostic(const ThreadWaitDiagnostic& wait, HugeStaticString& line) {
  if (!wait.hasWait) {
    if (wait.sleeping) {
      line += " [INVALID: sleeping without waitq]";
    }
    return;
  }

  line += " [waitq=";
  line.append(wait.queue, 16);
  line += ", channel=";
  line.append(wait.channelOwner, 16);
  line += ":";
  line.append(wait.channelValue, 16);
  line += ", reason=";
  line += threadWaitReasonName(wait.reason);
  line += ", level=";
  line.append(wait.stateLevel);

  if (wait.mutexOwner) {
    line += ", owner=";
    line.append(wait.mutexOwner, 16);
  }

  line += wait.queued ? ", queued]" : ", publishing]";
}

#endif
