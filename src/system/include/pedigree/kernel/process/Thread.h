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
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/SchedulingAlgorithm.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/new"

class ExtensibleBitmap;
class Process;

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
    // To set uninterruptible state.
    friend class Uninterruptible;

  public:
    /** The state that a thread can possibly have. */
    enum Status
    {
        Ready,
        Running,
        Sleeping,
        Zombie,
        AwaitingJoin,
        Suspended,  /// Suspended (eg, POSIX SIGSTOP)
    };

    /** "Debug state" - higher level state of the thread. */
    enum DebugState
    {
        None,
        SemWait,
        CondWait,
        Joining
    };

    /** Reasons for a wakeup. */
    enum WakeReason
    {
        NotWoken,  // can be used to check if a reason has been set yet
        WokenByAlarm,
        WokenByEvent,
        WokenBecauseTerminating,
        WokenBecauseUnwinding,
        Unknown
    };

    /** Thread start function type. */
    typedef int (*ThreadStartFunc)(void *);

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
       of the state stack. This also pushes to the top of the inhibited events
       stack, copying the current inhibit mask. \todo This should also push
       errno and m_bInterrupted, so syscalls can be used safely in interrupt
       handlers. \return A reference to the previous state. */
    SchedulerState &pushState();

    /** Decreases the state nesting level by one, popping both the state stack
       and the inhibit mask stack. If clean == true, the stacks and other
       resources will also be cleaned up. Pass clean = false if losing the
       stack would be dangerous in a particular context. */
    void popState(bool clean = true);

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

    void setParent(Process *p)
    {
        m_pParent = p;
    }

    /** Retrieves our current status. */
    Status getStatus() const
    {
        return m_Status;
    }

    /** Sets our current status. */
    void setStatus(Status s);

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
        return m_Errno;
    }

    /** Sets the last error - errno. */
    void setErrno(size_t err)
    {
        m_Errno = err;
    }

    /** Returns whether the thread was just interrupted deliberately (e.g.
        because of a timeout). */
    bool wasInterrupted()
    {
        return m_bInterrupted;
    }

    /** Sets whether the thread was just interrupted deliberately. */
    void setInterrupted(bool b)
    {
        m_bInterrupted = b;
    }

    /** Enum used by the following function. */
    enum UnwindType
    {
        Continue = 0,           ///< No unwind necessary, carry on as normal.
        ReleaseBlockingThread,  ///< (a) below.
        Exit                    ///< (b) below.
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
    UnwindType getUnwindState()
    {
        return m_UnwindState;
    }
    /** Sets the above unwind state. */
    void setUnwindState(UnwindType ut)
    {
        m_UnwindState = ut;

        if (ut != Continue)
        {
            reportWakeup(WokenBecauseUnwinding);
        }
    }

    void setBlockingThread(Thread *pT)
    {
        m_StateLevels[getStateLevel()].m_pBlockingThread = pT;
    }
    Thread *getBlockingThread(size_t level = ~0UL)
    {
        if (level == ~0UL)
            level = getStateLevel();
        return m_StateLevels[level].m_pBlockingThread;
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

    /** Returns the thread's scheduler lock. */
    Spinlock &getLock()
    {
        return m_Lock;
    }

    /** Sends the asynchronous event pEvent to this thread.

        If the thread ID is greater than or equal to EVENT_TID_MAX, the event
       will be ignored. */
    bool sendEvent(Event *pEvent);

    /** Sets the given event number as inhibited.
        \param bInhibit True if the event is to be inhibited, false if the event
       is to be allowed. */
    void inhibitEvent(size_t eventNumber, bool bInhibit);

    /** Walks the event queue, removing the event \p pEvent , if found. */
    void cullEvent(Event *pEvent);

    /** Walks the event queue, removing the event with number \p eventNumber ,
     * if found. */
    void cullEvent(size_t eventNumber);

    /** Grabs the first available unmasked event and pops it off the queue.
        This also pushes the inhibit mask stack.

        \note This is intended only to be called by PerProcessorScheduler. */
    Event *getNextEvent();

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

    /** Adds a request to the Thread's pending request list */
    void addRequest(RequestQueue::Request *req);

    /** Removes a request from the Thread's pending request list */
    void removeRequest(RequestQueue::Request *req);

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

    /** Gets whether this thread is interruptible or not. */
    bool isInterruptible();

    /**
     * Add a new watcher location that is updated when this thread is woken.
     * \note the location is removed upon the first wakeup
     */
    void addWakeupWatcher(WakeReason *watcher);

    /** Remove a wakeup watcher. */
    void removeWakeupWatcher(WakeReason *watcher);

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

  protected:
    /** Sets the scheduler for the Thread. */
    void setScheduler(class PerProcessorScheduler *pScheduler);

    /** Sets or unsets the interruptible state of the Thread. */
    void setInterruptible(bool state);

  private:
    /** Copy-constructor */
    Thread(const Thread &);
    /** Assignment operator */
    Thread &operator=(const Thread &);

    /** Cleans up the given state level. */
    void cleanStateLevel(size_t level);

    /** Report a wakeup. */
    void reportWakeup(WakeReason reason);
    void reportWakeupUnlocked(WakeReason reason);

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

        Thread *m_pBlockingThread;
    };

    /** An optional name for the thread for debugging. */
    String m_Name;

    /** The current index into m_States (head of the state stack). */
    size_t m_nStateLevel = 0;

    /** Our parent process. */
    Process *m_pParent = nullptr;

    /** The stack that we allocated from the VMM. This may or may not also be
        the kernel stack - depends on whether we are a user or kernel mode
        thread. This is used solely for housekeeping/cleaning up purposes. */
    void *m_pAllocatedStack = nullptr;

    /** Our thread ID. */
    size_t m_Id = 0;

    /** The number of the last error to occur. */
    size_t m_Errno = 0;

    /** Address to supplement the DebugState information */
    uintptr_t m_DebugStateAddress = 0;

    class PerProcessorScheduler *m_pScheduler = nullptr;

    /** Thread priority: 0..MAX_PRIORITIES-1, 0 being highest. */
    size_t m_Priority = DEFAULT_PRIORITY;

    /** Memory mapping for the TLS base of this thread (userspace-only) */
    void *m_pTlsBase = nullptr;

#if MULTIPROCESSOR
    ProcessorId
#else
    size_t
#endif
        m_ProcId = 0;

    /** Waiters on this thread. */
    Thread *m_pWaiter = nullptr;

    /** Lock for schedulers. */
    Spinlock m_Lock;

    /** General concurrency lock, not touched by schedulers. */
    Spinlock m_ConcurrencyLock;

    /** Queue of Events ready to run. */
    List<Event *> m_EventQueue;

    /** List of requests pending on this Thread */
    List<RequestQueue::Request *> m_PendingRequests;

    /** List of wakeup watchers that need to be informed when we wake up. */
    List<WakeReason *> m_WakeWatchers;

    StateLevel m_StateLevels[MAX_NESTED_EVENTS];

    /** Our current status. */
    volatile Status m_Status = Ready;

    /** Our exit code. */
    int m_ExitCode = 0;

    /** Debug state - a higher level state information for display in the
     * debugger for debugging races and deadlocks. */
    DebugState m_DebugState = None;

    UnwindType m_UnwindState = Continue;

    /** Whether the thread was interrupted deliberately.
        \see Thread::wasInterrupted */
    bool m_bInterrupted = false;

    /** Whether or not userspace has overridden its TLS base. */
    bool m_bTlsBaseOverride = false;

    /** Are we in the process of removing tracked RequestQueue::Request objects?
     */
    bool m_bRemovingRequests = false;

    /** Whether this thread has been detached or not. */
    bool m_bDetached = false;

    /** Whether this thread has been marked interruptible or not. */
    bool m_bInterruptible = true;
};

#endif
