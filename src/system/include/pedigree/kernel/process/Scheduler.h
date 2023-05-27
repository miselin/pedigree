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

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/new"

class Thread;
class Process;
class PerProcessorScheduler;

/** \brief This class manages how processes and threads are scheduled across
 * processors.
 *
 * This is the "long term" scheduler - it load balances between processors and
 * provides the interface for adding, listing and removing threads.
 *
 * The load balancing is "lazy" in that the algorithm only runs on thread
 * addition and removal.
 */
class EXPORTED_PUBLIC Scheduler
{
  public:
    /** Get the instance of the scheduler */
    static Scheduler &instance()
    {
        return m_Instance;
    }

    /** Initialises the scheduler. */
    bool initialise(Process *pKernelProcess);

    /** Adds a thread to be load-balanced and accounted.
        \param pThread The new thread.
        \param PPSched The per-processor scheduler the thread will start on. */
    void addThread(Thread *pThread, PerProcessorScheduler &PPSched);
    /** Removes a thread from being load-balanced and accounted. */
    void removeThread(Thread *pThread);

    /** Whether a thread is entered into the scheduler at all. */
    bool threadInSchedule(Thread *pThread);

    /** Adds a process.
     *  \note This is purely for enumeration purposes.
     *  \return The ID that should be applied to this Process. */
    size_t addProcess(Process *pProcess);
    /** Removes a process.
     *  \note This is purely for enumeration purposes. */
    void removeProcess(Process *pProcess);

    /** Causes a manual reschedule. */
    void yield();

    /** Returns the number of processes currently in operation. */
    size_t getNumProcesses();

    /** Returns the n'th process currently in operation. */
    Process *getProcess(size_t n);

    void threadStatusChanged(Thread *pThread);

    Process *getKernelProcess() const
    {
        return m_pKernelProcess;
    }

    PerProcessorScheduler *getBootstrapProcessorScheduler() const
    {
        return m_pBspScheduler;
    }

  private:
    Scheduler();
    NOT_COPYABLE_OR_ASSIGNABLE(Scheduler);

    /** The Scheduler instance. */
    static Scheduler m_Instance;

    /** All the processes currently in operation, for enumeration purposes. */
    List<Process *, 0> m_Processes;

    /** The next available process ID. */
    Atomic<size_t> m_NextPid;

    /** Map of processor->thread mappings, for load-balance accounting. */
    Tree<PerProcessorScheduler *, List<Thread *> *> m_PTMap;

    /** Map of thread->processor mappings. */
    Tree<Thread *, PerProcessorScheduler *> m_TPMap;

    /** Pointer to the kernel process. */
    Process *m_pKernelProcess;

    /**
     * Pointer to the BSP's scheduler.
     *
     * This may be necessary for threads that need to depend on e.g. interrupts
     * that are only coming to the BSP, and having them run on a different CPU
     * means they cannot control things like IRQs being enabled (not good).
     */
    PerProcessorScheduler *m_pBspScheduler;

    /** Main scheduler lock for modifying internal structures. */
    Spinlock m_SchedulerLock;
};

#endif  // SCHEDULER_H
