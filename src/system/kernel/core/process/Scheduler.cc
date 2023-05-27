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

#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/ProcessorThreadAllocator.h"
#include "pedigree/kernel/process/RoundRobinCoreAllocator.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

Scheduler Scheduler::m_Instance;

// Scheduler can be used at times where it is not yet safe to do the useful
// "safer" Spinlock deadlock detection.
#define SCHEDULER_HAS_SAFE_SPINLOCKS true

// Do we allow recursing in the Scheduler lock? Note that the lock surrounds
// memory operations (editing a List<T>), so if e.g. VirtualAddressSpace depends
// on Scheduler, you need to recurse.
#define SCHEDULER_HAS_RECURSIVE_SPINLOCKS true

Scheduler::Scheduler()
    : m_Processes(), m_NextPid(0), m_PTMap(), m_TPMap(), m_pKernelProcess(0),
      m_pBspScheduler(0), m_SchedulerLock(false)
{
}

bool Scheduler::initialise(Process *pKernelProcess)
{
    RoundRobinCoreAllocator *pRoundRobin = new RoundRobinCoreAllocator();
    ProcessorThreadAllocator::instance().setAlgorithm(pRoundRobin);

    m_pKernelProcess = pKernelProcess;

    List<PerProcessorScheduler *> procList;

#if MULTIPROCESSOR
    size_t i = 0;
    for (Vector<ProcessorInformation *>::Iterator it =
             Processor::m_ProcessorInformation.begin();
         it != Processor::m_ProcessorInformation.end(); it++, i += 2)
    {
        procList.pushBack(&((*it)->getScheduler()));
    }
#else
    procList.pushBack(&Processor::information().getScheduler());
#endif

    m_pBspScheduler = &Processor::information().getScheduler();

    pRoundRobin->initialise(procList);

    return true;
}

void Scheduler::addThread(Thread *pThread, PerProcessorScheduler &PPSched)
{
    m_SchedulerLock.acquire(
        SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
    m_TPMap.insert(pThread, &PPSched);
    m_SchedulerLock.release();
}

void Scheduler::removeThread(Thread *pThread)
{
    m_SchedulerLock.acquire(
        SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
    PerProcessorScheduler *pPpSched = m_TPMap.lookup(pThread);
    if (pPpSched)
    {
        pPpSched->removeThread(pThread);
        m_TPMap.remove(pThread);
    }
    m_SchedulerLock.release();
}

bool Scheduler::threadInSchedule(Thread *pThread)
{
    m_SchedulerLock.acquire(
        SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
    PerProcessorScheduler *pPpSched = m_TPMap.lookup(pThread);
    m_SchedulerLock.release();
    return pPpSched != 0;
}

size_t Scheduler::addProcess(Process *pProcess)
{
    m_SchedulerLock.acquire(
        SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
    m_Processes.pushBack(pProcess);
    size_t result = (m_NextPid += 1) - 1;  // little dance for Atomic
    m_SchedulerLock.release();
    return result;
}

void Scheduler::removeProcess(Process *pProcess)
{
    m_SchedulerLock.acquire(
        SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
    for (List<Process *>::Iterator it = m_Processes.begin();
         it != m_Processes.end(); it++)
    {
        if (*it == pProcess)
        {
            m_Processes.erase(it);
            break;
        }
    }
    m_SchedulerLock.release();
}

void Scheduler::yield()
{
    Processor::information().getScheduler().schedule();
}

size_t Scheduler::getNumProcesses()
{
    m_SchedulerLock.acquire(
        SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
    size_t result = m_Processes.count();
    m_SchedulerLock.release();
    return result;
}

Process *Scheduler::getProcess(size_t n)
{
    m_SchedulerLock.acquire(
        SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
    if (n >= m_Processes.count())
    {
        WARNING(
            "Scheduler::getProcess(" << Dec << n
                                     << ") parameter outside range.");
        m_SchedulerLock.release();
        return 0;
    }

    size_t i = 0;
    Process *pResult = 0;
    for (List<Process *>::Iterator it = m_Processes.begin();
         it != m_Processes.end(); it++)
    {
        if (i == n)
        {
            pResult = *it;
            break;
        }
        i++;
    }

    m_SchedulerLock.release();
    return pResult;
}

void Scheduler::threadStatusChanged(Thread *pThread)
{
    m_SchedulerLock.acquire(
        SCHEDULER_HAS_RECURSIVE_SPINLOCKS, SCHEDULER_HAS_SAFE_SPINLOCKS);
    PerProcessorScheduler *pSched = m_TPMap.lookup(pThread);
    assert(pSched);
    m_SchedulerLock.release();

    pSched->threadStatusChanged(pThread);
}

#endif
