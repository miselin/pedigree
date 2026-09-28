/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_UTILITIES_BUFFERMONITORS_H
#define KERNEL_UTILITIES_BUFFERMONITORS_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/utilities/List.h"

class Event;
class Semaphore;
class Thread;

/**
 * One-shot buffer readiness subscriptions. The caller serializes registration,
 * notification and removal with the mutex protecting its buffer state.
 */
class EXPORTED_PUBLIC BufferMonitors {
 public:
  BufferMonitors();
  ~BufferMonitors();

  /** Returns false if the Event has closed registration admission. */
  bool add(Thread* thread, Event* event);
  void add(Semaphore* semaphore);

  /** Delivers every subscription and releases its registration. */
  void notify();

  /** Wakes semaphore waiters but retires Event registrations without delivery. */
  void clear();

  void cull(Thread* thread);
  void cull(Semaphore* semaphore);
  void cull(Event* event);

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(BufferMonitors);

  struct MonitorTarget;
  List<MonitorTarget*> m_Targets;
};

#endif  // KERNEL_UTILITIES_BUFFERMONITORS_H
