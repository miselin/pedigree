/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_OWNEDTHREAD_H
#define PEDIGREE_KERNEL_PROCESS_OWNEDTHREAD_H
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Thread.h"

#include <config.h>

/**
 * Owns one joinable worker and makes cancellation plus joining part of the
 * containing object's lifetime.
 */
class OwnedThread {
 public:
  OwnedThread() : m_pThread(nullptr) {}

  explicit OwnedThread(Thread* thread) : m_pThread(thread) {}

  ~OwnedThread() {
    stop();
  }

  void adopt(Thread* thread) {
    if (m_pThread || !thread || thread->detached()) {
      panic("OwnedThread cannot adopt this worker.");
    }
    m_pThread = thread;
  }

  void stop() {
    if (!m_pThread) {
      return;
    }

    Thread* thread = m_pThread;
    if (thread->getStatus() != Thread::AwaitingJoin) {
      thread->setUnwindState(Thread::TerminateThread);
    }
    join();
  }

  /** Joins a worker whose containing object has published its own stop. */
  void join() {
    if (!m_pThread) {
      return;
    }

    Thread* thread = m_pThread;
    if (!thread->joinForCompletion()) {
      panic("OwnedThread could not join its worker.");
    }
    m_pThread = nullptr;
  }

  Thread* get() const {
    return m_pThread;
  }

  Thread* operator->() const {
    return m_pThread;
  }

  explicit operator bool() const {
    return m_pThread != nullptr;
  }

 private:
  OwnedThread(const OwnedThread&) = delete;
  OwnedThread& operator=(const OwnedThread&) = delete;

  Thread* m_pThread;
};

#endif
