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
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/AtomicStateCleanup.h"
#include "pedigree/kernel/process/CpuAffinity.h"
#include "pedigree/kernel/process/DeferredScope.h"
#include "pedigree/kernel/process/DeferredThreadReap.h"
#include "pedigree/kernel/process/DeferredTimeAccounting.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/ExecutionContext.h"
#include "pedigree/kernel/process/ExecutionPersonality.h"
#include "pedigree/kernel/process/SchedulingAlgorithm.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"

#include <config.h>

class ExtensibleBitmap;
class PageFaultHandler;
class Process;
struct FilesystemCredentials;
class SyscallManager;
class TerminationDeferral;
class IrqHandlerRegistry;
class TimerHandlerRegistry;
class HostedInterruptManager;
class RoundRobin;
class RequestQueueCallbackScope;
class SchedulerTimerDispatchCleanup;
class AdmittedThread;
class UserReturnFrame;

/** Thread TLS area size */
#define THREAD_TLS_SIZE PAGE_SIZE

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
class EXPORTED_PUBLIC Thread {
  friend class PerProcessorScheduler;
  friend class Scheduler;
  friend class PageFaultHandler;
  friend class Process;
  friend class WaitQueue;
  friend class Uninterruptible;
  friend class TerminationDeferral;
  friend class SyscallManager;
  friend class IrqHandlerRegistry;
  friend class TimerHandlerRegistry;
  friend class HostedInterruptManager;
  friend class RoundRobin;
  friend class ExecutionContextGuard;
  friend class RequestQueueCallbackScope;
  friend class SchedulerTimerDispatchCleanup;
  friend class AdmittedThread;

 public:
  class EXPORTED_PUBLIC StackDiscardScope {
   public:
    StackDiscardScope(DeferredScopeRecord::Cleanup cleanup, void* context);
    ~StackDiscardScope();

    void disarm();

   private:
    NOT_COPYABLE_OR_ASSIGNABLE(StackDiscardScope);

    Thread* m_pThread;
    DeferredScopeRecord m_Record;
  };

  class EXPORTED_PUBLIC UserReturnFrameScope {
   public:
    UserReturnFrameScope(Thread& owner, UserReturnFrame& frame);
    ~UserReturnFrameScope();

   private:
    NOT_COPYABLE_OR_ASSIGNABLE(UserReturnFrameScope);
    static void restore(void* context);
    Thread* m_Owner;
    size_t m_StateLevel;
    UserReturnFrame* m_Frame;
    UserReturnFrame* m_Previous;
    DeferredScopeRecord m_Record;
  };

  UserReturnFrame* currentUserReturnFrame() const;
  bool tryRequireSignalFrames();
  void clearSignalFrameRequirement();
  void setUserReturnSignalParked(bool parked);

  enum UserReturnWorkFlag : size_t {
    UserReturnExternalWork = 1,
    UserReturnEventsDeferred = 1 << 1,
    UserReturnTerminationDeferred = 1 << 2,
    UserReturnSignalFrames = 1 << 3,
    UserReturnDeferredException = 1 << 4,
    UserReturnOriginalSyscall = 1 << 5,
  };

  bool canSkipUserReturnWork();
  bool clearUserReturnWorkIfIdle();
  bool userReturnWorkPending() const {
    return __atomic_load_n(&m_UserReturnWorkPending, __ATOMIC_ACQUIRE) != 0;
  }
  bool requiresSignalFrames() const {
    return __atomic_load_n(&m_SignalFramesRequired, __ATOMIC_ACQUIRE);
  }

  /** Temporarily replaces this state level's signal mask for one blocking wait. */
  class EXPORTED_PUBLIC TemporarySignalMask {
   public:
    TemporarySignalMask(Thread& thread, uint64_t signalMask);
    ~TemporarySignalMask();

    /** Restores the owning state level and reports whether a signal interrupted the wait. */
    bool finish();

   private:
    NOT_COPYABLE_OR_ASSIGNABLE(TemporarySignalMask);

    static void discard(void* context);

    Thread* m_pThread;
    size_t m_StateLevel;
    DeferredScopeRecord m_Record;
  };

  /** The state that a thread can possibly have. */
  enum Status {
    Created,
    Ready,
    Running,
    Sleeping,
    Zombie,
    AwaitingJoin,
  };

  /** "Debug state" - higher level state of the thread. */
  enum DebugState {
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
  typedef int (*ThreadStartFunc)(void*);

  /** Releases a start parameter if a delayed thread retires before entry. */
  typedef void (*ThreadStartCleanup)(void*);

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
  Thread(Process* pParent, ThreadStartFunc pStartFunction, void* pParam, void* pStack = 0,
         bool semiUser = false, bool bDontPickCore = false, bool delayedStart = false,
         const ThreadPlacement* placement = nullptr);

  /** Alternative constructor - this should be used only by
   * initialiseMultitasking() to define the first kernel thread. */
  Thread(Process* pParent);

  /** Constructor for when forking a process. Assumes pParent has already been
   * set up with a clone of the current address space and sets up the new
   * thread to return to the caller in that address space. */
  Thread(Process* pParent, SyscallState& state, bool delayedStart = false,
         const ThreadPlacement* placement = nullptr);

  void snapshotPlacement(ThreadPlacement& placement);
  /** The allocator already holds the new Thread's construction lock. */
  void snapshotPlacementLocked(ThreadPlacement& placement) const;
  /** Success admits the returned generation; Busy names the existing one. */
  AffinityResult requestAffinity(const CpuAffinityMask& mask, uint64_t& generation);
  /** Wait without retaining caller locks; the caller must retain this Thread. */
  AffinityResult waitAffinity(uint64_t generation);
  /**
   * Current-thread gate for an audited continuation without CPU-local state.
   * Applies the latest acknowledged mask without changing it; returns with
   * interrupts disabled, including Terminal. Ordinary waits are not gates.
   */
  AffinityResult completeAffinityAtSafePoint(bool* waited = nullptr);
  bool affinityWorkPending() const {
    return __atomic_load_n(&m_AffinityReturnPending, __ATOMIC_ACQUIRE) != 0;
  }
  /** Placement inhibition only; the registration retains its own lifetime. */
  bool tryPinLegacyUserCallbacks();
  void unpinLegacyUserCallbacks();
#if PEDIGREE_AFFINITY_TESTS
  using AffinityCommitHook = void (*)(Thread*);
  static void setAffinityCommitHookForTest(Thread* target, AffinityCommitHook hook);
#endif

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

  /**
   * Runs subsystem exit hooks once, while the lifecycle owner can still
   * block and before the final scheduler handoff.
   */
  void notifySubsystemExit();
  bool prepareInputUserStack();
  void retireInputUserStack();
  VirtualAddressSpace::Stack* inputUserStack() const {
    return m_pInputUserStack;
  }

  /* Forces the thread to run on the bootstrap processor. */
  void forceToStartupProcessor();

  /** Returns a reference to the Thread's saved context. This function is
   * intended only for use by the Scheduler. */
  SchedulerState& state();

  /** Increases the state nesting level by one - pushes a new state to the top
     of the state stack. This also pushes the event mask and per-context
     syscall state. \return A reference to the previous state. */
  /** Pushes an event state, or returns null when nesting is exhausted. */
  SchedulerState* pushState();

  /** Decreases the state nesting level by one, popping both the state stack
     and the inhibit mask stack. If clean == true, the stacks and other
     resources will also be cleaned up. Pass clean = false if losing the
     stack would be dangerous in a particular context. */
  void popState(bool clean = true);

  /** Abandons the current event state without running stack destructors. */
  void abandonCurrentState(bool clean = false);

  /** Abandons every nested event state before a no-return user transition. */
  void abandonAllStates();

  VirtualAddressSpace::Stack* getStateUserStack();

  void setStateUserStack(VirtualAddressSpace::Stack* st);

  /**
   * Drops descriptors for user stacks whose mappings were removed by exec.
   * The discarded image has already been unmapped, so this must not ask the
   * address space to free those virtual ranges again.
   */
  void discardUserStackMetadataForExec();

  /** Gives the replacement image's initial stack to this Thread's base state. */
  void adoptInitialUserStackForExec(VirtualAddressSpace::Stack* stack);

  /** Returns the state nesting level. */
  size_t getStateLevel() const {
    return __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
  }

  void* getSyscallDispatchContext() const {
    return m_SyscallDispatchContext;
  }
  void setSyscallDispatchContext(void* context) {
    m_SyscallDispatchContext = context;
  }

  /** Allocates a new stack for a specific nesting level, if required */
  void allocateStackAtLevel(size_t stateLevel);

  /** Sets the new kernel stack for the current state level in the TSS */
  void setKernelStack();

  /** Overwrites the state at the given nesting level.
   *\param stateLevel The nesting level to edit.
   *\param state The state to copy.
   */
  void pokeState(size_t stateLevel, SchedulerState& state);

  /** Retrieves a pointer to this Thread's parent process. */
  Process* getParent() const {
    return m_pParent;
  }

  /** Replaces the userspace address cleared when this thread exits. */
  void setClearChildTid(uintptr_t address);

  /** Claims and resets the clear-child-TID registration exactly once. */
  uintptr_t takeClearChildTid() {
    return __atomic_exchange_n(&m_ClearChildTid, static_cast<uintptr_t>(0), __ATOMIC_ACQ_REL);
  }

  void setRobustList(uintptr_t address, size_t ownerId);

  uintptr_t getRobustList() const {
    return __atomic_load_n(&m_RobustList, __ATOMIC_ACQUIRE);
  }

  uintptr_t takeRobustList(size_t& ownerId);

  /** Records this Thread's monotonic entry baseline for one CPU-time mode. */
  void recordTime(CpuTimeMode mode);

  /** Publishes elapsed time from this Thread into its Process aggregate. */
  void trackTime(CpuTimeMode mode);

  /** Accounts one CPU-mode transition from a single monotonic sample. */
  void transitionTime(CpuTimeMode from, CpuTimeMode to,
                      bool interruptsAlreadyDisabled = false);

  /**
   * Accounts the final interrupt return transition while the architecture
   * already has IRQ delivery physically masked.
   */
  void transitionTimeAtInterruptReturn(CpuTimeMode from, CpuTimeMode to);

  /** Current accounting owner; never used to classify interrupt origin. */
  CpuTimeMode currentTimeAccountingMode() const;

#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
  static constexpr size_t NoSyscallTimingSlot = ~static_cast<size_t>(0);

  size_t installSyscallTimingSlot(size_t slot) {
    return __atomic_exchange_n(&m_ActiveSyscallTimingSlot, slot, __ATOMIC_ACQ_REL);
  }

  void restoreSyscallTimingSlot(size_t slot) {
    __atomic_store_n(&m_ActiveSyscallTimingSlot, slot, __ATOMIC_RELEASE);
  }
#endif

  /** Gets CPU time charged specifically to this Thread. */
  Time::Timestamp getUserTime() const {
    return __atomic_load_n(&m_UserTime, __ATOMIC_ACQUIRE);
  }
  Time::Timestamp getKernelTime() const {
    return __atomic_load_n(&m_KernelTime, __ATOMIC_ACQUIRE);
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  /** Publishes a deterministic per-thread accounting batch without sampling a clock. */
  void publishTimeAccountingForHostedTest(Time::Timestamp user, Time::Timestamp system);
#endif

  void setParent(Process* p) {
    m_pParent = p;
  }

  ExecutionPersonality& executionPersonality() {
    return m_ExecutionPersonality;
  }

  /** Retrieves our current status. */
  Status getStatus() const {
    return m_Status;
  }

  /**
   * Starts a thread constructed with delayedStart=true.
   *
   * This is deliberately distinct from waking a blocked thread: only its
   * WaitQueue may perform that transition.
   */
  bool start();

  /**
   * Publishes a delayed worker as detached while retaining its final-use
   * claim. A terminal cancellation which already owns the thread is accepted
   * as successful publication. Success may destroy this Thread before the
   * call returns, so callers must not access it afterward.
   */
  bool startDetached();

  /** Retrieves the exit status of the Thread. */
  int getExitCode() {
    return m_ExitCode;
  }

  /** Retrieves a pointer to the top of the Thread's kernel stack. */
  void* getKernelStack();

  /** Retrieves a pointer to the bottom of the Thread's kernel stack, and its size. */
  void* getKernelStackBase(size_t* size) const;

  /** Returns the Thread's ID. */
  size_t getId() {
    return m_Id;
  }

  /** Linux task IDs share the PID namespace; internal slots remain local. */
  size_t getTaskId() const {
    return __atomic_load_n(&m_TaskId, __ATOMIC_ACQUIRE);
  }

  /** Returns the last error that occurred (errno). */
  size_t getErrno() {
    return m_StateLevels[m_nStateLevel].m_Errno;
  }

  /** Sets the last error - errno. */
  void setErrno(size_t err) {
    m_StateLevels[m_nStateLevel].m_Errno = err;
  }

  enum InterruptionReason {
    NotInterrupted,
    InterruptedByTimeout,
    InterruptedBySignal,
  };

  /** Returns whether the current context's wait was interrupted. */
  bool wasInterrupted() {
    return getInterruptionReason() != NotInterrupted;
  }

  void clearInterruption() {
    m_StateLevels[m_nStateLevel].m_InterruptionReason = NotInterrupted;
  }

  InterruptionReason getInterruptionReason() {
    return m_StateLevels[m_nStateLevel].m_InterruptionReason;
  }

  void setInterruptionReason(InterruptionReason reason) {
    m_StateLevels[m_nStateLevel].m_InterruptionReason = reason;
  }

  /** Marks a timeout in the context interrupted by the current event. */
  void markTimeoutInterruptedWait();

  /** Marks a signal only when its delivery actually interrupted a wait. */
  void markSignalInterruptedWait();

  /** Enum used by the following function. */
  enum UnwindType {
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
  UnwindType getUnwindState() {
    return __atomic_load_n(&m_UnwindState, __ATOMIC_ACQUIRE);
  }
  /** Sets the above unwind state. */
  void setUnwindState(UnwindType ut);

  /** Defers process exit, including its status, to a safe thread boundary. */
  void deferProcessExit(int code);

  /** Defers signal-caused process exit to a safe thread boundary. */
  void deferSignalExit(int signal);

  struct DeferredProcessExit {
    int code;
    Subsystem::ExitCause cause;
  };

  /** Claims the status and cause attached to a deferred process exit. */
  DeferredProcessExit takeDeferredProcessExit();

  /**
   * Publishes a synchronous userspace exception without taking locks or
   * allocating from its raw interrupt frame.
   */
  bool deferSubsystemException(size_t type, uintptr_t faultAddress, uintptr_t errorCode);

  bool hasDeferredSubsystemException() const {
    return __atomic_load_n(&m_DeferredSubsystemExceptionState, __ATOMIC_ACQUIRE) != 0;
  }

  /** Claims a synchronous exception at an IRQ-enabled return boundary. */
  bool takeDeferredSubsystemException(size_t& type, uintptr_t& faultAddress, uintptr_t& errorCode);

  /** True while an explicit terminal-teardown deferral is active. */
  bool isTerminationDeferred() const {
    return __atomic_load_n(&m_TerminationDeferralDepth, __ATOMIC_ACQUIRE) != 0;
  }

  /** Returns the thread's debug state. */
  DebugState getDebugState(uintptr_t& address) {
    for (size_t attempt = 0; attempt < 2; ++attempt) {
      const size_t generation = __atomic_load_n(&m_DebugStateGeneration, __ATOMIC_ACQUIRE);
      if (generation & 1) {
        continue;
      }

      const DebugState state = __atomic_load_n(&m_DebugState, __ATOMIC_RELAXED);
      const uintptr_t observedAddress = __atomic_load_n(&m_DebugStateAddress, __ATOMIC_RELAXED);
      if (generation == __atomic_load_n(&m_DebugStateGeneration, __ATOMIC_ACQUIRE)) {
        address = observedAddress;
        return state;
      }
    }

    address = 0;
    return None;
  }
  /** Sets the thread's debug state. */
  void setDebugState(DebugState state, uintptr_t address) {
    __atomic_add_fetch(&m_DebugStateGeneration, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
    __atomic_store_n(&m_DebugStateAddress, address, __ATOMIC_RELAXED);
    __atomic_store_n(&m_DebugState, state, __ATOMIC_RELAXED);
    __atomic_add_fetch(&m_DebugStateGeneration, static_cast<size_t>(1), __ATOMIC_RELEASE);
  }

  struct WaitDebugInfo {
    WaitQueue* queue;
    const void* channelOwner;
    uintptr_t channelValue;
    WaitQueue::WakeReason reason;
    size_t stateLevel;
    bool queued;
  };

  /** Takes a best-effort snapshot of the active wait for the debugger. */
  bool getWaitDebugInfo(WaitDebugInfo& info);

  /** Returns the thread's scheduler lock. */
  Spinlock& getLock() {
    return m_Lock;
  }

  /** Sends the asynchronous event pEvent to this thread.

      If the thread ID is greater than or equal to EVENT_TID_MAX, the event
     will be ignored.

     The event is not cloned. On success the thread owns a deletable event;
     on failure ownership remains with the caller. */
  bool sendEvent(Event* pEvent);

  /** Blocks until an event is delivered or terminal state is pending. */
  void waitForEvent(WaitQueue::StackDiscardCleanup onStackDiscard = nullptr,
                    void* stackDiscardContext = nullptr);

  /**
   * Blocks for one event unless an armed temporary signal wait was already
   * interrupted. Returns whether a caught signal interrupted that wait.
   */
  bool waitForEventOrSignalInterruption(WaitQueue::StackDiscardCleanup onStackDiscard = nullptr,
                                        void* stackDiscardContext = nullptr);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  enum StateTransitionWindow {
    StatePushBeforePublish,
    StatePushAfterPublish,
    StatePopAfterPublish,
  };
  enum ExternalLeaseReleasePhase {
    ExternalLeaseFinalReleaseUnlocked,
    ExternalLeaseBeforeWaiterWake,
  };
  using StateTransitionHook = void (*)(StateTransitionWindow window, Thread* thread,
                                       size_t previousLevel, size_t nextLevel);
  using JoinOperationHook = void (*)(Thread* target, Process* parent);
  using ExternalLeaseReleaseHook = void (*)(Thread* target, ExternalLeaseReleasePhase phase);
  using DeferredScopeLockHook = void (*)();
  using SignalWaitPreEnrolmentHook = void (*)(Thread* target);

  /** Installs a deterministic observer around state-level publication. */
  static void setStateTransitionHook(StateTransitionHook hook);

  /** Pauses an admitted join after reapability but before final deletion. */
  static void setJoinOperationHook(JoinOperationHook hook);

  /** Observes final external-lease release phases in deterministic hosted tests. */
  static void setExternalLeaseReleaseHookForHostedTest(Thread* target,
                                                       ExternalLeaseReleaseHook hook);

  /** Pauses or injects work immediately before signal-wait enrolment. */
  static void setSignalWaitPreEnrolmentHookForHostedTest(Thread* target,
                                                         SignalWaitPreEnrolmentHook hook);

  /** Exposes scheduler handoff completion to deterministic hosted tests. */
  bool isReapableForHostedTest();

  /** Reports whether delayed-start publication reached runnable state. */
  bool wasStartPublishedForHostedTest();

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

  /** Exercises exec-time user-stack metadata replacement from a nested state. */
  bool runHostedExecStackOwnershipRegression();

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

  /** Saves the pre-wait mask when delivery completes an interrupted masked wait. */
  uint64_t getSignalMaskForReturnFrame();

  /** Transfers a deferred mask restoration into a successfully published frame. */
  void commitSignalHandlerMask(uint64_t mask);

  /** Restores an interrupted wait's mask when no signal frame took ownership. */
  void restoreDeferredSignalMask(size_t stateLevel);

  /** Original syscall image, available only during its normal user-return work. */
  const SyscallState* getOriginalSyscallState() const {
    return m_OriginalSyscallState;
  }

  void setOriginalSyscallState(const SyscallState* state) {
    m_OriginalSyscallState = state;
    if (state) {
      __atomic_fetch_or(&m_UserReturnWorkPending, UserReturnOriginalSyscall, __ATOMIC_RELEASE);
    } else {
      __atomic_fetch_and(&m_UserReturnWorkPending, ~static_cast<size_t>(UserReturnOriginalSyscall),
                         __ATOMIC_RELEASE);
    }
  }

  /** Records trusted metadata for the signal dispatched at the current level. */
  void setCurrentSignalDelivery(size_t signalNumber, size_t continuationEpoch);

  /** Retrieves trusted metadata for the signal dispatched at the current level. */
  bool getCurrentSignalDelivery(size_t& signalNumber, size_t& continuationEpoch);

  /** Carries the effective signal state into a replacement process image. */
  void prepareSignalStateForExec();

  /** True only while the current state owns an interrupted temporary signal wait. */
  bool hasTemporarySignalWaitInterruption();

  /** True while the current state owns a temporary signal mask. */
  bool hasActiveTemporarySignalMask();

  /** Retains an armed temporary-wait signal, or atomically clears stale interruption state. */
  bool retainTemporarySignalWaitInterruptionOrClear();

  struct AlternateSignalStack {
    AlternateSignalStack() : base(0), size(0), enabled(false), inUse(false) {}

    uintptr_t base;
    size_t size;
    bool enabled;
    bool inUse;
  };

  AlternateSignalStack& getAlternateSignalStack() {
    return m_AlternateSignalStack;
  }

  /** Walks the event queue, removing the event \p pEvent , if found. */
  void cullEvent(Event* pEvent);

  /** Walks the event queue, removing the event with number \p eventNumber ,
   * if found. */
  void cullEvent(size_t eventNumber);

  /** Removes only signal events matching \p signalNumber from the queue. */
  void cullSignalEvent(size_t signalNumber);

  /**
   * Moves queued process signals, retaining rejected deliveries on this thread.
   * The caller pins both threads and serialises process signal publication.
   */
  bool transferProcessSignalsTo(Thread& target);

  /**
   * Replaces one queued signal delivery without making it transiently
   * deliverable. On success the Thread owns \p replacement; otherwise the
   * caller retains ownership.
   */
  bool replaceSignalEvent(size_t signalNumber, Event* replacement, int processDirected = -1,
                          uint64_t rebindGeneration = 0);

  /** Filters process/thread provenance when specified; -1 matches either. */
  bool hasSignalEvent(size_t signalNumber, int processDirected = -1);

  bool acceptingEvents();
  uint64_t pendingSignalMask(bool processOnly = false);
  uint64_t pendingSignalOrder(size_t signal, bool processOnly = false);
  Event::Delivery reservePendingSignal(uint64_t mask, bool processOnly = false,
                                       uint64_t expectedSequence = ~uint64_t(0));
  bool restorePendingSignal(Event::Delivery& delivery);
  void cullSignalSource(const void* source);
  void setSynchronousSignalMask(uint64_t mask);
  uint64_t getSynchronousSignalMask();

  bool hasEvents();

  /** Determines if the given event is currently in the event queue. */
  bool hasEvent(Event* pEvent);
  bool hasEvent(size_t eventNumber);

  void setPriority(size_t p) {
    m_Priority = p;
  }
  size_t getPriority() {
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

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  enum TlsResetPhase {
    TlsResetBeforeClear,
    TlsResetCleared,
    TlsResetRemapped,
  };
  using TlsResetHook = void (*)(Thread* thread, TlsResetPhase phase, uintptr_t base);

  /** Observes the protected TLS reset window in deterministic hosted tests. */
  static void setTlsResetHookForHostedTest(Thread* target, TlsResetHook hook);
#endif

  /**
   * Set the TLS base for this thread. Once set, it must be cleaned up by
   * the caller when the thread terminates, which makes this primarily useful
   * for userspace TLS segments. This does not access the supplied memory.
   */
  void setTlsBase(uintptr_t base);

#if X64 && !HOSTED
  uintptr_t getUserGsBase() const {
    return m_UserGsBase;
  }
  void setUserGsBase(uintptr_t base);
  void saveUserGsBase();
#endif

  /** Gets this thread's CPU ID */
  inline
#if MULTIPROCESSOR
      ProcessorId
#else
      size_t
#endif
      getCpuId() {
    return m_ProcId;
  }

  /** Sets this thread's CPU ID */
  inline void setCpuId(
#if MULTIPROCESSOR
      ProcessorId
#else
      size_t
#endif
          id) {
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
  bool detached() const {
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
  bool eventsDeferred() const;

#if PEDIGREE_BENCHMARK_VM_ABLATIONS
  bool benchmarkVmOperationGuardNested() const {
    return m_BenchmarkVmOperationGuardDepth != 0;
  }

  void enterBenchmarkVmOperationGuard() {
    ++m_BenchmarkVmOperationGuardDepth;
  }

  void leaveBenchmarkVmOperationGuard() {
    assert(m_BenchmarkVmOperationGuardDepth);
    --m_BenchmarkVmOperationGuardDepth;
  }

  void enterBenchmarkVmMunmap() {
    ++m_BenchmarkVmMunmapDepth;
  }

  void leaveBenchmarkVmMunmap() {
    assert(m_BenchmarkVmMunmapDepth);
    --m_BenchmarkVmMunmapDepth;
  }

  bool benchmarkVmMunmapActive() const {
    return m_BenchmarkVmMunmapDepth != 0;
  }
#endif

  /** Returns this Thread's explicit logical execution context. */
  ExecutionContext executionContext() const;

  /** Gets the per-processor scheduler for this Thread. */
  class PerProcessorScheduler* getScheduler() const {
    return __atomic_load_n(&m_pScheduler, __ATOMIC_ACQUIRE);
  }

  const String& getName() const {
    return m_Name;
  }

  void setName(const String& name) {
    m_Name = name;
  }

  template <size_t N>
  void setName(const char (&name)[N]) {
    m_Name.assign(name, N);
  }

#if HOSTED
  /** Records a host signal frame on the current physical state stack. */
  size_t enterHostedSignalHandler();

  /** Retires a host signal frame from its captured physical state stack. */
  void leaveHostedSignalHandler(size_t stateLevel);

  size_t getHostedSignalDepth() const {
    const size_t level = __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&m_StateLevels[level].m_HostedSignalDepth, __ATOMIC_ACQUIRE);
  }
#endif

 protected:
  /** Sets the scheduler for the Thread. */
  void setScheduler(class PerProcessorScheduler* pScheduler);

  void deferEvents();
  void resumeEvents();
  void deferTermination();
  void resumeTermination();
  void markUserReturnWorkFlag(UserReturnWorkFlag flag) {
    __atomic_fetch_or(&m_UserReturnWorkPending, static_cast<size_t>(flag), __ATOMIC_RELEASE);
  }
  void clearUserReturnWorkFlag(UserReturnWorkFlag flag) {
    __atomic_fetch_and(&m_UserReturnWorkPending, ~static_cast<size_t>(flag), __ATOMIC_RELEASE);
  }
  void markUserReturnWorkPending() {
    markUserReturnWorkFlag(UserReturnExternalWork);
  }
  void registerDeferredScope(DeferredScopeRecord& record, bool termination, bool events);
  void armStateCleanup(DeferredScopeRecord& record, DeferredScopeRecord::Cleanup cleanup,
                       void* context);
  void unregisterDeferredScope(DeferredScopeRecord& record);
  void disarmStateCleanup(DeferredScopeRecord& record);
  void unregisterTerminationDeferral(DeferredScopeRecord& record);
  void moveTerminationDeferral(DeferredScopeRecord& from, DeferredScopeRecord& to);
  void retireDeferredScopes(bool allStateLevels, size_t stateLevel = 0);
  size_t stateCleanupCheckpoint();
  void retireDeferredScopesAfter(size_t checkpoint);
  void retireDeferredScopesMatching(bool allStateLevels, size_t stateLevel,
                                    bool newerThanCheckpoint, size_t checkpoint);
  void armAtomicStateCleanup(AtomicStateCleanupRecord& record,
                             AtomicStateCleanupRecord::Cleanup cleanup, void* context);
  void disarmAtomicStateCleanup(AtomicStateCleanupRecord& record);

 private:
  /** Kernel-owned start cleanup; unloadable code must use AdmittedThread. */
  Thread(Process* pParent, ThreadStartFunc pStartFunction, void* pParam, void* pStack,
         bool semiUser, bool bDontPickCore, bool delayedStart, ThreadStartCleanup startCleanup,
         const ThreadPlacement* placement = nullptr);

  /** Copy-constructor */
  Thread(const Thread&);
  /** Assignment operator */
  Thread& operator=(const Thread&);

  /** Adds one elapsed interval to this Thread and its Process aggregate. */
  void publishTimeAccounting(CpuTimeMode mode, Time::Timestamp elapsed);

  void initialisePlacement(const ThreadPlacement* placement);
  void publishReadyNotification();

  /** Cleans up the given state level. */
  void cleanStateLevel(size_t level);

  size_t beginTemporarySignalMask(uint64_t signalMask);
  bool finishTemporarySignalMask(size_t stateLevel, bool deferForUserReturn = true);
  bool waitForEventInternal(bool stopOnSignalInterruption,
                            WaitQueue::StackDiscardCleanup onStackDiscard,
                            void* stackDiscardContext);

  enum class EventSelection {
    AnyDeliverable,
    WithoutExactUserReturn,
    KernelDeliverable,
    StoppedProcessKernel,
  };

  /** Checks one event against masks and process state while m_Lock is held. */
  bool eventNeedsUserReturnFrameUnlocked(Event* event) const;
  bool eventIsDeliverableUnlocked(Event* event,
                                  EventSelection selection = EventSelection::AnyDeliverable);

  /** Checks for an event that can run while m_Lock is already held. */
  bool hasEventsUnlocked(EventSelection selection = EventSelection::AnyDeliverable);
  bool hasDeliverableEventsUnlocked(EventSelection selection = EventSelection::AnyDeliverable);

  /** Marks a caught signal deferred until an exact user-return boundary. */
  void markDeferredUserReturnSignalInterruption();

  /** Rechecks queued events after the parent process becomes runnable. */
  void wakeForDeliverableEvents();

  /** Scheduler-only dequeue that retains the Event delivery registration. */
  MUST_USE_RESULT Event::Delivery getNextEvent(
      EventSelection selection = EventSelection::AnyDeliverable);

  /** Scheduler-only status transition primitive. */
  void setStatus(Status s);
  void setStatusUnlocked(Status s);

  /**
   * Completes exit after the scheduler has switched away from our stack.
   * Returns true when a concurrent detach made immediate deletion safe.
   */
  bool markReapable();

  /** Implements ordinary and completion-safe exclusive joins. */
  bool joinInternal(bool completion);

  struct JoinDiscardContext {
    Thread* target;
    Process* parent;
    bool claimed;
  };

  /** Releases a join claim if an exceptional stack discard cannot return. */
  static void discardJoin(void* context);

  /** Admits a Process::ThreadLease while this object remains discoverable. */
  bool beginExternalLease();

  /** Releases a successful external lease admission. */
  void endExternalLease();

  /** Prevents any further Process::ThreadLease acquisition. */
  void closeExternalLeaseAdmission();

  /** Closes external admission and drains every previously admitted lease. */
  void closeExternalLeaseAdmissionAndDrain();

  /** Interrupts the active wait at the current event nesting level. */
  bool interruptWaitUnlocked(WaitQueue::WakeReason reason, PerProcessorScheduler*& readyScheduler);
  bool hasActiveWaitUnlocked() const;
  bool hasActiveWaitAtAnyLevel() const;
  bool activeWaitPendingUnlocked() const;

  /** Unpublishes every waiter before exceptional stack cleanup can run. */
  void unlinkWaitsForStackDiscard();

  /**
   * A terminal cancel can unlink a published waiter before its owner has
   * entered the scheduler sleep transition. These helpers carry that handoff
   * under m_Lock without leaving a stale waiter visible to future sleeps.
   */
  void markTerminalWaitCancelledBeforeBlockUnlocked(size_t level);
  bool consumeTerminalWaitCancelledBeforeBlockUnlocked();
  void clearTerminalWaitCancelledBeforeBlockUnlocked(size_t level);

  /** A level of thread state */
  struct StateLevel {
    StateLevel();
    ~StateLevel();

    StateLevel(const StateLevel& s);
    StateLevel& operator=(const StateLevel& s);

    /** The processor state for this level. */
    SchedulerState* m_State;
    UserReturnFrame* m_UserReturnFrame = nullptr;

    /** Our kernel stack. */
    VirtualAddressSpace::Stack* m_pKernelStack;

    VirtualAddressSpace::Stack* m_pUserStack;

    /** Auxillary stack, to be freed in case the kernel stack is null.
     *  This allows kernel mode threads to have stacks freed, as they
     *  are left hanging otherwise.
     */
    VirtualAddressSpace::Stack* m_pAuxillaryStack;

    /** Stack of inhibited Event masks, gets pushed with a new value when an
       Event handler is run, and popped when one completes.

        \note A '1' here means the event is inhibited, '0' means it can be
       fired. */
    SharedPointer<ExtensibleBitmap> m_InhibitMask;

    /** POSIX signals blocked at this event nesting level. */
    uint64_t m_SignalMask;

    /** Original mask and interruption state for an armed signal-aware wait. */
    uint64_t m_SavedSignalMask;
    bool m_TemporarySignalMaskActive;
    bool m_TemporarySignalWaitInterrupted;
    bool m_DeferredSignalMaskRestore;

    /** Kernel-owned metadata for the signal dispatched at this level. */
    size_t m_DispatchedSignalNumber;
    size_t m_DispatchedSignalContinuationEpoch;

    /** This event level owns the thread's active alternate signal stack. */
    bool m_bOwnsAlternateSignalStack;

    /** Syscall-local state isolated from nested event handlers. */
    size_t m_Errno;
    InterruptionReason m_InterruptionReason;

    /** Event dispatch at this level was initiated by a WaitQueue wake. */
    bool m_bDispatchingWaitEvent;

    /** Logical context scopes belong to the Thread state they interrupted. */
    ExecutionContextState m_ExecutionContext;

    /** Active RequestQueue callback scopes inherited by nested events. */
    RequestQueueCallbackScope* m_pRequestQueueCallback;

    /**
     * A terminal cancel unlinked this level's waiter while its owner was
     * still running toward the scheduler sleep transition. Protected by
     * Thread::m_Lock.
     */
    bool m_bTerminalWaitCancelledBeforeBlock;

#if HOSTED
    /** Host signal frames physically suspended on this state's stack. */
    size_t m_HostedSignalDepth;
#endif

    /** Persistent wait record for this event nesting level. */
    WaitQueue::Waiter m_Waiter;
  };

  /** An optional name for the thread for debugging. */
  String m_Name;

  /** The current index into m_States (head of the state stack). */
  size_t m_nStateLevel = 0;

  /** Our parent process. */
  Process* m_pParent = nullptr;
  // VFS uses task-local filesystem IDs even when the POSIX identity is shared.
  uint32_t m_FilesystemUid = 0, m_FilesystemGid = 0;
  bool m_FilesystemIdsValid = false;
  const FilesystemCredentials* m_FilesystemOverride = nullptr;

  /** Allocation-free publication storage for scheduler-side retirement. */
  DeferredThreadReapNode m_DeferredReapNode;

  /** Per-thread baselines avoid cross-CPU corruption within one Process. */
  ThreadTimeAccounting m_TimeAccounting;

  /** CPU time charged to this Thread, split by user and kernel mode. */
  Time::Timestamp m_UserTime = 0;
  Time::Timestamp m_KernelTime = 0;

  /** Mode owning time since the most recent accounting baseline. */
  size_t m_CurrentTimeAccountingMode = static_cast<size_t>(CpuTimeMode::Kernel);

#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
  size_t m_ActiveSyscallTimingSlot = NoSyscallTimingSlot;
#endif

  /** The stack that we allocated from the VMM. This may or may not also be
      the kernel stack - depends on whether we are a user or kernel mode
      thread. This is used solely for housekeeping/cleaning up purposes. */
  void* m_pAllocatedStack = nullptr;

  /** Our thread ID. */
  size_t m_Id = 0;
  size_t m_TaskId = 0;

  /** Address to supplement the DebugState information */
  uintptr_t m_DebugStateAddress = 0;

  /** Even while the single-writer detached debug state is stable. */
  size_t m_DebugStateGeneration = 0;

  class PerProcessorScheduler* m_pScheduler = nullptr;

  ThreadPlacement m_Placement;
  CpuAffinityMask m_RequestedAffinity;
  WaitQueue m_AffinityWaiters;
  uint64_t m_AffinityGeneration = 0;
  uint64_t m_AffinityCompleted = 0;
  bool m_AffinityPending = false;
  bool m_AffinityGatePending = false;
  bool m_AffinityWorkQueued = false;
  size_t m_AffinityReturnPending = 0;
  size_t m_LegacyUserCallbackPins = 0;
  bool m_SignalFramesRequired = false;
  bool m_UserReturnSignalParked = false;
  size_t m_UserReturnWorkPending = 0;
  Thread* m_AffinityNext = nullptr;
  bool m_HasSchedulerContext = false;
  bool m_ReadyPublicationPending = false;

  /** Thread priority: 0..MAX_PRIORITIES-1, 0 being highest. */
  size_t m_Priority = DEFAULT_PRIORITY;

  /** Intrusive ready-queue state; scheduler publication must not allocate. */
  Thread* m_pReadyPrevious = nullptr;
  Thread* m_pReadyNext = nullptr;
  size_t m_ReadyQueuePriority = MAX_PRIORITIES;
  bool m_bReadyQueued = false;

  /** Memory mapping for the TLS base of this thread (userspace-only) */
  VirtualAddressSpace::Stack* m_pInputUserStack = nullptr;
  void* m_pTlsBase = nullptr;
#if X64 && !HOSTED
  uintptr_t m_UserGsBase = 0;
#endif

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
  List<Event*> m_EventQueue;

  /** sendEvent calls admitted before shutdown closed the event queue. */
  size_t m_EventSendersInFlight = 0;

  StateLevel m_StateLevels[MAX_NESTED_EVENTS];

  /** Alternate signal stack configuration is per-thread. */
  AlternateSignalStack m_AlternateSignalStack;

  const SyscallState* m_OriginalSyscallState = nullptr;
  void* m_SyscallDispatchContext = nullptr;

  /** Our current status. Sleeping is reserved for an active WaitQueue. */
  volatile Status m_Status = Ready;
  ExecutionPersonality m_ExecutionPersonality;

  /** One-shot terminal handoff used by the orderly shutdown coordinator. */
  bool m_ExitToIdle = false;

  /** Our exit code. */
  int m_ExitCode = 0;

  /** Debug state - a higher level state information for display in the
   * debugger for debugging races and deadlocks. */
  DebugState m_DebugState = None;

  UnwindType m_UnwindState = Continue;

  /** Status preserved while Exit crosses nested/event/IRQ boundaries. */
  uint64_t m_DeferredProcessExitRequest = 0;
  uint64_t m_SynchronousSignalMask = 0;

  /** Empty, publishing, or pending state for the preallocated exception. */
  size_t m_DeferredSubsystemExceptionState = 0;
  size_t m_DeferredSubsystemExceptionType = 0;
  uintptr_t m_DeferredSubsystemExceptionFaultAddress = 0;
  uintptr_t m_DeferredSubsystemExceptionErrorCode = 0;

  /** Whether or not userspace has overridden its TLS base. */
  bool m_bTlsBaseOverride = false;

  /** Whether shutdown() has completed its one-way transition. */
  bool m_bShutdown = false;

  /** Process teardown can notify before shutdown retires the final thread. */
  bool m_bSubsystemExitNotified = false;

  /** Userspace TID word cleared by the subsystem during exit publication. */
  uintptr_t m_ClearChildTid = 0;

  /** The Linux robust-list head is registered independently by each thread. */
  uintptr_t m_RobustList = 0;
  size_t m_RobustListOwnerId = 0;

  /** The add worker may publish a delayed thread after this request. */
  bool m_bStartRequested = false;

  /** Whether this thread has been detached or not. */
  bool m_bDetached = false;

  /** Whether join() has been claimed by another thread. */
  bool m_bJoinClaimed = false;

  /** Exactly one path owns detached Thread destruction. */
  bool m_bDetachedRetirementClaimed = false;

  /** One-way syscall exit intent, protected by the parent Process lock. */
  bool m_bThreadExitRequested = false;

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

#if PEDIGREE_BENCHMARK_VM_ABLATIONS
  size_t m_BenchmarkVmOperationGuardDepth = 0;
  size_t m_BenchmarkVmMunmapDepth = 0;
#endif

  /** Nesting depth for scopes which must run cleanup before teardown. */
  size_t m_TerminationDeferralDepth = 0;

  /** Per-state LIFO of stack records which teardown must retire explicitly. */
  DeferredScopeRecord* m_pDeferredScopes[MAX_NESTED_EVENTS] = {};
  size_t m_NextStateCleanupSequence = 0;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  Spinlock m_DeferredScopeRegressionLock;
#endif
};

#endif
