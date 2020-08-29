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

#include "pedigree/kernel/process/RoundRobinCoreAllocator.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

class PerProcessorScheduler;
class Thread;

RoundRobinCoreAllocator::RoundRobinCoreAllocator() : m_ProcMap(), m_pNext(0)
{
}

RoundRobinCoreAllocator::~RoundRobinCoreAllocator()
{
}

bool RoundRobinCoreAllocator::initialise(
    List<PerProcessorScheduler *> &procList)
{
    List<PerProcessorScheduler *>::Iterator it = procList.begin();
    PerProcessorScheduler *pFirst = m_pNext = *it;
    it++;

    // 1 CPU?
    if (it == procList.end())
    {
        NOTICE("RoundRobinCoreAllocator: quitting, only one CPU was present.");
        m_ProcMap.insert(pFirst, pFirst);
        return true;
    }

    for (; it != procList.end(); it++)
    {
        m_ProcMap.insert(pFirst, *it);
        pFirst = *it;
    }

    // Loop.
    m_ProcMap.insert(pFirst, m_pNext);

    return true;
}

PerProcessorScheduler *RoundRobinCoreAllocator::allocateThread(Thread *pThread)
{
    PerProcessorScheduler *pReturn = m_ProcMap.lookup(m_pNext);
    m_pNext = pReturn;
    return pReturn;
}
