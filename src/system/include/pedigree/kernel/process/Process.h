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

#ifndef PROCESS_H
#define PROCESS_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/DeferredTimeAccounting.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/MemoryAllocator.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/new"

#include "pedigree/kernel/Subsystem.h"

class VirtualAddressSpace;
class File;
class User;
class Group;
class DynamicLinker;
class ZombieProcess;
class ZombieQueue;
class Scheduler;

/**
 * An abstraction of a Process - a container for one or more threads all running
 * in the same address space.
 */
class EXPORTED_PUBLIC Process
{
    friend class PerProcessorScheduler;
    friend class Scheduler;
    friend class Thread;
    friend class ZombieProcess;
    friend class ZombieQueue;

  public:
    /**
     * Move-only ownership of the one destruction publication permitted for a
     * Process. A successful claim must be published before leaving scope.
     */
    class EXPORTED_PUBLIC ReaperClaim
    {
      public:
        ReaperClaim();
        ReaperClaim(ReaperClaim &&other);
        ~ReaperClaim();

        ReaperClaim &operator=(ReaperClaim &&other);

        explicit operator bool() const
        {
            return m_pProcess != nullptr;
        }

        void publish();

      private:
        friend class Process;
        explicit ReaperClaim(Process *process);

        ReaperClaim(const ReaperClaim &) = delete;
        ReaperClaim &operator=(const ReaperClaim &) = delete;

        Process *m_pProcess;
        TerminationDeferral m_TerminationDeferral;
    };

    /**
     * Pins one Thread together with its owning Process.
     *
     * Detached-thread deletion closes admission and drains these leases after
     * the scheduler has switched off the target stack. Process destruction
     * likewise drains the Process half before deleting retained Threads.
     * Leases are thread-affine and must be released by their acquiring Thread.
     */
    class EXPORTED_PUBLIC ThreadLease
    {
      public:
        ThreadLease();
        ThreadLease(ThreadLease &&other);
        ~ThreadLease();

        ThreadLease &operator=(ThreadLease &&other);

        Thread *get() const
        {
            return m_pThread;
        }

        Thread *operator->() const
        {
            return m_pThread;
        }

        explicit operator bool() const
        {
            return m_pThread != nullptr;
        }

        void reset();

      private:
        friend class Process;

        ThreadLease(Process *process, Thread *thread);
        ThreadLease(const ThreadLease &) = delete;
        ThreadLease &operator=(const ThreadLease &) = delete;

        Process *m_pProcess;
        Thread *m_pThread;
        TerminationDeferral m_TerminationDeferral;
    };

    /** Subsystems may inherit Process to provide custom functionality. However,
     * they need to know whether a Process pointer is subsystem-specific. This
     * enumeration is designed to allow functions using Process objects in
     * subsystems with inherited Process objects to be able to figure out what
     * type the Process is without depending on any external accounting.
     */
    enum ProcessType
    {
        Stock,
        Posix
    };

    /**
     * Processes have their own state, aside from the state of their threads.
     * These states are very general and don't reflect the current scheduling
     * state of the process as a whole in intricate detail.
     */
    enum ProcessState
    {
        Active,
        Suspended,
        Terminating,
        Terminated,
        Reaped,  /// Reaped means the process has had a status retrieved.
    };

    /** Default constructor. */
    Process();

    /** Constructor for creating a new Process. Creates a new Process as
     * a UNIX fork() would, from the given parent process. This constructor
     * does not create any threads.
     * \param pParent The parent process.
     * \param bCopyOnWrite Whether to mark the address space copy-on-write
     (default) or to share it read/write with the new child.
     */
    Process(Process *pParent, bool bCopyOnWrite = true);

    /** Destructor. */
    virtual ~Process();

    /** Adds a thread to this process.
     *  \return The thread ID to be assigned to the new Thread. */
    size_t addThread(Thread *pThread);
    /** Removes a thread from this process. */
    void removeThread(Thread *pThread);

    /** Returns the number of threads in this process. */
    size_t getNumThreads();
    /**
     * Pins the n'th thread in this process into \p lease.
     * Any previous lease is released; failure leaves \p lease empty.
     */
    MUST_USE_RESULT bool acquireThread(ThreadLease &lease, size_t n);

    /**
     * Pins an expected Thread into \p lease if this Process still owns it.
     * Any previous lease is released; failure leaves \p lease empty.
     */
    MUST_USE_RESULT bool acquireThread(
        ThreadLease &lease, Thread *expected);

    /** Returns the process ID. */
    size_t getId()
    {
        return m_Id;
    }

    /** Returns the description string of this process. */
    LargeStaticString &description()
    {
        return str;
    }

    /** Returns our address space */
    VirtualAddressSpace *getAddressSpace()
    {
        return m_pAddressSpace;
    }

    /** Sets the exit status of the process. */
    void setExitStatus(int code)
    {
        m_ExitStatus = code;
    }
    /** Gets the exit status of the process. */
    int getExitStatus()
    {
        return m_ExitStatus;
    }

    /**
     * Marks the process as reaped.
     *
     * Callers must hold the parent's child-state wait guard so only one
     * concurrent wait operation can claim the status.
     */
    void reap();

    /**
     * Atomically elects the current thread to own process exit and requests
     * thread-only termination for every peer. Returns true only to the elected
     * owner (including its nested-event re-entry).
     */
    bool beginTermination();

    /**
     * Claims shared teardown for the elected owner and waits until every peer
     * is off-stack. Exactly one call can succeed.
     */
    bool quiesceTermination();

    /** Completes an elected teardown and retires the current thread. */
    void finishTermination() NORETURN;

    /** Claims the sole deferred-destruction publication for this Process. */
    ReaperClaim tryClaimReaper();

    /** Performs the complete election, quiesce, and teardown sequence. */
    void kill() NORETURN;
    /** Suspends the process. */
    void suspend();
    /** Resumes the process from suspend. */
    void resume();

    /** Returns the parent process. */
    Process *getParent()
    {
        return __atomic_load_n(&m_pParent, __ATOMIC_ACQUIRE);
    }

    /** Returns the current working directory. */
    File *getCwd()
    {
        return m_Cwd;
    }
    /** Sets the current working directory. */
    void setCwd(File *f)
    {
        m_Cwd = f;
    }

    /** Returns the current controlling terminal. */
    File *getCtty()
    {
        return m_Ctty;
    }
    /** Sets the controlling terminal. */
    void setCtty(File *f)
    {
        m_Ctty = f;
    }

    /** Returns the memory space allocator for primary address space. */
    MemoryAllocator &getSpaceAllocator()
    {
        return m_SpaceAllocator;
    }
    /** Returns the memory space allocator for dynamic address space. */
    MemoryAllocator &getDynamicSpaceAllocator()
    {
        return m_DynamicSpaceAllocator;
    }

    /** Gets the current user. */
    User *getUser() const
    {
        return m_pUser;
    }
    /** Sets the current user. */
    void setUser(User *pUser)
    {
        m_pUser = pUser;
    }

    /** Gets the effective user. */
    User *getEffectiveUser() const
    {
        return m_pEffectiveUser;
    }
    /** Sets the effective user. */
    void setEffectiveUser(User *pUser)
    {
        m_pEffectiveUser = pUser;
    }

    /** Gets the current group. */
    Group *getGroup() const
    {
        return m_pGroup;
    }
    /** Sets the current group. */
    void setGroup(Group *pGroup)
    {
        m_pGroup = pGroup;
    }

    /** Gets the current effective group. */
    Group *getEffectiveGroup() const
    {
        return m_pEffectiveGroup;
    }
    void setEffectiveGroup(Group *pGroup)
    {
        m_pEffectiveGroup = pGroup;
    }

    /** Direct, overrideable ways to get IDs (redirects to User/Group interface
     * by default) */
    virtual int64_t getUserId() const;
    virtual int64_t getGroupId() const;
    virtual int64_t getEffectiveUserId() const;
    virtual int64_t getEffectiveGroupId() const;
    virtual void getSupplementalGroupIds(Vector<int64_t> &vec) const;

    void setLinker(DynamicLinker *pDl)
    {
        m_pDynamicLinker = pDl;
    }
    DynamicLinker *getLinker()
    {
        return m_pDynamicLinker;
    }

    void setSubsystem(Subsystem *pSubsystem)
    {
        m_pSubsystem = pSubsystem;
        m_pSubsystem->setProcess(this);
    }
    Subsystem *getSubsystem()
    {
        return m_pSubsystem;
    }

    /** Gets the type of the Process (subsystems may override) */
    virtual ProcessType getType()
    {
        return Stock;
    }

    /**
     * Serialises child-state inspection with blocking and with other waiters.
     */
    WaitQueue::Guard acquireChildStateWait()
    {
        return m_ChildStateWaiters.acquire();
    }

    /**
     * Blocks a deferred reaper until the terminating thread is off-stack.
     * Returns false rather than self-deadlocking if called by that thread.
     */
    bool waitUntilTerminationReapable();

    bool hasSuspended()
    {
        bool bRet = m_bUnreportedSuspend;
        m_bUnreportedSuspend = false;
        return bRet;
    }
    bool hasResumed()
    {
        bool bRet = m_bUnreportedResume;
        m_bUnreportedResume = false;
        return bRet;
    }

    ProcessState getState() const
    {
        return __atomic_load_n(&m_State, __ATOMIC_ACQUIRE);
    }

    bool isSuspended()
    {
        return getState() == Suspended;
    }

    void markTerminating();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    using TerminationElectionHook = void (*)(Process *, Thread *);
    static void setTerminationElectionHook(TerminationElectionHook hook);

    enum class OrphanPublicationPhase
    {
        Preparing,
        Published,
    };
    using OrphanPublicationHook = void (*)(
        Process *, OrphanPublicationPhase, bool interruptsEnabled,
        bool processLockHeld);
    static void setOrphanPublicationHook(OrphanPublicationHook hook);

    /** Publishes one deterministic worker batch without sampling a clock. */
    void publishTimeAccountingForHostedTest(
        Time::Timestamp user, Time::Timestamp system);

    /** Closes worker admission exactly as process teardown does. */
    void closeTimeAccountingForHostedTest();
#endif

    void trackHeap(ssize_t nBytes)
    {
        __atomic_fetch_add(
            &m_Metadata.heapUsage, nBytes, __ATOMIC_RELAXED);
    }

    void trackPages(ssize_t nVirtual, ssize_t nPhysical, ssize_t nShared)
    {
        __atomic_fetch_add(
            &m_Metadata.virtualPages, nVirtual, __ATOMIC_RELAXED);
        __atomic_fetch_add(
            &m_Metadata.physicalPages, nPhysical, __ATOMIC_RELAXED);
        __atomic_fetch_add(
            &m_Metadata.sharedPages, nShared, __ATOMIC_RELAXED);
    }

    void resetCounts()
    {
        __atomic_store_n(
            &m_Metadata.virtualPages, static_cast<ssize_t>(0),
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &m_Metadata.physicalPages, static_cast<ssize_t>(0),
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &m_Metadata.sharedPages, static_cast<ssize_t>(0),
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &m_Metadata.startTime, Time::getTimeNanoseconds(),
            __ATOMIC_RELEASE);
    }

    /** Gets timestamps. */
    Time::Timestamp getUserTime() const
    {
        return __atomic_load_n(&m_Metadata.userTime, __ATOMIC_ACQUIRE);
    }
    Time::Timestamp getKernelTime() const
    {
        return __atomic_load_n(&m_Metadata.kernelTime, __ATOMIC_ACQUIRE);
    }
    Time::Timestamp getStartTime() const
    {
        return __atomic_load_n(&m_Metadata.startTime, __ATOMIC_ACQUIRE);
    }

    /** Get process usage. */
    ssize_t getHeapUsage() const
    {
        return __atomic_load_n(&m_Metadata.heapUsage, __ATOMIC_ACQUIRE);
    }
    ssize_t getVirtualPageCount() const
    {
        return __atomic_load_n(
            &m_Metadata.virtualPages, __ATOMIC_ACQUIRE);
    }
    ssize_t getPhysicalPageCount() const
    {
        return __atomic_load_n(
            &m_Metadata.physicalPages, __ATOMIC_ACQUIRE);
    }
    ssize_t getSharedPageCount() const
    {
        return __atomic_load_n(&m_Metadata.sharedPages, __ATOMIC_ACQUIRE);
    }

    /** Set this process' root. */
    void setRootFile(File *pFile)
    {
        m_pRootFile = pFile;
    }

    /** Get this process' root. */
    File *getRootFile() const
    {
        return m_pRootFile;
    }

    /**
     * Get whether this process has a shared address space with its parent.
     * Copy-on-write (i.e. not shared) is the default for processes.
     */
    bool hasSharedAddressSpace() const
    {
        return m_bSharedAddressSpace;
    }

    /**
     * Get the init process (first userspace process, parent of all
     * userspace processes).
     */
    static Process *getInit();

    /** Set the init process. */
    static void setInit(Process *pProcess);

  protected:
    /**
     * Selects construction without scheduler publication. Derived Process
     * classes must call publish() after all of their members are initialised.
     */
    struct DeferredPublication
    {
    };

    Process(DeferredPublication);
    Process(
        DeferredPublication, Process *pParent, bool bCopyOnWrite = true);

    /** Makes a completely constructed Process visible to enumeration. */
    void publish();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    /**
     * Clears the requested parent before a deferred hosted-test publication.
     * The Process retains the owned address-space clone made from that parent.
     */
    void makeOrphanBeforePublicationForHostedTest();
#endif

    /**
     * Removes this process from enumeration and drains inspectors.
     *
     * Every derived destructor must call this before touching down its own
     * members; the base destructor runs too late to protect derived storage.
     */
    void prepareForDestruction();

    /** Enables deferred timer reporting for a derived process type. */
    void enableTimeAccountingReports();

  private:
    Process(const Process &);
    Process &operator=(const Process &);

    /**
     * Called by the accounting worker with absolute userspace and total time.
     * This is never invoked by trackTime() or while scheduler locks are held.
     */
    virtual void
    reportTimesUpdated(Time::Timestamp userTotal, Time::Timestamp total)
    {
    }

    /** Drains one coalesced batch from an ordinary accounting worker. */
    void drainDeferredTimeAccounting();

    /** Stops timer-report admission and discards any unpublished residue. */
    void closeDeferredTimeAccounting();

    /** Adds one Thread's elapsed monotonic time to Process-wide totals. */
    void publishTimeAccounting(CpuTimeMode mode, Time::Timestamp elapsed);

    /** Common fixed-cost accumulator publication after aggregate accounting. */
    void publishTimeAccountingBatch(
        Time::Timestamp user, Time::Timestamp system);

    /** Called when the process is terminated to allow for subclass cleanup. */
    virtual void processTerminated()
    {
    }

    /**
     * Our list of threads.
     */
    Vector<Thread *> m_Threads;
    /**
     * The next available thread ID.
     */
    Atomic<size_t> m_NextTid;
    /**
     * Our Process ID.
     */
    size_t m_Id;
    /**
     * Our description string.
     */
    LargeStaticString str;
    /**
     * Our parent process.
     */
    Process *m_pParent;
    /**
     * Our virtual address space.
     */
    VirtualAddressSpace *m_pAddressSpace;
    /**
     * Process exit status.
     */
    int m_ExitStatus;
    /**
     * Current working directory.
     */
    File *m_Cwd;
    /**
     * Current controlling terminal.
     */
    File *m_Ctty;
    /**
     * Memory allocator for primary address space.
     */
    MemoryAllocator m_SpaceAllocator;
    /**
     * Memory allocator for dynamic address space, if any.
     */
    MemoryAllocator m_DynamicSpaceAllocator;
    /** Current user. */
    User *m_pUser;
    /** Current group. */
    Group *m_pGroup;
    /** Effective user. */
    User *m_pEffectiveUser;
    /** Effective group. */
    Group *m_pEffectiveGroup;

    /** The Process' dynamic linker. */
    DynamicLinker *m_pDynamicLinker;

    /** The subsystem for this process */
    Subsystem *m_pSubsystem;

    /** Waiters for state changes in any direct child of this process. */
    WaitQueue m_ChildStateWaiters;

    /** Completion used by deferred Process destruction. */
    WaitQueue m_TerminationWaiters;

    /** Predicate queue for the process stopped/running state. */
    WaitQueue m_SuspensionWaiters;

    /** Lifetime barrier for Thread::join operations using this process. */
    WaitQueue m_ThreadJoinWaiters;

    /** Join operations admitted before destruction closed the process. */
    size_t m_nThreadJoinOperations;

    /** Whether Process destruction has closed Thread::join admission. */
    bool m_bThreadJoinAdmissionClosed;

    /**
     * Short predicate lock. Scheduler enumeration may nest this under the
     * global scheduler lock; it is never held while waking lease drainers.
     */
    Spinlock m_ExternalLeaseLock;

    /** Lifetime barrier for scheduler enumeration leases. */
    WaitQueue m_ExternalLeaseWaiters;

    /** Process leases admitted before scheduler removal. */
    size_t m_nExternalLeases;

    /** Whether scheduler removal has closed Process lease admission. */
    bool m_bExternalLeaseAdmissionClosed;

    /** Whether we have suspended but not reported it. */
    bool m_bUnreportedSuspend;

    /** Whether we have resumed but not reported it. */
    bool m_bUnreportedResume;

    /** Our current state. */
    ProcessState m_State;

    /**
     * Changes process state only if it still matches the expected state.
     * Active and Suspended may alternate, but no caller may overwrite a
     * terminal lifecycle transition.
     */
    bool transitionState(ProcessState expected, ProcessState desired);

    /** Advances Active or Suspended to Terminating without downgrading. */
    void transitionToTerminating();

    /** Process destruction is iterating m_Threads and owns its cleanup. */
    bool m_bDestroying;

    /** Whether this object has been published to scheduler enumeration. */
    bool m_bPublished;

    /** Scheduler enumeration no longer contains this process. */
    bool m_bUnregistered;

    /** Thread that initiated exit, used to reject self-waiting destruction. */
    Thread *m_pTerminatingThread;

    /** Process-exit participants that have not yet switched off-stack. */
    size_t m_nTerminationParticipants;

    /** Process::kill has installed the complete live-thread rendezvous. */
    bool m_bTerminationRendezvousStarted;

    /** The elected owner has exclusively claimed shared process teardown. */
    bool m_bTerminationCleanupStarted;

    /** No further thread may join the rendezvous before publication. */
    bool m_bTerminationSealed;

    /** Whether the scheduler has completed the terminating thread switch. */
    bool m_bTerminationReapable;

    enum ReaperState
    {
        ReaperUnclaimed,
        ReaperClaimed,
        ReaperPublished,
    };

    /** One-shot ownership of deferred Process destruction. */
    size_t m_ReaperState;

    /** Concurrency lock for complex Process data structures. */
    Spinlock m_Lock;

    /**
     * Accounts for one process-exit participant after it is off-stack.
     * The caller holds m_Lock. Returns true for the last participant and sets
     * wakeOwner when an elected owner may be waiting for peer progress.
     */
    bool terminatingThreadReapable(Thread *pThread, bool &wakeOwner);

    /** Publishes termination after the final participant is reapable. */
    void publishTermination();

    /** Publishes a previously claimed deferred destruction exactly once. */
    void publishReaperClaim();

    /** Pins this Process and its retained Thread objects for Thread::join. */
    bool beginThreadJoin();

    /** Releases a successful beginThreadJoin admission. */
    void endThreadJoin();

    /** Pins this Process while it remains in scheduler enumeration. */
    bool beginExternalLease();

    /** Releases a scheduler or ThreadLease pin. */
    void endExternalLease();

    /** Prevents any further ProcessLease or ThreadLease acquisition. */
    void closeExternalLeaseAdmission();

    /** Waits until all leases admitted before closure have left scope. */
    void drainExternalLeases();

    /** Releases both halves of an admitted ThreadLease. */
    void releaseThreadLease(Thread *thread);

    /** Stores metadata about this process. */
    struct ProcessMetadata
    {
        ProcessMetadata()
            : heapUsage(0), virtualPages(0), physicalPages(0), sharedPages(0),
              userTime(0), kernelTime(0), startTime(0)
        {
        }

        /// Bytes used in the kernel heap by this process.
        ssize_t heapUsage;
        /// Virtual address space consumed, including that which would trigger
        /// a successful trap to page data in.
        ssize_t virtualPages;
        /// Physical address space consumed, barring that which is shared.
        ssize_t physicalPages;
        /// Shared pages consumed.
        ssize_t sharedPages;

        /// Time spent in userspace as this process.
        Time::Timestamp userTime;
        /// Time spent in the kernel as this process.
        Time::Timestamp kernelTime;

        /// Time at which process started.
        Time::Timestamp startTime;
    } m_Metadata;

    /** Lock-free IRQ/scheduler publication consumed by an ordinary worker. */
    DeferredTimeAccounting m_DeferredTimeAccounting;

    /** Prevents time-report callbacks from crossing process teardown. */
    OperationBarrier m_TimeAccountingReports;

    /** Stock kernel processes do not need timer-report worker publications. */
    bool m_bTimeAccountingReportsEnabled;

    /** Root directory for this process. NULL == system-wide default. */
    File *m_pRootFile;

    /** Is our address space shared with the parent? */
    bool m_bSharedAddressSpace;

    /** Init process (terminated processes' children will reparent to this). */
    static Process *m_pInitProcess;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    static TerminationElectionHook m_TerminationElectionHook;
    static OrphanPublicationHook m_OrphanPublicationHook;
#endif

};

#endif
