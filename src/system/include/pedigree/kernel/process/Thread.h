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

#ifndef THREAD_H
#define THREAD_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/AtomicStateCleanup.h"
#include "pedigree/kernel/process/DeferredScope.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/DeferredTimeAccounting.h"
#include "pedigree/kernel/process/SchedulingAlgorithm.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/new"

class ExtensibleBitmap;
class PageFaultHandler;
class Process;
class SyscallManager;
class TerminationDeferral;
class TimeoutGuard;
class IrqHandlerRegistry;
class TimerHandlerRegistry;
class HostedInterruptManager;
class RoundRobin;

/** Thread TLS area size */
#define THREAD_TLS_SIZE 0x1000

/**
 * An abstraction of a thread of execution.
 *
 * The thread maintains not just one execution context (SchedulerState) but a
 * stack of them, along with a stack of masks for inhibiting event dispatch.
 *
 * This enables event dispatch at any time without affecting the previous state,
 * as well as changing the event mask from nested event handlers without
 * affecting the state of any other running handler.
 */
class EXPORTED_PUBLIC Thread
{
    friend class PerProcessorScheduler;
    friend class PageFaultHandler;
    friend class Process;
    friend class WaitQueue;
    friend class Uninterruptible;
    friend class TerminationDeferral;
    friend class SyscallManager;
    friend class TimeoutGuard;
    friend class IrqHandlerRegistry;
    friend class TimerHandlerRegistry;
    friend class HostedInterruptManager;
    friend class RoundRobin;

  public:
    /** The state that a thread can possibly have. */
    enum Status
    {
        Created,
        Ready,
        Running,
        Sleeping,
        Zombie,
        AwaitingJoin,
    };

    /** "Debug state" - higher level state of the thread. */
    enum DebugState
    {
        None,
        SemWait,
        CondWait,
        Joining,
        FutexWait,
        EventWait,
        ProcessWait,
        CallbackDrain
    };

    /** Thread start function type. */
    typedef int (*ThreadStartFunc)(void *);

    /**
     * Optional scheduler-side admission predicate for a ready kernel worker.
     *
     * The thread remains on its processor's ready queue while the predicate is
     * false, but the scheduler skips it. This lets an IRQ publish an atomic
     * work predicate without mutating a wait queue or ready queue from hard
     * context. It must be installed before start() on a delayed thread.
     */
    typedef bool (*SchedulerReadyPredicate)(void *);

    /** Creates a new Thread belonging to the given Process. It shares the
     Process' * virtual address space.
     *
     * The constructor registers itself with the Scheduler and parent process -
     this * does not need to be done manually.
     *
     * If kernelMode is true, and pStack is NULL, no stack space is assigned.
     *
     * \param pParent The parent process. Can never be NULL.
     * \param kernelMode Is the thread going to be operating in kernel space
     only? * \param pStartFunction The function to be run when the thread
     starts. * \param pParam A parameter to give the startFunction. * \param
     pStack (Optional) A (user mode) stack to give the thread - applicable for
     user mode threads *               only. * \param semiUser (Optional)
     Whether to start the thread as if it was a user mode thread, but begin in
     kernel mode (to do setup and jump to usermode manually). * \param
     delayedStart (Optional) Start the thread in a halted state.
     */
    Thread(
        Process *pParent, ThreadStartFunc pStartFunction, void *pParam,
        void *pStack = 0, bool semiUser = false, bool bDontPickCore = false,
        bool delayedStart = false);

    /** Alternative constructor - this should be used only by
     * initialiseMultitasking() to define the first kernel thread. */
    Thread(Process *pParent);

    /** Constructor for when forking a process. Assumes pParent has already been
     * set up with a clone of the current address space and sets up the new
     * thread to return to the caller in that address space. */
    Thread(Process *pParent, SyscallState &state, bool delayedStart = false);

    /** Destroys the Thread.
     *
     * The destructor unregisters itself with the Scheduler and parent process -
     * this does not need to be done manually. */
    virtual ~Thread();

    /**
     * Performs termination steps on the thread, while the thread is still able
     * to reschedule. Required as ~Thread() is called in a context where the
     * thread has been removed from the scheduler, and triggering a reschedule
     * may add the thread back to the ready queue by accident.
     */
    void shutdown();

    /* Forces the thread to run on the bootstrap processor. */
    void forceToStartupProcessor();

    /** Returns a reference to the Thread's saved context. This function is
     * intended only for use by the Scheduler. */
    SchedulerState &state();

    /** Increases the state nesting level by one - pushes a new state to the top
       of the state stack. This also pushes the event mask and per-context
       syscall state. \return A reference to the previous state. */
    /** Pushes an event state, or returns null when nesting is exhausted. */
    SchedulerState *pushState();

    /** Decreases the state nesting level by one, popping both the state stack
       and the inhibit mask stack. If clean == true, the stacks and other
       resources will also be cleaned up. Pass clean = false if losing the
       stack would be dangerous in a particular context. */
    void popState(bool clean = true);

    /** Abandons the current event state without running stack destructors. */
    void abandonCurrentState(bool clean = false);

    /** Abandons every nested event state before a no-return user transition. */
    void abandonAllStates();

    VirtualAddressSpace::Stack *getStateUserStack();

    void setStateUserStack(VirtualAddressSpace::Stack *st);

    /** Returns the state nesting level. */
    size_t getStateLevel() const;

    /** Allocates a new stack for a specific nesting level, if required */
    void allocateStackAtLevel(size_t stateLevel);

    /** Sets the new kernel stack for the current state level in the TSS */
    void setKernelStack();

    /** Overwrites the state at the given nesting level.
     *\param stateLevel The nesting level to edit.
     *\param state The state to copy.
     */
    void pokeState(size_t stateLevel, SchedulerState &state);

    /** Retrieves a pointer to this Thread's parent process. */
    Process *getParent() const
    {
        return m_pParent;
    }

    /** Records this Thread's monotonic entry baseline for one CPU-time mode. */
    void recordTime(CpuTimeMode mode);

    /** Publishes elapsed time from this Thread into its Process aggregate. */
    void trackTime(CpuTimeMode mode);

    /** Accounts one CPU-mode transition from a single monotonic sample. */
    void transitionTime(CpuTimeMode from, CpuTimeMode to);

    void setParent(Process *p)
    {
        m_pParent = p;
    }

    /** Retrieves our current status. */
    Status getStatus() const
    {
        return m_Status;
    }

    /**
     * Starts a thread constructed with delayedStart=true.
     *
     * This is deliberately distinct from waking a blocked thread: only its
     * WaitQueue may perform that transition.
     */
    bool start();

    bool setSchedulerReadyPredicate(
        SchedulerReadyPredicate predicate, void *context);

    /** Retrieves the exit status of the Thread. */
    int getExitCode()
    {
        return m_ExitCode;
    }

    /** Retrieves a pointer to the top of the Thread's kernel stack. */
    void *getKernelStack();

    /** Retrieves a pointer to the bottom of the Thread's kernel stack, and its size. */
    void *getKernelStackBase(size_t *size) const;

    /** Returns the Thread's ID. */
    size_t getId()
    {
        return m_Id;
    }

    /** Returns the last error that occurred (errno). */
    size_t getErrno()
    {
        return m_StateLevels[m_nStateLevel].m_Errno;
    }

    /** Sets the last error - errno. */
    void setErrno(size_t err)
    {
        m_StateLevels[m_nStateLevel].m_Errno = err;
    }

    enum InterruptionReason
    {
        NotInterrupted,
        InterruptedByTimeout,
        InterruptedBySignal,
    };

    /** Returns whether the current context's wait was interrupted. */
    bool wasInterrupted()
    {
        return getInterruptionReason() != NotInterrupted;
    }

    void clearInterruption()
    {
        m_StateLevels[m_nStateLevel].m_InterruptionReason = NotInterrupted;
    }

    InterruptionReason getInterruptionReason()
    {
        return m_StateLevels[m_nStateLevel].m_InterruptionReason;
    }

    void setInterruptionReason(InterruptionReason reason)
    {
        m_StateLevels[m_nStateLevel].m_InterruptionReason = reason;
    }

    /** Marks a timeout in the context interrupted by the current event. */
    void markTimeoutInterruptedWait();

    /** Marks a signal only when its delivery actually interrupted a wait. */
    void markSignalInterruptedWait();

    /** Enum used by the following function. */
    enum UnwindType
    {
        Continue = 0,    ///< No unwind necessary, carry on as normal.
        Exit,            ///< Exit the owning process at the next safe boundary.
        TerminateThread  ///< Exit only this thread during Process exit.
    };

    /** Returns nonzero if the thread has been asked to unwind quickly.

        This happens if this thread (or a thread blocking on this thread) is
       scheduled for deletion. The intended behaviour is that the stack is
       unwound as quickly as possible with all semaphores and buffers deleted to
       a point where

          (a) no threads can possibly be blocking on this or
          (b) The thread has no more locks taken and is ready to be destroyed,
       at which point it should call the subsys exit() function.

        Whether to adopt option A or B depends on whether this thread or not has
       been asked to terminate, given by the return value. **/
    UnwindType getUnwindState();
    /** Sets the above unwind state. */
    void setUnwindState(UnwindType ut);

    /** True while stack-owned lifetime state must be retired before teardown. */
    bool isTerminationDeferred() const
    {
        if (
            __atomic_load_n(
                &m_TerminationDeferralDepth, __ATOMIC_ACQUIRE) != 0)
        {
            return true;
        }

        for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level)
        {
            if (
                __atomic_load_n(
                    &m_pDeferredScopes[level], __ATOMIC_ACQUIRE))
            {
                return true;
            }
        }
        return false;
    }

    /** Returns the thread's debug state. */
    DebugState getDebugState(uintptr_t &address)
    {
        address = m_DebugStateAddress;
        return m_DebugState;
    }
    /** Sets the thread's debug state. */
    void setDebugState(DebugState state, uintptr_t address)
    {
        m_DebugState = state;
        m_DebugStateAddress = address;
    }

    struct WaitDebugInfo
    {
        WaitQueue *queue;
        const void *channelOwner;
        uintptr_t channelValue;
        WaitQueue::WakeReason reason;
        size_t stateLevel;
        bool queued;
    };

    /** Takes a best-effort snapshot of the active wait for the debugger. */
    bool getWaitDebugInfo(WaitDebugInfo &info);

    /** Returns the thread's scheduler lock. */
    Spinlock &getLock()
    {
        return m_Lock;
    }

    /** Sends the asynchronous event pEvent to this thread.

        If the thread ID is greater than or equal to EVENT_TID_MAX, the event
       will be ignored.

       The event is not cloned. On success the thread owns a deletable event;
       on failure ownership remains with the caller. */
    bool sendEvent(Event *pEvent);

    /**
     * Blocks until an event can be delivered to this thread.
     * onAbandon runs if terminal cancellation prevents this call returning.
     */
    void waitForEvent(
        WaitQueue::AbandonCallback onAbandon = nullptr,
        void *abandonContext = nullptr);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    enum StateTransitionWindow
    {
        StatePushBeforePublish,
        StatePopAfterPublish,
    };
    using StateTransitionHook = void (*)(
        StateTransitionWindow window, Thread *thread, size_t previousLevel,
        size_t nextLevel);
    using JoinOperationHook = void (*)(Thread *target, Process *parent);
    using DeferredScopeLockHook = void (*)();

    /** Installs a deterministic observer around state-level publication. */
    static void setStateTransitionHook(StateTransitionHook hook);

    /** Pauses an admitted join after reapability but before final deletion. */
    static void setJoinOperationHook(JoinOperationHook hook);

    /** Exposes scheduler handoff completion to deterministic hosted tests. */
    bool isReapableForHostedTest();

    /** Waits for scheduler handoff without claiming or deleting the target. */
    bool waitUntilReapableForHostedTest();

    /**
     * Runs the prequeued-event wait regression wholly inside the hosted
     * kernel, avoiding C++ Event object ABI differences in dynamic modules.
     */
    bool runHostedPrequeuedEventRegression();

    /** Verifies that a dequeued Event remains leased until dispatch releases it. */
    bool runHostedEventDeliveryLeaseRegression();

    /** Verifies shutdown rejects and drains queued Event registrations. */
    bool runHostedEventShutdownRegression();

    /** Exercises event publication across push/pop state transitions. */
    bool runHostedStatePublicationRegression();

    /** Exercises LIFO, checkpoint, and per-level abandoned-state cleanup. */
    bool runHostedStateCleanupRegression();

    /** Simulates an interrupted legacy scope writer for IRQ regressions. */
    void withDeferredScopeLockForTest(DeferredScopeLockHook hook);
#endif

    /** Sets the given event number as inhibited.
        \param bInhibit True if the event is to be inhibited, false if the event
       is to be allowed. */
    void inhibitEvent(size_t eventNumber, bool bInhibit);

    /** Retrieves the POSIX signal mask for the current event nesting level. */
    uint64_t getSignalMask();

    /** Sets the POSIX signal mask for the current event nesting level. */
    void setSignalMask(uint64_t mask);

    struct AlternateSignalStack
    {
        AlternateSignalStack()
            : base(0), size(0), enabled(false), inUse(false)
        {
        }

        uintptr_t base;
        size_t size;
        bool enabled;
        bool inUse;
    };

    AlternateSignalStack &getAlternateSignalStack()
    {
        return m_AlternateSignalStack;
    }

    /** Walks the event queue, removing the event \p pEvent , if found. */
    void cullEvent(Event *pEvent);

    /** Walks the event queue, removing the event with number \p eventNumber ,
     * if found. */
    void cullEvent(size_t eventNumber);

    bool hasEvents();

    /** Determines if the given event is currently in the event queue. */
    bool hasEvent(Event *pEvent);
    bool hasEvent(size_t eventNumber);

    void setPriority(size_t p)
    {
        m_Priority = p;
    }
    size_t getPriority()
    {
        return m_Priority;
    }

    /** An unexpected exit has occurred, perform cleanup */
    void unexpectedExit();

    /** Gets the TLS base address for this thread. */
    uintptr_t getTlsBase();

    /**
     * Resets the TLS base address for this thread and re-maps it.
     * Note: doesn't free the memory - only call after a call to something like
     * revertToKernelAddressSpace!
     */
    void resetTlsBase();

    /**
     * Set the TLS base for this thread. Once set, it must be cleaned up by
     * the caller when the thread terminates, which makes this primarily useful
     * for userspace TLS segments.
     */
    void setTlsBase(uintptr_t base);

    /** Gets this thread's CPU ID */
    inline
#if MULTIPROCESSOR
        ProcessorId
#else
        size_t
#endif
        getCpuId()
    {
        return m_ProcId;
    }

    /** Sets this thread's CPU ID */
    inline void setCpuId(
#if MULTIPROCESSOR
        ProcessorId
#else
        size_t
#endif
            id)
    {
        m_ProcId = id;
    }

    /**
     * Blocks until the Thread returns.
     *
     * After join() returns successfully, the thread object is NOT valid.
     *
     * \return whether the thread was joined or not.
     */
    bool join();

    /**
     * Joins as a lifetime barrier which terminal teardown cannot abandon.
     *
     * Use only for owned worker destruction/cleanup. Ordinary callers should
     * use join(), which remains interruptible.
     */
    bool joinForCompletion();

    /**
     * Marks the thread as detached.
     *
     * A detached thread cannot be joined and will be automatically cleaned up
     * when the thread entry point returns, or the thread is otherwise
     * terminated. A thread cannot be detached if another thread is already
     * join()ing it.
     */
    bool detach();

    /**
     * Checks detached state of the thread.
     */
    bool detached() const
    {
        return m_bDetached;
    }

    /**
     * Sets the exit code of the Thread and sets the state to Zombie, if it is
     * being waited on; if it is not being waited on the Thread is destroyed.
     * \note This is meant to be called only by the thread trampoline - this is
     * the only reason it is public. It should NOT be called by anyone else!
     */
    static void threadExited() NORETURN;

    /** Gets whether event delivery is currently deferred. */
    bool eventsDeferred();

    /** Gets the per-processor scheduler for this Thread. */
    class PerProcessorScheduler *getScheduler() const;

    const String &getName() const
    {
        return m_Name;
    }

    void setName(const String &name)
    {
        m_Name = name;
    }

    template <size_t N>
    void setName(const char (&name)[N])
    {
        m_Name.assign(name, N);
    }

#if HOSTED
    void enterHostedSignalHandler()
    {
        ++m_HostedSignalDepth;
    }

    void leaveHostedSignalHandler()
    {
        --m_HostedSignalDepth;
    }

    size_t getHostedSignalDepth() const
    {
        return m_HostedSignalDepth;
    }
#endif

  protected:
    /** Sets the scheduler for the Thread. */
    void setScheduler(class PerProcessorScheduler *pScheduler);

    void deferEvents();
    void resumeEvents();
    void deferTermination();
    void resumeTermination();
    void registerDeferredScope(
        DeferredScopeRecord &record, bool termination, bool events);
    void armStateCleanup(
        DeferredScopeRecord &record,
        DeferredScopeRecord::Cleanup cleanup, void *context);
    void unregisterDeferredScope(DeferredScopeRecord &record);
    void disarmStateCleanup(DeferredScopeRecord &record);
    void moveDeferredScope(
        DeferredScopeRecord &from, DeferredScopeRecord &to);
    void retireDeferredScopes(
        bool allStateLevels, size_t stateLevel = 0);
    size_t stateCleanupCheckpoint();
    void retireDeferredScopesAfter(size_t checkpoint);
    void retireDeferredScopesMatching(
        bool allStateLevels, size_t stateLevel,
        bool newerThanCheckpoint, size_t checkpoint);
    void armAtomicStateCleanup(
        AtomicStateCleanupRecord &record,
        AtomicStateCleanupRecord::Cleanup cleanup, void *context);
    void disarmAtomicStateCleanup(AtomicStateCleanupRecord &record);

  private:
    /** Copy-constructor */
    Thread(const Thread &);
    /** Assignment operator */
    Thread &operator=(const Thread &);

    /** Cleans up the given state level. */
    void cleanStateLevel(size_t level);

    /** Checks for an event that can run while m_Lock is already held. */
    bool hasEventsUnlocked();
    bool hasDeliverableEventsUnlocked();

    /** Scheduler-only dequeue that retains the Event delivery registration. */
    MUST_USE_RESULT Event::Delivery getNextEvent();

    /** Scheduler-only status transition primitive. */
    void setStatus(Status s);

    /**
     * Completes exit after the scheduler has switched away from our stack.
     * Returns true when a concurrent detach made immediate deletion safe.
     */
    bool markReapable();

    /** Releases an exclusive join claim when a terminal wait cannot return. */
    static void abandonJoin(void *context);

    bool joinInternal(bool completion);

    /** Admits a Process::ThreadLease while this object remains discoverable. */
    bool beginExternalLease();

    /** Releases a successful external lease admission. */
    void endExternalLease();

    /** Prevents any further Process::ThreadLease acquisition. */
    void closeExternalLeaseAdmission();

    /** Closes external admission and drains every previously admitted lease. */
    void closeExternalLeaseAdmissionAndDrain();

    /** Interrupts the active wait at the current event nesting level. */
    bool interruptWaitUnlocked(
        WaitQueue::WakeReason reason,
        PerProcessorScheduler *&readyScheduler);
    bool hasActiveWaitUnlocked() const;
    bool activeWaitPendingUnlocked() const;

    /** A level of thread state */
    struct StateLevel
    {
        StateLevel();
        ~StateLevel();

        StateLevel(const StateLevel &s);
        StateLevel &operator=(const StateLevel &s);

        /** The processor state for this level. */
        SchedulerState *m_State;

        /** Our kernel stack. */
        VirtualAddressSpace::Stack *m_pKernelStack;

        VirtualAddressSpace::Stack *m_pUserStack;

        /** Auxillary stack, to be freed in case the kernel stack is null.
         *  This allows kernel mode threads to have stacks freed, as they
         *  are left hanging otherwise.
         */
        VirtualAddressSpace::Stack *m_pAuxillaryStack;

        /** Stack of inhibited Event masks, gets pushed with a new value when an
           Event handler is run, and popped when one completes.

            \note A '1' here means the event is inhibited, '0' means it can be
           fired. */
        SharedPointer<ExtensibleBitmap> m_InhibitMask;

        /** POSIX signals blocked at this event nesting level. */
        uint64_t m_SignalMask;

        /** Syscall-local state isolated from nested event handlers. */
        size_t m_Errno;
        InterruptionReason m_InterruptionReason;

        /** Event dispatch at this level was initiated by a WaitQueue wake. */
        bool m_bDispatchingWaitEvent;

        /** Persistent wait record for this event nesting level. */
        WaitQueue::Waiter m_Waiter;
    };

    /** An optional name for the thread for debugging. */
    String m_Name;

    /** The current index into m_States (head of the state stack). */
    size_t m_nStateLevel = 0;

    /** Our parent process. */
    Process *m_pParent = nullptr;

    /** Per-thread baselines avoid cross-CPU corruption within one Process. */
    ThreadTimeAccounting m_TimeAccounting;

    /** The stack that we allocated from the VMM. This may or may not also be
        the kernel stack - depends on whether we are a user or kernel mode
        thread. This is used solely for housekeeping/cleaning up purposes. */
    void *m_pAllocatedStack = nullptr;

    /** Our thread ID. */
    size_t m_Id = 0;

    /** Address to supplement the DebugState information */
    uintptr_t m_DebugStateAddress = 0;

    class PerProcessorScheduler *m_pScheduler = nullptr;

    /** Thread priority: 0..MAX_PRIORITIES-1, 0 being highest. */
    size_t m_Priority = DEFAULT_PRIORITY;

    /** Intrusive ready-queue state; scheduler publication must not allocate. */
    Thread *m_pReadyPrevious = nullptr;
    Thread *m_pReadyNext = nullptr;
    size_t m_ReadyQueuePriority = MAX_PRIORITIES;
    bool m_bReadyQueued = false;
    SchedulerReadyPredicate m_SchedulerReadyPredicate = nullptr;
    void *m_SchedulerReadyContext = nullptr;

    /** Memory mapping for the TLS base of this thread (userspace-only) */
    void *m_pTlsBase = nullptr;

#if MULTIPROCESSOR
    ProcessorId
#else
    size_t
#endif
        m_ProcId = 0;

    /** Lock for schedulers. */
    Spinlock m_Lock;

    /** Completion queue used by join(). */
    WaitQueue m_JoinWaiters;

    /** Short predicate lock, never held while waking lease drainers. */
    Spinlock m_ExternalLeaseLock;

    /** Lifetime barrier for Process::ThreadLease users. */
    WaitQueue m_ExternalLeaseWaiters;

    /** External leases admitted before retirement closed the target. */
    size_t m_nExternalLeases = 0;

    /** Whether no new external Thread leases may be acquired. */
    bool m_bExternalLeaseAdmissionClosed = false;

    /**
     * The final releaser is still deciding detached retirement.
     *
     * A zero lease count alone is not enough for scheduler-side deletion:
     * the releasing call still has to inspect this Thread after decrementing.
     */
    bool m_bExternalLeaseReleaseInProgress = false;

    /** Serialises event queue inspection with waitForEvent(). */
    WaitQueue m_EventWaiters;

    /** Drains sendEvent operations admitted before shutdown. */
    WaitQueue m_EventSenderDrainWaiters;

    /** Queue of Events ready to run. */
    List<Event *> m_EventQueue;

    /** sendEvent calls admitted before shutdown closed the event queue. */
    size_t m_EventSendersInFlight = 0;

    StateLevel m_StateLevels[MAX_NESTED_EVENTS];

    /** Alternate signal stack configuration is per-thread. */
    AlternateSignalStack m_AlternateSignalStack;

    /** Our current status. Sleeping is reserved for an active WaitQueue. */
    volatile Status m_Status = Ready;

    /** Our exit code. */
    int m_ExitCode = 0;

    /** Debug state - a higher level state information for display in the
     * debugger for debugging races and deadlocks. */
    DebugState m_DebugState = None;

    UnwindType m_UnwindState = Continue;

    /** Whether or not userspace has overridden its TLS base. */
    bool m_bTlsBaseOverride = false;

    /** Whether shutdown() has completed its one-way transition. */
    bool m_bShutdown = false;

    /** The add worker may publish a delayed thread after this request. */
    bool m_bStartRequested = false;

    /** Whether this thread has been detached or not. */
    bool m_bDetached = false;

    /** Whether join() has been claimed by another thread. */
    bool m_bJoinClaimed = false;

    /** Exactly one path owns detached Thread destruction. */
    bool m_bDetachedRetirementClaimed = false;

    /** Thread shutdown has started, but its stack may still be in use. */
    bool m_bExitStarted = false;

    /** The scheduler has switched off this thread's stack. */
    bool m_bReapable = false;

    /** Process destruction owns this Thread object once it is off-stack. */
    bool m_bProcessExitOwned = false;

    /** This thread is included in its Process' exit rendezvous. */
    bool m_bProcessExitParticipant = false;

    /** Nesting depth for deferred event delivery. */
    size_t m_EventDeferralDepth = 0;

    /** Nesting depth for scopes which must run cleanup before teardown. */
    size_t m_TerminationDeferralDepth = 0;

    /** Per-state LIFO of stack records which teardown must retire explicitly. */
    DeferredScopeRecord *m_pDeferredScopes[MAX_NESTED_EVENTS] = {};
    size_t m_NextStateCleanupSequence = 0;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    Spinlock m_DeferredScopeRegressionLock;
#endif

#if HOSTED
    /** Number of live host signal frames owned by this Pedigree thread. */
    size_t m_HostedSignalDepth = 0;
#endif
};

#endif
