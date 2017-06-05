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

#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/ZombieQueue.h"

#include "pedigree/kernel/process/SignalEvent.h"

#include "pedigree/kernel/Subsystem.h"

#include "modules/system/vfs/File.h"

Process *Process::m_pInitProcess = 0;

Process::Process()
    : m_Threads(), m_NextTid(0), m_Id(0), str(), m_pParent(0),
      m_pAddressSpace(&VirtualAddressSpace::getKernelAddressSpace()),
      m_ExitStatus(0), m_Cwd(0), m_Ctty(0), m_SpaceAllocator(false),
      m_DynamicSpaceAllocator(false), m_pUser(0), m_pGroup(0),
      m_pEffectiveUser(0), m_pEffectiveGroup(0), m_pDynamicLinker(0),
      m_pSubsystem(0), m_Waiters(), m_bUnreportedSuspend(false),
      m_bUnreportedResume(false), m_State(Active),
      m_BeforeSuspendState(Thread::Ready), m_Lock(false), m_Metadata(),
      m_LastKernelEntry(0), m_LastUserspaceEntry(0), m_pRootFile(0),
      m_bSharedAddressSpace(false), m_DeadThreads(0)
{
    resetCounts();
    m_Metadata.startTime = Time::getTimeNanoseconds();

    m_Id = Scheduler::instance().addProcess(this);
    getSpaceAllocator().free(
        getAddressSpace()->getUserStart(),
        getAddressSpace()->getUserReservedStart() -
            getAddressSpace()->getUserStart());
    if (getAddressSpace()->getDynamicStart())
    {
        getDynamicSpaceAllocator().free(
            getAddressSpace()->getDynamicStart(),
            getAddressSpace()->getDynamicEnd() -
                getAddressSpace()->getDynamicStart());
    }
}

Process::Process(Process *pParent, bool bCopyOnWrite)
    : m_Threads(), m_NextTid(0), m_Id(0), str(), m_pParent(pParent),
      m_pAddressSpace(0), m_ExitStatus(0), m_Cwd(pParent->m_Cwd),
      m_Ctty(pParent->m_Ctty), m_SpaceAllocator(pParent->m_SpaceAllocator),
      m_DynamicSpaceAllocator(pParent->m_DynamicSpaceAllocator),
      m_pUser(pParent->m_pUser), m_pGroup(pParent->m_pGroup),
      m_pEffectiveUser(pParent->m_pEffectiveUser),
      m_pEffectiveGroup(pParent->m_pEffectiveGroup),
      m_pDynamicLinker(pParent->m_pDynamicLinker), m_pSubsystem(0), m_Waiters(),
      m_bUnreportedSuspend(false), m_bUnreportedResume(false),
      m_State(pParent->getState()), m_BeforeSuspendState(Thread::Ready),
      m_Lock(false), m_Metadata(pParent->m_Metadata), m_LastKernelEntry(0),
      m_LastUserspaceEntry(0), m_pRootFile(pParent->m_pRootFile),
      m_bSharedAddressSpace(!bCopyOnWrite), m_DeadThreads(0)
{
    m_pAddressSpace = pParent->m_pAddressSpace->clone(bCopyOnWrite);

    m_Id = Scheduler::instance().addProcess(this);

    // Set a temporary description.
    str = m_pParent->str;
    if (m_bSharedAddressSpace)
    {
        str += "<C>";  // C for cloned (i.e. shared address space)
    }
    else
    {
        str += "<F>";  // F for forked.
    }
}

Process::~Process()
{
    // Make sure we have full mutual exclusion on the Subsystem before we lock
    // here. This ensures we have full access to the subsystem and avoids a case
    // where we lock here but the subsystem destruction needs to reschedule to
    // acquire the subsystem locks.
    if (m_pSubsystem)
    {
        m_pSubsystem->acquire();
    }

    bool isSelf =
        Processor::information().getCurrentThread()->getParent() == this;

    for (Vector<Thread *>::Iterator it = m_Threads.begin();
         it != m_Threads.end(); ++it)
    {
        Thread *pThread = (*it);

        // Clean up thread if not actually us.
        if (pThread != Processor::information().getCurrentThread())
        {
            // Child thread is not current thread - terminate the child
            // properly.
            pThread->setStatus(Thread::Zombie);
            pThread->shutdown();
        }
    }

    // Block until we are the only one touching this Process object.
    LockGuard<Spinlock> guard(m_Lock);

    // Guards things like removeThread.
    m_State = Terminating;

    // Now that all threads are shut down and marked as zombies, and we have
    // taken the main process Spinlock, we can clean up the detached threads.
    for (Vector<Thread *>::Iterator it = m_Threads.begin();
         it != m_Threads.end(); ++it)
    {
        Thread *t = (*it);
        if (t != Processor::information().getCurrentThread())
        {
            if (t->detached())
            {
                delete t;
            }
        }
    }

    Scheduler::instance().removeProcess(this);

    if (m_pSubsystem)
        delete m_pSubsystem;

    VirtualAddressSpace &VAddressSpace =
        Processor::information().getVirtualAddressSpace();

    bool bInterrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);

    Processor::switchAddressSpace(*m_pAddressSpace);
    m_pAddressSpace->revertToKernelAddressSpace();
    Processor::switchAddressSpace(VAddressSpace);

    delete m_pAddressSpace;

    Processor::setInterrupts(bInterrupts);

    if (isSelf)
    {
        // Killed current process, so kill off the thread too.
        // NOTE: this DOES NOT RETURN. Anything critical to process shutdown
        // must be completed by this point.
        Processor::information().getScheduler().killCurrentThread(&m_Lock);
    }
}

size_t Process::addThread(Thread *pThread)
{
    LockGuard<Spinlock> guard(m_Lock);
    if (!pThread)
        return ~0;
    m_Threads.pushBack(pThread);
    return m_NextTid += 1;
}

void Process::removeThread(Thread *pThread)
{
    // Don't bother in these states: already done, or is about to be done.
    if (m_State == Terminating || m_State == Terminated)
        return;

    LockGuard<Spinlock> guard(m_Lock);
    for (Vector<Thread *>::Iterator it = m_Threads.begin();
         it != m_Threads.end(); it++)
    {
        if (*it == pThread)
        {
            m_Threads.erase(it);
            break;
        }
    }

    if (m_pSubsystem)
        m_pSubsystem->threadRemoved(pThread);
}

size_t Process::getNumThreads()
{
    LockGuard<Spinlock> guard(m_Lock);
    return m_Threads.count();
}

Thread *Process::getThread(size_t n)
{
    LockGuard<Spinlock> guard(m_Lock);
    if (n >= m_Threads.count())
    {
        m_Lock.release();
        FATAL(
            "Process::getThread(" << Dec << n << Hex
                                  << ") - Parameter out of bounds.");
        return 0;
    }
    return m_Threads[n];
}

void Process::kill()
{
    m_Lock.acquire();

    if (m_pParent)
        NOTICE("Kill: " << m_Id << " (parent: " << m_pParent->getId() << ")");
    else
        NOTICE("Kill: " << m_Id << " (parent: <orphan>)");

    // Bye bye process - have we got any zombie children?
    for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); i++)
    {
        Process *pProcess = Scheduler::instance().getProcess(i);

        if (pProcess && (pProcess->m_pParent == this))
        {
            if (pProcess->getThread(0)->getStatus() == Thread::Zombie)
            {
                // Kill 'em all!
                delete pProcess;
            }
            else
            {
                pProcess->m_pParent = Process::getInit();
            }
        }
    }

    m_State = Terminated;

    // Add to the zombie queue if the process is an orphan.
    if (!m_pParent)
    {
        NOTICE(
            "Process::kill() - process is an orphan, adding to ZombieQueue.");

        ZombieQueue::instance().addObject(new ZombieProcess(this));
        Processor::information().getScheduler().killCurrentThread(&m_Lock);

        // Should never get here.
        FATAL("Process: should never get here");
    }

    // We'll get reaped elsewhere
    NOTICE(
        "Process::kill() - not adding to ZombieQueue, process has a parent.");
    Processor::information().getScheduler().schedule(
        Thread::Zombie, nullptr, &m_Lock);

    FATAL("Should never get here");
}

void Process::suspend()
{
    m_bUnreportedSuspend = true;
    m_ExitStatus = 0x7F;
    m_BeforeSuspendState = m_Threads[0]->getStatus();
    m_State = Suspended;
    notifyWaiters();
    // Notify parent that we're suspending.
    if (m_pParent && m_pParent->getSubsystem())
        m_pParent->getSubsystem()->threadException(
            m_pParent->getThread(0), Subsystem::Child);
    Processor::information().getScheduler().schedule(Thread::Suspended);
}

void Process::resume()
{
    m_bUnreportedResume = true;
    m_ExitStatus = 0xFF;
    m_State = Active;
    notifyWaiters();
    Processor::information().getScheduler().schedule(Thread::Ready);
}

void Process::addWaiter(Semaphore *pWaiter)
{
    m_Waiters.pushBack(pWaiter);
}

void Process::removeWaiter(Semaphore *pWaiter)
{
    for (List<Semaphore *>::Iterator it = m_Waiters.begin();
         it != m_Waiters.end();)
    {
        if ((*it) == pWaiter)
        {
            it = m_Waiters.erase(it);
        }
        else
            ++it;
    }
}

size_t Process::waiterCount() const
{
    return m_Waiters.count();
}

void Process::notifyWaiters()
{
    for (List<Semaphore *>::Iterator it = m_Waiters.begin();
         it != m_Waiters.end(); ++it)
    {
        (*it)->release();
    }
}

Process *Process::getInit()
{
    return m_pInitProcess;
}

void Process::setInit(Process *pProcess)
{
    if (m_pInitProcess)
    {
        return;
    }
    m_pInitProcess = pProcess;
}

#endif  // defined(THREADS)
