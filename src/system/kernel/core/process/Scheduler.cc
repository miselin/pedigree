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

#if defined(THREADS)

#include <process/Scheduler.h>
#include <processor/Processor.h>
#include <Log.h>
#include <utilities/assert.h>
#include <process/PerProcessorScheduler.h>
#include <process/RoundRobinCoreAllocator.h>
#include <process/ProcessorThreadAllocator.h>

Scheduler Scheduler::m_Instance;

Scheduler::Scheduler() :
    m_Processes(), m_NextPid(0), m_PTMap(), m_TPMap(), m_pKernelProcess(0)
{
}

Scheduler::~Scheduler()
{
}

bool Scheduler::initialise(Process *pKernelProcess)
{
  RoundRobinCoreAllocator *pRoundRobin = new RoundRobinCoreAllocator();
  ProcessorThreadAllocator::instance().setAlgorithm(pRoundRobin);

  m_pKernelProcess = pKernelProcess;
  
  List<PerProcessorScheduler*> procList;
  
#ifdef MULTIPROCESSOR
  size_t i = 0;
  for (Vector<ProcessorInformation*>::Iterator it = Processor::m_ProcessorInformation.begin();
       it != Processor::m_ProcessorInformation.end();
       it++, i += 2)
  {
    procList.pushBack(&((*it)->getScheduler()));
  }
#else
  procList.pushBack(&Processor::information().getScheduler());
#endif
  
  pRoundRobin->initialise(procList);

  return true;
}

void Scheduler::addThread(Thread *pThread, PerProcessorScheduler &PPSched)
{
    m_TPMap.insert(pThread, &PPSched);
}

void Scheduler::removeThread(Thread *pThread)
{
    PerProcessorScheduler *pPpSched = m_TPMap.lookup(pThread);
    if (pPpSched)
    {
        pPpSched->removeThread(pThread);
        m_TPMap.remove(pThread);
    }
}

bool Scheduler::threadInSchedule(Thread *pThread)
{
    PerProcessorScheduler *pPpSched = m_TPMap.lookup(pThread);
    return pPpSched != 0;
}

size_t Scheduler::addProcess(Process *pProcess)
{
  m_Processes.pushBack(pProcess);
  return (m_NextPid += 1);
}

void Scheduler::removeProcess(Process *pProcess)
{
  for(List<Process*>::Iterator it = m_Processes.begin();
      it != m_Processes.end();
      it++)
  {
    if (*it == pProcess)
    {
      m_Processes.erase(it);
      break;
    }
  }
}

void Scheduler::yield()
{
  Processor::information().getScheduler().schedule();
}

size_t Scheduler::getNumProcesses()
{
  return m_Processes.count();
}

Process *Scheduler::getProcess(size_t n)
{
  if (n >= m_Processes.count())
  {
    WARNING("Scheduler::getProcess(" << Dec << n << ") parameter outside range.");
    return 0;
  }
  size_t i = 0;
  for(List<Process*>::Iterator it = m_Processes.begin();
      it != m_Processes.end();
      it++)
  {
    if (i == n)
      return *it;
    i++;
  }

  return 0;
}

void Scheduler::threadStatusChanged(Thread *pThread)
{
    PerProcessorScheduler *pSched = m_TPMap.lookup(pThread);
    assert(pSched);
    pSched->threadStatusChanged(pThread);
}

#endif
