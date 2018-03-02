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

#include "system-syscalls.h"
#include "file-syscalls.h"
#include "modules/system/linker/DynamicLinker.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Version.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/StackFrame.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pipe-syscalls.h"
#include "posixSyscallNumbers.h"
#include "pthread-syscalls.h"
#include "signal-syscalls.h"

#define MACHINE_FORWARD_DECL_ONLY
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"

#include "pedigree/kernel/Subsystem.h"
#include <PosixProcess.h>
#include <PosixSubsystem.h>

#include "modules/system/console/Console.h"
#include "modules/system/users/UserManager.h"

#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <syslog.h>

// arch_prctl
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

//
// Syscalls pertaining to system operations.
//

#define GET_CWD() \
    (Processor::information().getCurrentThread()->getParent()->getCwd())

/// Saves a char** array in the Vector of String*s given.
static size_t
save_string_array(const char **array, Vector<SharedPointer<String>> &rArray)
{
    size_t result = 0;
    while (*array)
    {
        String *pStr = new String(*array);
        rArray.pushBack(SharedPointer<String>(pStr));
        array++;

        result += pStr->length() + 1;
    }

    return result;
}

/// Creates a char** array, properly null-terminated, from the Vector of
/// String*s given, at the location "arrayLoc", returning the end of the char**
/// array created in arrayEndLoc and the start as the function return value.
static char **load_string_array(
    Vector<SharedPointer<String>> &rArray, uintptr_t arrayLoc,
    uintptr_t &arrayEndLoc)
{
    char **pMasterArray = reinterpret_cast<char **>(arrayLoc);

    char *pPtr = reinterpret_cast<char *>(
        arrayLoc + sizeof(char *) * (rArray.count() + 1));
    int i = 0;
    for (auto it = rArray.begin(); it != rArray.end(); it++)
    {
        SharedPointer<String> pStr = *it;

        StringCopy(pPtr, *pStr);
        pPtr[pStr->length()] = '\0';  // Ensure NULL-termination.

        pMasterArray[i] = pPtr;

        pPtr += pStr->length() + 1;
        i++;
    }

    pMasterArray[i] = 0;  // Null terminate.
    arrayEndLoc = reinterpret_cast<uintptr_t>(pPtr);

    return pMasterArray;
}

long posix_sbrk(int delta)
{
    SC_NOTICE("sbrk(" << delta << ")");

    long ret = reinterpret_cast<long>(
        Processor::information().getVirtualAddressSpace().expandHeap(
            delta, VirtualAddressSpace::Write));
    SC_NOTICE("    -> " << ret);
    if (ret == 0)
    {
        SYSCALL_ERROR(OutOfMemory);
        return -1;
    }
    else
        return ret;
}

uintptr_t posix_brk(uintptr_t theBreak)
{
    SC_NOTICE("brk(" << theBreak << ")");

    void *newBreak = reinterpret_cast<void *>(theBreak);

    void *currentBreak =
        Processor::information().getVirtualAddressSpace().getEndOfHeap();
    if (newBreak < currentBreak)
    {
        SC_NOTICE(" -> " << currentBreak);
        return reinterpret_cast<uintptr_t>(currentBreak);
    }

    intptr_t difference = pointer_diff(currentBreak, newBreak);
    if (!difference)
    {
        SC_NOTICE(" -> " << currentBreak);
        return reinterpret_cast<uintptr_t>(currentBreak);
    }

    // OK, good to go.
    void *result = Processor::information().getVirtualAddressSpace().expandHeap(
        difference, VirtualAddressSpace::Write);
    if (!result)
    {
        SYSCALL_ERROR(OutOfMemory);
        SC_NOTICE(" -> ENOMEM");
        return -1;
    }

    // Return new end of heap.
    currentBreak =
        Processor::information().getVirtualAddressSpace().getEndOfHeap();

    SC_NOTICE(" -> " << currentBreak);
    return reinterpret_cast<uintptr_t>(currentBreak);
}

long posix_clone(
    SyscallState &state, unsigned long flags, void *child_stack, int *ptid,
    int *ctid, unsigned long newtls)
{
    SC_NOTICE(
        "clone(" << Hex << flags << ", " << child_stack << ", " << ptid << ", "
                 << ctid << ", " << newtls << ")");

    Processor::setInterrupts(false);

    // Must clone state as we make modifications for the new thread here.
    SyscallState clonedState = state;

    // Basic warnings to start with.
    if (flags & CLONE_CHILD_CLEARTID)
    {
        WARNING(" -> CLONE_CHILD_CLEARTID is not yet supported!");
    }
    if (flags & CLONE_PARENT)
    {
        WARNING(" -> CLONE_PARENT is not yet supported!");
    }
    if (flags & CLONE_VFORK)
    {
        // Halts parent until child ruins execve() or exit(), just like vfork.
        // We should support this properly.
        WARNING(" -> CLONE_VFORK is not yet supported!");
    }

#if 0
    if (flags & CLONE_VM) NOTICE("\t\t-> CLONE_VM");
    if (flags & CLONE_FS) NOTICE("\t\t-> CLONE_FS");
    if (flags & CLONE_FILES) NOTICE("\t\t-> CLONE_FILES");
    if (flags & CLONE_SIGHAND) NOTICE("\t\t-> CLONE_SIGHAND");
    if (flags & CLONE_PTRACE) NOTICE("\t\t-> CLONE_PTRACE");
    if (flags & CLONE_VFORK) NOTICE("\t\t-> CLONE_VFORK");
    if (flags & CLONE_PARENT) NOTICE("\t\t-> CLONE_PARENT");
    if (flags & CLONE_THREAD) NOTICE("\t\t-> CLONE_THREAD");
    if (flags & CLONE_NEWNS) NOTICE("\t\t-> CLONE_NEWNS");
    if (flags & CLONE_SYSVSEM) NOTICE("\t\t-> CLONE_SYSVSEM");
    if (flags & CLONE_SETTLS) NOTICE("\t\t-> CLONE_SETTLS");
    if (flags & CLONE_PARENT_SETTID) NOTICE("\t\t-> CLONE_PARENT_SETTID");
    if (flags & CLONE_CHILD_CLEARTID) NOTICE("\t\t-> CLONE_CHILD_CLEARTID");
    if (flags & CLONE_DETACHED) NOTICE("\t\t-> CLONE_DETACHED");
    if (flags & CLONE_UNTRACED) NOTICE("\t\t-> CLONE_UNTRACED");
    if (flags & CLONE_CHILD_SETTID) NOTICE("\t\t-> CLONE_CHILD_SETTID");
    if (flags & CLONE_NEWUTS) NOTICE("\t\t-> CLONE_NEWUTS");
    if (flags & CLONE_NEWIPC) NOTICE("\t\t-> CLONE_NEWIPC");
    if (flags & CLONE_NEWUSER) NOTICE("\t\t-> CLONE_NEWUSER");
    if (flags & CLONE_NEWPID) NOTICE("\t\t-> CLONE_NEWPID");
    if (flags & CLONE_NEWNET) NOTICE("\t\t-> CLONE_NEWNET");
    if (flags & CLONE_IO) NOTICE("\t\t-> CLONE_IO");
#endif

    if ((flags & CLONE_VM) == CLONE_VM)
    {
        // clone vm doesn't actually copy the address space, it shares it

        // New child's stack. Must be valid as we're sharing the address space.
        if (!child_stack)
        {
            SYSCALL_ERROR(InvalidArgument);
            return -1;
        }

        // Set up stack for new thread.
        clonedState.setStackPointer(reinterpret_cast<uintptr_t>(child_stack));

        // Child returns 0 -- parent returns the new thread ID.
        clonedState.setSyscallReturnValue(0);

        // pretty much just a thread
        Process *pParentProcess =
            Processor::information().getCurrentThread()->getParent();

        // Create a new thread for the new process. Make sure it's
        // delayed-start so we can ensure the new thread ID gets written to the
        // right places in memory.
        Thread *pThread = new Thread(pParentProcess, clonedState, true);
        pThread->setTlsBase(newtls);
        pThread->detach();

        // Update the child ID before letting the child run
        if (flags & CLONE_CHILD_SETTID)
        {
            *ctid = pThread->getId();
        }
        if (flags & CLONE_PARENT_SETTID)
        {
            *ptid = pThread->getId();
        }

        pThread->setStatus(Thread::Ready);  // good to go now.

        // Parent gets the new thread ID.
        SC_NOTICE(" -> " << pThread->getId() << " [new thread]");
        return pThread->getId();
    }

    // No child stack means CoW the existing one, but if one is specified we
    // should use it instead!
    if (child_stack)
    {
        clonedState.setStackPointer(reinterpret_cast<uintptr_t>(child_stack));
    }

    // Inhibit signals to the parent
    for (int sig = 0; sig < 32; sig++)
        Processor::information().getCurrentThread()->inhibitEvent(sig, true);

    // Create a new process.
    Process *pParentProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixProcess *pProcess = new PosixProcess(pParentProcess);
    if (!pProcess)
    {
        SYSCALL_ERROR(OutOfMemory);
        SC_NOTICE(" -> ENOMEM");
        return -1;
    }

    PosixSubsystem *pParentSubsystem =
        reinterpret_cast<PosixSubsystem *>(pParentProcess->getSubsystem());
    PosixSubsystem *pSubsystem = new PosixSubsystem(*pParentSubsystem);
    if (!pSubsystem || !pParentSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");

        if (pSubsystem)
            delete pSubsystem;
        if (pParentSubsystem)
            delete pParentSubsystem;
        delete pProcess;

        SYSCALL_ERROR(OutOfMemory);

        // Allow signals again, something went wrong
        for (int sig = 0; sig < 32; sig++)
            Processor::information().getCurrentThread()->inhibitEvent(
                sig, false);
        SC_NOTICE(" -> ENOMEM");
        return -1;
    }
    pProcess->setSubsystem(pSubsystem);
    pSubsystem->setProcess(pProcess);

    // Copy POSIX Process Group information if needed
    if (pParentProcess->getType() == Process::Posix)
    {
        PosixProcess *p = static_cast<PosixProcess *>(pParentProcess);
        pProcess->setProcessGroup(p->getProcessGroup());

        // default to being a member of the group
        pProcess->setGroupMembership(PosixProcess::Member);

        // Do not adopt leadership status.
        if (p->getGroupMembership() == PosixProcess::Leader)
        {
            SC_NOTICE("fork parent was a group leader.");
        }
        else
        {
            SC_NOTICE(
                "fork parent had status "
                << static_cast<int>(p->getGroupMembership()) << "...");
            pProcess->setGroupMembership(p->getGroupMembership());
        }
    }

    // Register with the dynamic linker.
    DynamicLinker *oldLinker = pProcess->getLinker();
    if (oldLinker)
    {
        DynamicLinker *newLinker = new DynamicLinker(*oldLinker);
        pProcess->setLinker(newLinker);
    }

    MemoryMapManager::instance().clone(pProcess);

    // Copy the file descriptors from the parent
    pSubsystem->copyDescriptors(pParentSubsystem);

    // Child returns 0.
    clonedState.setSyscallReturnValue(0);

    // Allow signals to the parent again
    for (int sig = 0; sig < 32; sig++)
        Processor::information().getCurrentThread()->inhibitEvent(sig, false);

    // Set ctid in the new address space if we are required to.
    if (flags & CLONE_CHILD_SETTID)
    {
        VirtualAddressSpace &curr =
            Processor::information().getVirtualAddressSpace();
        VirtualAddressSpace *va = pProcess->getAddressSpace();
        Processor::switchAddressSpace(*va);
        *ctid = pProcess->getId();
        Processor::switchAddressSpace(curr);
    }

    // Create a new thread for the new process.
    Thread *pThread = new Thread(pProcess, clonedState);
    pThread->detach();

    // Fix up the main thread in the child.
    /// \todo this is too late - the Thread constructor starts the thread
    ///       already! We need a way to have threads start suspended so they
    ///       can be unblocked by callers when they are ready to run.
    pedigree_copy_posix_thread(
        Processor::information().getCurrentThread(), pParentSubsystem, pThread,
        pSubsystem);

    // Parent returns child ID.
    SC_NOTICE(" -> " << pProcess->getId() << " [new process]");
    return pProcess->getId();
}

int posix_fork(SyscallState &state)
{
    SC_NOTICE("fork");

    return posix_clone(state, 0, 0, 0, 0, 0);
}

int posix_execve(
    const char *name, const char **argv, const char **env, SyscallState &state)
{
    /// \todo Check argv/env??
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(name), PATH_MAX,
            PosixSubsystem::SafeRead))
    {
        SC_NOTICE("execve -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    SC_NOTICE("execve(\"" << name << "\")");

    // Bad arguments?
    if (argv == 0 || env == 0)
    {
        SYSCALL_ERROR(ExecFormatError);
        return -1;
    }

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    // Build argv and env lists.
    List<SharedPointer<String>> listArgv, listEnv;
    for (const char **arg = argv; *arg != 0; ++arg)
    {
        listArgv.pushBack(SharedPointer<String>(new String(*arg)));
    }
    for (const char **e = env; *e != 0; ++e)
    {
        listEnv.pushBack(SharedPointer<String>(new String(*e)));
    }

    // Normalise path to ensure we have the correct path to invoke.
    String invokePath;
    normalisePath(invokePath, name);

    if (!pSubsystem->invoke(invokePath, listArgv, listEnv, state))
    {
        SC_NOTICE(" -> execve failed in invoke");
        return -1;
    }

    // Technically, we never get here.
    return 0;
}

/**
 * Class intended to be used for RAII to clean up waitpid state on exit.
 */
class WaitCleanup
{
  public:
    WaitCleanup(List<Process *> *cleanupList, Semaphore *lock)
        : m_List(cleanupList), m_Lock(lock), m_pTerminated(0)
    {
    }

    /**
     * Call this with the process that terminated most recently, which
     * is necessary because otherwise upon exit from waitpid() we attempt
     * to access the (deleted) Process object, which is not safe.
     */
    void terminated(Process *pProcess)
    {
        m_pTerminated = pProcess;
        pProcess->removeWaiter(m_Lock);
    }

    ~WaitCleanup()
    {
        for (List<Process *>::Iterator it = m_List->begin();
             it != m_List->end(); ++it)
        {
            if ((*it) == m_pTerminated)
                continue;

            (*it)->removeWaiter(m_Lock);
        }
    }

  private:
    List<Process *> *m_List;
    Semaphore *m_Lock;
    Process *m_pTerminated;
};

int posix_waitpid(const int pid, int *status, int options)
{
    if (status && !PosixSubsystem::checkAddress(
                      reinterpret_cast<uintptr_t>(status), sizeof(int),
                      PosixSubsystem::SafeWrite))
    {
        SC_NOTICE("waitpid -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    SC_NOTICE(
        "waitpid(" << pid << " [" << Dec << pid << Hex << "], " << options
                   << ")");

    // Find the set of processes to check.
    List<Process *> processList;

    // Our lock, which we will assign to each process (assuming WNOHANG is not
    // set).
    Semaphore waitLock(0);

    // RAII object to clean up when we return (instead of goto or other
    // ugliness).
    WaitCleanup cleanup(&processList, &waitLock);

    // Metadata about the calling process.
    PosixProcess *pThisProcess = static_cast<PosixProcess *>(
        Processor::information().getCurrentThread()->getParent());
    ProcessGroup *pThisGroup = pThisProcess->getProcessGroup();

    // Check for the process(es) we need to check for.
    size_t i = 0;
    bool bBlock = (options & WNOHANG) != WNOHANG;
    for (; i < Scheduler::instance().getNumProcesses(); ++i)
    {
        Process *pProcess = Scheduler::instance().getProcess(i);
        if (pProcess == pThisProcess)
            continue;  // Don't wait for ourselves.

        if (pProcess->getState() == Process::Reaped)
            continue;  // Reaped but not yet destroyed.

        if ((pid <= 0) && (pProcess->getType() == Process::Posix))
        {
            PosixProcess *pPosixProcess = static_cast<PosixProcess *>(pProcess);
            ProcessGroup *pGroup = pPosixProcess->getProcessGroup();
            if (pid == 0)
            {
                // Any process in the same process group as the caller.
                if (!(pGroup && pThisGroup))
                    continue;
                if (pGroup->processGroupId != pThisGroup->processGroupId)
                    continue;
            }
            else if (pid == -1)
            {
                // Wait for any child.
                if (pProcess->getParent() != pThisProcess)
                    continue;
            }
            else if (pGroup && (pGroup->processGroupId != (pid * -1)))
            {
                // Absolute group ID reference
                continue;
            }
        }
        else if ((pid > 0) && (static_cast<int>(pProcess->getId()) != pid))
            continue;
        else if (pProcess->getType() != Process::Posix)
            continue;

        // Okay, the process is good.
        processList.pushBack(pProcess);

        // If not WNOHANG, subscribe our lock to this process' state changes.
        // If the process is in the process of terminating, we can add our
        // lock and hope for the best.
        if (bBlock || (pProcess->getState() == Process::Terminating))
        {
            SC_NOTICE(
                "  -> adding our wait lock to process " << pProcess->getId());
            pProcess->addWaiter(&waitLock);
            bBlock = true;
        }
    }

    // No children?
    if (processList.count() == 0)
    {
        SYSCALL_ERROR(NoChildren);
        SC_NOTICE("  -> no children");
        return -1;
    }

    // Main wait loop.
    while (1)
    {
        // Check each process for state.
        for (List<Process *>::Iterator it = processList.begin();
             it != processList.end(); ++it)
        {
            Process *pProcess = *it;
            int this_pid = pProcess->getId();

            // Zombie?
            if (pProcess->getState() == Process::Terminated)
            {
                if (status)
                    *status = pProcess->getExitStatus();

                // Delete the process; it's been reaped good and proper.
                SC_NOTICE(
                    "waitpid: " << this_pid << " reaped ["
                                << pProcess->getExitStatus() << "]");
                cleanup.terminated(pProcess);
                if (pProcess->waiterCount() < 1)
                    delete pProcess;
                else
                    pProcess->reap();
                return this_pid;
            }
            // Suspended (and WUNTRACED)?
            else if ((options & 2) && pProcess->hasSuspended())
            {
                if (status)
                    *status = pProcess->getExitStatus();

                SC_NOTICE("waitpid: " << this_pid << " suspended.");
                return this_pid;
            }
            // Continued (and WCONTINUED)?
            else if ((options & 4) && pProcess->hasResumed())
            {
                if (status)
                    *status = pProcess->getExitStatus();

                SC_NOTICE("waitpid: " << this_pid << " resumed.");
                return this_pid;
            }
        }

        // Don't wait for any processes to report status if we are not meant
        // to be blocking.
        if (!bBlock)
        {
            return 0;
        }

        // Wait for processes to report in.
        waitLock.acquire();

        // We can get woken up by our process dying. Handle that here.
        if (Processor::information().getCurrentThread()->getUnwindState() ==
            Thread::Exit)
        {
            SC_NOTICE("waitpid: unwind state means exit");
            return -1;
        }

        // We get notified by processes just before they change state.
        // Make sure they are scheduled into that state by yielding.
        Scheduler::instance().yield();
    }
}

int posix_exit(int code, bool allthreads)
{
    SC_NOTICE("exit(" << Dec << (code & 0xFF) << Hex << ")");

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());

    if (allthreads)
    {
        SC_NOTICE(" -> thread group");
        pSubsystem->exit(code);
    }
    else
    {
        // Not all threads - only kill current thread!
        SC_NOTICE(" -> current thread");
        Processor::information().getScheduler().killCurrentThread();
    }

    // Should NEVER get here.
    FATAL("exit method returned in posix_exit");
}

int posix_getpid()
{
    SC_NOTICE("getpid");

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    return pProcess->getId();
}

int posix_getppid()
{
    SC_NOTICE("getppid");

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    if (!pProcess->getParent())
        return 0;
    return pProcess->getParent()->getId();
}

int posix_gettimeofday(timeval *tv, struct timezone *tz)
{
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(tv), sizeof(timeval),
            PosixSubsystem::SafeWrite))
    {
        SC_NOTICE("gettimeofday -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    SC_NOTICE("gettimeofday");

    Timer *pTimer = Machine::instance().getTimer();

    // UNIX timestamp + remaining time portion, in microseconds.
    tv->tv_sec = pTimer->getUnixTimestamp();
    tv->tv_usec = pTimer->getNanosecond() / 1000U;

    return 0;
}

int posix_settimeofday(const timeval *tv, const struct timezone *tz)
{
    SC_NOTICE("settimeofday");

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(tv), sizeof(timeval),
            PosixSubsystem::SafeRead))
    {
        SC_NOTICE(" -> invalid address");
        SYSCALL_ERROR(BadAddress);
        return -1;
    }

    /// \todo support this

    return 0;
}

time_t posix_time(time_t *tval)
{
    SC_NOTICE("time");

    if (tval && !PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(tval), sizeof(time_t),
            PosixSubsystem::SafeWrite))
    {
        SC_NOTICE(" -> invalid address");
        SYSCALL_ERROR(BadAddress);
        return -1;
    }

    time_t result = Time::getTime();
    if (tval)
    {
        *tval = result;
    }

    return result;
}

clock_t posix_times(struct tms *tm)
{
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(tm), sizeof(struct tms),
            PosixSubsystem::SafeWrite))
    {
        SC_NOTICE("posix_times -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    SC_NOTICE("times");

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();

    ByteSet(tm, 0, sizeof(struct tms));
    tm->tms_utime = pProcess->getUserTime();
    tm->tms_stime = pProcess->getKernelTime();

    NOTICE(
        "times: u=" << pProcess->getUserTime()
                    << ", s=" << pProcess->getKernelTime());

    return Time::getTimeNanoseconds() - pProcess->getStartTime();
}

int posix_getrusage(int who, struct rusage *r)
{
    SC_NOTICE("getrusage who=" << who);

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(r), sizeof(struct rusage),
            PosixSubsystem::SafeWrite))
    {
        SC_NOTICE("posix_getrusage -> invalid address");
        SYSCALL_ERROR(BadAddress);
        return -1;
    }

    if (who != RUSAGE_SELF)
    {
        SC_NOTICE("posix_getrusage -> non-RUSAGE_SELF not supported");
        SYSCALL_ERROR(InvalidArgument);
        ByteSet(r, 0, sizeof(struct rusage));
        return -1;
    }

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();

    Time::Timestamp user = pProcess->getUserTime();
    Time::Timestamp kernel = pProcess->getKernelTime();

    ByteSet(r, 0, sizeof(struct rusage));
    r->ru_utime.tv_sec = user / Time::Multiplier::Second;
    r->ru_utime.tv_usec =
        (user % Time::Multiplier::Second) / Time::Multiplier::Microsecond;
    r->ru_stime.tv_sec = kernel / Time::Multiplier::Second;
    r->ru_stime.tv_usec =
        (kernel % Time::Multiplier::Second) / Time::Multiplier::Microsecond;

    return 0;
}

static char *store_str_to(char *str, char *strend, String s)
{
    int i = 0;
    while (s[i] && str != strend)
        *str++ = s[i++];
    *str++ = '\0';

    return str;
}

int posix_getpwent(passwd *pw, int n, char *str)
{
    /// \todo 'str' is not very nice here, can we do this better?
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(pw), sizeof(passwd),
            PosixSubsystem::SafeWrite))
    {
        SC_NOTICE("getpwent -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    SC_NOTICE("getpwent(" << Dec << n << Hex << ")");

    // Grab the given user.
    User *pUser = UserManager::instance().getUser(n);
    if (!pUser)
        return -1;

    char *strend = str + 256;  // If we get here, we've gone off the end of str.

    pw->pw_name = str;
    str = store_str_to(str, strend, pUser->getUsername());

    pw->pw_passwd = str;
    *str++ = '\0';

    pw->pw_uid = pUser->getId();
    pw->pw_gid = pUser->getDefaultGroup()->getId();
    str = store_str_to(str, strend, pUser->getFullName());

    pw->pw_gecos = str;
    *str++ = '\0';
    pw->pw_dir = str;
    str = store_str_to(str, strend, pUser->getHome());

    pw->pw_shell = str;
    store_str_to(str, strend, pUser->getShell());

    return 0;
}

int posix_getpwnam(passwd *pw, const char *name, char *str)
{
    /// \todo Again, str is not very nice here.
    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(pw), sizeof(passwd),
              PosixSubsystem::SafeWrite) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(name), PATH_MAX,
              PosixSubsystem::SafeRead)))
    {
        SC_NOTICE("getpwname -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    SC_NOTICE("getpwname(" << name << ")");

    // Grab the given user.
    User *pUser = UserManager::instance().getUser(String(name));
    if (!pUser)
        return -1;

    char *strend = str + 256;  // If we get here, we've gone off the end of str.

    pw->pw_name = str;
    str = store_str_to(str, strend, pUser->getUsername());

    pw->pw_passwd = str;
    *str++ = '\0';

    pw->pw_uid = pUser->getId();
    pw->pw_gid = pUser->getDefaultGroup()->getId();
    str = store_str_to(str, strend, pUser->getFullName());

    pw->pw_gecos = str;
    *str++ = '\0';

    pw->pw_dir = str;
    str = store_str_to(str, strend, pUser->getHome());

    pw->pw_shell = str;
    store_str_to(str, strend, pUser->getShell());

    return 0;
}

int posix_getgrnam(const char *name, struct group *out)
{
    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(name), PATH_MAX,
              PosixSubsystem::SafeRead) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(out), sizeof(struct group),
              PosixSubsystem::SafeWrite)))
    {
        SC_NOTICE("getgrnam -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    SC_NOTICE("getgrnam(" << name << ")");

    Group *pGroup = UserManager::instance().getGroup(String(name));
    if (!pGroup)
    {
        // No error needs to be set if not found.
        return -1;
    }

    /// \todo this ignores the members field
    StringCopy(out->gr_name, static_cast<const char *>(pGroup->getName()));
    out->gr_gid = pGroup->getId();

    return 0;
}

int posix_getgrgid(gid_t id, struct group *out)
{
    if (!(PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(out), sizeof(struct group),
            PosixSubsystem::SafeWrite)))
    {
        SC_NOTICE("getgrgid( -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    SC_NOTICE("getgrgid(" << id << ")");

    Group *pGroup = UserManager::instance().getGroup(id);
    if (!pGroup)
    {
        // No error needs to be set if not found.
        return -1;
    }

    /// \todo this ignores the members field
    StringCopy(out->gr_name, static_cast<const char *>(pGroup->getName()));
    out->gr_gid = pGroup->getId();

    return 0;
}

uid_t posix_getuid()
{
    SC_NOTICE(
        "getuid() -> " << Dec
                       << Processor::information()
                              .getCurrentThread()
                              ->getParent()
                              ->getUser()
                              ->getId());
    return Processor::information()
        .getCurrentThread()
        ->getParent()
        ->getUser()
        ->getId();
}

gid_t posix_getgid()
{
    SC_NOTICE(
        "getgid() -> " << Dec
                       << Processor::information()
                              .getCurrentThread()
                              ->getParent()
                              ->getGroup()
                              ->getId());
    return Processor::information()
        .getCurrentThread()
        ->getParent()
        ->getGroup()
        ->getId();
}

uid_t posix_geteuid()
{
    SC_NOTICE(
        "geteuid() -> " << Dec
                        << Processor::information()
                               .getCurrentThread()
                               ->getParent()
                               ->getEffectiveUser()
                               ->getId());
    return Processor::information()
        .getCurrentThread()
        ->getParent()
        ->getEffectiveUser()
        ->getId();
}

gid_t posix_getegid()
{
    SC_NOTICE(
        "getegid() -> " << Dec
                        << Processor::information()
                               .getCurrentThread()
                               ->getParent()
                               ->getEffectiveGroup()
                               ->getId());
    return Processor::information()
        .getCurrentThread()
        ->getParent()
        ->getEffectiveGroup()
        ->getId();
}

int posix_setuid(uid_t uid)
{
    SC_NOTICE("setuid(" << uid << ")");

    /// \todo Missing "set user"
    User *user = UserManager::instance().getUser(uid);
    if (!user)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    /// \todo Make sure we are actually allowed to do this!
    Processor::information().getCurrentThread()->getParent()->setUser(user);
    Processor::information().getCurrentThread()->getParent()->setEffectiveUser(
        user);

    return 0;
}

int posix_setgid(gid_t gid)
{
    SC_NOTICE("setgid(" << gid << ")");

    /// \todo Missing "set user"
    Group *group = UserManager::instance().getGroup(gid);
    if (!group)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    /// \todo Make sure we are actually allowed to do this!
    Processor::information().getCurrentThread()->getParent()->setGroup(group);
    Processor::information().getCurrentThread()->getParent()->setEffectiveGroup(
        group);

    return 0;
}

int posix_seteuid(uid_t euid)
{
    SC_NOTICE("seteuid(" << euid << ")");

    /// \todo Missing "set user"
    User *user = UserManager::instance().getUser(euid);
    if (!user)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    Processor::information().getCurrentThread()->getParent()->setEffectiveUser(
        user);

    return 0;
}

int posix_setegid(gid_t egid)
{
    SC_NOTICE("setegid(" << egid << ")");

    /// \todo Missing "set user"
    Group *group = UserManager::instance().getGroup(egid);
    if (!group)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    Processor::information().getCurrentThread()->getParent()->setEffectiveGroup(
        group);

    return 0;
}

int pedigree_login(int uid, const char *password)
{
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(password), PATH_MAX,
            PosixSubsystem::SafeRead))
    {
        SC_NOTICE("pedigree_login -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Grab the given user.
    User *pUser = UserManager::instance().getUser(uid);
    if (!pUser)
        return -1;

    if (pUser->login(String(password)))
        return 0;
    else
        return -1;
}

int posix_setsid()
{
    SC_NOTICE("setsid");

    // Not a POSIX process
    Process *pStockProcess =
        Processor::information().getCurrentThread()->getParent();
    if (pStockProcess->getType() != Process::Posix)
    {
        ERROR("setsid called on something not a POSIX process");
        return -1;
    }

    PosixProcess *pProcess = static_cast<PosixProcess *>(pStockProcess);

    // Already in a group?
    PosixProcess::Membership myMembership = pProcess->getGroupMembership();
    if (myMembership != PosixProcess::NoGroup)
    {
        // If we don't actually have a group, something's gone wrong
        if (!pProcess->getProcessGroup())
            FATAL("Process' is apparently a member of a group, but its group "
                  "pointer is invalid.");

        // Are we the group leader of that other group?
        if (myMembership == PosixProcess::Leader)
        {
            SC_NOTICE("setsid() called while the leader of another group");
            SYSCALL_ERROR(PermissionDenied);
            return -1;
        }
        else
        {
            SC_NOTICE(
                "setsid() called while a member of another group ["
                << pProcess->getProcessGroup()->processGroupId << "]");
        }
    }

    // Delete the old group, if any
    ProcessGroup *pGroup = pProcess->getProcessGroup();
    if (pGroup)
    {
        pProcess->setProcessGroup(0);

        /// \todo Remove us from the list
        /// \todo Remove others from the list!?
        if (pGroup->Members.count() <= 1)  // Us or nothing
            delete pGroup;
    }

    // Create the new session.
    PosixSession *pNewSession = new PosixSession();
    pNewSession->Leader = pProcess;
    pProcess->setSession(pNewSession);

    // Create a new process group and join it.
    ProcessGroup *pNewGroup = new ProcessGroup;
    pNewGroup->processGroupId = pProcess->getId();
    pNewGroup->Leader = pProcess;
    pNewGroup->Members.clear();

    // We're now a group leader - we got promoted!
    pProcess->setProcessGroup(pNewGroup);
    pProcess->setGroupMembership(PosixProcess::Leader);

    // Remove controlling terminal.
    pProcess->setCtty(0);

    SC_NOTICE(
        "setsid: now part of a group [id=" << pNewGroup->processGroupId
                                           << "]!");

    // Success!
    return pNewGroup->processGroupId;
}

int posix_setpgid(int pid_, int pgid)
{
    size_t pid = pid_;
    SC_NOTICE("setpgid(" << pid << ", " << pgid << ")");

    // Handle invalid group ID
    if (pgid < 0)
    {
        SYSCALL_ERROR(InvalidArgument);
        SC_NOTICE(" -> EINVAL");
        return -1;
    }

    Process *pBaseProcess =
        Processor::information().getCurrentThread()->getParent();
    if (pBaseProcess->getType() != Process::Posix)
    {
        SC_NOTICE("  -> not a posix process");
        return -1;
    }

    // Are we already a leader of a session?
    PosixProcess *pProcess = static_cast<PosixProcess *>(pBaseProcess);

    // Handle zero PID and PGID.
    if (!pid)
    {
        pid = pProcess->getId();
    }
    if (!pgid)
    {
        pgid = pid;
    }

    ProcessGroup *pGroup = pProcess->getProcessGroup();
    PosixSession *pSession = pProcess->getSession();

    // Is this us or a child of us?
    /// \todo pid == child, but child not in this session = EPERM
    if (pid != pProcess->getId())
    {
        // Find the target process - it's not us
        Process *pTargetProcess = nullptr;
        for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); ++i)
        {
            Process *check = Scheduler::instance().getProcess(i);
            if (check->getType() != Process::Posix)
                continue;

            if (check->getId() == pid)
            {
                pTargetProcess = check;
                break;
            }
        }

        if (!pTargetProcess)
        {
            SC_NOTICE("  -> process doesn't exist");
            SYSCALL_ERROR(NoSuchProcess);
            return -1;
        }

        // Is this process a child of us?
        Process *parent = pTargetProcess->getParent();
        while (parent != nullptr)
        {
            if (parent == pProcess)
            {
                // ok!
                break;
            }
        }

        if (parent != pProcess)
        {
            // Not a child!
            SC_NOTICE("  -> target process is not a descendant of the current process");
            SYSCALL_ERROR(NoSuchProcess);
            return -1;
        }

        if (static_cast<PosixProcess *>(pTargetProcess)->getSession() != pSession)
        {
            SC_NOTICE("  -> target process is in a different session");
            SYSCALL_ERROR(NotEnoughPermissions);
            return -1;
        }

        pBaseProcess = pTargetProcess;
        pProcess = static_cast<PosixProcess *>(pTargetProcess);
        pGroup = pProcess->getProcessGroup();
        pSession = pProcess->getSession();
    }

    if (pGroup && (pGroup->processGroupId == pgid))
    {
        // Already a member.
        SC_NOTICE(" -> OK, already a member!");
        return 0;
    }

    if (pSession && (pSession->Leader == pProcess))
    {
        // Already a session leader.
        SYSCALL_ERROR(PermissionDenied);
        SC_NOTICE(" -> EPERM (already leader)");
        return 0;
    }

    // Does the process group exist?
    Process *check = 0;
    for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); ++i)
    {
        check = Scheduler::instance().getProcess(i);
        if (check->getType() != Process::Posix)
            continue;

        PosixProcess *posixCheck = static_cast<PosixProcess *>(check);
        ProcessGroup *pGroupCheck = posixCheck->getProcessGroup();
        if (pGroupCheck)
        {
            if (pGroupCheck->processGroupId == pgid)
            {
                // Join this group.
                pProcess->setProcessGroup(pGroupCheck);
                pProcess->setGroupMembership(PosixProcess::Member);
                SC_NOTICE(" -> OK, joined!");
                return 0;
            }
        }
    }

    // No, the process group does not exist. Create it.
    ProcessGroup *pNewGroup = new ProcessGroup;
    pNewGroup->processGroupId = pProcess->getId();
    pNewGroup->Leader = pProcess;
    pNewGroup->Members.clear();

    // We're now a group leader - we got promoted!
    pProcess->setProcessGroup(pNewGroup);
    pProcess->setGroupMembership(PosixProcess::Leader);

    SC_NOTICE(" -> OK, created!");
    return 0;
}

int posix_getpgid(int pid)
{
    if (!pid)
    {
        return posix_getpgrp();
    }

    size_t pid_ = pid;

    SC_NOTICE("getpgid(" << pid << ")");

    // Find the target process - it's not us
    Process *pTargetProcess = nullptr;
    for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); ++i)
    {
        Process *check = Scheduler::instance().getProcess(i);
        if (check->getType() != Process::Posix)
            continue;

        if (check->getId() == pid_)
        {
            pTargetProcess = check;
            break;
        }
    }

    if (!pTargetProcess)
    {
        SC_NOTICE(" -> target process not found");
        SYSCALL_ERROR(NoSuchProcess);
        return -1;
    }

    PosixProcess *pProcess = static_cast<PosixProcess *>(pTargetProcess);
    ProcessGroup *pGroup = pProcess->getProcessGroup();

    if (pGroup)
    {
        SC_NOTICE(" -> " << pGroup->processGroupId);
        return pGroup->processGroupId;
    }

    SC_NOTICE(" -> target process did not have a group");
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
}

int posix_getpgrp()
{
    SC_NOTICE("getpgrp");

    PosixProcess *pProcess = static_cast<PosixProcess *>(
        Processor::information().getCurrentThread()->getParent());
    ProcessGroup *pGroup = pProcess->getProcessGroup();

    int result = 0;
    if (pGroup)
    {
        SC_NOTICE(" -> using existing group id");
        result = pGroup->processGroupId;
    }
    else
    {
        SC_NOTICE(" -> using pid only");
        result = pProcess->getId();  // Fallback if no ProcessGroup pointer yet
    }

    SC_NOTICE(" -> " << result);
    return result;
}

mode_t posix_umask(mode_t mask)
{
    SC_NOTICE("umask(" << Oct << mask << ")");

    // Not a POSIX process
    Process *pStockProcess =
        Processor::information().getCurrentThread()->getParent();
    if (pStockProcess->getType() != Process::Posix)
    {
        SC_NOTICE("umask -> called on something not a POSIX process");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    PosixProcess *pProcess = static_cast<PosixProcess *>(pStockProcess);

    uint32_t previous = pProcess->getMask();
    pProcess->setMask(mask);

    return previous;
}

int posix_linux_syslog(int type, char *buf, int len)
{
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(buf), len, PosixSubsystem::SafeRead))
    {
        SC_NOTICE("linux_syslog -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    SC_NOTICE("linux_syslog");

    if (len > 512)
        len = 512;

    switch (type)
    {
        case 0:
            SC_NOTICE(" -> close log");
            return 0;

        case 1:
            SC_NOTICE(" -> open log");
            return 0;

        case 2:
            /// \todo expose kernel log via this interface
            // NOTE: blocking call...
            SC_NOTICE(" -> read log");
            Processor::information().getScheduler().sleep(nullptr);
            return 0;

        case 3:
            /// \todo expose kernel log via this interface
            SC_NOTICE(" -> read up to last 4k");
            return 0;

        case 4:
            /// \todo expose kernel log via this interface
            SC_NOTICE(" -> read and clear last 4k");
            return 0;

        case 5:
            SC_NOTICE(" -> clear");
            return 0;

        case 6:
            SC_NOTICE(" -> disable write to console");
            return 0;

        case 7:
            SC_NOTICE(" -> enable write to console");
            return 0;

        case 8:
            SC_NOTICE(" -> set console write level");
            return 0;

        default:
            SC_NOTICE(" -> unknown!");
            SYSCALL_ERROR(InvalidArgument);
            return -1;
    }
}

int posix_syslog(const char *msg, int prio)
{
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(msg), PATH_MAX,
            PosixSubsystem::SafeRead))
    {
        SC_NOTICE("klog -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    uint64_t id =
        Processor::information().getCurrentThread()->getParent()->getId();
    if (id <= 1)
    {
        if (prio <= LOG_CRIT)
            FATAL("[" << Dec << id << Hex << "]\tklog: " << msg);
    }

    if (prio <= LOG_ERR)
        ERROR("[" << Dec << id << Hex << "]\tklog: " << msg);
    else if (prio == LOG_WARNING)
        WARNING("[" << Dec << id << Hex << "]\tklog: " << msg);
    else if (prio == LOG_NOTICE || prio == LOG_INFO)
        NOTICE("[" << Dec << id << Hex << "]\tklog: " << msg);
#if DEBUGGER
    else
        NOTICE("[" << Dec << id << Hex << "]\tklog: " << msg);
#endif
    return 0;
}

extern void system_reset();

int pedigree_reboot()
{
    // Are we superuser?
    User *pUser =
        Processor::information().getCurrentThread()->getParent()->getUser();
    if (pUser->getId())
    {
        SYSCALL_ERROR(NotEnoughPermissions);
        return -1;
    }

    WARNING("System shutting down...");
    for (int i = Scheduler::instance().getNumProcesses() - 1; i >= 0; i--)
    {
        Process *proc = Scheduler::instance().getProcess(i);
        Subsystem *subsys = proc->getSubsystem();

        if (proc == Processor::information().getCurrentThread()->getParent())
            continue;

        if (subsys)
        {
            // If there's a subsystem, kill it that way.
            /// \todo need to set a timeout and SIGKILL if it expires...
            subsys->kill(Subsystem::Terminated, proc->getThread(0));
        }
        else
        {
            // If no subsystem, outright kill the process without sending a
            // signal
            Scheduler::instance().removeProcess(proc);

            /// \todo Process::kill() acts as if that process is already
            /// running.
            ///       It needs to allow other Processes to call it without
            ///       causing the calling thread to become a zombie.
            // proc->kill();
        }
    }

    // Wait for remaining processes to terminate.
    while (true)
    {
        Processor::setInterrupts(false);
        if (Scheduler::instance().getNumProcesses() <= 1)
        {
            break;
        }
        bool allZombie = true;
        for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); i++)
        {
            if (Scheduler::instance().getProcess(i) ==
                Processor::information().getCurrentThread()->getParent())
            {
                continue;
            }
            if (Scheduler::instance()
                    .getProcess(i)
                    ->getThread(0)
                    ->getStatus() != Thread::Zombie)
            {
                allZombie = false;
            }
        }

        if (allZombie)
        {
            break;
        }
        Processor::setInterrupts(true);

        Scheduler::instance().yield();
    }

    // All dead, reap them all.
    while (Scheduler::instance().getNumProcesses() > 1)
    {
        if (Scheduler::instance().getProcess(0) ==
            Processor::information().getCurrentThread()->getParent())
        {
            continue;
        }
        delete Scheduler::instance().getProcess(0);
    }

    // Reset the system
    system_reset();
    return 0;
}

int posix_uname(struct utsname *n)
{
    if (!n)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());

    StringCopy(n->sysname, "Pedigree");

    if (pSubsystem->getAbi() == PosixSubsystem::LinuxAbi)
    {
        // Lie a bit to Linux ABI callers.
        StringCopy(n->release, "2.6.32-generic");
        StringCopy(n->version, g_pBuildRevision);
    }
    else
    {
        StringCopy(n->release, g_pBuildRevision);
        StringCopy(n->version, "Foster");
    }

    StringCopy(n->machine, g_pBuildTarget);

    /// \todo: better handle node name
    StringCopy(n->nodename, "pedigree.local");
    return 0;
}

int posix_arch_prctl(int code, unsigned long addr)
{
    unsigned long *pAddr = reinterpret_cast<unsigned long *>(addr);

    switch (code)
    {
        case ARCH_SET_FS:
            Processor::information().getCurrentThread()->setTlsBase(addr);
            break;

        case ARCH_GET_FS:
            *pAddr = Processor::information().getCurrentThread()->getTlsBase();
            break;

        default:
            SYSCALL_ERROR(InvalidArgument);
            return -1;
    }

    return 0;
}

int posix_pause()
{
    SC_NOTICE("pause");

    Processor::information().getScheduler().sleep();

    SYSCALL_ERROR(Interrupted);
    return -1;
}

int posix_setgroups(size_t size, const gid_t *list)
{
    SC_NOTICE("setgroups(" << size << ")");

    /// \todo check permissions

    /// \todo support this (currently a stub)

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(list), size * sizeof(gid_t),
            PosixSubsystem::SafeRead))
    {
        SC_NOTICE(" -> invalid address");
        SYSCALL_ERROR(BadAddress);
        return -1;
    }

    return 0;
}

int posix_getgroups(int size, gid_t *list)
{
    SC_NOTICE("getgroups(" << size << ")");

    /// \todo support this (currently a stub)

    if (!size)
    {
        // Only return number of groups.
        return 0;
    }

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(list), size * sizeof(gid_t),
            PosixSubsystem::SafeWrite))
    {
        SC_NOTICE("execve -> invalid address");
        SYSCALL_ERROR(BadAddress);
        return -1;
    }

    return 0;
}

int posix_getrlimit(int resource, struct rlimit *rlim)
{
    /// \todo check access on rlim
    SC_NOTICE("getrlimit(" << Dec << resource << ")");

    switch (resource)
    {
        case RLIMIT_CPU:
            break;
        case RLIMIT_FSIZE:
            rlim->rlim_cur = rlim->rlim_max = RLIM_INFINITY;
            break;
        case RLIMIT_DATA:
            rlim->rlim_cur = rlim->rlim_max = RLIM_INFINITY;
            break;
        case RLIMIT_STACK:
            rlim->rlim_cur = rlim->rlim_max = RLIM_INFINITY;
            break;
        case RLIMIT_CORE:
            rlim->rlim_cur = 0;
            rlim->rlim_max = RLIM_INFINITY;
            break;
        case RLIMIT_RSS:
            rlim->rlim_cur = rlim->rlim_max = 1ULL << 48ULL;
            break;
        case RLIMIT_NPROC:
            rlim->rlim_cur = rlim->rlim_max = RLIM_INFINITY;
            break;
        case RLIMIT_NOFILE:
            rlim->rlim_cur = rlim->rlim_max = 16384;
            break;
        case RLIMIT_MEMLOCK:
            rlim->rlim_cur = rlim->rlim_max = 1ULL << 24ULL;
            break;
        case RLIMIT_AS:
            rlim->rlim_cur = rlim->rlim_max = 1ULL << 48ULL;
            break;
        case RLIMIT_LOCKS:
            rlim->rlim_cur = rlim->rlim_max = 1024;
            break;
        case RLIMIT_SIGPENDING:
            rlim->rlim_cur = rlim->rlim_max = 16;
            break;
        case RLIMIT_MSGQUEUE:
            rlim->rlim_cur = rlim->rlim_max = 0x100000;
            break;
        case RLIMIT_NICE:
            rlim->rlim_cur = rlim->rlim_max = 1;
            break;
        case RLIMIT_RTPRIO:
            SYSCALL_ERROR(InvalidArgument);
            SC_NOTICE(" -> RTPRIO not supported");
            return -1;
        default:
            SYSCALL_ERROR(InvalidArgument);
            SC_NOTICE(" -> unknown resource!");
            return -1;
    }

    SC_NOTICE(" -> cur = " << rlim->rlim_cur);
    SC_NOTICE(" -> max = " << rlim->rlim_max);
    return 0;
}

int posix_setrlimit(int resource, const struct rlimit *rlim)
{
    /// \todo check access on rlim
    SC_NOTICE("setrlimit(" << Dec << resource << ")");

    /// \todo write setrlimit

    return 0;
}

int posix_getpriority(int which, int who)
{
    /// \todo better expose priorities
    SC_NOTICE("getpriority(" << which << ", " << Dec << who << ")");
    SYSCALL_ERROR(NoError);  // clear errno if not already
    return 0;
}

int posix_setpriority(int which, int who, int prio)
{
    /// \todo could do more with this
    SC_NOTICE(
        "setpriority(" << which << ", " << Dec << who << ", " << prio << ")");
    return 0;
}

int posix_setreuid(uid_t ruid, uid_t euid)
{
    SC_NOTICE("setreuid(" << ruid << ", " << euid << ")");

    /// \todo Make sure we are actually allowed to do this! (EPERM)

    if (ruid != static_cast<uid_t>(-1))
    {
        User *realUser = UserManager::instance().getUser(ruid);
        if (realUser)
        {
            Processor::information().getCurrentThread()->getParent()->setUser(
                realUser);
        }
    }

    if (euid != static_cast<uid_t>(-1))
    {
        User *effectiveUser = UserManager::instance().getUser(ruid);
        if (effectiveUser)
        {
            Processor::information()
                .getCurrentThread()
                ->getParent()
                ->setEffectiveUser(effectiveUser);
        }
    }

    return 0;
}

int posix_setregid(gid_t rgid, gid_t egid)
{
    SC_NOTICE("setregid(" << rgid << ", " << egid << ")");

    /// \todo Make sure we are actually allowed to do this! (EPERM)

    if (rgid != static_cast<gid_t>(-1))
    {
        Group *realGroup = UserManager::instance().getGroup(rgid);
        if (realGroup)
        {
            Processor::information().getCurrentThread()->getParent()->setGroup(
                realGroup);
        }
    }

    if (egid != static_cast<gid_t>(-1))
    {
        Group *effectiveGroup = UserManager::instance().getGroup(egid);
        if (effectiveGroup)
        {
            Processor::information()
                .getCurrentThread()
                ->getParent()
                ->setEffectiveGroup(effectiveGroup);
        }
    }

    return 0;
}

int posix_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    SC_NOTICE("setresuid(" << ruid << ", " << euid << ", " << suid << ")");
    return posix_setreuid(ruid, euid);
}

int posix_setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
    SC_NOTICE("setresgid(" << rgid << ", " << egid << ", " << sgid << ")");
    return posix_setregid(rgid, egid);
    return 0;
}

int posix_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid)
{
    SC_NOTICE("getresuid");

    if (ruid)
    {
        *ruid = posix_getuid();
    }

    if (euid)
    {
        *euid = posix_geteuid();
    }

    if (suid)
    {
        *suid = 0;
    }

    return 0;
}

int posix_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
    SC_NOTICE("getresgid");

    if (rgid)
    {
        *rgid = posix_getgid();
    }

    if (egid)
    {
        *egid = posix_getegid();
    }

    if (sgid)
    {
        *sgid = 0;
    }

    return 0;
}

int posix_get_robust_list(
    int pid, struct robust_list_head **head_ptr, size_t *len_ptr)
{
    SC_NOTICE("get_robust_list");

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(head_ptr), sizeof(void *),
              PosixSubsystem::SafeWrite) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(len_ptr), sizeof(size_t),
              PosixSubsystem::SafeWrite)))
    {
        SC_NOTICE(" -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    PosixProcess *pProcess = static_cast<PosixProcess *>(
        Processor::information().getCurrentThread()->getParent());

    auto data = pProcess->getRobustList();
    *head_ptr = reinterpret_cast<struct robust_list_head *>(data.head);
    *len_ptr = data.head_len;

    return 0;
}

int posix_set_robust_list(struct robust_list_head *head, size_t len)
{
    SC_NOTICE("set_robust_list");

    PosixProcess *pProcess = static_cast<PosixProcess *>(
        Processor::information().getCurrentThread()->getParent());

    PosixProcess::RobustListData data;
    data.head = head;
    data.head_len = len;

    pProcess->setRobustList(data);

    return 0;
}

int posix_ioperm(unsigned long from, unsigned long num, int turn_on)
{
    SC_NOTICE("ioperm(" << from << ", " << num << ", " << turn_on << ")");

    /// \todo set the io permissions bitmap properly and use this to enable stuff
    return 0;
}

int posix_iopl(int level)
{
    SC_NOTICE("iopl(" << level << ")");
    return 0;
}

int posix_getitimer(int which, struct itimerval *curr_value)
{
    POSIX_VERBOSE_LOG("test", "posix_getitimer(" << which << ", " << curr_value << ")");
    return 0;
}

int posix_setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value)
{
    POSIX_VERBOSE_LOG("test", "posix_setitimer(" << which << ", " << new_value << ", " << old_value << ")");

    if (which == ITIMER_REAL)
    {
        NOTICE(" -> ITIMER_REAL");
    }
    else if (which == ITIMER_VIRTUAL)
    {
        NOTICE(" -> ITIMER_VIRTUAL");
    }
    else if (which == ITIMER_PROF)
    {
        NOTICE(" -> ITIMER_VIRTUAL");
    }
    else
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    NOTICE(" -> period = " << new_value->it_interval.tv_sec << "s " << new_value->it_interval.tv_usec << "us");
    NOTICE(" -> value = " << new_value->it_value.tv_sec << "s " << new_value->it_value.tv_usec << "us");

    return 0;
}

