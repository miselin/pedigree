/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"

#include "modules/Module.h"

extern void system_reset();
extern bool runHostedWaitRegressions();
#if PEDIGREE_AFFINITY_TESTS
extern bool runAffinityRegressions();
#endif
#if PEDIGREE_CHILD_WAIT_TESTS
extern bool runChildWaitRegressions();
#endif
#if PEDIGREE_PTRACE_TESTS
extern bool runPtraceFrameRegressions();
#endif

static bool entry() {
  bool passed = runHostedWaitRegressions();
#if PEDIGREE_AFFINITY_TESTS
  if (passed)
    passed = runAffinityRegressions();
#endif
#if PEDIGREE_CHILD_WAIT_TESTS
  if (passed)
    passed = runChildWaitRegressions();
#endif
#if PEDIGREE_PTRACE_TESTS
  if (passed)
    passed = runPtraceFrameRegressions();
#endif
  if (passed) {
    NOTICE("HOSTED-SMOKE: Darwin core smoke executed");
  }
  system_reset();
  return true;
}

static void exit() {}

MODULE_INFO("hosted-core-smoke", &entry, &exit);
