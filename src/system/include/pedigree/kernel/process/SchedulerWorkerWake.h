/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_SCHEDULERWORKERWAKE_H
#define PEDIGREE_KERNEL_PROCESS_SCHEDULERWORKERWAKE_H
#include "pedigree/kernel/Atomic.h"

class PerProcessorScheduler;
class WaitQueue;

/** Preallocated scheduler-side wake edge for an ordinary kernel worker. */
class SchedulerWorkerWake {
 public:
  SchedulerWorkerWake() : m_Pending(0), m_pWaiters(nullptr), m_pNext(nullptr) {}

 private:
  friend class PerProcessorScheduler;

  Atomic<size_t> m_Pending;
  WaitQueue* m_pWaiters;
  SchedulerWorkerWake* m_pNext;
};

#endif
