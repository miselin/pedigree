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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

namespace {
struct ConditionStackDiscard {
  void* alarmHandle;
  WaitQueue::StackDiscardCleanup caller;
  void* callerContext;
};

void discardConditionWait(void* context) {
  ConditionStackDiscard* discard = reinterpret_cast<ConditionStackDiscard*>(context);
  void* alarmHandle = discard->alarmHandle;
  WaitQueue::StackDiscardCleanup caller = discard->caller;
  void* callerContext = discard->callerContext;
  discard->alarmHandle = nullptr;
  discard->caller = nullptr;
  discard->callerContext = nullptr;

  if (alarmHandle) {
    Time::removeAlarm(alarmHandle);
  }
  if (caller) {
    caller(callerContext);
  }
}
}  // namespace

ConditionVariable::ConditionVariable() : m_Waiters() {}

ConditionVariable::~ConditionVariable() {}

bool ConditionVariable::wait(Mutex& mutex, Error& error,
                             WaitQueue::StackDiscardCleanup onStackDiscard,
                             void* stackDiscardContext) {
  Time::Timestamp timeout = Time::Infinity;
  return wait(mutex, timeout, error, onStackDiscard, stackDiscardContext);
}

bool ConditionVariable::wait(Mutex& mutex, Time::Timestamp& timeout, Error& error,
                             WaitQueue::StackDiscardCleanup onStackDiscard,
                             void* stackDiscardContext) {
  error = NoError;
  Time::Timestamp startTime = Time::getTicks();

  if (!mutex.isOwnedByCurrentThread()) {
    WARNING("ConditionVariable::wait called without owning its mutex");
    error = MutexNotLocked;
    return false;
  }

  if (timeout == 0) {
    error = TimedOut;
    return false;
  }

  Thread* me = Processor::information().getCurrentThread();
  me->clearInterruption();

  auto guard = m_Waiters.acquire();

  ConditionStackDiscard discard = {nullptr, onStackDiscard, stackDiscardContext};
  if (timeout != Time::Infinity) {
    discard.alarmHandle = Time::addAlarm(timeout);
  }
  Thread::StackDiscardScope discardScope(
      (discard.alarmHandle || onStackDiscard) ? &discardConditionWait : nullptr, &discard);

  uintptr_t ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  WaitQueue::WakeReason wakeReason =
      guard.waitAndUnlock(mutex, WaitQueue::Channel(), Thread::CondWait, ra);

  // Event delivery can follow an ordinary wake which already won
  // waiter.reason. The per-wait marker remains authoritative.
  const Thread::InterruptionReason interruption = me->getInterruptionReason();

  // Woken up by something. Remove any alarm we have pending as we're
  // finishing our wait now.
  if (discard.alarmHandle) {
    void* alarmHandle = discard.alarmHandle;
    discard.alarmHandle = nullptr;
    Time::removeAlarm(alarmHandle);
  }

  me->clearInterruption();

  if (interruption == Thread::InterruptedByTimeout) {
    error = TimedOut;
  } else if (interruption == Thread::InterruptedBySignal) {
    error = Interrupted;
  } else if (wakeReason == WaitQueue::WakeReason::Unwinding ||
             wakeReason == WaitQueue::WakeReason::Terminating) {
    // WaitQueue has already reacquired mutex, so callers can retire
    // stack-owned state while propagating the terminal request.
    error = TerminationDeferred;
  }

  Time::Timestamp endTime = Time::getTicks();

  // Update timeout value to suit. We want to be able to make consecutive
  // calls to wait() without changing the timeout value to allow for wakeups
  // with an unchanged time limit.
  if (timeout != Time::Infinity) {
    Time::Timestamp timeConsumed = endTime - startTime;
    if (timeConsumed >= timeout) {
      timeout = 0;
    } else {
      timeout -= timeConsumed;
    }
  }

  return error == NoError;
}

void ConditionVariable::waitForCompletion(Mutex& mutex) {
  if (!mutex.isOwnedByCurrentThread()) {
    FATAL(
        "ConditionVariable::waitForCompletion called without owning "
        "its mutex");
  }

  auto guard = m_Waiters.acquire();
  uintptr_t ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  const WaitQueue::WakeReason reason =
      guard.waitAndUnlockForCompletion(mutex, WaitQueue::Channel(), Thread::CondWait, ra);
  (void)reason;
}

void ConditionVariable::signal() {
  m_Waiters.wakeOne();
}

void ConditionVariable::broadcast() {
  m_Waiters.wakeAll();
}
