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

#include <PosixSubsystem.h>
#include <Log.h>

#include <process/Thread.h>
#include <processor/types.h>
#include <processor/Processor.h>
#include <process/SignalEvent.h>
#include <process/Scheduler.h>

#include <utilities/RadixTree.h>
#include <utilities/Tree.h>
#include <LockGuard.h>

#include <utilities/assert.h>

#include "PosixProcess.h"

#include <linker/DynamicLinker.h>
#include <vfs/File.h>
#include <vfs/LockedFile.h>

// #define POSIX_SUBSYS_DEBUG

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2

#define	FD_CLOEXEC	1

typedef Tree<size_t, PosixSubsystem::SignalHandler*> sigHandlerTree;
typedef Tree<size_t, FileDescriptor*> FdMap;

RadixTree<LockedFile*> g_PosixGlobalLockedFiles;

ProcessGroupManager ProcessGroupManager::m_Instance;

ProcessGroupManager::ProcessGroupManager() : m_GroupIds()
{
    m_GroupIds.set(0);
}

ProcessGroupManager::~ProcessGroupManager()
{
}

size_t ProcessGroupManager::allocateGroupId()
{
    size_t bit = m_GroupIds.getFirstClear();
    m_GroupIds.set(bit);
    return bit;
}

void ProcessGroupManager::setGroupId(size_t gid)
{
    if(m_GroupIds.test(gid))
        WARNING("ProcessGroupManager: setGroupId called on a group ID that existed already!");
    m_GroupIds.set(gid);
}

bool ProcessGroupManager::isGroupIdValid(size_t gid)
{
    return m_GroupIds.test(gid);
}

void ProcessGroupManager::returnGroupId(size_t gid)
{
    m_GroupIds.clear(gid);
}

/// Default constructor
FileDescriptor::FileDescriptor() :
    file(0), offset(0), fd(0xFFFFFFFF), fdflags(0), flflags(0),
    so_domain(0), so_type(0), so_local(0), lockedFile(0)
{
}

/// Parameterised constructor
FileDescriptor::FileDescriptor(File *newFile, uint64_t newOffset, size_t newFd, int fdFlags, int flFlags, LockedFile *lf) :
    file(newFile), offset(newOffset), fd(newFd), fdflags(fdFlags), flflags(flFlags), lockedFile(lf)
{
    if(file)
    {
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
}

/// Copy constructor
FileDescriptor::FileDescriptor(FileDescriptor &desc) :
    file(desc.file), offset(desc.offset), fd(desc.fd), fdflags(desc.fdflags), flflags(desc.flflags), lockedFile(0)
{
    if(file)
    {
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
}

/// Pointer copy constructor
FileDescriptor::FileDescriptor(FileDescriptor *desc) :
    file(0), offset(0), fd(0), fdflags(0), flflags(0), lockedFile(0)
{
    if(!desc)
        return;

    file = desc->file;
    offset = desc->offset;
    fd = desc->fd;
    fdflags = desc->fdflags;
    flflags = desc->flflags;
    if(file)
    {
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
}

/// Assignment operator implementation
FileDescriptor &FileDescriptor::operator = (FileDescriptor &desc)
{
    file = desc.file;
    offset = desc.offset;
    fd = desc.fd;
    fdflags = desc.fdflags;
    flflags = desc.flflags;
    if(file)
    {
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
    return *this;
}

/// Destructor - decreases file reference count
FileDescriptor::~FileDescriptor()
{
    if(file)
    {
        // Unlock the file we have a lock on, release from the global lock table
        if(lockedFile)
        {
            g_PosixGlobalLockedFiles.remove(file->getFullPath());
            lockedFile->unlock();
            delete lockedFile;
        }
        file->decreaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
}

PosixSubsystem::PosixSubsystem(PosixSubsystem &s) :
    Subsystem(s), m_SignalHandlers(), m_SignalHandlersLock(), m_FdMap(), m_NextFd(s.m_NextFd),
    m_FdLock(), m_FdBitmap(), m_LastFd(0), m_FreeCount(s.m_FreeCount),
    m_AltSigStack(), m_SyncObjects(), m_Threads(), m_ThreadWaiters(),
    m_NextThreadWaiter(1)
{
    while(!m_SignalHandlersLock.acquire());
    while(!s.m_SignalHandlersLock.enter());

    // Copy all signal handlers
    for(sigHandlerTree::Iterator it = s.m_SignalHandlers.begin(); it != s.m_SignalHandlers.end(); it++)
    {
        size_t key = it.key();
        void *value = it.value();
        if(!value)
            continue;

        SignalHandler *newSig = new SignalHandler(*reinterpret_cast<SignalHandler *>(value));
        m_SignalHandlers.insert(key, newSig);
    }

    s.m_SignalHandlersLock.leave();
    m_SignalHandlersLock.release();

    // Copy across waiter state.
    for(Tree<void *, Semaphore *>::Iterator it = s.m_ThreadWaiters.begin();
        it != s.m_ThreadWaiters.end();
        ++it)
    {
        void *key = it.key();

        Semaphore *sem = new Semaphore(0);
        m_ThreadWaiters.insert(key, sem);
    }

    m_NextThreadWaiter = s.m_NextThreadWaiter;
}

PosixSubsystem::~PosixSubsystem()
{
    assert(--m_FreeCount == 0);

    // Ensure that no descriptor operations are taking place (and then, will take place)
    while(!m_FdLock.acquire());

    // Modifying signal handlers, ensure that they are not in use
    while(!m_SignalHandlersLock.acquire());

    // Destroy all signal handlers
    for(sigHandlerTree::Iterator it = m_SignalHandlers.begin(); it != m_SignalHandlers.end(); it++)
    {
        // Get the signal handler and remove it. Note that there shouldn't be null
        // SignalHandlers, at all.
        SignalHandler *sig = it.value();
        assert(sig);

        // SignalHandler's destructor will delete the Event itself
        delete sig;
    }

    // And now that the signals are destroyed, remove them from the Tree
    m_SignalHandlers.clear();

    m_SignalHandlersLock.release();

    // For sanity's sake, destroy any remaining descriptors
    m_FdLock.release();
    freeMultipleFds();

    // Remove any POSIX threads that might still be lying around
    for(Tree<size_t, PosixThread *>::Iterator it = m_Threads.begin(); it != m_Threads.end(); it++)
    {
        PosixThread *thread = it.value();
        assert(thread); // There shouldn't have ever been a null PosixThread in there

        // If the thread is still running, it should be killed
        if(!thread->isRunning.tryAcquire())
        {
            WARNING("PosixSubsystem object freed when a thread is still running?");
            // Thread will just stay running, won't be deallocated or killed
        }

        // Clean up any thread-specific data
        for(Tree<size_t, PosixThreadKey *>::Iterator it2 = thread->m_ThreadData.begin(); it2 != thread->m_ThreadData.end(); it2++)
        {
            PosixThreadKey *p = reinterpret_cast<PosixThreadKey *>(it.value());
            assert(p);

            /// \todo Call the destructor (need a way to call into userspace and return back here)
            delete p;
        }

        thread->m_ThreadData.clear();
        delete thread;
    }

    m_Threads.clear();

    // Clean up synchronisation objects
    for(Tree<size_t, PosixSyncObject *>::Iterator it = m_SyncObjects.begin(); it != m_SyncObjects.end(); it++)
    {
        PosixSyncObject *p = it.value();
        assert(p);

        if(p->pObject)
        {
            if(p->isMutex)
                delete reinterpret_cast<Mutex*>(p->pObject);
            else
                delete reinterpret_cast<Semaphore*>(p->pObject);
        }
    }

    m_SyncObjects.clear();

    for(Tree<void *, Semaphore *>::Iterator it = m_ThreadWaiters.begin();
        it != m_ThreadWaiters.end();
        ++it)
    {
        // Wake up everything waiting and then destroy the waiter object.
        Semaphore *sem = it.value();
        sem->release(-sem->getValue());
        delete sem;
    }

    m_ThreadWaiters.clear();

    // Spinlock as a quick way of disabling interrupts.
    Spinlock spinlock;
    spinlock.acquire();

    // Switch to the address space of the process we're destroying.
    // We need to unmap memory maps, and we can't do that in our address space.
    VirtualAddressSpace &curr = Processor::information().getVirtualAddressSpace();
    VirtualAddressSpace *va = m_pProcess->getAddressSpace();

    if(va != &curr) {
        // Switch into the address space we want to unmap inside.
        Processor::switchAddressSpace(*va);
    }

    // Remove all existing mappings, if any.
    MemoryMapManager::instance().unmapAll();

    if(va != &curr) {
        Processor::switchAddressSpace(curr);
    }

    spinlock.release();
}

bool PosixSubsystem::checkAddress(uintptr_t addr, size_t extent, size_t flags)
{
#ifdef POSIX_SUBSYS_DEBUG
    NOTICE("PosixSubsystem::checkAddress(" << addr << ", " << extent << ", " << flags << ")");
#endif

    // No memory access expected, all good.
    if(!extent)
    {
#ifdef POSIX_SUBSYS_DEBUG
        NOTICE("  -> zero extent, address is sane.");
#endif
        return true;
    }

    // Check address range.
    VirtualAddressSpace &va = Processor::information().getVirtualAddressSpace();
    if((addr < va.getUserStart()) || (addr >= va.getKernelStart()))
    {
#ifdef POSIX_SUBSYS_DEBUG
        NOTICE("  -> outside of user address area.");
#endif
        return false;
    }

    // Short-circuit if this is a memory mapped region.
    if(MemoryMapManager::instance().contains(addr, extent))
    {
#ifdef POSIX_SUBSYS_DEBUG
        NOTICE("  -> inside memory map.");
#endif
        return true;
    }

    // Check the range.
    for(size_t i = 0; i < extent; i+= PhysicalMemoryManager::getPageSize())
    {
        void *pAddr = reinterpret_cast<void *>(addr + i);
        if(!va.isMapped(pAddr))
        {
#ifdef POSIX_SUBSYS_DEBUG
            NOTICE("  -> not mapped.");
#endif
            return false;
        }

        if(flags & SafeWrite)
        {
            size_t vFlags = 0;
            physical_uintptr_t phys = 0;
            va.getMapping(pAddr, phys, vFlags);

            if(!(vFlags & (VirtualAddressSpace::Write | VirtualAddressSpace::CopyOnWrite)))
            {
#ifdef POSIX_SUBSYS_DEBUG
                NOTICE("  -> not writeable.");
#endif
                return false;
            }
        }
    }

    return true;
}

void PosixSubsystem::exit(int code)
{
    Thread *pThread = Processor::information().getCurrentThread();

    Process *pProcess = pThread->getParent();
    pProcess->markTerminating();

    if (pProcess->getExitStatus() == 0 || // Normal exit.
        pProcess->getExitStatus() == 0x7F || // Suspended.
        pProcess->getExitStatus() == 0xFF) // Continued.
        pProcess->setExitStatus( (code&0xFF) << 8 );
    if(code)
    {
        WARNING("Sending unexpected exit event (" << code << ") to thread");
        pThread->unexpectedExit();
    }

    // Exit called, but we could be at any nesting level in the event stack.
    // We have to propagate this exit() to all lower stack levels because they may have
    // semaphores and stuff open.

    // So, if we're not dealing with the lowest in the stack...
    /// \note If we're at state level one, we're potentially running as a thread that has
    ///       had an event sent to it from another process. If this is changed to > 0, it
    ///       is impossible to return to a shell when a segfault occurs in an app.
    if (pThread->getStateLevel() > 1)
    {
        // OK, we have other events running. They'll have to die first before we can do anything.
        pThread->setUnwindState(Thread::Exit);

        Thread *pBlockingThread = pThread->getBlockingThread(pThread->getStateLevel()-1);
        while (pBlockingThread)
        {
            pBlockingThread->setUnwindState(Thread::ReleaseBlockingThread);
            pBlockingThread = pBlockingThread->getBlockingThread();
        }

        Processor::information().getScheduler().eventHandlerReturned();
    }
    Processor::setInterrupts(false);

    // We're the lowest in the stack, so we can proceed with the exit function.

    delete pProcess->getLinker();

    MemoryMapManager::instance().unmapAll();

    // If it's a POSIX process, remove group membership
    if(pProcess->getType() == Process::Posix)
    {
        PosixProcess *p = static_cast<PosixProcess*>(pProcess);
        ProcessGroup *pGroup = p->getProcessGroup();
        if(pGroup && (p->getGroupMembership() == PosixProcess::Member))
        {
            for(List<PosixProcess*>::Iterator it = pGroup->Members.begin(); it != pGroup->Members.end(); it++)
            {
                if((*it) == p)
                {
                    it = pGroup->Members.erase(it);
                    break;
                }
            }
        }
        else if(pGroup && (p->getGroupMembership() == PosixProcess::Leader))
        {
            // Pick a new process to be the leader, remove this one from the list
            PosixProcess *pNewLeader = 0;
            for(List<PosixProcess*>::Iterator it = pGroup->Members.begin(); it != pGroup->Members.end();)
            {
                if((*it) == p)
                    it = pGroup->Members.erase(it);
                else
                {
                    if(!pNewLeader)
                        pNewLeader = *it;
                    ++it;
                }
            }

            // Set the new leader
            if(pNewLeader)
            {
                pNewLeader->setGroupMembership(PosixProcess::Leader);
                pGroup->Leader = pNewLeader;
            }
            else
            {
                // No new leader! Destroy the group, we're the last process in it.
                delete pGroup;
                pGroup = 0;
            }
        }
    }

    // Notify parent that we terminated (we may be in a separate process group).
    Process *pParent = pProcess->getParent();
    if(pParent && pParent->getSubsystem())
    {
        pParent->getSubsystem()->threadException(pParent->getThread(0), Child);
    }

    // Clean up the descriptor table
    freeMultipleFds();

    pProcess->kill();

    // Should NEVER get here.
    FATAL("PosixSubsystem::exit() running after Process::kill()!");
}

bool PosixSubsystem::kill(KillReason killReason, Thread *pThread)
{
    // Send SIGKILL. getSignalHandler handles all that locking shiz for us.
    SignalHandler *sig = getSignalHandler(killReason == Interrupted ? 2 : 9);
    if(!pThread)
        pThread = Processor::information().getCurrentThread();

    if(sig && sig->pEvent)
    {
        // Send the kill event
        /// \todo we probably want to avoid allocating a new stack..
        pThread->sendEvent(sig->pEvent);

        // Allow the event to run
        Processor::setInterrupts(true);
        Scheduler::instance().yield();
    }

    return true;
}

void PosixSubsystem::threadException(Thread *pThread, ExceptionType eType)
{
    NOTICE_NOLOCK("PosixSubsystem::threadException");

    // What was the exception?
    SignalHandler *sig = 0;
    switch(eType)
    {
        case PageFault:
            NOTICE_NOLOCK("    (Page fault)");
            // Send SIGSEGV
            sig = getSignalHandler(11);
            break;
        case InvalidOpcode:
            NOTICE_NOLOCK("    (Invalid opcode)");
            // Send SIGILL
            sig = getSignalHandler(4);
            break;
        case GeneralProtectionFault:
            NOTICE_NOLOCK("    (General Fault)");
            // Send SIGSEGV
            sig = getSignalHandler(14);
            break;
        case DivideByZero:
            NOTICE_NOLOCK("    (Division by zero)");
            // Send SIGFPE
            sig = getSignalHandler(8);
            break;
        case FpuError:
            NOTICE_NOLOCK("    (FPU error)");
            // Send SIGFPE
            sig = getSignalHandler(8);
            break;
        case SpecialFpuError:
            NOTICE_NOLOCK("    (FPU error - special)");
            // Send SIGFPE
            sig = getSignalHandler(8);
            break;
        case TerminalInput:
            NOTICE_NOLOCK("    (Attempt to read from terminal by non-foreground process)");
            // Send SIGTTIN
            sig = getSignalHandler(21);
            break;
        case TerminalOutput:
            NOTICE_NOLOCK("    (Output to terminal by non-foreground process)");
            // Send SIGTTOU
            sig = getSignalHandler(22);
            break;
        case Continue:
            NOTICE_NOLOCK("    (Continuing a stopped process)");
            // Send SIGCONT
            sig = getSignalHandler(19);
            break;
        case Stop:
            NOTICE_NOLOCK("    (Stopping a process)");
            // Send SIGTSTP
            sig = getSignalHandler(18);
            break;
        case Interrupt:
            NOTICE_NOLOCK("    (Interrupting a process)");
            // Send SIGINT
            sig = getSignalHandler(2);
            break;
        case Quit:
            NOTICE_NOLOCK("    (Requesting quit)");
            // Send SIGTERM
            sig = getSignalHandler(15);
            break;
        case Child:
            NOTICE_NOLOCK("    (Child status changed)");
            // Send SIGCHLD
            sig = getSignalHandler(20);
            break;
        default:
            NOTICE_NOLOCK("    (Unknown)");
            // Unknown exception
            ERROR_NOLOCK("Unknown exception type in threadException - POSIX subsystem");
            break;
    }

    // If we're good to go, send the signal.
    if(sig && sig->pEvent)
    {
        Thread *pCurrentThread = Processor::information().getCurrentThread();

        pThread->sendEvent(sig->pEvent);

        if(pCurrentThread == pThread)
        {
            // Attempt to execute the new event immediately.
            Processor::information().getScheduler().checkEventState(0);
        }
        else
        {
            // Yield so the event can fire.
            Scheduler::instance().yield();
        }
    }
}

void PosixSubsystem::setSignalHandler(size_t sig, SignalHandler* handler)
{
    while(!m_SignalHandlersLock.acquire());

    sig %= 32;
    if(handler)
    {
        SignalHandler* tmp;
        tmp = m_SignalHandlers.lookup(sig);
        if(tmp)
        {
            // Remove from the list
            m_SignalHandlers.remove(sig);

            // Destroy the SignalHandler struct
            delete tmp;
        }

        // Insert into the signal handler table
        handler->sig = sig;

        m_SignalHandlers.insert(sig, handler);
    }

    m_SignalHandlersLock.release();
}


size_t PosixSubsystem::getFd()
{
    // Enter critical section for writing.
    while(!m_FdLock.acquire());

    // Try to recycle if possible
    for(size_t i = m_LastFd; i < m_NextFd; i++)
    {
        if(!(m_FdBitmap.test(i)))
        {
            m_LastFd = i;
            m_FdBitmap.set(i);
            m_FdLock.release();
            return i;
        }
    }

    // Otherwise, allocate
    // m_NextFd will always contain the highest allocated fd
    m_FdBitmap.set(m_NextFd);
    size_t ret = m_NextFd++;
    m_FdLock.release();
    return ret;
}

void PosixSubsystem::allocateFd(size_t fdNum)
{
    // Enter critical section for writing.
    while(!m_FdLock.acquire());

    if(fdNum >= m_NextFd)
        m_NextFd = fdNum + 1;
    m_FdBitmap.set(fdNum);

    m_FdLock.release();
}

void PosixSubsystem::freeFd(size_t fdNum)
{
    // Enter critical section for writing.
    while(!m_FdLock.acquire());

    m_FdBitmap.clear(fdNum);

    FileDescriptor *pFd = m_FdMap.lookup(fdNum);
    if(pFd)
    {
        m_FdMap.remove(fdNum);
        delete pFd;
    }

    if(fdNum < m_LastFd)
        m_LastFd = fdNum;

    m_FdLock.release();
}

bool PosixSubsystem::copyDescriptors(PosixSubsystem *pSubsystem)
{
    assert(pSubsystem);

    // We're totally resetting our local state, ensure there's no files hanging around.
    freeMultipleFds();

    // Totally changing everything... Don't allow other functions to meddle.
    while(!m_FdLock.acquire());
    while(!pSubsystem->m_FdLock.acquire());

    // Copy each descriptor across from the original subsystem
    FdMap &map = pSubsystem->m_FdMap;
    for(FdMap::Iterator it = map.begin(); it != map.end(); it++)
    {
        FileDescriptor *pFd = it.value();
        if(!pFd)
            continue;
        size_t newFd = it.key();

        FileDescriptor *pNewFd = new FileDescriptor(*pFd);

        // Perform the same action as addFileDescriptor. We need to duplicate here because
        // we currently hold the FD lock, which will deadlock if we call any function which
        // attempts to acquire it.
        if(newFd >= m_NextFd)
            m_NextFd = newFd + 1;
        m_FdBitmap.set(newFd);
        m_FdMap.insert(newFd, pNewFd);
    }

    pSubsystem->m_FdLock.release();
    m_FdLock.release();
    return true;
}

void PosixSubsystem::freeMultipleFds(bool bOnlyCloExec, size_t iFirst, size_t iLast)
{
    assert(iFirst < iLast);

    while(!m_FdLock.acquire()); // Don't allow any access to the FD data

    // Because removing FDs as we go from the Tree can actually leave the Tree
    // iterators in a dud state, we'll add all the FDs to remove to this list.
    List<void*> fdsToRemove;

    // Are all FDs to be freed? Or only a selection?
    bool bAllToBeFreed = ((iFirst == 0 && iLast == ~0UL) && !bOnlyCloExec);
    if(bAllToBeFreed)
        m_LastFd = 0;

    FdMap &map = m_FdMap;
    for(FdMap::Iterator it = map.begin(); it != map.end(); it++)
    {
        size_t Fd = it.key();
        FileDescriptor *pFd = it.value();
        if(!pFd)
            continue;

        if(!(Fd >= iFirst && Fd <= iLast))
            continue;

        if(bOnlyCloExec)
        {
            if(!(pFd->fdflags & FD_CLOEXEC))
                continue;
        }

        // Perform the same action as freeFd. We need to duplicate code here because we currently
        // hold the FD lock, which will deadlock if we call any function which attempts to
        // acquire it.

        // No longer usable
        m_FdBitmap.clear(Fd);

        // Add to the list of FDs to remove, iff we won't be cleaning up the entire set
        if(!bAllToBeFreed)
            fdsToRemove.pushBack(reinterpret_cast<void*>(Fd));

        // Delete the descriptor itself
        delete pFd;

        // And reset the "last freed" tracking variable, if this is lower than it already.
        if(Fd < m_LastFd)
            m_LastFd = Fd;
    }

    // Clearing all AND not caring about CLOEXEC FDs? If so, clear the map. Otherwise, only
    // clear the FDs that are supposed to be cleared.
    if(bAllToBeFreed)
        m_FdMap.clear();
    else
    {
        for(List<void*>::Iterator it = fdsToRemove.begin(); it != fdsToRemove.end(); it++)
            m_FdMap.remove(reinterpret_cast<size_t>(*it));
    }

    m_FdLock.release();
}

void PosixSubsystem::threadRemoved(Thread *pThread)
{
    for(Tree<size_t, PosixThread *>::Iterator it = m_Threads.begin(); it != m_Threads.end(); it++)
    {
        PosixThread *thread = it.value();
        if (thread->pThread != pThread)
            continue;

        // Can safely assert that this thread is no longer running.
        // We do not however kill the thread object yet. It can be cleaned up
        // when the PosixSubsystem quits (if this was the last thread). Or, it
        // will be cleaned up by a join().
        thread->isRunning.release();
        break;
    }
}
