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

#ifndef UNLIKELY_LOCK_H
#define UNLIKELY_LOCK_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"

/** \file UnlikelyLock.h
    \author James Molloy
    \date 16 September 2009 */

#define UNLIKELY_LOCK_MAX_READERS 9999

/**
 * A reader/writer lock optimised for the case where exclusive access is rare.
 *
 * Admission waits internally until ownership is available. Signal delivery
 * remains visible to the outer operation, but cannot make a caller retry and
 * accidentally lose the interruption or bypass lock ownership.
 */
class EXPORTED_PUBLIC UnlikelyLock
{
  public:
    UnlikelyLock();
    ~UnlikelyLock();

    /** Enters the critical section after all active writers have left. */
    void enter();

    /** Leaving the critical section. */
    void leave();

    /** Locks the lock. Will not return until all other threads have exited
        the critical region. */
    void acquire();

    /** Releases the lock. */
    void release();

  private:
    Mutex m_Lock;
    ConditionVariable m_Condition;

    uint64_t m_nReaders;
    uint64_t m_nWaitingWriters;
    bool m_bActiveWriter;
};

#endif
