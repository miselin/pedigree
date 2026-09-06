/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/KernelElf.h"

#include "modules/Module.h"

extern void system_reset();
extern bool runHostedWaitRegressions();
#if defined(PEDIGREE_HOSTED_GLOBAL_SYNC_TESTS)
extern bool runHostedCacheSyncRegressions();
extern bool runHostedScsiSyncRegressions();
#endif
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
#if defined(PEDIGREE_HOSTED_GLOBAL_SYNC_TESTS)
  if (passed)
    passed = runHostedCacheSyncRegressions() && runHostedScsiSyncRegressions();
#endif
  if (passed) {
    passed = KernelElf::moduleExecutionWaitsForUnloadForTest();
    if (passed)
      NOTICE("HOSTED-MODULE-TEST: PASS execution-unload-admission");
    else
      ERROR("HOSTED-MODULE-TEST: FAIL execution-unload-admission");
  }
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

#if defined(PEDIGREE_HOSTED_GLOBAL_SYNC_TESTS)
MODULE_INFO("hosted-core-smoke", &entry, &exit, "scsi");
#else
MODULE_INFO("hosted-core-smoke", &entry, &exit);
#endif
