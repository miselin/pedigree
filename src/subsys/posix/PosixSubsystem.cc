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

#include "pedigree/kernel/Log.h"
#include <PosixSubsystem.h>

#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/utilities/RadixTree.h"
#include "pedigree/kernel/utilities/Tree.h"

#include "pedigree/kernel/utilities/assert.h"

#include "PosixProcess.h"
#include "logging.h"

#include <linker/DynamicLinker.h>
#include "pedigree/kernel/linker/Elf.h"
#include <vfs/File.h>
#include <vfs/LockedFile.h>
#include <vfs/MemoryMappedFile.h>
#include <vfs/Symlink.h>
#include <vfs/VFS.h>

#include "file-syscalls.h"

#include <signal.h>

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2

#define FD_CLOEXEC 1

typedef Tree<size_t, PosixSubsystem::SignalHandler *> sigHandlerTree;
typedef Tree<size_t, FileDescriptor *> FdMap;

RadixTree<LockedFile *> g_PosixGlobalLockedFiles;

ProcessGroupManager ProcessGroupManager::m_Instance;

extern void pedigree_init_sigret();
extern void pedigree_init_pthreads();

/// Default constructor
FileDescriptor::FileDescriptor()
    : file(0), offset(0), fd(0xFFFFFFFF), fdflags(0), flflags(0), so_domain(0),
      so_type(0), so_local(0), lockedFile(0)
{
}

/// Parameterised constructor
FileDescriptor::FileDescriptor(
    File *newFile, uint64_t newOffset, size_t newFd, int fdFlags, int flFlags,
    LockedFile *lf)
    : file(newFile), offset(newOffset), fd(newFd), fdflags(fdFlags),
      flflags(flFlags), lockedFile(lf)
{
    if (file)
    {
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
}

/// Copy constructor
FileDescriptor::FileDescriptor(FileDescriptor &desc)
    : file(desc.file), offset(desc.offset), fd(desc.fd), fdflags(desc.fdflags),
      flflags(desc.flflags), lockedFile(0)
{
    if (file)
    {
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
}

/// Pointer copy constructor
FileDescriptor::FileDescriptor(FileDescriptor *desc)
    : file(0), offset(0), fd(0), fdflags(0), flflags(0), lockedFile(0)
{
    if (!desc)
        return;

    file = desc->file;
    offset = desc->offset;
    fd = desc->fd;
    fdflags = desc->fdflags;
    flflags = desc->flflags;
    if (file)
    {
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
}

/// Assignment operator implementation
FileDescriptor &FileDescriptor::operator=(FileDescriptor &desc)
{
    file = desc.file;
    offset = desc.offset;
    fd = desc.fd;
    fdflags = desc.fdflags;
    flflags = desc.flflags;
    if (file)
    {
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
    return *this;
}

/// Destructor - decreases file reference count
FileDescriptor::~FileDescriptor()
{
    if (file)
    {
        // Unlock the file we have a lock on, release from the global lock table
        if (lockedFile)
        {
            g_PosixGlobalLockedFiles.remove(file->getFullPath());
            lockedFile->unlock();
            delete lockedFile;
        }
        file->decreaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
}

PosixSubsystem::PosixSubsystem(PosixSubsystem &s)
    : Subsystem(s), m_SignalHandlers(), m_SignalHandlersLock(), m_FdMap(),
      m_NextFd(s.m_NextFd), m_FdLock(), m_FdBitmap(), m_LastFd(0),
      m_FreeCount(s.m_FreeCount), m_AltSigStack(), m_SyncObjects(), m_Threads(),
      m_ThreadWaiters(), m_NextThreadWaiter(1)
{
    while (!m_SignalHandlersLock.acquire())
        ;
    while (!s.m_SignalHandlersLock.enter())
        ;

    // Copy all signal handlers
    for (sigHandlerTree::Iterator it = s.m_SignalHandlers.begin();
         it != s.m_SignalHandlers.end(); it++)
    {
        size_t key = it.key();
        void *value = it.value();
        if (!value)
            continue;

        SignalHandler *newSig =
            new SignalHandler(*reinterpret_cast<SignalHandler *>(value));
        m_SignalHandlers.insert(key, newSig);
    }

    s.m_SignalHandlersLock.leave();
    m_SignalHandlersLock.release();

    // Copy across waiter state.
    for (Tree<void *, Semaphore *>::Iterator it = s.m_ThreadWaiters.begin();
         it != s.m_ThreadWaiters.end(); ++it)
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

    acquire();

    // Destroy all signal handlers
    for (sigHandlerTree::Iterator it = m_SignalHandlers.begin();
         it != m_SignalHandlers.end(); it++)
    {
        // Get the signal handler and remove it. Note that there shouldn't be
        // null SignalHandlers, at all.
        SignalHandler *sig = it.value();
        assert(sig);

        // SignalHandler's destructor will delete the Event itself
        delete sig;
    }

    // And now that the signals are destroyed, remove them from the Tree
    m_SignalHandlers.clear();

    release();

    // For sanity's sake, destroy any remaining descriptors
    freeMultipleFds();

    // Remove any POSIX threads that might still be lying around
    for (Tree<size_t, PosixThread *>::Iterator it = m_Threads.begin();
         it != m_Threads.end(); it++)
    {
        PosixThread *thread = it.value();
        assert(thread);  // There shouldn't have ever been a null PosixThread in
                         // there

        // If the thread is still running, it should be killed
        if (!thread->isRunning.tryAcquire())
        {
            WARNING(
                "PosixSubsystem object freed when a thread is still running?");
            // Thread will just stay running, won't be deallocated or killed
        }

        // Clean up any thread-specific data
        for (Tree<size_t, PosixThreadKey *>::Iterator it2 =
                 thread->m_ThreadData.begin();
             it2 != thread->m_ThreadData.end(); it2++)
        {
            PosixThreadKey *p = reinterpret_cast<PosixThreadKey *>(it.value());
            assert(p);

            /// \todo Call the destructor (need a way to call into userspace and
            /// return back here)
            delete p;
        }

        thread->m_ThreadData.clear();
        delete thread;
    }

    m_Threads.clear();

    // Clean up synchronisation objects
    for (Tree<size_t, PosixSyncObject *>::Iterator it = m_SyncObjects.begin();
         it != m_SyncObjects.end(); it++)
    {
        PosixSyncObject *p = it.value();
        assert(p);

        if (p->pObject)
        {
            if (p->isMutex)
                delete reinterpret_cast<Mutex *>(p->pObject);
            else
                delete reinterpret_cast<Semaphore *>(p->pObject);
        }
    }

    m_SyncObjects.clear();

    for (Tree<void *, Semaphore *>::Iterator it = m_ThreadWaiters.begin();
         it != m_ThreadWaiters.end(); ++it)
    {
        // Wake up everything waiting and then destroy the waiter object.
        Semaphore *sem = it.value();
        sem->release(-sem->getValue());
        delete sem;
    }

    m_ThreadWaiters.clear();

    // Take the memory map lock before we become uninterruptible.
    while (!MemoryMapManager::instance().acquireLock())
        ;

    // Spinlock as a quick way of disabling interrupts.
    Spinlock spinlock;
    spinlock.acquire();

    // Switch to the address space of the process we're destroying.
    // We need to unmap memory maps, and we can't do that in our address space.
    VirtualAddressSpace &curr =
        Processor::information().getVirtualAddressSpace();
    VirtualAddressSpace *va = m_pProcess->getAddressSpace();

    if (va != &curr)
    {
        // Switch into the address space we want to unmap inside.
        Processor::switchAddressSpace(*va);
    }

    // Remove all existing mappings, if any.
    MemoryMapManager::instance().unmapAllUnlocked();

    if (va != &curr)
    {
        Processor::switchAddressSpace(curr);
    }

    spinlock.release();

    // Give back the memory map lock now - we're interruptible again.
    MemoryMapManager::instance().releaseLock();
}

void PosixSubsystem::acquire()
{
    Thread *me = Processor::information().getCurrentThread();

    m_Lock.acquire();
    if (m_bAcquired && m_pAcquiredThread == me)
    {
        m_Lock.release();
        return;  // already acquired
    }
    m_Lock.release();

    // Ensure that no descriptor operations are taking place (and then, will
    // take place)
    while (!m_FdLock.acquire())
        ;

    // Modifying signal handlers, ensure that they are not in use
    while (!m_SignalHandlersLock.acquire())
        ;

    // Safe to do without spinlock as we hold the other locks now.
    m_pAcquiredThread = me;
    m_bAcquired = true;
}

void PosixSubsystem::release()
{
    // Opposite order to acquire()
    m_Lock.acquire();
    m_bAcquired = false;
    m_pAcquiredThread = nullptr;

    m_SignalHandlersLock.release();
    m_FdLock.release();

    m_Lock.release();
}

bool PosixSubsystem::checkAddress(uintptr_t addr, size_t extent, size_t flags)
{
    Uninterruptible while_checking;

    PS_NOTICE(
        "PosixSubsystem::checkAddress(" << Hex << addr << ", " << Dec << extent
                                        << ", " << Hex << flags << ")");

    // No memory access expected, all good.
    if (!extent)
    {
        PS_NOTICE("  -> zero extent, address is sane.");
        return true;
    }

    uintptr_t aa = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    PS_NOTICE(" -> ret: " << aa);

    // Check address range.
    VirtualAddressSpace &va = Processor::information().getVirtualAddressSpace();
    if ((addr < va.getUserStart()) || (addr >= va.getKernelStart()))
    {
        PS_NOTICE("  -> outside of user address area.");
        return false;
    }

    // Short-circuit if this is a memory mapped region.
    if (MemoryMapManager::instance().contains(addr, extent))
    {
        PS_NOTICE("  -> inside memory map.");
        return true;
    }

    // Check the range.
    for (size_t i = 0; i < extent; i += PhysicalMemoryManager::getPageSize())
    {
        void *pAddr = reinterpret_cast<void *>(addr + i);
        if (!va.isMapped(pAddr))
        {
            PS_NOTICE("  -> page " << Hex << pAddr << " is not mapped.");
            return false;
        }

        if (flags & SafeWrite)
        {
            size_t vFlags = 0;
            physical_uintptr_t phys = 0;
            va.getMapping(pAddr, phys, vFlags);

            if (!(vFlags & (VirtualAddressSpace::Write |
                            VirtualAddressSpace::CopyOnWrite)))
            {
                PS_NOTICE("  -> not writeable.");
                return false;
            }
        }
    }

    PS_NOTICE("  -> mapped and available.");
    return true;
}

void PosixSubsystem::exit(int code)
{
    Thread *pThread = Processor::information().getCurrentThread();

    Process *pProcess = pThread->getParent();
    pProcess->markTerminating();

    if (pProcess->getExitStatus() == 0 ||     // Normal exit.
        pProcess->getExitStatus() == 0x7F ||  // Suspended.
        pProcess->getExitStatus() == 0xFF)    // Continued.
        pProcess->setExitStatus((code & 0xFF) << 8);
    if (code)
    {
        WARNING("Sending unexpected exit event (" << code << ") to thread");
        pThread->unexpectedExit();
    }

    // Exit called, but we could be at any nesting level in the event stack.
    // We have to propagate this exit() to all lower stack levels because they
    // may have semaphores and stuff open.

    // So, if we're not dealing with the lowest in the stack...
    /// \note If we're at state level one, we're potentially running as a thread
    /// that has
    ///       had an event sent to it from another process. If this is changed
    ///       to > 0, it is impossible to return to a shell when a segfault
    ///       occurs in an app.
    if (pThread->getStateLevel() > 1)
    {
        // OK, we have other events running. They'll have to die first before we
        // can do anything.
        pThread->setUnwindState(Thread::Exit);

        Thread *pBlockingThread =
            pThread->getBlockingThread(pThread->getStateLevel() - 1);
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
    if (pProcess->getType() == Process::Posix)
    {
        PosixProcess *p = static_cast<PosixProcess *>(pProcess);
        ProcessGroup *pGroup = p->getProcessGroup();
        if (pGroup && (p->getGroupMembership() == PosixProcess::Member))
        {
            for (List<PosixProcess *>::Iterator it = pGroup->Members.begin();
                 it != pGroup->Members.end(); it++)
            {
                if ((*it) == p)
                {
                    it = pGroup->Members.erase(it);
                    break;
                }
            }
        }
        else if (pGroup && (p->getGroupMembership() == PosixProcess::Leader))
        {
            // Pick a new process to be the leader, remove this one from the
            // list
            PosixProcess *pNewLeader = 0;
            for (List<PosixProcess *>::Iterator it = pGroup->Members.begin();
                 it != pGroup->Members.end();)
            {
                if ((*it) == p)
                    it = pGroup->Members.erase(it);
                else
                {
                    if (!pNewLeader)
                        pNewLeader = *it;
                    ++it;
                }
            }

            // Set the new leader
            if (pNewLeader)
            {
                pNewLeader->setGroupMembership(PosixProcess::Leader);
                pGroup->Leader = pNewLeader;
            }
            else
            {
                // No new leader! Destroy the group, we're the last process in
                // it.
                delete pGroup;
                pGroup = 0;
            }
        }
    }

    // Notify parent that we terminated (we may be in a separate process group).
    Process *pParent = pProcess->getParent();
    if (pParent && pParent->getSubsystem())
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
    if (!pThread)
        pThread = Processor::information().getCurrentThread();
    Process *pProcess = pThread->getParent();
    if (pProcess->getType() != Process::Posix)
    {
        ERROR("PosixSubsystem::kill called with a non-POSIX process!");
        return false;
    }
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(pProcess->getSubsystem());

    // Send SIGKILL. getSignalHandler handles all that locking shiz for us.
    SignalHandler *sig = 0;
    switch (killReason)
    {
        case Interrupted:
            sig = pSubsystem->getSignalHandler(2);
            break;

        case Terminated:
            sig = pSubsystem->getSignalHandler(15);
            break;

        default:
            sig = pSubsystem->getSignalHandler(9);
            break;
    }

    if (sig && sig->pEvent)
    {
        NOTICE("PosixSubsystem - killing " << pThread->getParent()->getId());

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
    NOTICE(
        "PosixSubsystem::threadException -> "
        << Dec << pThread->getParent()->getId() << ":" << pThread->getId());

    Process *pProcess = pThread->getParent();
    if (pProcess->getType() != Process::Posix)
    {
        ERROR(
            "PosixSubsystem::threadException called with a non-POSIX process!");
        return;
    }
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(pProcess->getSubsystem());

    // What was the exception?
    SignalHandler *sig = 0;
    switch (eType)
    {
        case PageFault:
            NOTICE("    (Page fault)");
            // Send SIGSEGV
            sig = pSubsystem->getSignalHandler(SIGSEGV);
            break;
        case InvalidOpcode:
            NOTICE("    (Invalid opcode)");
            // Send SIGILL
            sig = pSubsystem->getSignalHandler(SIGILL);
            break;
        case GeneralProtectionFault:
            NOTICE("    (General Fault)");
            // Send SIGBUS
            sig = pSubsystem->getSignalHandler(SIGBUS);
            break;
        case DivideByZero:
            NOTICE("    (Division by zero)");
            // Send SIGFPE
            sig = pSubsystem->getSignalHandler(SIGFPE);
            break;
        case FpuError:
            NOTICE("    (FPU error)");
            // Send SIGFPE
            sig = pSubsystem->getSignalHandler(SIGFPE);
            break;
        case SpecialFpuError:
            NOTICE("    (FPU error - special)");
            // Send SIGFPE
            sig = pSubsystem->getSignalHandler(SIGFPE);
            break;
        case TerminalInput:
            NOTICE("    (Attempt to read from terminal by non-foreground "
                   "process)");
            // Send SIGTTIN
            sig = pSubsystem->getSignalHandler(SIGTTIN);
            break;
        case TerminalOutput:
            NOTICE("    (Output to terminal by non-foreground process)");
            // Send SIGTTOU
            sig = pSubsystem->getSignalHandler(SIGTTOU);
            break;
        case Continue:
            NOTICE("    (Continuing a stopped process)");
            // Send SIGCONT
            sig = pSubsystem->getSignalHandler(SIGCONT);
            break;
        case Stop:
            NOTICE("    (Stopping a process)");
            // Send SIGTSTP
            sig = pSubsystem->getSignalHandler(SIGTSTP);
            break;
        case Interrupt:
            NOTICE("    (Interrupting a process)");
            // Send SIGINT
            sig = pSubsystem->getSignalHandler(SIGINT);
            break;
        case Quit:
            NOTICE("    (Requesting quit)");
            // Send SIGTERM
            sig = pSubsystem->getSignalHandler(SIGTERM);
            break;
        case Child:
            NOTICE("    (Child status changed)");
            // Send SIGCHLD
            sig = pSubsystem->getSignalHandler(SIGCHLD);
            break;
        case Pipe:
            NOTICE("    (Pipe broken)");
            // Send SIGPIPE
            sig = pSubsystem->getSignalHandler(SIGPIPE);
            break;
        default:
            NOTICE("    (Unknown)");
            // Unknown exception
            ERROR_NOLOCK(
                "Unknown exception type in threadException - POSIX subsystem");
            break;
    }

    // If we're good to go, send the signal.
    if (sig && sig->pEvent)
    {
        Thread *pCurrentThread = Processor::information().getCurrentThread();

        pThread->sendEvent(sig->pEvent);

        if (pCurrentThread == pThread)
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

void PosixSubsystem::setSignalHandler(size_t sig, SignalHandler *handler)
{
    while (!m_SignalHandlersLock.acquire())
        ;

    sig %= 32;
    if (handler)
    {
        SignalHandler *tmp;
        tmp = m_SignalHandlers.lookup(sig);
        if (tmp)
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

/**
 * Note: POSIX  requires open()/accept()/etc to be safe during a signal
 * handler, which requires us to not allow signals during these file descriptor
 * calls. They cannot re-enter as they take process-specific locks.
 */

size_t PosixSubsystem::getFd()
{
    Uninterruptible throughout;

    // Enter critical section for writing.
    while (!m_FdLock.acquire())
        ;

    // Try to recycle if possible
    for (size_t i = m_LastFd; i < m_NextFd; i++)
    {
        if (!(m_FdBitmap.test(i)))
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
    Uninterruptible throughout;

    // Enter critical section for writing.
    while (!m_FdLock.acquire())
        ;

    if (fdNum >= m_NextFd)
        m_NextFd = fdNum + 1;
    m_FdBitmap.set(fdNum);

    m_FdLock.release();
}

void PosixSubsystem::freeFd(size_t fdNum)
{
    Uninterruptible throughout;

    // Enter critical section for writing.
    while (!m_FdLock.acquire())
        ;

    m_FdBitmap.clear(fdNum);

    FileDescriptor *pFd = m_FdMap.lookup(fdNum);
    if (pFd)
    {
        m_FdMap.remove(fdNum);
        delete pFd;
    }

    if (fdNum < m_LastFd)
        m_LastFd = fdNum;

    m_FdLock.release();
}

bool PosixSubsystem::copyDescriptors(PosixSubsystem *pSubsystem)
{
    Uninterruptible throughout;

    assert(pSubsystem);

    // We're totally resetting our local state, ensure there's no files hanging
    // around.
    freeMultipleFds();

    // Totally changing everything... Don't allow other functions to meddle.
    while (!m_FdLock.acquire())
        ;
    while (!pSubsystem->m_FdLock.acquire())
        ;

    // Copy each descriptor across from the original subsystem
    FdMap &map = pSubsystem->m_FdMap;
    for (FdMap::Iterator it = map.begin(); it != map.end(); it++)
    {
        FileDescriptor *pFd = it.value();
        if (!pFd)
            continue;
        size_t newFd = it.key();

        FileDescriptor *pNewFd = new FileDescriptor(*pFd);

        // Perform the same action as addFileDescriptor. We need to duplicate
        // here because we currently hold the FD lock, which will deadlock if we
        // call any function which attempts to acquire it.
        if (newFd >= m_NextFd)
            m_NextFd = newFd + 1;
        m_FdBitmap.set(newFd);
        m_FdMap.insert(newFd, pNewFd);
    }

    pSubsystem->m_FdLock.release();
    m_FdLock.release();
    return true;
}

void PosixSubsystem::freeMultipleFds(
    bool bOnlyCloExec, size_t iFirst, size_t iLast)
{
    Uninterruptible throughout;

    assert(iFirst < iLast);

    while (!m_FdLock.acquire())
        ;  // Don't allow any access to the FD data

    // Because removing FDs as we go from the Tree can actually leave the Tree
    // iterators in a dud state, we'll add all the FDs to remove to this list.
    List<void *> fdsToRemove;

    // Are all FDs to be freed? Or only a selection?
    bool bAllToBeFreed = ((iFirst == 0 && iLast == ~0UL) && !bOnlyCloExec);
    if (bAllToBeFreed)
        m_LastFd = 0;

    FdMap &map = m_FdMap;
    for (FdMap::Iterator it = map.begin(); it != map.end(); it++)
    {
        size_t Fd = it.key();
        FileDescriptor *pFd = it.value();
        if (!pFd)
            continue;

        if (!(Fd >= iFirst && Fd <= iLast))
            continue;

        if (bOnlyCloExec)
        {
            if (!(pFd->fdflags & FD_CLOEXEC))
                continue;
        }

        // Perform the same action as freeFd. We need to duplicate code here
        // because we currently hold the FD lock, which will deadlock if we call
        // any function which attempts to acquire it.

        // No longer usable
        m_FdBitmap.clear(Fd);

        // Add to the list of FDs to remove, iff we won't be cleaning up the
        // entire set
        if (!bAllToBeFreed)
            fdsToRemove.pushBack(reinterpret_cast<void *>(Fd));

        // Delete the descriptor itself
        delete pFd;

        // And reset the "last freed" tracking variable, if this is lower than
        // it already.
        if (Fd < m_LastFd)
            m_LastFd = Fd;
    }

    // Clearing all AND not caring about CLOEXEC FDs? If so, clear the map.
    // Otherwise, only clear the FDs that are supposed to be cleared.
    if (bAllToBeFreed)
        m_FdMap.clear();
    else
    {
        for (List<void *>::Iterator it = fdsToRemove.begin();
             it != fdsToRemove.end(); it++)
            m_FdMap.remove(reinterpret_cast<size_t>(*it));
    }

    m_FdLock.release();
}

FileDescriptor *PosixSubsystem::getFileDescriptor(size_t fd)
{
    Uninterruptible throughout;

    // Enter the critical section, for reading.
    while (!m_FdLock.enter())
        ;

    FileDescriptor *pFd = m_FdMap.lookup(fd);

    m_FdLock.leave();

    return pFd;
}

void PosixSubsystem::addFileDescriptor(size_t fd, FileDescriptor *pFd)
{
    /// \todo this is possibly racy
    freeFd(fd);
    allocateFd(fd);

    {
        Uninterruptible throughout;

        // Enter critical section for writing.
        while (!m_FdLock.acquire())
            ;

        m_FdMap.insert(fd, pFd);

        m_FdLock.release();
    }
}

void PosixSubsystem::threadRemoved(Thread *pThread)
{
    for (Tree<size_t, PosixThread *>::Iterator it = m_Threads.begin();
         it != m_Threads.end(); it++)
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

bool PosixSubsystem::checkAccess(
    FileDescriptor *pFileDescriptor, bool bRead, bool bWrite,
    bool bExecute) const
{
    return VFS::checkAccess(pFileDescriptor->file, bRead, bWrite, bExecute);
}

bool PosixSubsystem::loadElf(
    File *pFile, uintptr_t mappedAddress, uintptr_t &newAddress,
    uintptr_t &finalAddress)
{
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();

    // Grab the file header to check magic and find program headers.
    Elf::ElfHeader_t *pHeader =
        reinterpret_cast<Elf::ElfHeader_t *>(mappedAddress);
    if ((pHeader->ident[1] != 'E') || (pHeader->ident[2] != 'L') ||
        (pHeader->ident[3] != 'F') || (pHeader->ident[0] != 127))
    {
        return false;
    }

    size_t phnum = pHeader->phnum;
    Elf::ElfProgramHeader_t *phdrs =
        reinterpret_cast<Elf::ElfProgramHeader_t *>(
            mappedAddress + pHeader->phoff);

    // Find full memory size that we need to map in.
    uintptr_t startAddress = ~0U;
    uintptr_t unalignedStartAddress = 0;
    uintptr_t endAddress = 0;
    for (size_t i = 0; i < phnum; ++i)
    {
        if (phdrs[i].type != PT_LOAD)
        {
            continue;
        }

        if (phdrs[i].vaddr < startAddress)
        {
            startAddress = phdrs[i].vaddr;
        }

        uintptr_t maybeEndAddress = phdrs[i].vaddr + phdrs[i].memsz;
        if (maybeEndAddress > endAddress)
        {
            endAddress = maybeEndAddress;
        }
    }

    // Align to page boundaries.
    size_t pageSz = PhysicalMemoryManager::getPageSize();
    unalignedStartAddress = startAddress;
    startAddress &= ~(pageSz - 1);
    if (endAddress & (pageSz - 1))
    {
        endAddress = (endAddress + pageSz) & ~(pageSz - 1);
    }

    // OK, we can allocate space for the file now.
    bool bRelocated = false;
    if (pHeader->type == ET_REL || pHeader->type == ET_DYN)
    {
        if (!pProcess->getDynamicSpaceAllocator().allocate(
                endAddress - startAddress, newAddress))
            if (!pProcess->getSpaceAllocator().allocate(
                    endAddress - startAddress, newAddress))
                return false;

        bRelocated = true;
        unalignedStartAddress = newAddress + (startAddress & (pageSz - 1));
        startAddress = newAddress;

        newAddress = unalignedStartAddress;
    }
    else
    {
        if (!pProcess->getDynamicSpaceAllocator().allocateSpecific(
                startAddress, endAddress - startAddress))
            if (!pProcess->getSpaceAllocator().allocateSpecific(
                    startAddress, endAddress - startAddress))
                return false;

        newAddress = unalignedStartAddress;
    }

    finalAddress = startAddress + (endAddress - startAddress);

    // Can now do another pass, mapping in as needed.
    for (size_t i = 0; i < phnum; ++i)
    {
        if (phdrs[i].type != PT_LOAD)
        {
            continue;
        }

        uintptr_t base = phdrs[i].vaddr;
        if (bRelocated)
        {
            base += startAddress;
        }
        uintptr_t unalignedBase = base;
        if (base & (pageSz - 1))
        {
            base &= ~(pageSz - 1);
        }

        uintptr_t offset = phdrs[i].offset;
        if (offset & (pageSz - 1))
        {
            offset &= ~(pageSz - 1);
        }

        // if we don't add the unaligned part to the length, we can map only
        // enough to cover the aligned page even though the alignment may lead
        // to the region covering two pages...
        size_t length = phdrs[i].memsz + (unalignedBase & (pageSz - 1));
        if (length & (pageSz - 1))
        {
            length = (length + pageSz) & ~(pageSz - 1);
        }

        // Map.
        MemoryMappedObject::Permissions perms = MemoryMappedObject::Read;
        if (phdrs[i].flags & PF_X)
        {
            perms |= MemoryMappedObject::Exec;
        }
        if (phdrs[i].flags & PF_R)
        {
            perms |= MemoryMappedObject::Read;
        }
        if (phdrs[i].flags & PF_W)
        {
            perms |= MemoryMappedObject::Write;
        }

        NOTICE(
            "PHDR[" << i << "]: @" << Hex << base << " -> " << base + length);
        MemoryMappedObject *pObject = MemoryMapManager::instance().mapFile(
            pFile, base, length, perms, offset);
        if (!pObject)
        {
            ERROR("PosixSubsystem::loadElf: failed to map PT_LOAD section");
            return false;
        }

        if (phdrs[i].memsz > phdrs[i].filesz)
        {
            uintptr_t end = unalignedBase + phdrs[i].memsz;
            uintptr_t zeroStart = unalignedBase + phdrs[i].filesz;
            if (zeroStart & (pageSz - 1))
            {
                size_t numBytes = pageSz - (zeroStart & (pageSz - 1));
                if ((zeroStart + numBytes) > end)
                {
                    numBytes = end - zeroStart;
                }
                ByteSet(reinterpret_cast<void *>(zeroStart), 0, numBytes);
                zeroStart += numBytes;
            }

            if (zeroStart < end)
            {
                MemoryMappedObject *pObject =
                    MemoryMapManager::instance().mapAnon(
                        zeroStart, end - zeroStart, perms);
                if (!pObject)
                {
                    ERROR("PosixSubsystem::loadElf: failed to map anonymous "
                          "pages for filesz/memsz mismatch");
                    return false;
                }
            }
        }
    }

    return true;
}

#define STACK_PUSH(stack, value) *--stack = value
#define STACK_PUSH2(stack, value1, value2) \
    STACK_PUSH(stack, value1);             \
    STACK_PUSH(stack, value2)
#define STACK_PUSH_COPY(stack, value, length) \
    stack = adjust_pointer(stack, -length);   \
    MemoryCopy(stack, value, length)
#define STACK_PUSH_ZEROES(stack, length)    \
    stack = adjust_pointer(stack, -length); \
    ByteSet(stack, 0, length)

bool PosixSubsystem::invoke(
    const char *name, List<SharedPointer<String>> &argv,
    List<SharedPointer<String>> &env)
{
    return invoke(name, argv, env, 0);
}

bool PosixSubsystem::invoke(
    const char *name, List<SharedPointer<String>> &argv,
    List<SharedPointer<String>> &env, SyscallState &state)
{
    return invoke(name, argv, env, &state);
}

bool PosixSubsystem::parseShebang(
    File *pFile, File *&pOutFile, List<SharedPointer<String>> &argv)
{
    // Try and read the shebang, if any.
    /// \todo this loop could terminate MUCH faster
    String fileContents;
    bool bSearchDone = false;
    size_t offset = 0;
    while (!bSearchDone)
    {
        char buff[129];
        size_t nRead =
            pFile->read(offset, 128, reinterpret_cast<uintptr_t>(buff));
        buff[nRead] = 0;
        offset += nRead;

        if (nRead)
        {
            // Truncate at the newline if one is found (and then stop
            // iterating).
            char *newline = const_cast<char *>(StringFind(buff, '\n'));
            if (newline)
            {
                bSearchDone = true;
                *newline = 0;
            }
            fileContents += String(buff);
        }

        if (nRead < 128)
        {
            bSearchDone = true;
            break;
        }
    }

    NOTICE("checking: " << fileContents);

    // Is this even a shebang line?
    if (!fileContents.startswith("#!"))
    {
        NOTICE("no shebang found");
        return true;
    }

    // Strip the shebang.
    fileContents.lchomp();
    fileContents.lchomp();

    // OK, we have a shebang line. We need to tokenize.
    List<SharedPointer<String>> additionalArgv = fileContents.tokenise(' ');
    if (!additionalArgv.count())
    {
        // Not a true shebang line.
        NOTICE("split didn't find anything");
        return true;
    }

    // Normalise path to ensure we have the correct path to invoke.
    String invokePath;
    SharedPointer<String> newTarget = *additionalArgv.begin();
    if (normalisePath(invokePath, static_cast<const char *>(*newTarget)))
    {
        // rewrote, update argv[0] accordingly.
        *newTarget = invokePath;
    }

    // Can we load the new program?
    File *pNewTarget = findFileWithAbiFallbacks(*newTarget);
    if (!pNewTarget)
    {
        // No, we cannot.
        NOTICE("target not found");
        SYSCALL_ERROR(DoesNotExist);
        return false;
    }

    // OK, we can now insert to argv - we do so backwards so it's just a simple
    // pushFront.
    for (auto it = additionalArgv.rbegin(); it != additionalArgv.rend(); ++it)
    {
        NOTICE(
            "shebang: inserting " << **it << " [l=" << (*it)->length() << "]");
        argv.pushFront(*it);
    }

    pOutFile = pNewTarget;

    return true;
}

static File *traverseForInvoke(File *pFile)
{
    // Do symlink traversal.
    while (pFile && pFile->isSymlink())
    {
        pFile = Symlink::fromFile(pFile)->followLink();
    }
    if (!pFile)
    {
        ERROR("PosixSubsystem::invoke: symlink traversal failed");
        SYSCALL_ERROR(DoesNotExist);
        return 0;
    }

    // Check for directory.
    if (pFile->isDirectory())
    {
        ERROR("PosixSubsystem::invoke: target is a directory");
        SYSCALL_ERROR(IsADirectory);
        return 0;
    }

    return pFile;
}

bool PosixSubsystem::invoke(
    const char *name, List<SharedPointer<String>> &argv,
    List<SharedPointer<String>> &env, SyscallState *state)
{
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());

    NOTICE(
        "PosixSubsystem::invoke(" << name << ") [pid=" << pProcess->getId()
                                  << "]");

    // Grab the thread we're going to return into - need to tweak it.
    Thread *pThread = pProcess->getThread(0);

    // Ensure we only have one thread running (us).
    if (pProcess->getNumThreads() > 1)
    {
        /// \todo actually we are supposed to kill them all here
        return false;
    }

    // Save the original name before we trash the old stack.
    String originalName(name);

    // Try and find the target file we want to invoke.
    File *originalFile = findFileWithAbiFallbacks(String(name));
    if (!originalFile)
    {
        ERROR("PosixSubsystem::invoke: could not find file '" << name << "'");
        SYSCALL_ERROR(DoesNotExist);
        return false;
    }

    originalFile = traverseForInvoke(originalFile);
    if (!originalFile)
    {
        // traverseForInvoke does a SYSCALL_ERROR for us
        return false;
    }

    uint8_t validateBuffer[128];
    size_t nBytes =
        originalFile->read(0, 128, reinterpret_cast<uintptr_t>(validateBuffer));

    Elf *validElf = new Elf();
    if (!validElf->validate(validateBuffer, nBytes))
    {
        WARNING(
            "PosixSubsystem::invoke: '"
            << name << "' is not an ELF binary, looking for shebang...");

        File *shebangFile = 0;
        if (!parseShebang(originalFile, shebangFile, argv))
        {
            ERROR(
                "PosixSubsystem::invoke: failed to parse shebang line in '"
                << name << "'");
            return false;
        }

        // Switch to the real target if we must; parseShebang adjusts argv for
        // us.
        if (shebangFile)
        {
            originalFile = shebangFile;

            // Handle symlinks in shebang target.
            originalFile = traverseForInvoke(originalFile);
            if (!originalFile)
            {
                return false;
            }
        }
    }

    // Can we read & execute the given target?
    if (!VFS::checkAccess(originalFile, true, false, true))
    {
        // checkAccess does a SYSCALL_ERROR for us.
        return -1;
    }

    File *interpreterFile = 0;

    // Inhibit all signals from coming in while we trash the address space...
    for (int sig = 0; sig < 32; sig++)
        Processor::information().getCurrentThread()->inhibitEvent(sig, true);

    // Determine if the target uses an interpreter or not.
    String interpreter("");
    DynamicLinker *pLinker = new DynamicLinker();
    pProcess->setLinker(pLinker);
    if (pLinker->checkInterpreter(originalFile, interpreter))
    {
        // Ensure we can actually find the interpreter.
        interpreterFile = findFileWithAbiFallbacks(interpreter);
        interpreterFile = traverseForInvoke(interpreterFile);
        if (!interpreterFile)
        {
            ERROR(
                "PosixSubsystem::invoke: could not find interpreter '"
                << interpreter << "'");
            SYSCALL_ERROR(ExecFormatError);
            return false;
        }

        // No longer need the DynamicLinker instance.
        delete pLinker;
        pLinker = 0;
        pProcess->setLinker(pLinker);
    }
    else
    {
        ERROR("PosixSubsystem::invoke: target does not have a dynamic linker");
        SYSCALL_ERROR(ExecFormatError);
        return false;
    }

    // Wipe out old address space.
    MemoryMapManager::instance().unmapAll();
    pProcess->getAddressSpace()->revertToKernelAddressSpace();

    // We now need to clean up the process' address space.
    pProcess->getSpaceAllocator().clear();
    pProcess->getDynamicSpaceAllocator().clear();
    pProcess->getSpaceAllocator().free(
        pProcess->getAddressSpace()->getUserStart(),
        pProcess->getAddressSpace()->getUserReservedStart() -
            pProcess->getAddressSpace()->getUserStart());
    if (pProcess->getAddressSpace()->getDynamicStart())
    {
        pProcess->getDynamicSpaceAllocator().free(
            pProcess->getAddressSpace()->getDynamicStart(),
            pProcess->getAddressSpace()->getDynamicEnd() -
                pProcess->getAddressSpace()->getDynamicStart());
    }
    pProcess->getAddressSpace()->revertToKernelAddressSpace();

    // Map in the two ELF files so we can load them into the address space.
    uintptr_t originalBase = 0, interpreterBase = 0;
    MemoryMappedObject::Permissions perms = MemoryMappedObject::Read |
                                            MemoryMappedObject::Write |
                                            MemoryMappedObject::Exec;
    MemoryMappedObject *pOriginal = MemoryMapManager::instance().mapFile(
        originalFile, originalBase, originalFile->getSize(), perms);
    if (!pOriginal)
    {
        ERROR("PosixSubsystem::invoke: failed to map target");
        SYSCALL_ERROR(OutOfMemory);
        return false;
    }

    MemoryMappedObject *pInterpreter = MemoryMapManager::instance().mapFile(
        interpreterFile, interpreterBase, interpreterFile->getSize(), perms);
    if (!pInterpreter)
    {
        ERROR("PosixSubsystem::invoke: failed to map interpreter");
        MemoryMapManager::instance().unmap(pOriginal);
        SYSCALL_ERROR(OutOfMemory);
        return false;
    }

    // Load the target application first.
    uintptr_t originalLoadedAddress = 0;
    uintptr_t originalFinalAddress = 0;
    if (!loadElf(
            originalFile, originalBase, originalLoadedAddress,
            originalFinalAddress))
    {
        /// \todo cleanup
        ERROR("PosixSubsystem::invoke: failed to load target");
        SYSCALL_ERROR(ExecFormatError);
        return false;
    }

    // Now load the interpreter.
    uintptr_t interpreterLoadedAddress = 0;
    uintptr_t interpreterFinalAddress = 0;
    if (!loadElf(
            interpreterFile, interpreterBase, interpreterLoadedAddress,
            interpreterFinalAddress))
    {
        /// \todo cleanup
        ERROR("PosixSubsystem::invoke: failed to load interpreter");
        SYSCALL_ERROR(ExecFormatError);
        return false;
    }

    // Extract entry points.
    uintptr_t originalEntryPoint = 0, interpreterEntryPoint = 0;
    Elf::extractEntryPoint(
        reinterpret_cast<uint8_t *>(originalBase), originalFile->getSize(),
        originalEntryPoint);
    Elf::extractEntryPoint(
        reinterpret_cast<uint8_t *>(interpreterBase),
        interpreterFile->getSize(), interpreterEntryPoint);

    // Pull out the ELF header information for the original image.
    Elf::ElfHeader_t *originalHeader =
        reinterpret_cast<Elf::ElfHeader_t *>(originalBase);

    // Past point of no return, so set up the process for the new image.
    pProcess->description() = originalName;
    pProcess->resetCounts();
    pThread->resetTlsBase();
    if (pSubsystem)
        pSubsystem->freeMultipleFds(true);
    while (pThread->getStateLevel())
        pThread->popState();

    // We can now build the auxiliary vector to pass to the dynamic linker.
    VirtualAddressSpace::Stack *stack =
        Processor::information().getVirtualAddressSpace().allocateStack();
    uintptr_t *loaderStack = reinterpret_cast<uintptr_t *>(stack->getTop());

    char **envs = new char *[env.count()];
    size_t envc = 0;
    for (auto it : env)
    {
        STACK_PUSH(loaderStack, 0);
        STACK_PUSH_COPY(
            loaderStack, static_cast<const char *>(*it), it->length());
        envs[envc++] = reinterpret_cast<char *>(loaderStack);
    }

    // Push argv/env.
    char **argvs = new char *[argv.count()];
    size_t argc = 0;
    for (auto it : argv)
    {
        STACK_PUSH(loaderStack, 0);
        STACK_PUSH_COPY(
            loaderStack, static_cast<const char *>(*it), it->length());
        NOTICE("argv[" << argc << "]: " << *it);
        argvs[argc++] = reinterpret_cast<char *>(loaderStack);
    }

    /// \todo platform assumption here.
    STACK_PUSH_COPY(loaderStack, "x86_64", 7);
    void *platform = loaderStack;

    /// \todo 16 random bytes, not 16 zero bytes
    STACK_PUSH_ZEROES(loaderStack, 16);
    void *random = loaderStack;

    // Align to 16 bytes.
    STACK_PUSH_ZEROES(
        loaderStack, 16 - (reinterpret_cast<uintptr_t>(loaderStack) & 15));

    // Build the aux vector now.
    STACK_PUSH2(loaderStack, 0, 0);  // AT_NULL
    STACK_PUSH2(
        loaderStack, reinterpret_cast<uintptr_t>(platform), 15);  // AT_PLATFORM
    STACK_PUSH2(
        loaderStack, reinterpret_cast<uintptr_t>(random), 25);  // AT_RANDOM
    STACK_PUSH2(loaderStack, 0, 15);                            // AT_SECURE
    /// \todo get from pProcess
    STACK_PUSH2(loaderStack, 0, 15);  // AT_EGID
    STACK_PUSH2(loaderStack, 0, 15);  // AT_GID
    STACK_PUSH2(loaderStack, 0, 15);  // AT_EUID
    STACK_PUSH2(loaderStack, 0, 15);  // AT_UID

    // ELF parts in the aux vector.
    STACK_PUSH2(loaderStack, originalEntryPoint, 9);        // AT_ENTRY
    STACK_PUSH2(loaderStack, interpreterLoadedAddress, 7);  // AT_BASE
    STACK_PUSH2(
        loaderStack, PhysicalMemoryManager::getPageSize(), 6);  // AT_PAGESZ
    STACK_PUSH2(loaderStack, originalHeader->phnum, 5);         // AT_PHNUM
    STACK_PUSH2(loaderStack, originalHeader->phentsize, 4);     // AT_PHENT
    STACK_PUSH2(
        loaderStack, originalLoadedAddress + originalHeader->phoff,
        3);  // AT_PHDR

    // env
    STACK_PUSH(loaderStack, 0);  // env[N]
    for (ssize_t i = envc - 1; i >= 0; --i)
    {
        STACK_PUSH(loaderStack, reinterpret_cast<uintptr_t>(envs[i]));
    }

    // argv
    STACK_PUSH(loaderStack, 0);  // argv[N]
    for (ssize_t i = argc - 1; i >= 0; --i)
    {
        STACK_PUSH(loaderStack, reinterpret_cast<uintptr_t>(argvs[i]));
    }

    // argc
    STACK_PUSH(loaderStack, argc);

    // We can now unmap both original objects as they've been loaded and
    // consumed.
    MemoryMapManager::instance().unmap(pInterpreter);
    MemoryMapManager::instance().unmap(pOriginal);
    pInterpreter = pOriginal = 0;

    // Initialise the sigret and pthreads shizzle if not already done for this
    // process (the calls detect).
    pedigree_init_sigret();
    pedigree_init_pthreads();

    Processor::setInterrupts(true);
    pProcess->recordTime(true);

    if (!state)
    {
        // Just create a new thread, this is not a full replace.
        Thread *pThread = new Thread(
            pProcess,
            reinterpret_cast<Thread::ThreadStartFunc>(
                interpreterEntryPoint + interpreterLoadedAddress),
            0, loaderStack);
        pThread->detach();

        return true;
    }
    else
    {
        // This is a replace and requires a jump to userspace.
        SchedulerState s;
        ByteSet(&s, 0, sizeof(s));
        pThread->state() = s;

        // Allow signals again now that everything's loaded
        for (int sig = 0; sig < 32; sig++)
        {
            Processor::information().getCurrentThread()->inhibitEvent(
                sig, false);
        }

        // Jump to the new process.
        Processor::jumpUser(
            0, interpreterEntryPoint + interpreterLoadedAddress,
            reinterpret_cast<uintptr_t>(loaderStack));
    }

    return true;
}
