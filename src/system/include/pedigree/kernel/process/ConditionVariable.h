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

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Result.h"
#include "pedigree/kernel/utilities/new"

class Mutex;
class Thread;

/**
 * ConditionVariable provides an abstraction over condition variables.
 */
class EXPORTED_PUBLIC ConditionVariable
{
  public:
    enum Error
    {
        NoError,
        TimedOut,
        ThreadTerminating,
        MutexNotLocked,
        MutexNotAcquired
    };

    typedef Result<bool, Error> WaitResult;

    ConditionVariable();
    ~ConditionVariable();

    /** Wait for a signal on the condition variable with a specific timeout.
     *
     * If the given timeout is non-zero, it specifies a timeout for the wait
     * operation. If the operation times out, the value will be set to zero.
     * If the operation succeeds before the timeout expires, the value will be
     * the amount of time remaining in the timeout.
     *
     * \param[in] mutex an acquired mutex protecting the resource.
     * \param[inout] timeout a timeout in nanoseconds to wait (or zero for
     * none).
     */
    WaitResult wait(Mutex &mutex, Time::Timestamp &timeout);

    /** Wait for a signal on the condition variable with no timeout. */
    WaitResult wait(Mutex &mutex);

    /** Wake up at least one thread that is currently waiting. */
    void signal();

    /** Wake up all threads currently waiting. */
    void broadcast();

  private:
    /// Lock around m_Waiters.
    Spinlock m_Lock;

    /// Threads waiting for a signal.
    List<Thread *> m_Waiters;

    /// Private data.
    void *m_Private;
};

#endif
