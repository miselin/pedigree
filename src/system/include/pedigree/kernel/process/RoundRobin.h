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

#ifndef ROUND_ROBIN_H
#define ROUND_ROBIN_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/process/SchedulingAlgorithm.h"

class Thread;

class RoundRobin : public SchedulingAlgorithm
{
  public:
    /** Constructor. */
    RoundRobin();

    /** Destructor. */
    virtual ~RoundRobin();

    virtual void addThread(Thread *pThread);

    virtual void removeThread(Thread *pThread);

    virtual Thread *getNext(Thread *pCurrentThread);

    virtual void threadStatusChanged(Thread *pThread);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    /** Exercises intrusive ready-list invariants without scheduling. */
    static EXPORTED_PUBLIC bool runHostedIntrusiveQueueRegressions(
        Thread *pThread);
#endif

  private:
    static bool isReady(Thread *pThread);

    void enqueue(Thread *pThread);
    void unlink(Thread *pThread);

    Thread *m_pReadyQueueHeads[MAX_PRIORITIES];
    Thread *m_pReadyQueueTails[MAX_PRIORITIES];

    Spinlock m_Lock;
};

#endif
