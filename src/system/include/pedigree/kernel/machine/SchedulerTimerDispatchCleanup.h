/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_SCHEDULERTIMERDISPATCHCLEANUP_H
#define KERNEL_MACHINE_SCHEDULERTIMERDISPATCHCLEANUP_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/SchedulerTimerHandlerSlot.h"
#include "pedigree/kernel/process/AtomicStateCleanup.h"

class Thread;

/**
 * Binds one scheduler-timer admission to the current Thread state.
 *
 * A scheduler callback may switch away and later have its suspended hard
 * frame abandoned. Thread teardown retires this record before discarding the
 * stack, so the handler slot cannot retain an admission whose destructor will
 * never run.
 */
class EXPORTED_PUBLIC SchedulerTimerDispatchCleanup {
 public:
  explicit SchedulerTimerDispatchCleanup(SchedulerTimerHandlerSlot::DispatchGuard& dispatch);
  ~SchedulerTimerDispatchCleanup();

 private:
  SchedulerTimerDispatchCleanup(const SchedulerTimerDispatchCleanup&);
  SchedulerTimerDispatchCleanup& operator=(const SchedulerTimerDispatchCleanup&);

  static void abandon(void* context);
  void release();

  SchedulerTimerHandlerSlot::DispatchGuard& m_Dispatch;
  Thread* m_Thread;
  AtomicStateCleanupRecord m_Cleanup;
  bool m_Active;
};

#endif
