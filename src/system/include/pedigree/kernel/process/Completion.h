/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_COMPLETION_H
#define PEDIGREE_KERNEL_PROCESS_COMPLETION_H
#include <config.h>

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/WaitQueue.h"

/**
 * A one-shot, latched completion with an exactly-once wait claim.
 *
 * complete() may run before or after wait(), including from interrupt context.
 * Repeated completion attempts are ignored. A second call to wait() is a
 * contract violation instead of an unexplained permanent block.
 */
class EXPORTED_PUBLIC Completion {
 public:
  Completion();
  ~Completion();

  /**
   * Waits for completion.
   *
   * A Completion has exactly one wait operation in its lifetime.
   * A pending terminal request returns false after retiring the wait record;
   * caller-owned state can therefore be released by ordinary scope cleanup.
   * onStackDiscard is only a best-effort safety hook for an explicitly
   * discarded physical stack.
   */
  MUST_USE_RESULT bool wait(WaitQueue::StackDiscardCleanup onStackDiscard = nullptr,
                            void* stackDiscardContext = nullptr);

  /**
   * Latches completion and wakes the waiter.
   *
   * \return true for the first completion, false for duplicates.
   */
  bool complete();

  bool isComplete();

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(Completion);

  WaitQueue m_Waiters;
  bool m_Completed;
  bool m_WaitClaimed;
};

#endif
