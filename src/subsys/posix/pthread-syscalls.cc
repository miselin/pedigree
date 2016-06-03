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

#include <process/Scheduler.h>
#include <pthread-syscalls.h>
#include <syscallError.h>
#include "errors.h"
#include "PosixSubsystem.h"

extern "C"
{
    extern void pthread_stub();
    extern char pthread_stub_end;
}

/**
 * Forcefully registers the given thread with the given PosixSubsystem.
 */
void pedigree_copy_posix_thread(Thread *origThread, PosixSubsystem *origSubsystem, Thread *newThread, PosixSubsystem *newSubsystem)
{
    PosixSubsystem::PosixThread *pOldPosixThread = origSubsystem->getThread(origThread->getId());
    if (!pOldPosixThread)
    {
        // Nothing to see here.
        return;
    }

    PosixSubsystem::PosixThread *pNewPosixThread = new PosixSubsystem::PosixThread;
    pNewPosixThread->pThread = newThread;
    pNewPosixThread->returnValue = 0;

    // Copy thread-specific data across.
    for (Tree<size_t, PosixSubsystem::PosixThreadKey *>::Iterator it = pOldPosixThread->m_ThreadData.begin();
        it != pOldPosixThread->m_ThreadData.end();
        ++it)
    {
        size_t key = it.key();
        PosixSubsystem::PosixThreadKey *data = it.value();

        pNewPosixThread->addThreadData(key, data);
        pNewPosixThread->m_ThreadKeys.set(key);
    }

    pNewPosixThread->lastDataKey = pOldPosixThread->lastDataKey;
    pNewPosixThread->nextDataKey = pOldPosixThread->nextDataKey;

    newSubsystem->insertThread(newThread->getId(), pNewPosixThread);
}

/**
 * pedigree_init_pthreads
 *
 * This function copies the user mode thread wrapper from the kernel to a known
 * user mode location. The location is already mapped by pedigree_init_signals
 * which must be called before this function.
 */
void pedigree_init_pthreads()
{
    PT_NOTICE("init_pthreads");
    // Make sure we can write to the trampoline area.
    Processor::information().getVirtualAddressSpace().setFlags(
            reinterpret_cast<void*> (Event::getTrampoline()),
            VirtualAddressSpace::Write);
    MemoryCopy(reinterpret_cast<void*>(Event::getSecondaryTrampoline()), reinterpret_cast<void*>(pthread_stub), (reinterpret_cast<uintptr_t>(&pthread_stub_end) - reinterpret_cast<uintptr_t>(pthread_stub)));
    Processor::information().getVirtualAddressSpace().setFlags(
            reinterpret_cast<void*> (Event::getTrampoline()),
            VirtualAddressSpace::Execute | VirtualAddressSpace::Shared);

    // Make sure the main thread is actually known.
    Thread *pThread = Processor::information().getCurrentThread();
    Process *pProcess = pThread->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if(!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return;
    }

    PosixSubsystem::PosixThread *pPosixThread = new PosixSubsystem::PosixThread;
    pPosixThread->pThread = pThread;
    pPosixThread->returnValue = 0;
    pSubsystem->insertThread(pThread->getId(), pPosixThread);
}

void *posix_pedigree_create_waiter()
{
    PT_NOTICE("posix_pedigree_create_waiter");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if(!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return 0;
    }

    Semaphore *sem = new Semaphore(0);
    void *descriptor = pSubsystem->insertThreadWaiter(sem);
    if (!descriptor)
    {
        delete sem;
    }

    return descriptor;
}

int posix_pedigree_thread_wait_for(void *waiter)
{
    PT_NOTICE("posix_pedigree_thread_wait_for");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if(!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    Semaphore *sem = pSubsystem->getThreadWaiter(waiter);
    if (!sem)
    {
        return -1;
    }

    // Deadlock detection - don't wait if nothing can wake this waiter.
    /// \todo Check for more than just one thread - there's probably other
    ///       detections we can do here.
    if (pProcess->getNumThreads() <= 1)
    {
        SYSCALL_ERROR(Deadlock);
        return -1;
    }

    while(!sem->acquire(1));

    return 0;
}

int posix_pedigree_thread_trigger(void *waiter)
{
    PT_NOTICE("posix_pedigree_thread_trigger");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if(!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return 0;
    }

    Semaphore *sem = pSubsystem->getThreadWaiter(waiter);
    if (!sem)
        return 0;
    if (sem->getValue())
        return 0;  // Nothing to wake up.

    // Wake up a waiter.
    sem->release();
    return 1;
}

void posix_pedigree_destroy_waiter(void *waiter)
{
    PT_NOTICE("posix_pedigree_destroy_waiter");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if(!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return;
    }

    Semaphore *sem = pSubsystem->getThreadWaiter(waiter);
    if (!sem)
    {
        return;
    }
    pSubsystem->removeThreadWaiter(waiter);
    delete sem;
}
