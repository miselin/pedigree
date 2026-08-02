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

#ifndef PERPROCESSORSCHEDULER_H
#define PERPROCESSORSCHEDULER_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/SchedulerTimerHandler.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/DeferredTimeAccounting.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OwnedThread.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"

class SchedulingAlgorithm;
class Spinlock;
class WaitQueue;

class EXPORTED_PUBLIC PerProcessorScheduler : public SchedulerTimerHandler
{
  public:
    /** Default constructor - Creates an empty scheduler with a new idle thread.
     */
    PerProcessorScheduler();

    ~PerProcessorScheduler();

    /** Initialises the scheduler with the given thread. */
    void initialise(Thread *pThread);

    /** Looks for event handlers to run, and if found, dispatches one.
        \param userStack The stack to use if the event has a user-mode handler.
       Usually obtained from an interruptState or syscallState. */
    void checkEventState(uintptr_t userStack);

    /** Assumes this thread has just returned from executing a event handler,
        and lets it resume normal execution. */
    void eventHandlerReturned() NORETURN;

    /** Adds a new thread.
        \param pThread The thread to add.
        \param pStartFunction The function to start the thread with.
        \param pParam void* parameter to give to the function.
        \param bUsermode Start the thread in User Mode?
        \param pStack Stack to start the thread with. */
    void addThread(
        Thread *pThread, Thread::ThreadStartFunc pStartFunction, void *pParam,
        bool bUsermode, void *pStack);

    /** Adds a new thread.
        \param pThread The thread to add.
        \param state The syscall state to jump to. */
    void addThread(Thread *pThread, SyscallState &state);

    /** Destroys the currently running thread.
        \note This calls Thread::~Thread itself! */
    void killCurrentThread(Spinlock *pLock = 0) NORETURN;

    /** Selects the registered idle owner when the current Thread exits. */
    void requestCurrentThreadExitToIdle();

    /** SchedulerTimerHandler callback. */
    void timer(uint64_t delta, InterruptState &state);

    void removeThread(Thread *pThread);

    void threadStatusChanged(Thread *pThread);

    /** Atomic hard-IRQ publication; does not touch a lock or ready queue. */
    void ringIrqWorkDoorbell();

    /**
     * Publishes deferred process timer accounting from IRQ/scheduler context.
     * The accounting worker is made runnable through the shared IRQ doorbell.
     */
    void publishDeferredTimeAccounting();

    /** Reschedules once from ordinary thread context during lifecycle work. */
    void serviceIrqWorkDoorbell();

    /**
     * Delivers pending Events and terminal work immediately before a user
     * return, after the raw interrupt frame has released its C++ scopes.
     */
    void serviceUserReturnWork(InterruptState &state);

    void setIdle(Thread *pThread);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    /** Exercises the real add-worker predicate and owned shutdown path. */
    bool runHostedNewThreadWorkerRegressions();

    static bool currentIrqWorkDoorbellPendingForTest();
    static void serviceCurrentIrqWorkDoorbellForTest();
#endif

  private:
    friend class Scheduler;
    friend class Thread;
    friend class WaitQueue;

    /** Picks another runnable thread and switches to it. */
    void schedule(
        Thread::Status nextStatus = Thread::Ready,
        bool dispatchEvents = true);

    /** Blocks the current thread after WaitQueue has published its wait record. */
    void blockCurrent();

    /** Publishes a completed wait directly to this scheduler's ready queue. */
    void publishReadyFromWait(Thread *pThread);

    /** Consumes terminal state at an IRQ-enabled ordinary thread boundary. */
    void serviceTerminalStateAtThreadBoundary();

    void killCurrentThreadImpl(Spinlock *pLock, bool transferToIdle) NORETURN;

    /** Runs a raw-frame exception through its subsystem in ordinary context. */
    void serviceDeferredSubsystemException(InterruptState &state);

    /** Copy-constructor
     *  \note Not implemented - singleton class. */
    PerProcessorScheduler(const PerProcessorScheduler &);
    /** Assignment operator
     *  \note Not implemented - singleton class */
    PerProcessorScheduler &operator=(const PerProcessorScheduler &);

    /** Switches stacks, calls PerProcessorScheduler::deleteThread, then context
        switches.

        \note Implemented in core/processor/ARCH/asm*/
    static void deleteThreadThenRestoreState(
        Thread *pThread, SchedulerState &newState,
        volatile uintptr_t *pLock = 0) NORETURN;

    static void deleteThread(Thread *pThread);

    void startNewThreadWorker(Process *pParent);
    void stopNewThreadWorker();

    void startTimeAccountingWorker(Process *pParent);
    void stopTimeAccountingWorker();
    static int timeAccountingWorkerEntry(void *instance);
    static bool timeAccountingWorkerReady(void *instance);
    int runTimeAccountingWorker();

    /** The current SchedulingAlgorithm */
    SchedulingAlgorithm *m_pSchedulingAlgorithm;

    Mutex m_NewThreadDataLock;
    ConditionVariable m_NewThreadDataCondition;

    List<void *> m_NewThreadData;
    List<void *> m_DelayedNewThreadData;
    bool m_NewThreadAdmissionOpen;
    bool m_StopNewThreadWorker;
    OwnedThread m_NewThreadWorker;
    DeferredTimeAccountingWorkerState m_TimeAccountingState;
    Atomic<size_t> m_StopTimeAccountingWorker;
    OwnedThread m_TimeAccountingWorker;
    Atomic<size_t> m_IrqWorkDoorbell;

    static int processorAddThread(void *instance);

    Thread *m_pIdleThread;
};

#endif
