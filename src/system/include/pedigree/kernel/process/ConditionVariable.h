/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef KERNEL_PROCESS_CONDITIONVARIABLE_H
#define KERNEL_PROCESS_CONDITIONVARIABLE_H
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/new"

#include <config.h>

class Mutex;
class Thread;

/**
 * ConditionVariable provides an abstraction over condition variables.
 */
class EXPORTED_PUBLIC ConditionVariable {
 public:
  enum Error { NoError, TimedOut, Interrupted, TerminationDeferred, MutexNotLocked };

  /** Whether wait() returned with ownership of the supplied mutex. */
  static bool mutexAcquired(Error error) {
    return error != MutexNotLocked;
  }

  ConditionVariable();
  ~ConditionVariable();

  /** Wait for a signal on the condition variable with a specific timeout.
   *
   * The timeout specifies a relative deadline in nanoseconds. Zero requests
   * an immediate timeout; use Time::Infinity or the overload without a
   * timeout parameter to wait indefinitely. If the operation times out, the
   * value is set to zero. If it succeeds first, the value is the remaining
   * timeout.
   *
   * The mutex is reacquired for every returning outcome. Forced thread
   * termination returns TerminationDeferred with the mutex held so the caller
   * can retire stack-owned state while unwinding toward a thread boundary.
   *
   * \param[in] mutex an acquired mutex protecting the resource.
   * \param[inout] timeout a relative timeout in nanoseconds, zero for an
   * immediate deadline, or Time::Infinity for no deadline.
   */
  MUST_USE_RESULT bool wait(Mutex& mutex, Time::Timestamp& timeout, Error& error,
                            WaitQueue::StackDiscardCleanup onStackDiscard = nullptr,
                            void* stackDiscardContext = nullptr);

  /** Wait for a signal on the condition variable with no timeout. */
  MUST_USE_RESULT bool wait(Mutex& mutex, Error& error,
                            WaitQueue::StackDiscardCleanup onStackDiscard = nullptr,
                            void* stackDiscardContext = nullptr);

  /**
   * Wait for a lifetime predicate which must be rechecked with mutex held.
   *
   * Signal and terminal events are delivered but cannot abandon the wait or
   * mutex reacquisition. A delivered signal remains recorded on the current
   * Thread so an outer syscall boundary can still report interruption after
   * the protected lifetime state has drained.
   */
  void waitForCompletion(Mutex& mutex);

  /** Wake up at least one thread that is currently waiting. */
  void signal();

  /** Wake up all threads currently waiting. */
  void broadcast();

 private:
#if THREADS
  WaitQueue m_Waiters;
#endif

  /// Private data.
  void* m_Private;
};

#endif
