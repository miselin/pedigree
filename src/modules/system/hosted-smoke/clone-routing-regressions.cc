/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"

#include <sched.h>
#include <signal.h>

class Process;

extern "C" int posixCloneRouteForTest(unsigned long flags);

bool runHostedCloneRoutingRegressions(Process*) {
  const unsigned long spawnFlags = CLONE_VM | CLONE_VFORK | SIGCHLD;
  const unsigned long pthreadFlags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                                     CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
                                     CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED;

  const bool passed =
      posixCloneRouteForTest(spawnFlags) == 0 && posixCloneRouteForTest(pthreadFlags) == 1 &&
      posixCloneRouteForTest(0) == 0 && posixCloneRouteForTest(SIGCHLD) == 0 &&
      posixCloneRouteForTest(CLONE_VM) == -1 && posixCloneRouteForTest(CLONE_THREAD) == -1 &&
      posixCloneRouteForTest(CLONE_VM | CLONE_THREAD) == -1 &&
      posixCloneRouteForTest(CLONE_VM | CLONE_SIGHAND | CLONE_THREAD) == -1 &&
      posixCloneRouteForTest(CLONE_SIGHAND) == -1 &&
      posixCloneRouteForTest(CLONE_FILES | SIGCHLD) == -1 &&
      posixCloneRouteForTest(CLONE_PARENT | SIGCHLD) == -1;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL clone-process-routing: "
        "process-shaped clone and pthread clone no longer take distinct paths");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS clone-process-routing");
  return true;
}
