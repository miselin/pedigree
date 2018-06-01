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

#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/utilities/RadixTree.h"
#include "pedigree/kernel/utilities/Tree.h"

#include "pedigree/kernel/utilities/assert.h"

#include "FileDescriptor.h"
#include "PosixProcess.h"
#include "logging.h"

#include "modules/system/linker/DynamicLinker.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/LockedFile.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/linker/Elf.h"

#include "file-syscalls.h"

#include <signal.h>

#include <vdso.h>  // Header with the vdso.so binary in it.

extern char __posix_compat_vsyscall_base;

#define POSIX_VSYSCALL_ADDRESS 0xffffffffff600000

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2

#define FD_CLOEXEC 1

typedef Tree<size_t, PosixSubsystem::SignalHandler *> sigHandlerTree;
typedef Tree<size_t, FileDescriptor *> FdMap;

ProcessGroupManager ProcessGroupManager::m_Instance;

extern void pedigree_init_sigret();
extern void pedigree_init_pthreads();

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
    if (m_GroupIds.test(gid))
    {
        PS_NOTICE("ProcessGroupManager: setGroupId called on a group ID that "
                  "existed already!");
    }
    m_GroupIds.set(gid);
}

bool ProcessGroupManager::isGroupIdValid(size_t gid) const
{
    return m_GroupIds.test(gid);
}

void ProcessGroupManager::returnGroupId(size_t gid)
{
    m_GroupIds.clear(gid);
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
#ifdef POSIX_NO_EFAULT
    return true;
#endif

    Uninterruptible while_checking;

#ifdef VERBOSE_KERNEL
    PS_NOTICE(
        "PosixSubsystem::checkAddress(" << Hex << addr << ", " << Dec << extent
                                        << ", " << Hex << flags << ")");
#endif

    // No memory access expected, all good.
    if (!extent)
    {
#ifdef VERBOSE_KERNEL
        PS_NOTICE("  -> zero extent, address is sane.");
#endif
        return true;
    }

    uintptr_t aa = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#ifdef VERBOSE_KERNEL
    PS_NOTICE(" -> ret: " << aa);
#endif

    // Check address range.
    VirtualAddressSpace &va = Processor::information().getVirtualAddressSpace();
    if ((addr < va.getUserStart()) || (addr >= va.getKernelStart()))
    {
#ifdef VERBOSE_KERNEL
        PS_NOTICE("  -> outside of user address area.");
#endif
        return false;
    }

    // Short-circuit if this is a memory mapped region.
    if (MemoryMapManager::instance().contains(addr, extent))
    {
#ifdef VERBOSE_KERNEL
        PS_NOTICE("  -> inside memory map.");
#endif
        return true;
    }

    // Check the range.
    for (size_t i = 0; i < extent; i += PhysicalMemoryManager::getPageSize())
    {
        void *pAddr = reinterpret_cast<void *>(addr + i);
        if (!va.isMapped(pAddr))
        {
#ifdef VERBOSE_KERNEL
            PS_NOTICE("  -> page " << Hex << pAddr << " is not mapped.");
#endif
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
#ifdef VERBOSE_KERNEL
                PS_NOTICE("  -> not writeable.");
#endif
                return false;
            }
        }
    }

#ifdef VERBOSE_KERNEL
    PS_NOTICE("  -> mapped and available.");
#endif
    return true;
}

void PosixSubsystem::exit(int code)
{
    Thread *pThread = Processor::information().getCurrentThread();

    Process *pProcess = pThread->getParent();
    NOTICE(
        "PosixSubsystem::exit(" << Dec << pProcess->getId() << ", code=" << code
                                << ")");
    pProcess->markTerminating();

    if (pProcess->getExitStatus() == 0 ||     // Normal exit.
        pProcess->getExitStatus() == 0x7F ||  // Suspended.
        pProcess->getExitStatus() == 0xFF)    // Continued.
        pProcess->setExitStatus((code & 0xFF) << 8);
    if (code)
    {
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
        if (pGroup)
        {
            if (p->getGroupMembership() == PosixProcess::Member)
            {
                for (List<PosixProcess *>::Iterator it =
                         pGroup->Members.begin();
                     it != pGroup->Members.end(); it++)
                {
                    if ((*it) == p)
                    {
                        it = pGroup->Members.erase(it);
                        break;
                    }
                }
            }
            else if (p->getGroupMembership() == PosixProcess::Leader)
            {
                // Group loses a leader, this is fine
                pGroup->Leader = nullptr;
            }

            if (!pGroup->Members.size())
            {
                // Destroy the group, we were the last process in it.
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

    // Tell some interesting info
    NOTICE("at exit for pid " << Dec << pProcess->getId() << "...");
    NOTICE(" -> file lookup LRU cache had " << m_FindFileCache.hits() << " hits and " << m_FindFileCache.misses() << " misses");

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
        PS_NOTICE("PosixSubsystem - killing " << pThread->getParent()->getId());

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
    PS_NOTICE(
        "PosixSubsystem::threadException -> "
        << Dec << pThread->getParent()->getId() << ":" << pThread->getId());

    // What was the exception?
    int signal = -1;
    switch (eType)
    {
        case PageFault:
            PS_NOTICE("    (Page fault)");
            // Send SIGSEGV
            signal = SIGSEGV;
            break;
        case InvalidOpcode:
            PS_NOTICE("    (Invalid opcode)");
            // Send SIGILL
            signal = SIGILL;
            break;
        case GeneralProtectionFault:
            PS_NOTICE("    (General Fault)");
            // Send SIGBUS
            signal = SIGBUS;
            break;
        case DivideByZero:
            PS_NOTICE("    (Division by zero)");
            // Send SIGFPE
            signal = SIGFPE;
            break;
        case FpuError:
            PS_NOTICE("    (FPU error)");
            // Send SIGFPE
            signal = SIGFPE;
            break;
        case SpecialFpuError:
            PS_NOTICE("    (FPU error - special)");
            // Send SIGFPE
            signal = SIGFPE;
            break;
        case TerminalInput:
            PS_NOTICE("    (Attempt to read from terminal by non-foreground "
                      "process)");
            // Send SIGTTIN
            signal = SIGTTIN;
            break;
        case TerminalOutput:
            PS_NOTICE("    (Output to terminal by non-foreground process)");
            // Send SIGTTOU
            signal = SIGTTOU;
            break;
        case Continue:
            PS_NOTICE("    (Continuing a stopped process)");
            // Send SIGCONT
            signal = SIGCONT;
            break;
        case Stop:
            PS_NOTICE("    (Stopping a process)");
            // Send SIGTSTP
            signal = SIGTSTP;
            break;
        case Interrupt:
            PS_NOTICE("    (Interrupting a process)");
            // Send SIGINT
            signal = SIGINT;
            break;
        case Quit:
            PS_NOTICE("    (Requesting quit)");
            // Send SIGTERM
            signal = SIGTERM;
            break;
        case Child:
            PS_NOTICE("    (Child status changed)");
            // Send SIGCHLD
            signal = SIGCHLD;
            break;
        case Pipe:
            PS_NOTICE("    (Pipe broken)");
            // Send SIGPIPE
            signal = SIGPIPE;
            break;
        default:
            PS_NOTICE("    (Unknown)");
            // Unknown exception
            ERROR(
                "Unknown exception type in threadException - POSIX subsystem");
            break;
    }

    sendSignal(pThread, signal);
}

void PosixSubsystem::sendSignal(Thread *pThread, int signal, bool yield)
{
    PS_NOTICE(
        "PosixSubsystem::sendSignal #" << signal << " -> pid:tid " << Dec
                                       << pThread->getParent()->getId() << ":"
                                       << pThread->getId());

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
    SignalHandler *sig = pSubsystem->getSignalHandler(signal);
    if (!sig)
    {
        ERROR("Unknown signal in sendSignal - POSIX subsystem");
    }

    // If we're good to go, send the signal.
    if (sig && sig->pEvent)
    {
        // Is this process already pending a delivery of the given signal?
        if (pThread->hasEvent(sig->pEvent))
        {
            // yep! we need to drop this generated signal instead of sending it
            // again to the target thread
            WARNING("PosixSubsystem::sendSignal dropping signal as a previous "
                    "generation has not delivered yet.");
        }
        else
        {
            pThread->sendEvent(sig->pEvent);

            if (yield)
            {
                Thread *pCurrentThread =
                    Processor::information().getCurrentThread();
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
    }
    else
    {
        // PS_NOTICE("No event configured for signal #" << signal << ", silently
        // dropping!");
        NOTICE(
            "No event configured for signal #" << signal
                                               << ", silently dropping!");
    }
}

void PosixSubsystem::setSignalHandler(size_t sig, SignalHandler *handler)
{
    while (!m_SignalHandlersLock.acquire())
        ;

    SignalHandler *removal = nullptr;

    sig %= 32;
    if (handler)
    {
        removal = m_SignalHandlers.lookup(sig);
        if (removal)
        {
            // Remove from the list
            m_SignalHandlers.remove(sig);
        }

        // Insert into the signal handler table
        handler->sig = sig;

        m_SignalHandlers.insert(sig, handler);
    }

    m_SignalHandlersLock.release();

    // Complete the destruction of the handler (waiting for deletion) with no
    // lock held.
    if (removal)
    {
        delete removal;
    }
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
    uintptr_t &finalAddress, bool &relocated)
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

        relocated = true;
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

        PS_NOTICE(
            pFile->getName() << " PHDR[" << i << "]: @" << Hex << base << " -> "
                             << base + length);
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
                MemoryMappedObject *pAnonymousRegion =
                    MemoryMapManager::instance().mapAnon(
                        zeroStart, end - zeroStart, perms);
                if (!pAnonymousRegion)
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

File *PosixSubsystem::findFile(const String &path, File *workingDir)
{
    if (workingDir == nullptr)
    {
        assert(m_pProcess);
        workingDir = m_pProcess->getCwd();
    }

    bool mountAwareAbi = getAbi() != PosixSubsystem::LinuxAbi;

    // for non-mount-aware ABIs, we need to fall back if the path is absolute
    // this means we can be on dev»/ and still run things like /bin/ls because
    // the lookup for dev»/bin/ls fails and falls back to root»/bin/ls
    if (mountAwareAbi || (path[0] != '/'))
    {
        // no fall back for mount-aware ABIs (e.g. Pedigree's ABI)
        // or it's a non-absolute path on a non-mount-aware ABI, and therefore
        // needs to be based on the working directory - not a different FS
        return VFS::instance().find(path, workingDir);
    }

    File *target = nullptr;
    if (!m_FindFileCache.get(path, target))
    {
        // fall back to root filesystem
        if (!m_pRootFs)
        {
            m_pRootFs = VFS::instance().lookupFilesystem(String("root"));
        }

        if (m_pRootFs)
        {
            target = VFS::instance().find(path, m_pRootFs->getRoot());
        }
    }

    if (target)
    {
        m_FindFileCache.store(path, target);
    }

    return target;
}

#define STACK_PUSH(stack, value) *--stack = value
#define STACK_PUSH2(stack, value1, value2) \
    STACK_PUSH(stack, value1);             \
    STACK_PUSH(stack, value2)
#define STACK_PUSH_COPY(stack, value, length) \
    stack = adjust_pointer(stack, -(length)); \
    MemoryCopy(stack, value, length)
#define STACK_PUSH_STRING(stack, str, length) \
    stack = adjust_pointer(stack, -(length)); \
    StringCopyN(reinterpret_cast<char *>(stack), str, length)
#define STACK_PUSH_ZEROES(stack, length)      \
    stack = adjust_pointer(stack, -(length)); \
    ByteSet(stack, 0, length)
#define STACK_ALIGN(stack, to) \
    STACK_PUSH_ZEROES(         \
        stack,                 \
        (to) - ((to) - (reinterpret_cast<uintptr_t>(stack) & ((to) -1))))

bool PosixSubsystem::invoke(
    const char *name, Vector<String> &argv, Vector<String> &env)
{
    return invoke(name, argv, env, 0);
}

bool PosixSubsystem::invoke(
    const char *name, Vector<String> &argv, Vector<String> &env,
    SyscallState &state)
{
    return invoke(name, argv, env, &state);
}

bool PosixSubsystem::parseShebang(
    File *pFile, File *&pOutFile, Vector<String> &argv)
{
    PS_NOTICE("Attempting to parse shebang in " << pFile->getFullPath());

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

    // Is this even a shebang line?
    if (!fileContents.startswith("#!"))
    {
        PS_NOTICE("no shebang found");
        return true;
    }

    // Strip the shebang.
    fileContents.lchomp();
    fileContents.lchomp();

    // OK, we have a shebang line. We need to tokenize.
    Vector<String> additionalArgv = fileContents.tokenise(' ');
    if (!additionalArgv.count())
    {
        // Not a true shebang line.
        PS_NOTICE("split didn't find anything");
        return true;
    }

    // Normalise path to ensure we have the correct path to invoke.
    String invokePath;
    String newTarget = *additionalArgv.begin();
    if (normalisePath(invokePath, static_cast<const char *>(newTarget)))
    {
        // rewrote, update argv[0] accordingly.
        newTarget = invokePath;
    }

    // Can we load the new program?
    File *pNewTarget = findFileWithAbiFallbacks(newTarget);
    if (!pNewTarget)
    {
        // No, we cannot.
        PS_NOTICE("target not found");
        SYSCALL_ERROR(DoesNotExist);
        return false;
    }

    // OK, we can now insert to argv - we do so backwards so it's just a simple
    // pushFront.
    while (additionalArgv.count())
    {
        argv.pushFront(additionalArgv.popBack());
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
        PS_NOTICE("PosixSubsystem::invoke: symlink traversal failed");
        SYSCALL_ERROR(DoesNotExist);
        return 0;
    }

    // Check for directory.
    if (pFile->isDirectory())
    {
        PS_NOTICE("PosixSubsystem::invoke: target is a directory");
        SYSCALL_ERROR(IsADirectory);
        return 0;
    }

    return pFile;
}

bool PosixSubsystem::invoke(
    const char *name, Vector<String> &argv, Vector<String> &env,
    SyscallState *state)
{
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());

#ifdef POSIX_VERBOSE_SUBSYSTEM
    PS_NOTICE("PosixSubsystem::invoke(" << name << ")");
#else
    // smaller message that always shows up to make tracking progress in logs
    // easier, but without all the extra bits that come with more verbose
    // notices (like pids, thread ids, etc).
    NOTICE("invoke: " << name << " [pid=" << pProcess->getId() << "]");
#endif

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
        PS_NOTICE(
            "PosixSubsystem::invoke: could not find file '" << name << "'");
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
        PS_NOTICE(
            "PosixSubsystem::invoke: '"
            << name << "' is not an ELF binary, looking for shebang...");

        File *shebangFile = 0;
        if (!parseShebang(originalFile, shebangFile, argv))
        {
            PS_NOTICE(
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
            PS_NOTICE(
                "PosixSubsystem::invoke: could not find interpreter '"
                << interpreter << "'");
            SYSCALL_ERROR(ExecFormatError);
            return false;
        }
    }
    else
    {
        // No interpreter, just invoke the binary directly.
        /// \todo do we need to relocate at all?
        interpreterFile = originalFile;
    }

    // No longer need the DynamicLinker instance.
    delete pLinker;
    pLinker = 0;
    pProcess->setLinker(pLinker);

    // Wipe out old address space.
    MemoryMapManager::instance().unmapAll();

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
        PS_NOTICE("PosixSubsystem::invoke: failed to map target");
        SYSCALL_ERROR(OutOfMemory);
        return false;
    }

    MemoryMappedObject *pInterpreter = MemoryMapManager::instance().mapFile(
        interpreterFile, interpreterBase, interpreterFile->getSize(), perms);
    if (!pInterpreter)
    {
        PS_NOTICE("PosixSubsystem::invoke: failed to map interpreter");
        MemoryMapManager::instance().unmap(pOriginal);
        SYSCALL_ERROR(OutOfMemory);
        return false;
    }

    // Load the target application first.
    uintptr_t originalLoadedAddress = 0;
    uintptr_t originalFinalAddress = 0;
    bool originalRelocated = false;
    if (!loadElf(
            originalFile, originalBase, originalLoadedAddress,
            originalFinalAddress, originalRelocated))
    {
        /// \todo cleanup
        PS_NOTICE("PosixSubsystem::invoke: failed to load target");
        SYSCALL_ERROR(ExecFormatError);
        return false;
    }

    // Now load the interpreter.
    uintptr_t interpreterLoadedAddress = 0;
    uintptr_t interpreterFinalAddress = 0;
    bool interpreterRelocated = false;
    if (!loadElf(
            interpreterFile, interpreterBase, interpreterLoadedAddress,
            interpreterFinalAddress, interpreterRelocated))
    {
        /// \todo cleanup
        PS_NOTICE("PosixSubsystem::invoke: failed to load interpreter");
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

    if (originalRelocated)
    {
        originalEntryPoint += originalLoadedAddress;
    }
    if (interpreterRelocated)
    {
        interpreterEntryPoint += interpreterLoadedAddress;
    }

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

    if (pProcess->getType() == Process::Posix)
    {
        /// \todo should only do this for setuid/setgid programs
        PosixProcess *p = static_cast<PosixProcess *>(pProcess);
        p->setSavedUserId(p->getEffectiveUserId());
        p->setSavedGroupId(p->getEffectiveGroupId());
    }

    // Allocate some space for the VDSO
    MemoryMappedObject::Permissions vdsoPerms = MemoryMappedObject::Read |
                                                MemoryMappedObject::Write |
                                                MemoryMappedObject::Exec;
    uintptr_t vdsoAddress = 0;
    MemoryMappedObject *pVdso = MemoryMapManager::instance().mapAnon(
        vdsoAddress, __vdso_so_pages * PhysicalMemoryManager::getPageSize(),
        vdsoPerms);
    if (!pVdso)
    {
        PS_NOTICE("PosixSubsystem::invoke: failed to map VDSO");
    }
    else
    {
        // All good, copy in the VDSO ELF image now.
        MemoryCopy(
            reinterpret_cast<void *>(vdsoAddress), __vdso_so, __vdso_so_len);

        // Readjust permissions to remove write access now that the image is
        // loaded.
        MemoryMapManager::instance().setPermissions(
            vdsoAddress, __vdso_so_pages * PhysicalMemoryManager::getPageSize(),
            vdsoPerms & ~MemoryMappedObject::Write);
    }

    // Map in the vsyscall space.
    if (!Processor::information().getVirtualAddressSpace().isMapped(
            reinterpret_cast<void *>(POSIX_VSYSCALL_ADDRESS)))
    {
        physical_uintptr_t vsyscallBase = 0;
        size_t vsyscallFlags = 0;
        Processor::information().getVirtualAddressSpace().getMapping(
            &__posix_compat_vsyscall_base, vsyscallBase, vsyscallFlags);
        Processor::information().getVirtualAddressSpace().map(
            vsyscallBase, reinterpret_cast<void *>(POSIX_VSYSCALL_ADDRESS),
            VirtualAddressSpace::Execute);
    }

    // We can now build the auxiliary vector to pass to the dynamic linker.
    VirtualAddressSpace::Stack *stack =
        Processor::information().getVirtualAddressSpace().allocateStack();
    uintptr_t *loaderStack = reinterpret_cast<uintptr_t *>(stack->getTop());

    // Top of stack = zero to mark end
    STACK_PUSH(loaderStack, 0);

    // Align to 16 byte stack
    STACK_ALIGN(loaderStack, 16);

    // Push argv/env.
    char **envs = new char *[env.count()];
    size_t envc = 0;
    for (size_t i = 0; i < env.count(); ++i)
    {
        String &str = env[i];
        STACK_PUSH_STRING(
            loaderStack, static_cast<const char *>(str), str.length() + 1);
        PS_NOTICE("env[" << envc << "]: " << str);
        envs[envc++] = reinterpret_cast<char *>(loaderStack);
    }

    // Align to 16 bytes between env and argv
    STACK_ALIGN(loaderStack, 16);

    char **argvs = new char *[argv.count()];
    size_t argc = 0;
    for (size_t i = 0; i < argv.count(); ++i)
    {
        String &str = argv[i];
        STACK_PUSH_STRING(
            loaderStack, static_cast<const char *>(str), str.length() + 1);
        PS_NOTICE("argv[" << argc << "]: " << str);
        argvs[argc++] = reinterpret_cast<char *>(loaderStack);
    }

    // Align to 16 bytes between argv and remaining strings
    STACK_ALIGN(loaderStack, 16);

    /// \todo platform assumption here.
    STACK_PUSH_STRING(loaderStack, "x86_64", 7);
    void *platform = loaderStack;

    STACK_PUSH_STRING(loaderStack, name, originalName.length() + 1);
    void *execfn = loaderStack;

    // Align to 16 bytes to prepare for the auxv entries
    STACK_ALIGN(loaderStack, 16);

    /// \todo 16 random bytes, not 16 zero bytes
    STACK_PUSH_ZEROES(loaderStack, 16);
    void *random = loaderStack;

    // Ensure argc aligns to 16 bytes.
    if (((argc + envc) % 2) == 0)
    {
        STACK_PUSH_ZEROES(loaderStack, 8);
    }

    // Build the aux vector now.
    STACK_PUSH2(loaderStack, 0, 0);  // AT_NULL
    STACK_PUSH2(
        loaderStack, reinterpret_cast<uintptr_t>(platform), 15);  // AT_PLATFORM
    STACK_PUSH2(
        loaderStack, reinterpret_cast<uintptr_t>(random), 25);  // AT_RANDOM
    STACK_PUSH2(loaderStack, 0, 23);
    STACK_PUSH2(loaderStack, pProcess->getUserId(), 14);            // AT_EGID
    STACK_PUSH2(loaderStack, pProcess->getGroupId(), 13);           // AT_GID
    STACK_PUSH2(loaderStack, pProcess->getEffectiveUserId(), 12);   // AT_EUID
    STACK_PUSH2(loaderStack, pProcess->getEffectiveGroupId(), 11);  // AT_UID
    STACK_PUSH2(
        loaderStack, reinterpret_cast<uintptr_t>(execfn), 31);  // AT_EXECFN

    // Push the vDSO shared object.
    if (pVdso)
    {
        STACK_PUSH2(loaderStack, 0, 32);            // AT_SYSINFO - not present
        STACK_PUSH2(loaderStack, vdsoAddress, 33);  // AT_SYSINFO_EHDR
    }

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
    for (size_t i = 0; i < envc; ++i)
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

    // Initialise the sigret if not already done for this process
    pedigree_init_sigret();
    // pedigree_init_pthreads();

    Processor::setInterrupts(true);
    pProcess->recordTime(true);

    if (!state)
    {
        // Just create a new thread, this is not a full replace.
        Thread *pNewThread = new Thread(
            pProcess,
            reinterpret_cast<Thread::ThreadStartFunc>(interpreterEntryPoint), 0,
            loaderStack);
        pNewThread->detach();

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
            0, interpreterEntryPoint, reinterpret_cast<uintptr_t>(loaderStack));
    }

    // unreachable
}
