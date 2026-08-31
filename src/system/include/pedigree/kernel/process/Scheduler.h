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
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/new"

#include <config.h>

class Thread;
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
class EXPORTED_PUBLIC Scheduler {
 public:
  /**
   * Pins one enumerated Process until the lease leaves scope.
   *
   * Process teardown first removes the object from scheduler enumeration,
   * closes lease admission, and then waits for every admitted lease. This
   * makes it safe to inspect a Process without retaining an unowned raw
   * pointer after the scheduler lock is released. Leases are thread-affine:
   * they may be returned/moved locally but must be released by the Thread
   * which acquired them.
   */
  class EXPORTED_PUBLIC ProcessLease {
   public:
    ProcessLease();
    ProcessLease(ProcessLease&& other);
    ~ProcessLease();

    ProcessLease& operator=(ProcessLease&& other);

    Process* get() const {
      return m_pProcess;
    }

    Process* operator->() const {
      return m_pProcess;
    }

    explicit operator bool() const {
      return m_pProcess != nullptr;
    }

    void reset();

   private:
    friend class Scheduler;

    explicit ProcessLease(Process* process);
    ProcessLease(const ProcessLease&) = delete;
    ProcessLease& operator=(const ProcessLease&) = delete;

    Process* m_pProcess;
    TerminationDeferral m_TerminationDeferral;
  };

  /** Get the instance of the scheduler */
  static Scheduler& instance() {
    return m_Instance;
  }

  /** Initialises the scheduler. */
  bool initialise(Process* pKernelProcess);

  /** Adds a thread to be load-balanced and accounted.
      \param pThread The new thread.
      \param PPSched The per-processor scheduler the thread will start on. */
  void addThread(Thread* pThread, PerProcessorScheduler& PPSched);
  /** Removes a thread from being load-balanced and accounted. */
  void removeThread(Thread* pThread);

  /** Whether a thread is entered into the scheduler at all. */
  bool threadInSchedule(Thread* pThread);

  /** Adds a completely constructed process to enumeration. */
  void addProcess(Process* pProcess);
  /** Removes a process.
   *  \note This is purely for enumeration purposes. */
  void removeProcess(Process* pProcess);

  /** Causes a manual reschedule. */
  void yield();

  /** Returns the number of processes currently in operation. */
  size_t getNumProcesses();

  /**
   * Pins the n'th process currently in operation into \p lease.
   * Any previous lease is released; failure leaves \p lease empty.
   */
  MUST_USE_RESULT bool acquireProcess(ProcessLease& lease, size_t n);

  /** Pins the first enumerated process with the requested concrete type. */
  MUST_USE_RESULT bool acquireFirstProcessOfType(ProcessLease& lease, Process::ProcessType type);

  /**
   * Pins the process with the given scheduler-assigned ID into \p lease.
   * Any previous lease is released; failure leaves \p lease empty.
   */
  MUST_USE_RESULT bool acquireProcessById(ProcessLease& lease, size_t id);

  /**
   * Pins an expected pointer only if it is still in enumeration.
   *
   * Comparing the pointer under the scheduler lock does not dereference it,
   * so callers can safely validate a parent/candidate pointer which may have
   * been concurrently removed. Any previous lease is released; failure
   * leaves \p lease empty.
   */
  MUST_USE_RESULT bool acquireProcess(ProcessLease& lease, Process* expected);

  /** Completion barrier for deferred deletion after enumeration removal. */
  void waitUntilProcessRemoved(Process* expected);

  /**
   * Returns the n'th direct child of a process.
   *
   * The caller must hold the parent's child-state guard so the returned
   * Process pointer remains valid.
   */
  Process* getChildProcess(Process* pParent, size_t n);

  void threadStatusChanged(Thread* pThread);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  using GenericThreadStatusHook = void (*)(Thread* thread);

  /** Observes calls which still require the long-term scheduler lookup. */
  static EXPORTED_PUBLIC void setGenericThreadStatusHook(GenericThreadStatusHook hook);
#endif

  Process* getKernelProcess() const {
    return m_pKernelProcess;
  }

  PerProcessorScheduler* getBootstrapProcessorScheduler() const {
    return m_pBspScheduler;
  }

 private:
  friend class Process;
  friend class PerProcessorScheduler;

  Scheduler();
  NOT_COPYABLE_OR_ASSIGNABLE(Scheduler);

  /** Reserves an ID without making a partially built Process visible. */
  size_t reserveProcessId();

  /** Releases a lease through Scheduler's Process friendship. */
  void releaseProcessLease(Process* process);

  /**
   * Drains every currently enumerated Process's timer-accounting batch.
   * The caller is an ordinary per-processor accounting worker.
   */
  void drainDeferredTimeAccounting();

  /** The Scheduler instance. */
  static Scheduler m_Instance;

  /** All the processes currently in operation, for enumeration purposes. */
  List<Process*, 0> m_Processes;

  /** The next available process ID. */
  Atomic<size_t> m_NextPid;

  /** Map of processor->thread mappings, for load-balance accounting. */
  Tree<PerProcessorScheduler*, List<Thread*>*> m_PTMap;

  /** Map of thread->processor mappings. */
  Tree<Thread*, PerProcessorScheduler*> m_TPMap;

  /** Pointer to the kernel process. */
  Process* m_pKernelProcess;

  /**
   * Pointer to the BSP's scheduler.
   *
   * This may be necessary for threads that need to depend on e.g. interrupts
   * that are only coming to the BSP, and having them run on a different CPU
   * means they cannot control things like IRQs being enabled (not good).
   */
  PerProcessorScheduler* m_pBspScheduler;

  /** Main scheduler lock for modifying internal structures. */
  Spinlock m_SchedulerLock;

  /** Wakes lifecycle waiters after a Process leaves enumeration. */
#if THREADS
  WaitQueue m_ProcessRemovalWaiters;
#endif
};

#endif  // SCHEDULER_H
