/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_ADMITTEDTHREAD_H
#define PEDIGREE_KERNEL_PROCESS_ADMITTEDTHREAD_H

#include "pedigree/kernel/compiler.h"

class OperationBarrier;

/**
 * Runs module or subsystem work behind a kernel-resident admission boundary.
 *
 * The worker is published as a detached Thread. Its entry point may live in
 * unloadable module text, but the admission is released only after that
 * entry point has returned to the kernel trampoline. On success ownership of
 * parameter transfers to entry, or to cancel if the scheduler retires the
 * worker before entry begins. On failure neither is called and the caller
 * retains parameter. An explicit physical stack discard cannot return through
 * the trampoline, so it intentionally leaks the admission and blocks
 * teardown. The producer must itself be stopped or joined before the barrier
 * is drained; admission protects entry and cancel, not the producer's module
 * frame. A cancel callback runs on the scheduler's ordinary add worker and
 * must not wait for new thread publication on that CPU or drain the same
 * barrier whose admission it holds. A null cancel leaves parameter externally
 * owned if entry never begins.
 */
class EXPORTED_PUBLIC AdmittedThread {
 public:
  using Entry = int (*)(void*);
  using Cancel = void (*)(void*);

  /** False means no Thread was published and the caller still owns parameter. */
  MUST_USE_RESULT static bool launchDetached(Entry entry, void* parameter, Cancel cancel,
                                             OperationBarrier& barrier, const char* name = nullptr);

#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
  using BeforeStartHook = void (*)(class Thread* thread, void* context);
  static void setBeforeStartHookForTest(BeforeStartHook hook, void* context);
#endif

 private:
  struct Context;

  static void cancelBeforeStart(void* parameter);
  static int trampoline(void* parameter);
};

#endif
