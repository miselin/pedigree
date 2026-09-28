/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/utilities/BufferMonitors.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

#include <config.h>

#if THREADS
#include "pedigree/kernel/process/Thread.h"
#endif

struct BufferMonitors::MonitorTarget {
  MonitorTarget(Thread* thread, Event* event, Event::SendLease registration)
      : pThread(thread),
        pEvent(event),
        pSemaphore(nullptr),
        eventRegistration(pedigree_std::move(registration)) {}

  explicit MonitorTarget(Semaphore* semaphore)
      : pThread(nullptr), pEvent(nullptr), pSemaphore(semaphore), eventRegistration() {}

  Thread* pThread;
  Event* pEvent;
  Semaphore* pSemaphore;
  Event::SendLease eventRegistration;
};

BufferMonitors::BufferMonitors() : m_Targets() {}

BufferMonitors::~BufferMonitors() {
  clear();
}

bool BufferMonitors::add(Thread* thread, Event* event) {
  Event::SendLease registration;
  if (!event->tryAcquireRegistration(registration)) {
    return false;
  }

  m_Targets.pushBack(new MonitorTarget(thread, event, pedigree_std::move(registration)));
  return true;
}

void BufferMonitors::add(Semaphore* semaphore) {
  m_Targets.pushBack(new MonitorTarget(semaphore));
}

void BufferMonitors::notify() {
  while (m_Targets.count()) {
    MonitorTarget* target = m_Targets.popFront();
#if THREADS
    if (target->pThread) {
      target->pThread->sendEvent(target->pEvent);
    } else if (target->pSemaphore) {
      target->pSemaphore->release();
    }
#endif
    delete target;
  }
}

void BufferMonitors::clear() {
  while (m_Targets.count()) {
    MonitorTarget* target = m_Targets.popFront();
#if THREADS
    if (target->pSemaphore) {
      target->pSemaphore->release();
    }
#endif
    delete target;
  }
}

void BufferMonitors::cull(Thread* thread) {
  for (auto it = m_Targets.begin(); it != m_Targets.end();) {
    MonitorTarget* target = *it;
    if (target->pThread == thread) {
      delete target;
      it = m_Targets.erase(it);
    } else {
      ++it;
    }
  }
}

void BufferMonitors::cull(Semaphore* semaphore) {
  for (auto it = m_Targets.begin(); it != m_Targets.end();) {
    MonitorTarget* target = *it;
    if (target->pSemaphore == semaphore) {
      delete target;
      it = m_Targets.erase(it);
    } else {
      ++it;
    }
  }
}

void BufferMonitors::cull(Event* event) {
  for (auto it = m_Targets.begin(); it != m_Targets.end();) {
    MonitorTarget* target = *it;
    if (target->pEvent == event) {
      delete target;
      it = m_Targets.erase(it);
    } else {
      ++it;
    }
  }
}
