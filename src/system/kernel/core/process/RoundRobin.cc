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

#if THREADS
#include "pedigree/kernel/process/RoundRobin.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

RoundRobin::RoundRobin() : m_Lock(false)
{
}

RoundRobin::~RoundRobin()
{
}

void RoundRobin::addThread(Thread *pThread)
{
}

void RoundRobin::removeThread(Thread *pThread)
{
    LockGuard<Spinlock> guard(m_Lock);

    for (size_t i = 0; i < MAX_PRIORITIES; i++)
    {
        for (ThreadList::Iterator it = m_pReadyQueues[i].begin();
             it != m_pReadyQueues[i].end(); it++)
        {
            if (*it == pThread)
            {
                m_pReadyQueues[i].erase(it);
                return;
            }
        }
    }
}

Thread *RoundRobin::getNext(Thread *pCurrentThread)
{
    LockGuard<Spinlock> guard(m_Lock);

    Thread *pThread = 0;
    for (size_t i = 0; i < MAX_PRIORITIES; i++)
    {
        if (m_pReadyQueues[i].size())
        {
            pThread = m_pReadyQueues[i].popFront();
            if (pThread == pCurrentThread)
                continue;

            if (pThread)
            {
                return pThread;
            }
        }
    }
    return 0;
}

void RoundRobin::threadStatusChanged(Thread *pThread)
{
    if (RoundRobin::isReady(pThread))
    {
        assert(pThread->getPriority() < MAX_PRIORITIES);

        for (List<Thread *>::Iterator it =
                 m_pReadyQueues[pThread->getPriority()].begin();
             it != m_pReadyQueues[pThread->getPriority()].end(); ++it)
        {
            if ((*it) == pThread)
            {
                // WARNING("RoundRobin: A thread was already in this priority
                // queue");
                return;
            }
        }

        m_pReadyQueues[pThread->getPriority()].pushBack(pThread);
    }
}

bool RoundRobin::isReady(Thread *pThread)
{
    return pThread->getStatus() == Thread::Ready;
}

#endif
