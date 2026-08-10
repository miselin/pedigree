/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_TERMINATIONDEFERRAL_H
#define PEDIGREE_KERNEL_PROCESS_TERMINATIONDEFERRAL_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/DeferredScope.h"

class Thread;

/**
 * Keeps terminal thread teardown out of a scope which owns lifetime state.
 *
 * A terminal request still wakes waits inside the scope. Interruptible waits
 * return a terminal-pending result, while ownership waits such as Mutex
 * acquisition finish before returning. The pending request is consumed at the
 * next ordinary WaitQueue, syscall, or user-mode scheduler boundary after the
 * outermost scope has retired its state.
 */
class EXPORTED_PUBLIC TerminationDeferral {
 public:
  explicit TerminationDeferral(bool active = true);
  TerminationDeferral(TerminationDeferral&& other);
  ~TerminationDeferral();

  TerminationDeferral& operator=(TerminationDeferral&& other);

 private:
  TerminationDeferral(const TerminationDeferral&) = delete;
  TerminationDeferral& operator=(const TerminationDeferral&) = delete;

  Thread* m_pThread;
  DeferredScopeRecord m_Record;
};

#endif
