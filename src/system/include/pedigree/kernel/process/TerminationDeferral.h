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

#include <config.h>

class Thread;

/**
 * Keeps terminal thread teardown out of a scope which owns lifetime state.
 *
 * A terminal request still wakes waits inside the scope. Interruptible waits
 * return a terminal-pending result, while ownership waits such as Mutex
 * acquisition finish before returning. The scope does not turn ordinary waits
 * into completion barriers: callers still propagate their failure until a
 * syscall, kernel-thread, or user-mode scheduler boundary can commit the exit
 * after the outermost scope has retired its state.
 */
class EXPORTED_PUBLIC TerminationDeferral {
 public:
  // Keep the fresh-record constructor visible to avoid a redundant automatic
  // stack fill before initialise() writes every field and publishes it.
  explicit ALWAYS_INLINE TerminationDeferral(bool active = true)
      : m_pThread(nullptr), m_Record(DeferredScopeRecord::Uninitialised{}) {
    initialise(active);
  }
  TerminationDeferral(TerminationDeferral&& other) noexcept;
  ~TerminationDeferral();

  TerminationDeferral& operator=(TerminationDeferral&& other) noexcept;

 private:
  void initialise(bool active);

  TerminationDeferral(const TerminationDeferral&) = delete;
  TerminationDeferral& operator=(const TerminationDeferral&) = delete;

  Thread* m_pThread;
  DeferredScopeRecord m_Record;
};

#endif
