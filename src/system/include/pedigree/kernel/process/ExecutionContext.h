/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_EXECUTIONCONTEXT_H
#define PEDIGREE_KERNEL_PROCESS_EXECUTIONCONTEXT_H
#include <config.h>

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/AtomicStateCleanup.h"

class Thread;

/**
 * The logical execution boundary of the currently executing Thread.
 *
 * This deliberately describes what the caller is allowed to do, rather than
 * the architecture's raw interrupt masking. An ordinary Thread with IRQs
 * disabled is AtomicThread, while an explicitly scoped IRQ route remains an
 * IRQ context even if it later enables interrupts.
 */
enum class ExecutionContext {
  WaitableThread,
  AtomicThread,
  HardDeviceIrq,
  SchedulerIrq,
  HostedSyntheticIrq,
  DebuggerTrap,
};

/** A small Thread-state value with explicit nested save/restore semantics. */
class ExecutionContextState {
 public:
  ExecutionContextState(ExecutionContext context = ExecutionContext::WaitableThread)
      : m_Context(context) {}

  ExecutionContext current() const {
    return m_Context;
  }

  /** Replaces the current context and returns the value to restore. */
  ExecutionContext enter(ExecutionContext context) {
    const ExecutionContext previous = m_Context;
    m_Context = context;
    return previous;
  }

  void restore(ExecutionContext context) {
    m_Context = context;
  }

  void reset() {
    m_Context = ExecutionContext::WaitableThread;
  }

 private:
  ExecutionContext m_Context;
};

/**
 * Scopes one logical context on the current Thread's active state level.
 *
 * The cleanup record covers an explicitly audited logical-state replacement
 * or exceptional physical-stack discard. Ordinary thread termination returns
 * through this destructor before reaching its root exit boundary.
 */
class EXPORTED_PUBLIC ExecutionContextGuard {
 public:
  explicit ExecutionContextGuard(ExecutionContext context);
  ~ExecutionContextGuard();

 private:
  ExecutionContextGuard(const ExecutionContextGuard&);
  ExecutionContextGuard& operator=(const ExecutionContextGuard&);

  static void abandon(void* context);
  void restore();

  Thread* m_Thread;
  size_t m_StateLevel;
  ExecutionContext m_Previous;
  AtomicStateCleanupRecord m_Cleanup;
  bool m_Active;
};

#endif
