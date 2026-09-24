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
  if (!runHostedWaitRegressions()) {
    system_reset();
    return true;
  }
#if PEDIGREE_AFFINITY_TESTS
  if (!runAffinityRegressions()) {
    system_reset();
    return true;
  }
#endif
#if PEDIGREE_CHILD_WAIT_TESTS
  if (!runChildWaitRegressions()) {
    system_reset();
    return true;
  }
#endif
#if PEDIGREE_PTRACE_TESTS
  if (!runPtraceFrameRegressions()) {
    system_reset();
    return true;
  }
#endif

  NOTICE("HOSTED-SMOKE: populated initrd executed");
  system_reset();
  return true;
}

static void exit() {}

MODULE_INFO("hosted-smoke", &entry, &exit, "fat", "hid", "rawfs", "scsi", "usb", "usb-mass-storage",
            "vfs");
