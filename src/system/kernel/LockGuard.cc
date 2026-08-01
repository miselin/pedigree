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
#include "pedigree/kernel/Spinlock.h"
#if THREADS && !defined(STANDALONE_MUTEXES)
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Mutex.h"
#endif

template class LockGuard<Spinlock>;
template class RecursingLockGuard<Spinlock>;

#if THREADS && !defined(STANDALONE_MUTEXES)
LockGuard<Mutex>::LockGuard(Mutex &Lock, bool Condition)
    : m_TerminationDeferral(Condition), m_Lock(Lock),
      m_bCondition(Condition)
{
    if (m_bCondition && !m_Lock.acquireForCompletion())
    {
        m_bCondition = false;
        m_TerminationDeferral = TerminationDeferral(false);
        FATAL("LockGuard<Mutex> could not acquire its mutex");
    }
}

LockGuard<Mutex>::~LockGuard()
{
    if (m_bCondition)
    {
        m_Lock.release();
    }
}

void LockGuard<Mutex>::disown()
{
    m_bCondition = false;
    m_TerminationDeferral = TerminationDeferral(false);
}
#endif
