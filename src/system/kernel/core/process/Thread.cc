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

#include <config.h>

#if THREADS

#include "pedigree/kernel/ActivityDiagnostics.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/ProcessorThreadAllocator.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/process/eventNumbers.h"
#include "pedigree/kernel/processor/NMFaultHandler.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/MemoryAllocator.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

Thread::StackDiscardScope::StackDiscardScope(DeferredScopeRecord::Cleanup cleanup, void* context)
    : m_pThread(cleanup ? Processor::information().getCurrentThread() : nullptr), m_Record() {
  if (cleanup && !m_pThread) {
    FATAL("StackDiscardScope cleanup has no current Thread.");
  }
  if (m_pThread) {
    m_pThread->armStateCleanup(m_Record, cleanup, context);
  }
}

Thread::StackDiscardScope::~StackDiscardScope() {
  disarm();
}

void Thread::StackDiscardScope::disarm() {
  if (m_pThread) {
    m_pThread->disarmStateCleanup(m_Record);
    m_pThread = nullptr;
  }
}

Thread::TemporarySignalMask::TemporarySignalMask(Thread& thread, uint64_t signalMask)
    : m_pThread(&thread), m_StateLevel(0), m_Record() {
  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  m_StateLevel = thread.beginTemporarySignalMask(signalMask);
  thread.armStateCleanup(m_Record, &TemporarySignalMask::discard, this);
  Processor::setInterrupts(interruptsWereEnabled);
}

Thread::TemporarySignalMask::~TemporarySignalMask() {
  if (m_pThread) {
    finish();
  }
}

bool Thread::TemporarySignalMask::finish() {
  if (!m_pThread) {
    FATAL("Temporary signal mask restored more than once.");
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  const bool interrupted = m_pThread->finishTemporarySignalMask(m_StateLevel);
  m_pThread->disarmStateCleanup(m_Record);
  m_pThread = nullptr;
  Processor::setInterrupts(interruptsWereEnabled);
  return interrupted;
}

void Thread::TemporarySignalMask::discard(void* context) {
  TemporarySignalMask* scope = reinterpret_cast<TemporarySignalMask*>(context);
  if (!scope || !scope->m_pThread) {
    FATAL("Invalid temporary signal mask discard.");
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  scope->m_pThread->finishTemporarySignalMask(scope->m_StateLevel, false);
  scope->m_pThread = nullptr;
  Processor::setInterrupts(interruptsWereEnabled);
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
Thread::StateTransitionHook g_StateTransitionHook = nullptr;
Thread::JoinOperationHook g_JoinOperationHook = nullptr;
Thread::ExternalLeaseReleaseHook g_ExternalLeaseReleaseHook = nullptr;
Thread* g_ExternalLeaseReleaseTarget = nullptr;
Thread::SignalWaitPreEnrolmentHook g_SignalWaitPreEnrolmentHook = nullptr;
Thread* g_SignalWaitPreEnrolmentTarget = nullptr;
Thread::TlsResetHook g_TlsResetHook = nullptr;
Thread* g_TlsResetTarget = nullptr;
using EventAdmissionHook = void (*)(Thread*);
EventAdmissionHook g_EventAdmissionHook = nullptr;
Thread* g_EventAdmissionTarget = nullptr;

struct HostedStateCleanupOrder {
  size_t values[8] = {};
  size_t count = 0;
};

struct HostedStateCleanupItem {
  HostedStateCleanupOrder* order;
  size_t value;
};

void hostedStateCleanupCallback(void* context) {
  HostedStateCleanupItem* item = reinterpret_cast<HostedStateCleanupItem*>(context);
  if (item && item->order && item->order->count < 8) {
    item->order->values[item->order->count++] = item->value;
  }
}

void observeStateTransition(Thread::StateTransitionWindow window, Thread* thread,
                            size_t previousLevel, size_t nextLevel) {
  Thread::StateTransitionHook hook = __atomic_load_n(&g_StateTransitionHook, __ATOMIC_ACQUIRE);
  if (hook) {
    hook(window, thread, previousLevel, nextLevel);
  }
}
}  // namespace
#endif

namespace {
#if HOSTED
// The raw userspace return path needs one stack for its ordinary tail and a
// second for the first kernel Event it may dispatch. Deeper Events are already
// ordinary thread work and retain the existing lazy allocation behaviour.
constexpr size_t HostedPreallocatedUserStateLevel = 2;
static_assert(MAX_NESTED_EVENTS > HostedPreallocatedUserStateLevel,
              "Hosted return-tail state levels exceed the Thread nesting limit");
#endif

void requireThreadDestructionContext() {
  if (Processor::executionContext() != ExecutionContext::WaitableThread ||
      !Processor::getInterrupts()) {
    FATAL_NOLOCK("Thread destruction requires an IRQ-enabled WaitableThread boundary.");
  }
}

class CpuTimeSample {
 public:
  explicit CpuTimeSample(bool interruptsAlreadyDisabled = false)
      : timestamp(0), processor(0), m_InterruptsWereEnabled(false),
        m_RestoreInterrupts(!interruptsAlreadyDisabled) {
    if (!interruptsAlreadyDisabled) {
      m_InterruptsWereEnabled = Processor::getInterrupts();
      Processor::setInterrupts(false);
    }

    // Keep interrupts masked until the paired baseline/publication update
    // is complete, so migration cannot invalidate this CPU-clock sample.
    const auto sample = Time::sampleCpuTime();
    processor = sample.processor;
    timestamp = sample.timestamp;
  }

  ~CpuTimeSample() {
    if (m_RestoreInterrupts) {
      Processor::setInterrupts(m_InterruptsWereEnabled);
    }
  }

  Time::Timestamp timestamp;
  size_t processor;

 private:
  bool m_InterruptsWereEnabled;
  bool m_RestoreInterrupts;
  ActivityDiagnostics::TimeAccountingScope m_ActivityScope;
};
}  // namespace

Thread::Thread(Process* pParent, ThreadStartFunc pStartFunction, void* pParam, void* pStack,
               bool semiUser, bool bDontPickCore, bool delayedStart,
               const ThreadPlacement* placement)
    : Thread(pParent, pStartFunction, pParam, pStack, semiUser, bDontPickCore, delayedStart,
             nullptr, placement) {}

Thread::Thread(Process* pParent, ThreadStartFunc pStartFunction, void* pParam, void* pStack,
               bool semiUser, bool bDontPickCore, bool delayedStart,
               ThreadStartCleanup startCleanup, const ThreadPlacement* placement)
    : m_pParent(pParent), m_DeferredReapNode(this) {
  if (pParent == 0) {
    FATAL("Thread::Thread(): Parent process was NULL!");
  }
  initialisePlacement(placement);

  // Initialise our kernel stack.
  m_pAllocatedStack = 0;

  // Initialise state level zero
  m_StateLevels[0].m_pAuxillaryStack = 0;
  allocateStackAtLevel(0);

  Thread* pCurrent = Processor::information().getCurrentThread();
  if (pCurrent && pCurrent->getParent() == pParent) {
    m_StateLevels[0].m_SignalMask = pCurrent->m_StateLevels[pCurrent->m_nStateLevel].m_SignalMask;
  }

  // If we've been given a user stack pointer, we are a user mode thread.
  bool bUserMode = true;
  void* requestedStack = pStack;
  if (pStack == 0) {
    bUserMode = false;
    VirtualAddressSpace::Stack* kernelStack = m_StateLevels[0].m_pAuxillaryStack =
        m_StateLevels[0].m_pKernelStack;
    m_StateLevels[0].m_pKernelStack = 0;

    if (kernelStack)
      pStack = kernelStack->getTop();
  }

  if (semiUser) {
    // Still have a kernel stack for when we jump to user mode, but start
    // the thread in kernel mode first.
    bUserMode = false;

    // If no stack was given and we allocated, extract that allocated stack
    // back out again so we have a kernel stack proper.
    if (!requestedStack) {
      m_StateLevels[0].m_pKernelStack = m_StateLevels[0].m_pAuxillaryStack;
    }
  }

#if HOSTED
  if (requestedStack || semiUser) {
    // A hosted user IRQ reaches its return tail with allocation forbidden.
    // Guarantee the tail and its first Event stack before the Thread is
    // visible to the scheduler without reserving every possible nesting.
    for (size_t level = 1; level <= HostedPreallocatedUserStateLevel; ++level) {
      allocateStackAtLevel(level);
      m_StateLevels[level].m_InhibitMask = m_StateLevels[0].m_InhibitMask;
    }
  }
#endif

  m_pParent->inheritFilesystemIds(
      *this, pCurrent && pCurrent->getParent() == pParent ? pCurrent : nullptr);
  m_Id = m_pParent->addThread(this);

  // Firstly, grab our lock so that the scheduler cannot preemptively load
  // balance us while we're starting.
  m_Lock.acquire();

  if (delayedStart || getUnwindState() == Thread::TerminateThread) {
    m_Status = Created;
  }

  // Add to the scheduler
  if (!bDontPickCore || placement) {
    ProcessorThreadAllocator::instance().addThread(this, pStartFunction, pParam, bUserMode, pStack,
                                                   startCleanup);
  } else {
    Scheduler::instance().addThread(this, Processor::information().getScheduler());
    Processor::information().getScheduler().addThread(this, pStartFunction, pParam, bUserMode,
                                                      pStack, startCleanup);
  }
}

Thread::Thread(Process* pParent)
    : m_pParent(pParent),
      m_DeferredReapNode(this),
      m_pScheduler(&Processor::information().getScheduler()) {
  if (pParent == 0) {
    FATAL("Thread::Thread(): Parent process was NULL!");
  }
  m_pParent->inheritFilesystemIds(*this, nullptr);
  m_Id = m_pParent->addThread(this);

  // Initialise our kernel stack.
  // Kernel-mode threads use the auxiliary stack allocated above.

  // Still add the idle thread to the Scheduler for things like
  // threadInSchedule
  Scheduler::instance().addThread(this, *m_pScheduler);
}

Thread::Thread(Process* pParent, SyscallState& state, bool delayedStart,
               const ThreadPlacement* placement)
    : m_pParent(pParent), m_DeferredReapNode(this) {
  if (pParent == 0) {
    FATAL("Thread::Thread(): Parent process was NULL!");
  }
  initialisePlacement(placement);

  // Initialise our kernel stack.
  // m_pKernelStack =
  // VirtualAddressSpace::getKernelAddressSpace().allocateStack();
  m_pAllocatedStack = 0;

  // Initialise state level zero
  allocateStackAtLevel(0);
#if HOSTED
  for (size_t level = 1; level <= HostedPreallocatedUserStateLevel; ++level) {
    allocateStackAtLevel(level);
    m_StateLevels[level].m_InhibitMask = m_StateLevels[0].m_InhibitMask;
  }
#endif

  Thread* pCurrent = Processor::information().getCurrentThread();
  if (pCurrent) {
    m_StateLevels[0].m_SignalMask = pCurrent->m_StateLevels[pCurrent->m_nStateLevel].m_SignalMask;

    // A forked process inherits its alternate signal stack. A new thread
    // sharing the same process starts with the stack disabled.
    if (pCurrent->getParent() != pParent) {
      m_AlternateSignalStack = pCurrent->m_AlternateSignalStack;
    }

#if X64
    NMFaultHandler::inheritCurrentThreadFpuState(this);
#endif
  }

  m_pParent->inheritFilesystemIds(*this, pCurrent);
  m_Id = m_pParent->addThread(this);

  // SyscallState variant has to be called from the parent thread, so this is
  // OK to do.
  if (pCurrent && pCurrent->m_bTlsBaseOverride) {
    // Override our TLS base too (but this will be in the copied address
    // space).
    m_bTlsBaseOverride = true;
    m_pTlsBase = pCurrent->m_pTlsBase;
  }
#if X64 && !HOSTED
  m_UserGsBase = state.getUserEntryMetadata().gsBase;
#endif

  m_Lock.acquire();

  if (delayedStart || getUnwindState() == Thread::TerminateThread) {
    m_Status = Created;
  }

  // Now we are ready to go into the scheduler.
  ProcessorThreadAllocator::instance().addThread(this, state);
}

void Thread::recordTime(CpuTimeMode mode) {
#if PEDIGREE_TIME_ACCOUNTING && !PEDIGREE_SAMPLED_TIME_ACCOUNTING
  const CpuTimeSample sample;
  m_TimeAccounting.recordAtInterruptDisabled(mode, sample.timestamp, sample.processor);
#endif
  __atomic_store_n(&m_CurrentTimeAccountingMode, static_cast<size_t>(mode), __ATOMIC_RELEASE);
}

void Thread::trackTime(CpuTimeMode mode) {
#if PEDIGREE_TIME_ACCOUNTING && !PEDIGREE_SAMPLED_TIME_ACCOUNTING
  const CpuTimeSample sample;
  const Time::Timestamp elapsed =
      m_TimeAccounting.elapsedAtInterruptDisabled(mode, sample.timestamp, sample.processor);
  if (elapsed) {
    publishTimeAccounting(mode, elapsed, sample.processor);
  }
#else
  (void)mode;
#endif
}

void Thread::transitionTime(CpuTimeMode from, CpuTimeMode to, bool interruptsAlreadyDisabled) {
#if PEDIGREE_TIME_ACCOUNTING && !PEDIGREE_SAMPLED_TIME_ACCOUNTING
  const CpuTimeSample sample(interruptsAlreadyDisabled);
  const Time::Timestamp elapsed =
      m_TimeAccounting.elapsedAtInterruptDisabled(from, sample.timestamp, sample.processor);
  m_TimeAccounting.recordAtInterruptDisabled(to, sample.timestamp, sample.processor);
  __atomic_store_n(&m_CurrentTimeAccountingMode, static_cast<size_t>(to), __ATOMIC_RELEASE);
  if (elapsed) {
    publishTimeAccounting(from, elapsed, sample.processor);
  }
#else
  (void)from;
  (void)interruptsAlreadyDisabled;
  __atomic_store_n(&m_CurrentTimeAccountingMode, static_cast<size_t>(to), __ATOMIC_RELEASE);
#endif
}

void Thread::accountTimerTick(Time::Timestamp delta, bool kernelMode) {
#if PEDIGREE_TIME_ACCOUNTING && PEDIGREE_SAMPLED_TIME_ACCOUNTING
  // Interrupt entry has already changed the logical mode. Only the saved
  // frame tells us which mode was running when the timer arrived.
  if (delta && m_pParent) {
    publishTimeAccounting(kernelMode ? CpuTimeMode::Kernel : CpuTimeMode::User, delta,
                          Processor::index());
  }
#else
  (void)delta;
  (void)kernelMode;
#endif
}

void Thread::publishTimeAccounting(CpuTimeMode mode, Time::Timestamp elapsed, size_t processor) {
  Time::Timestamp* total = mode == CpuTimeMode::User ? &m_UserTime : &m_KernelTime;
#if X64
  // IRQ masking and scheduler ownership exclude writers on other CPUs. Keep
  // this one instruction so an NMI cannot interleave a load/add/store sequence.
  asm volatile("addq %1, %0" : "+m"(*total) : "r"(elapsed) : "cc");
#else
  __atomic_fetch_add(total, elapsed, __ATOMIC_RELAXED);
#endif
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
  if (mode == CpuTimeMode::Kernel) {
    const size_t slot = __atomic_load_n(&m_ActiveSyscallTimingSlot, __ATOMIC_ACQUIRE);
    if (slot != NoSyscallTimingSlot) {
      m_pParent->recordSyscallTimingKernel(slot, elapsed);
    }
  }
#endif
  m_pParent->publishTimeAccounting(mode, elapsed, processor);
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Thread::publishTimeAccountingForHostedTest(Time::Timestamp user, Time::Timestamp system) {
  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  const size_t processor = Processor::index();
  publishTimeAccounting(CpuTimeMode::User, user, processor);
  publishTimeAccounting(CpuTimeMode::Kernel, system, processor);
  Processor::setInterrupts(interruptsWereEnabled);
}
#endif

CpuTimeMode Thread::currentTimeAccountingMode() const {
  return static_cast<CpuTimeMode>(__atomic_load_n(&m_CurrentTimeAccountingMode, __ATOMIC_ACQUIRE));
}

Thread::~Thread() {
  {
    LockGuard<Spinlock> leaseGuard(m_ExternalLeaseLock);
    if (!m_bExternalLeaseAdmissionClosed || m_nExternalLeases ||
        m_bExternalLeaseReleaseInProgress) {
      FATAL(
          "Thread destroyed before external leases were closed and "
          "drained.");
    }
  }

  for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level) {
    if (__atomic_load_n(&m_pDeferredScopes[level], __ATOMIC_ACQUIRE)) {
      FATAL("Thread destroyed with armed state cleanup records.");
    }
  }
  if (__atomic_load_n(&m_TerminationDeferralDepth, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&m_EventDeferralDepth, __ATOMIC_ACQUIRE)) {
    FATAL("Thread destroyed with active deferral scopes: terminal="
          << Dec << __atomic_load_n(&m_TerminationDeferralDepth, __ATOMIC_ACQUIRE)
          << ", event=" << __atomic_load_n(&m_EventDeferralDepth, __ATOMIC_ACQUIRE) << ".");
  }

  if (InputManager::instance().removeCallbackByThread(this)) {
    WARNING(
        "A thread is being removed, but it never removed itself from "
        "InputManager.");
    WARNING("This warning indicates an application or kernel module is buggy!");
  }

  // Before removing from the scheduler, terminate if needed.
  if (!m_bShutdown) {
    shutdown();
  }

  // Clean up allocated stacks at each level.
  for (size_t i = 0; i < MAX_NESTED_EVENTS; i++) {
    cleanStateLevel(i);
  }

  // Clean up TLS base.
  if (m_pTlsBase && m_pParent && !m_bTlsBaseOverride) {
    // Unmap the TLS base.
    if (m_pParent->getAddressSpace()->isMapped(m_pTlsBase)) {
      physical_uintptr_t phys = 0;
      size_t flags = 0;
      m_pParent->getAddressSpace()->getMapping(m_pTlsBase, phys, flags);
      m_pParent->getAddressSpace()->unmap(m_pTlsBase);
      PhysicalMemoryManager::instance().freePage(phys);
    }

    // Give the address space back to the process.
    uintptr_t base = reinterpret_cast<uintptr_t>(m_pTlsBase);
    if (m_pParent->getAddressSpace()->getDynamicStart())
      m_pParent->freeUserRange(Process::UserRegion::Dynamic, base, THREAD_TLS_SIZE);
    else
      m_pParent->freeUserRange(Process::UserRegion::Normal, base, THREAD_TLS_SIZE);
  } else if (m_pTlsBase && !m_bTlsBaseOverride) {
    ERROR("Thread: no parent, but a TLS base exists.");
  }

  // Remove us from the scheduler.
  Scheduler::instance().removeThread(this);

  EMIT_IF(X86_COMMON) {
    // Make sure the floating-point fault handler doesn't care about us anymore
    NMFaultHandler::instance().threadTerminated(this);
  }

  if (m_pParent)
    m_pParent->removeThread(this);
}

void Thread::notifySubsystemExit() {
  // The final process owner calls this before mapping teardown. Its later
  // shutdown may hold the Process lock, where repeating blocking hooks is unsafe.
  if (__atomic_exchange_n(&m_bSubsystemExitNotified, true, __ATOMIC_ACQ_REL)) {
    return;
  }
  if (m_pParent) {
    m_pParent->threadExiting(this);
  }
  retireInputUserStack();
  // Robust-list and clear-TID work above still needs the departing user stack.
  // Retire owned raw stacks here, before a later scheduler-locked destruction.
  for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level) {
    VirtualAddressSpace::Stack* stack = m_StateLevels[level].m_pUserStack;
    if (!stack || !stack->regionId() || !m_pParent)
      continue;
    for (size_t other = level; other < MAX_NESTED_EVENTS; ++other) {
      if (m_StateLevels[other].m_pUserStack == stack)
        m_StateLevels[other].m_pUserStack = nullptr;
    }
    m_pParent->getAddressSpace()->freeStack(stack);
  }
}

bool Thread::prepareInputUserStack() {
  if (Processor::information().getCurrentThread() != this || !m_pParent ||
      !Processor::getInterrupts())
    return false;
  Uninterruptible events;
  TerminationDeferral termination;
  if (m_pInputUserStack)
    return true;
  if (!acceptingEvents() || !tryPinLegacyUserCallbacks())
    return false;
  m_pInputUserStack = m_pParent->getAddressSpace()->allocateStack();
  if (!m_pInputUserStack)
    unpinLegacyUserCallbacks();
  return m_pInputUserStack != nullptr;
}

void Thread::retireInputUserStack() {
  // Never-used and already-retired threads also reach shutdown under scheduler
  // locks. Their fast path must not enter the input or mapping gates.
  if (!m_pInputUserStack)
    return;
  Uninterruptible events;
  TerminationDeferral termination;
  InputManager::instance().removeCallbackByThread(this);
  while (true) {
    Event* removed = nullptr;
    {
      LockGuard<Spinlock> guard(m_Lock);
      for (auto it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
        if (!(*it)->isSignalEvent() && (*it)->getNumber() == EventNumbers::InputEvent) {
          removed = *it;
          m_EventQueue.erase(it);
          break;
        }
      }
    }
    if (!removed)
      break;
    removed->completeDelivery(this);
  }
  VirtualAddressSpace::Stack* stack = m_pInputUserStack;
  m_pInputUserStack = nullptr;
  for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level) {
    if (m_StateLevels[level].m_pUserStack == stack)
      m_StateLevels[level].m_pUserStack = nullptr;
  }
  if (m_pParent)
    m_pParent->getAddressSpace()->freeStack(stack);
  else
    delete stack;
  // Only committed exec and terminal teardown retire this domain. An old
  // nested callback cannot resume; exec reaches its migration gate only after
  // abandoning those states. Removing a public registration is not sufficient.
  unpinLegacyUserCallbacks();
}

void Thread::shutdown() {
  {
    auto senderGuard = m_EventSenderDrainWaiters.acquire();
    LockGuard<Spinlock> guard(m_Lock);
    if (m_bShutdown) {
      return;
    }
    m_bShutdown = true;
  }

  // Admission and this predicate share one WaitQueue guard. A sender which
  // passed the shutdown check therefore either releases its pin before this
  // check or publishes a wake after this waiter is visible.
  while (true) {
    auto senderGuard = m_EventSenderDrainWaiters.acquire();
    {
      LockGuard<Spinlock> guard(m_Lock);
      if (!m_EventSendersInFlight) {
        break;
      }
    }

    const WaitQueue::WakeReason reason = senderGuard.waitForCompletion(
        WaitQueue::Channel(this), Thread::EventWait, reinterpret_cast<uintptr_t>(this));
    (void)reason;
  }

  unlinkWaitsForStackDiscard();

  if (m_pParent) {
    m_pParent->transferExecProcessSignals(this);
  }

  // Once shutdown is visible, no sender can publish another event. Remove
  // each queued registration under the thread lock, but complete it outside
  // the lock because completion can wake waiters or destroy the Event.
  while (true) {
    Event* event = nullptr;
    {
      LockGuard<Spinlock> guard(m_Lock);
      if (!m_EventQueue.count()) {
        break;
      }
      event = m_EventQueue.popFront();
    }

    event->completeDelivery(this);
  }

  // Subsystem teardown must happen while the Process and address space are
  // still live, and before a joiner can observe this exit.
  notifySubsystemExit();

  // Make a joiner runnable before the scheduler chooses our replacement.
  // This matters during shutdown after the idle thread has been retired.
  // join() still checks m_bReapable and cannot delete us until the scheduler
  // has switched off this stack.
  {
    auto guard = m_JoinWaiters.acquire();
    m_bExitStarted = true;
    guard.wakeAll();
  }

  // This is only an exit-announced scheduler state. Join completion is
  // deliberately delayed until markReapable(). The status transition must
  // also withdraw any ready-queue publication before the thread is retired.
  setStatus(Thread::AwaitingJoin);
}

void Thread::unlinkWaitsForStackDiscard() {
  // A discard callback can release the final reference to the object which
  // owns a WaitQueue. Unpublish every intrusive waiter before invoking any
  // such callback.
  for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level) {
    WaitQueue::Waiter& waiter = m_StateLevels[level].m_Waiter;
    WaitQueue* queue = waiter.loadQueue();
    if (queue) {
      queue->cancel(&waiter, WaitQueue::WakeReason::Terminating);
    }
  }
}

void Thread::setClearChildTid(uintptr_t address) {
  LockGuard<Spinlock> guard(m_Lock);
  if (m_bShutdown) {
    // A delayed clone selected for termination before registration never
    // reaches userspace, so do not publish a pointer after its one-shot
    // subsystem exit hook has begun.
    return;
  }
  __atomic_store_n(&m_ClearChildTid, address, __ATOMIC_RELEASE);
}

void Thread::setRobustList(uintptr_t address, size_t ownerId) {
  LockGuard<Spinlock> guard(m_Lock);
  if (!m_bShutdown) {
    m_RobustListOwnerId = ownerId;
    __atomic_store_n(&m_RobustList, address, __ATOMIC_RELEASE);
  }
}

uintptr_t Thread::takeRobustList(size_t& ownerId) {
  LockGuard<Spinlock> guard(m_Lock);
  ownerId = m_RobustListOwnerId;
  m_RobustListOwnerId = 0;
  return __atomic_exchange_n(&m_RobustList, uintptr_t(0), __ATOMIC_ACQ_REL);
}

void Thread::forceToStartupProcessor() {
  PerProcessorScheduler* destination = Scheduler::instance().getBootstrapProcessorScheduler();
  if (getScheduler() == destination) {
    return;
  }

  if (Processor::information().getCurrentThread() != this) {
    ERROR(
        "Thread::forceToStartupProcessor must be run as the desired "
        "thread.");
    return;
  }

  const bool interrupts = Processor::getInterrupts();
  TerminationDeferral lifetime;
  bool migratable;
  {
    LockGuard<Spinlock> guard(m_Lock);
    migratable = m_Placement.migratable;
    m_Placement.migratable = true;
  }
  CpuAffinityMask startup;
  startup.set(destination->logicalCpu());
  while (true) {
    uint64_t generation = 0;
    const auto result = requestAffinity(startup, generation);
    if (result != AffinityResult::Success && result != AffinityResult::Busy)
      FATAL("Cannot admit startup processor migration.");
    if (waitAffinity(generation) != AffinityResult::Success)
      FATAL("Startup processor migration was terminated.");
    if (result == AffinityResult::Success)
      break;
  }
  if (completeAffinityAtSafePoint() != AffinityResult::Success)
    FATAL("Startup processor migration was terminated at its safe point.");
  Processor::setInterrupts(interrupts);
  LockGuard<Spinlock> guard(m_Lock);
  m_Placement.migratable = migratable;
}

void Thread::setStatus(Thread::Status s) {
  LockGuard<Spinlock> guard(m_Lock);
  setStatusUnlocked(s);
}

void Thread::setStatusUnlocked(Thread::Status s) {
  if (m_Status == Thread::Zombie) {
    if (s != Thread::Zombie) {
      WARNING("Error condition in Thread::setStatus, more info below...");
      WARNING("Parent process ID: " << m_pParent->getId());
      FATAL(
          "Thread::setStatus called with non-zombie status, when the "
          "thread is a zombie!");
    }

    return;
  }

  if (s == Thread::Zombie) {
    // shutdown owns event cleanup outside m_Lock; deleteThread publishes
    // terminal retirement only after the stack handoff.
    FATAL("Thread zombie transition must use off-stack retirement.");
  }

  m_Status = s;

  PerProcessorScheduler* scheduler = getScheduler();
  if (scheduler) {
    scheduler->m_pSchedulingAlgorithm->threadStatusChanged(this);
  }
}

bool Thread::start() {
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_Status != Thread::Created || getUnwindState() == Thread::TerminateThread ||
        m_bStartRequested) {
      return false;
    }

    m_bStartRequested = true;
  }

  // The add worker parks while holding m_Lock, then observes this
  // out-of-lock publication. This ordering makes start the wake predicate
  // without allowing the worker to preempt us under the thread lock.
  Scheduler::instance().threadStatusChanged(this);
  return true;
}

bool Thread::startDetached() {
  Process* parent = m_pParent;
  if (!parent->beginThreadJoin()) {
    return false;
  }

  bool claimed = false;
  bool processExitOwned = false;
  {
    RecursingLockGuard<Spinlock> processGuard(parent->m_Lock);
    auto guard = m_JoinWaiters.acquire();
    if (!m_bJoinClaimed && !m_bDetachedRetirementClaimed) {
      m_bDetached = true;
      m_bDetachedRetirementClaimed = true;
      processExitOwned = m_bProcessExitOwned;
      claimed = true;
    }
  }

  if (!claimed) {
    parent->endThreadJoin();
    return false;
  }

  if (processExitOwned) {
    setUnwindState(Thread::TerminateThread);
  }
  const bool started = start();
  const bool accepted = started || processExitOwned || getUnwindState() == Thread::TerminateThread;
  if (!accepted) {
    setUnwindState(Thread::TerminateThread);
  }

  bool deleteNow = false;
  {
    RecursingLockGuard<Spinlock> processGuard(parent->m_Lock);
    auto guard = m_JoinWaiters.acquire();
    deleteNow = m_bReapable && !m_bProcessExitOwned;
    if (!deleteNow) {
      m_bDetachedRetirementClaimed = false;
    }
  }

  if (deleteNow) {
    closeExternalLeaseAdmissionAndDrain();
    {
      RecursingLockGuard<Spinlock> processGuard(parent->m_Lock);
      {
        auto guard = m_JoinWaiters.acquire();
        deleteNow =
            m_bDetached && m_bReapable && !m_bProcessExitOwned && m_bDetachedRetirementClaimed;
      }
    }
    if (deleteNow) {
      requireThreadDestructionContext();
      delete this;
    }
  }

  parent->endThreadJoin();
  return accepted;
}

SchedulerState& Thread::state() {
  return *(m_StateLevels[m_nStateLevel].m_State);
}

ExecutionContext Thread::executionContext() const {
  const size_t level = __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
  return m_StateLevels[level].m_ExecutionContext.current();
}

SchedulerState* Thread::pushState() {
  const size_t previousLevel = __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
  if ((previousLevel + 1) >= MAX_NESTED_EVENTS) {
    ERROR("Thread: Max nested events!");
    return nullptr;
  }
  const size_t nextLevel = previousLevel + 1;

#if HOSTED
  if (m_StateLevels[0].m_pKernelStack && nextLevel <= HostedPreallocatedUserStateLevel &&
      !m_StateLevels[nextLevel].m_pKernelStack) {
    FATAL_NOLOCK("Hosted user Thread reached an unallocated return state stack");
  }
#endif

  if (__atomic_load_n(&m_pDeferredScopes[nextLevel], __ATOMIC_ACQUIRE)) {
    FATAL("Thread state level reused with an armed cleanup record.");
  }

  // Prepare the unused level before publishing it to remote event senders.
  // Stack allocation and replacing an old SharedPointer can free memory, so
  // neither belongs in the short publication critical section below.
  allocateStackAtLevel(nextLevel);
  m_StateLevels[nextLevel].m_InhibitMask = m_StateLevels[previousLevel].m_InhibitMask;
#if X64
  // State levels are reused. A new handler must not inherit an FPU image
  // left behind by an earlier handler at the same nesting depth.
  m_StateLevels[nextLevel].m_State->flags &= ~(1U << 1);
#endif
  m_StateLevels[nextLevel].m_InterruptionReason = NotInterrupted;
  m_StateLevels[nextLevel].m_bDispatchingWaitEvent = false;
  m_StateLevels[nextLevel].m_SavedSignalMask = 0;
  m_StateLevels[nextLevel].m_TemporarySignalMaskActive = false;
  m_StateLevels[nextLevel].m_TemporarySignalWaitInterrupted = false;
  m_StateLevels[nextLevel].m_DeferredSignalMaskRestore = false;
  m_StateLevels[nextLevel].m_DispatchedSignalNumber = 0;
  m_StateLevels[nextLevel].m_DispatchedSignalContinuationEpoch = 0;
  m_StateLevels[nextLevel].m_bOwnsAlternateSignalStack = false;
  m_StateLevels[nextLevel].m_ExecutionContext = m_StateLevels[previousLevel].m_ExecutionContext;
  m_StateLevels[nextLevel].m_pRequestQueueCallback =
      m_StateLevels[previousLevel].m_pRequestQueueCallback;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  observeStateTransition(StatePushBeforePublish, this, previousLevel, nextLevel);
#endif

  SchedulerState* previousState = m_StateLevels[previousLevel].m_State;
  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_nStateLevel != previousLevel) {
      FATAL("Thread state level changed during push publication.");
    }

    // Refresh mutable per-level scalars while serialised with their public
    // accessors, then release-publish the completely prepared level.
    m_StateLevels[nextLevel].m_SignalMask = m_StateLevels[previousLevel].m_SignalMask;
    m_StateLevels[nextLevel].m_Errno = m_StateLevels[previousLevel].m_Errno;
    __atomic_store_n(&m_nStateLevel, nextLevel, __ATOMIC_RELEASE);
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  observeStateTransition(StatePushAfterPublish, this, previousLevel, nextLevel);
#endif

  setKernelStack();
  Processor::setInterrupts(interruptsWereEnabled);

  return previousState;
}

void Thread::popState(bool clean) {
  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);

  const size_t origStateLevel = __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);

  if (origStateLevel == 0) {
    ERROR("Thread: Potential error: popStack() called with state level 0!");
    ERROR("Thread: (ignore this if longjmp has been called)");
    Processor::setInterrupts(interruptsWereEnabled);
    return;
  }

  if (__atomic_load_n(&m_pDeferredScopes[origStateLevel], __ATOMIC_ACQUIRE)) {
    FATAL("Normal state pop attempted with armed cleanup records.");
  }

  const size_t nextLevel = origStateLevel - 1;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_nStateLevel != origStateLevel) {
      FATAL("Thread state level changed during pop publication.");
    }
    if (m_StateLevels[origStateLevel].m_bOwnsAlternateSignalStack) {
      m_AlternateSignalStack.inUse = false;
      m_StateLevels[origStateLevel].m_bOwnsAlternateSignalStack = false;
    }
    __atomic_store_n(&m_nStateLevel, nextLevel, __ATOMIC_RELEASE);
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  observeStateTransition(StatePopAfterPublish, this, origStateLevel, nextLevel);
#endif

  setKernelStack();

  if (clean) {
    cleanStateLevel(origStateLevel);
  }

  Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::abandonCurrentState(bool clean) {
  const size_t level = __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
  if (!level) {
    FATAL("Cannot abandon the base Thread state.");
  }
  retireDeferredScopes(false, level);
  popState(clean);
}

void Thread::abandonAllStates() {
  while (getStateLevel()) {
    // The caller is still running on the outermost physical stack until
    // its no-return transition, so every logical pop preserves storage.
    abandonCurrentState(false);
  }
}

VirtualAddressSpace::Stack* Thread::getStateUserStack() {
  return m_StateLevels[m_nStateLevel].m_pUserStack;
}

void Thread::setStateUserStack(VirtualAddressSpace::Stack* st) {
  m_StateLevels[m_nStateLevel].m_pUserStack = st;
}

void Thread::discardUserStackMetadataForExec() {
  if (Processor::information().getCurrentThread() != this) {
    FATAL("Exec attempted to discard another Thread's user stacks.");
  }

  VirtualAddressSpace::Stack* discarded[MAX_NESTED_EVENTS] = {};
  size_t discardedCount = 0;
  {
    LockGuard<Spinlock> guard(m_Lock);
    for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level) {
      VirtualAddressSpace::Stack* stack = m_StateLevels[level].m_pUserStack;
      m_StateLevels[level].m_pUserStack = nullptr;
      if (!stack) {
        continue;
      }

      bool alreadyDiscarded = false;
      for (size_t i = 0; i < discardedCount; ++i) {
        if (discarded[i] == stack) {
          alreadyDiscarded = true;
          break;
        }
      }
      if (!alreadyDiscarded) {
        discarded[discardedCount++] = stack;
      }
    }
  }

  // revertToKernelAddressSpace has already retired the mappings. Calling
  // freeStack here could unmap the replacement image if it reuses an old
  // stack address; only the descriptor itself remains ours to release.
  for (size_t i = 0; i < discardedCount; ++i) {
    delete discarded[i];
  }
}

void Thread::adoptInitialUserStackForExec(VirtualAddressSpace::Stack* stack) {
  if (!stack) {
    FATAL("Cannot adopt an empty exec user stack.");
  }

  LockGuard<Spinlock> guard(m_Lock);
  if (m_StateLevels[0].m_pUserStack) {
    FATAL("Exec attempted to replace an owned base user stack.");
  }
  m_StateLevels[0].m_pUserStack = stack;
}

void Thread::threadExited() {
  Thread* thread = Processor::information().getCurrentThread();
  if (!thread) {
    FATAL("Kernel thread root returned without a current Thread.");
  }
  if (thread->getStateLevel()) {
    FATAL("Kernel thread root returned with nested event state still active.");
  }

  const UnwindType unwindState = thread->getUnwindState();
  if (unwindState == Exit) {
    Process* process = thread->getParent();
    Subsystem* subsystem = process ? process->getSubsystem() : nullptr;
    if (!subsystem) {
      FATAL("Kernel thread root reached process exit without a subsystem.");
    }
    const DeferredProcessExit request = thread->takeDeferredProcessExit();
    subsystem->exit(request.code, request.cause);
    FATAL("Subsystem::exit returned to a kernel thread root.");
  }

  Processor::information().getScheduler().commitCurrentThreadExit();
}

void Thread::allocateStackAtLevel(size_t stateLevel) {
  if (stateLevel >= MAX_NESTED_EVENTS)
    stateLevel = MAX_NESTED_EVENTS - 1;
  if (m_StateLevels[stateLevel].m_pKernelStack == 0)
    m_StateLevels[stateLevel].m_pKernelStack =
        VirtualAddressSpace::getKernelAddressSpace().allocateStack();
#if HOSTED
  VirtualAddressSpace::Stack* stack = m_StateLevels[stateLevel].m_pKernelStack;
  if (stack) {
    SchedulerState* state = m_StateLevels[stateLevel].m_State;
    state->stackBase = reinterpret_cast<uintptr_t>(stack->getBase());
    state->stackSize = stack->getSize();
  }
#endif
}

void* Thread::getKernelStack() {
  if (m_nStateLevel >= MAX_NESTED_EVENTS)
    FATAL("m_nStateLevel > MAX_NESTED_EVENTS: " << m_nStateLevel << "...");
  if (m_StateLevels[m_nStateLevel].m_pKernelStack != 0) {
    return m_StateLevels[m_nStateLevel].m_pKernelStack->getTop();
  } else {
    return 0;
  }
}

void* Thread::getKernelStackBase(size_t* size) const {
  if (m_nStateLevel >= MAX_NESTED_EVENTS)
    FATAL("m_nStateLevel > MAX_NESTED_EVENTS: " << m_nStateLevel << "...");
  VirtualAddressSpace::Stack* stack = m_StateLevels[m_nStateLevel].m_pKernelStack;
#if HOSTED
  if (!stack) {
    stack = m_StateLevels[m_nStateLevel].m_pAuxillaryStack;
  }
#endif
  if (stack) {
    *size = stack->getSize();
    return stack->getBase();
  } else {
    ERROR("No kernel stack at this level!");
    *size = 0;
    return 0;
  }
}

void Thread::setKernelStack() {
  uintptr_t stack = 0;
  if (m_StateLevels[m_nStateLevel].m_pKernelStack) {
    stack = reinterpret_cast<uintptr_t>(m_StateLevels[m_nStateLevel].m_pKernelStack->getTop());
  }
  Processor::information().setKernelStack(stack);
}

void Thread::pokeState(size_t stateLevel, SchedulerState& state) {
  if (stateLevel >= MAX_NESTED_EVENTS) {
    ERROR("Thread::pokeState(): stateLevel `" << stateLevel << "' is over the maximum.");
    return;
  }
  *(m_StateLevels[stateLevel].m_State) = state;
}

bool Thread::sendEvent(Event* pEvent) {
  Event::SendLease eventSendLease = pEvent->beginSend();
  if (!eventSendLease) {
    return false;
  }

  bool accepted = false;
  {
    auto senderGuard = m_EventSenderDrainWaiters.acquire();
    {
      LockGuard<Spinlock> guard(m_Lock);
      if (m_bShutdown || m_Status == Zombie) {
        return false;
      }
      ++m_EventSendersInFlight;
    }
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  EventAdmissionHook admissionHook = __atomic_load_n(&g_EventAdmissionHook, __ATOMIC_ACQUIRE);
  Thread* admissionTarget = __atomic_load_n(&g_EventAdmissionTarget, __ATOMIC_ACQUIRE);
  if (admissionHook && admissionTarget == this) {
    admissionHook(this);
  }
#endif

  // Event registration can allocate. The in-flight pin lets this happen
  // outside m_Lock without opening a registration-vs-destruction gap.
  const bool eventRegistered = pEvent->registerThread(this);

  bool duplicate = false;
  bool wakeThread = false;
  PerProcessorScheduler* readyScheduler = nullptr;
  if (eventRegistered) {
    // Serialise queue inspection in waitForEvent() with event publication.
    auto eventWaitGuard = m_EventWaiters.acquire();
    {
      LockGuard<Spinlock> guard(m_Lock);
      if (!m_bShutdown && m_Status != Zombie) {
        if (pEvent->isSignalEvent() && !static_cast<SignalEvent*>(pEvent)->queuedIndividually()) {
          for (List<Event*>::Iterator it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
            if ((*it)->isSignalEvent() && (*it)->getNumber() == pEvent->getNumber() &&
                static_cast<SignalEvent*>(*it)->isProcessDirected() ==
                    static_cast<SignalEvent*>(pEvent)->isProcessDirected()) {
              duplicate = true;
              break;
            }
          }
        }

        if (!duplicate) {
          m_EventQueue.pushBack(pEvent);
          markUserReturnWorkPending();
          wakeThread = hasDeliverableEventsUnlocked() &&
                       interruptWaitUnlocked(WaitQueue::WakeReason::Event, readyScheduler);
        }
        accepted = true;
      }
    }
  }

  if (!eventRegistered) {
    accepted = false;
  } else if (!accepted) {
    pEvent->deregisterThread(this);
  } else if (duplicate) {
    pEvent->completeDelivery(this);
  } else if (wakeThread) {
    assert(readyScheduler);
    readyScheduler->publishReadyFromWait(this);
  }

  {
    auto senderGuard = m_EventSenderDrainWaiters.acquire();
    bool drained = false;
    {
      LockGuard<Spinlock> guard(m_Lock);
      assert(m_EventSendersInFlight);
      drained = !--m_EventSendersInFlight;
    }
    if (drained) {
      senderGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
    }
  }
  return accepted;
}

void Thread::waitForEvent(WaitQueue::StackDiscardCleanup onStackDiscard,
                          void* stackDiscardContext) {
  waitForEventInternal(false, onStackDiscard, stackDiscardContext);
}

bool Thread::waitForEventOrSignalInterruption(WaitQueue::StackDiscardCleanup onStackDiscard,
                                              void* stackDiscardContext) {
  return waitForEventInternal(true, onStackDiscard, stackDiscardContext);
}

bool Thread::waitForEventInternal(bool stopOnSignalInterruption,
                                  WaitQueue::StackDiscardCleanup onStackDiscard,
                                  void* stackDiscardContext) {
  StackDiscardScope discardScope(onStackDiscard, stackDiscardContext);
  while (true) {
    if (getUnwindState() != Continue) {
      return false;
    }

    bool ready = false;
    bool signalInterrupted = false;
    WaitQueue::WakeReason reason = WaitQueue::WakeReason::Spurious;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (stopOnSignalInterruption) {
      SignalWaitPreEnrolmentHook hook =
          __atomic_load_n(&g_SignalWaitPreEnrolmentHook, __ATOMIC_ACQUIRE);
      Thread* hookTarget = __atomic_load_n(&g_SignalWaitPreEnrolmentTarget, __ATOMIC_ACQUIRE);
      if (hook && hookTarget == this) {
        hook(this);
      }
    }
#endif
    {
      auto guard = m_EventWaiters.acquire();

      m_Lock.acquire();
      ready = hasDeliverableEventsUnlocked();
      if (stopOnSignalInterruption) {
        const StateLevel& state = m_StateLevels[m_nStateLevel];
        signalInterrupted = state.m_TemporarySignalMaskActive &&
                            state.m_TemporarySignalWaitInterrupted &&
                            state.m_InterruptionReason == InterruptedBySignal;
      }
      m_Lock.release();
      if (!ready && !signalInterrupted) {
        reason = guard.wait(WaitQueue::Channel(), Thread::EventWait,
                            reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
      }
    }

    if (signalInterrupted) {
      return true;
    }

    if (ready && getUnwindState() == Continue) {
      // A pre-existing event did not pass through WaitQueue::wait(), so
      // dispatch it explicitly after dropping the event-wait guard and
      // identify it as the event which satisfied this wait.
      const size_t stateLevel = getStateLevel();
      m_StateLevels[stateLevel].m_bDispatchingWaitEvent = true;
      Processor::information().getScheduler().checkEventState(0);
      m_StateLevels[stateLevel].m_bDispatchingWaitEvent = false;
      return stopOnSignalInterruption && hasTemporarySignalWaitInterruption();
    }

    if (reason == WaitQueue::WakeReason::Event || reason == WaitQueue::WakeReason::Terminating ||
        reason == WaitQueue::WakeReason::Unwinding) {
      return stopOnSignalInterruption && hasTemporarySignalWaitInterruption();
    }
  }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
Atomic<size_t> g_HostedPrequeuedEventCalls(0);
Atomic<size_t> g_HostedShutdownEventCalls(0);
Atomic<size_t> g_HostedShutdownEventDestructions(0);
Atomic<size_t> g_HostedShutdownThreadCalls(0);
Atomic<size_t> g_HostedSelfRetireCalls(0);
Atomic<size_t> g_HostedSelfRetireDestructions(0);
Atomic<size_t> g_HostedAdmissionRetireDestructions(0);
Event* g_pHostedSelfRetireEvent = nullptr;

struct HostedAdmissionRetireContext {
  HostedAdmissionRetireContext(Event* event, size_t destructionsBefore)
      : event(event), destructionsBefore(destructionsBefore), calls(0), destroyedInsideHook(0) {}

  Event* event;
  size_t destructionsBefore;
  Atomic<size_t> calls;
  Atomic<size_t> destroyedInsideHook;
};

HostedAdmissionRetireContext* g_pHostedAdmissionRetireContext = nullptr;

void hostedPrequeuedEventHandler(size_t) {
  g_HostedPrequeuedEventCalls += 1;
}

void hostedShutdownEventHandler(size_t) {
  g_HostedShutdownEventCalls += 1;
}

void hostedSelfRetireEventHandler(size_t) {
  Event* event = g_pHostedSelfRetireEvent;
  g_pHostedSelfRetireEvent = nullptr;
  if (event) {
    event->retire();
    g_HostedSelfRetireCalls += 1;
  }
}

void hostedAdmissionRetireHook(Thread*) {
  HostedAdmissionRetireContext* context = g_pHostedAdmissionRetireContext;
  if (!context) {
    return;
  }

  context->calls += 1;
  context->event->retire();
  if (g_HostedAdmissionRetireDestructions != context->destructionsBefore) {
    context->destroyedInsideHook += 1;
  }
}

class HostedPrequeuedEvent : public Event {
 public:
  HostedPrequeuedEvent()
      : Event(reinterpret_cast<uintptr_t>(&hostedPrequeuedEventHandler), false) {}

  size_t serialize(uint8_t*) override {
    return 0;
  }

  size_t getNumber() override {
    return 0x57414954;
  }
};

class HostedShutdownStableEvent : public Event {
 public:
  HostedShutdownStableEvent()
      : Event(reinterpret_cast<uintptr_t>(&hostedShutdownEventHandler), false) {}

  size_t serialize(uint8_t*) override {
    return 0;
  }

  size_t getNumber() override {
    return 0x53484453;
  }
};

class HostedShutdownDeletableEvent : public Event {
 public:
  HostedShutdownDeletableEvent()
      : Event(reinterpret_cast<uintptr_t>(&hostedShutdownEventHandler), true) {}

  ~HostedShutdownDeletableEvent() override {
    g_HostedShutdownEventDestructions += 1;
  }

  size_t serialize(uint8_t*) override {
    return 0;
  }

  size_t getNumber() override {
    return 0x53484444;
  }
};

class HostedSelfRetireEvent : public Event {
 public:
  HostedSelfRetireEvent()
      : Event(reinterpret_cast<uintptr_t>(&hostedSelfRetireEventHandler), false) {}

  ~HostedSelfRetireEvent() override {
    g_HostedSelfRetireDestructions += 1;
  }

  size_t serialize(uint8_t*) override {
    return 0;
  }

  size_t getNumber() override {
    return 0x53455254;
  }
};

class HostedAdmissionRetireEvent : public Event {
 public:
  HostedAdmissionRetireEvent()
      : Event(reinterpret_cast<uintptr_t>(&hostedShutdownEventHandler), false) {}

  ~HostedAdmissionRetireEvent() override {
    g_HostedAdmissionRetireDestructions += 1;
  }

  size_t serialize(uint8_t*) override {
    return 0;
  }

  size_t getNumber() override {
    return 0x41525254;
  }
};

int hostedShutdownThread(void*) {
  g_HostedShutdownThreadCalls += 1;
  return 0;
}

struct HostedStatePublicationContext {
  HostedStatePublicationContext(Thread* thread, Event* event)
      : thread(thread), event(event), calls(0), failures(0) {}

  Thread* thread;
  Event* event;
  Atomic<size_t> calls;
  Atomic<size_t> failures;
};

HostedStatePublicationContext* g_StatePublicationContext = nullptr;

void hostedStatePublicationHook(Thread::StateTransitionWindow window, Thread* thread,
                                size_t previousLevel, size_t nextLevel) {
  HostedStatePublicationContext* context =
      __atomic_load_n(&g_StatePublicationContext, __ATOMIC_ACQUIRE);
  if (!context) {
    return;
  }

  context->calls += 1;
  const size_t expectedVisibleLevel =
      window == Thread::StatePushBeforePublish ? previousLevel : nextLevel;
  if (thread != context->thread || thread->getStateLevel() != expectedVisibleLevel ||
      (window != Thread::StatePushBeforePublish && Processor::getInterrupts()) ||
      !thread->sendEvent(context->event)) {
    context->failures += 1;
  }
}

struct HostedDeliveryLeaseContext {
  explicit HostedDeliveryLeaseContext(Event* event) : event(event), entered(0), completed(0) {}

  Event* event;
  Atomic<size_t> entered;
  Atomic<size_t> completed;
};

int hostedDeliveryLeaseWaiter(void* parameter) {
  HostedDeliveryLeaseContext* context = reinterpret_cast<HostedDeliveryLeaseContext*>(parameter);
  context->entered += 1;
  context->event->waitForDeliveries();
  context->completed += 1;
  return 0;
}
}  // namespace

void Thread::setStateTransitionHook(StateTransitionHook hook) {
  __atomic_store_n(&g_StateTransitionHook, hook, __ATOMIC_RELEASE);
}

void Thread::setJoinOperationHook(JoinOperationHook hook) {
  __atomic_store_n(&g_JoinOperationHook, hook, __ATOMIC_RELEASE);
}

void Thread::setExternalLeaseReleaseHookForHostedTest(Thread* target,
                                                      ExternalLeaseReleaseHook hook) {
  __atomic_store_n(&g_ExternalLeaseReleaseTarget, target, __ATOMIC_RELEASE);
  __atomic_store_n(&g_ExternalLeaseReleaseHook, hook, __ATOMIC_RELEASE);
}

void Thread::setSignalWaitPreEnrolmentHookForHostedTest(Thread* target,
                                                        SignalWaitPreEnrolmentHook hook) {
  __atomic_store_n(&g_SignalWaitPreEnrolmentTarget, target, __ATOMIC_RELEASE);
  __atomic_store_n(&g_SignalWaitPreEnrolmentHook, hook, __ATOMIC_RELEASE);
}

void Thread::setTlsResetHookForHostedTest(Thread* target, TlsResetHook hook) {
  __atomic_store_n(&g_TlsResetTarget, target, __ATOMIC_RELEASE);
  __atomic_store_n(&g_TlsResetHook, hook, __ATOMIC_RELEASE);
}

bool Thread::isReapableForHostedTest() {
  auto guard = m_JoinWaiters.acquire();
  return m_bReapable;
}

bool Thread::wasStartPublishedForHostedTest() {
  LockGuard<Spinlock> guard(m_Lock);
  return m_bStartRequested || m_Status == Ready || m_Status == Running;
}

bool Thread::waitUntilReapableForHostedTest() {
  TerminationDeferral terminationDeferral;
  while (true) {
    auto guard = m_JoinWaiters.acquire();
    if (m_bReapable) {
      return true;
    }

    const WaitQueue::WakeReason reason = guard.waitForCompletion(
        WaitQueue::Channel(), Thread::Joining, reinterpret_cast<uintptr_t>(this));
    (void)reason;
  }
}

bool Thread::runHostedPrequeuedEventRegression() {
  if (Processor::information().getCurrentThread() != this) {
    return false;
  }

  constexpr size_t Iterations = 16;
  HostedPrequeuedEvent event;
  const size_t initialStateLevel = getStateLevel();
  const size_t callsBefore = g_HostedPrequeuedEventCalls;
  for (size_t iteration = 0; iteration < Iterations; ++iteration) {
    if (!sendEvent(&event) || event.pendingCount() != 1 || !hasEvent(&event)) {
      return false;
    }

    waitForEvent();

    if (g_HostedPrequeuedEventCalls != (callsBefore + iteration + 1) || event.pendingCount() != 0 ||
        hasEvent(&event) || getStateLevel() != initialStateLevel) {
      return false;
    }
  }

  return true;
}

bool Thread::runHostedStatePublicationRegression() {
  if (Processor::information().getCurrentThread() != this ||
      (getStateLevel() + 1) >= MAX_NESTED_EVENTS) {
    return false;
  }

  HostedPrequeuedEvent event;
  HostedStatePublicationContext context(this, &event);
  const size_t initialStateLevel = getStateLevel();
  __atomic_store_n(&g_StatePublicationContext, &context, __ATOMIC_RELEASE);
  setStateTransitionHook(hostedStatePublicationHook);

  SchedulerState* previousState = pushState();
  const bool pushed = previousState && getStateLevel() == (initialStateLevel + 1);
  if (previousState) {
    popState();
  }

  setStateTransitionHook(nullptr);
  __atomic_store_n(&g_StatePublicationContext, static_cast<HostedStatePublicationContext*>(nullptr),
                   __ATOMIC_RELEASE);

  const bool publishedSafely = pushed && getStateLevel() == initialStateLevel &&
                               context.calls == 3 && context.failures == 0 &&
                               event.pendingCount() == 3 && hasEvent(&event);
  cullEvent(&event);
  const bool deliveriesCulled = event.pendingCount() == 0 && !hasEvent(&event);

  return publishedSafely && deliveriesCulled;
}

bool Thread::runHostedStateCleanupRegression() {
  const size_t initialLevel = getStateLevel();
  if (!clearUserReturnWorkIfIdle() || userReturnWorkPending() || !canSkipUserReturnWork()) {
    return false;
  }
  const auto terminationWorkPending = [this]() {
    return isTerminationDeferred() && userReturnWorkPending() && !canSkipUserReturnWork() &&
           !clearUserReturnWorkIfIdle();
  };
  HostedStateCleanupOrder order;
  HostedStateCleanupItem oldItem{&order, 0};
  HostedStateCleanupItem firstItem{&order, 1};
  HostedStateCleanupItem secondItem{&order, 2};
  HostedStateCleanupItem normalItem{&order, 3};
  HostedStateCleanupItem baseItem{&order, 4};
  HostedStateCleanupItem levelItem{&order, 5};
  DeferredScopeRecord oldRecord;
  DeferredScopeRecord firstRecord;
  AtomicStateCleanupRecord secondRecord;
  DeferredScopeRecord normalRecord;
  DeferredScopeRecord baseRecord;
  DeferredScopeRecord terminationRecord;
  DeferredScopeRecord checkpointTerminationRecord;
  DeferredScopeRecord levelTerminationRecord;
  AtomicStateCleanupRecord levelRecord;

  armStateCleanup(oldRecord, hostedStateCleanupCallback, &oldItem);
  const size_t checkpoint = stateCleanupCheckpoint();
  armStateCleanup(firstRecord, hostedStateCleanupCallback, &firstItem);
  armAtomicStateCleanup(secondRecord, hostedStateCleanupCallback, &secondItem);
  const bool cleanupDoesNotDeferTermination = !isTerminationDeferred();
  registerFreshTerminationDeferral(checkpointTerminationRecord);
  retireDeferredScopesAfter(checkpoint);

  const bool checkpointPassed = order.count == 2 && order.values[0] == 2 && order.values[1] == 1 &&
                                oldRecord.armed && !firstRecord.armed && !secondRecord.armed &&
                                !checkpointTerminationRecord.armed && !isTerminationDeferred() &&
                                !userReturnWorkPending() && canSkipUserReturnWork();
  disarmStateCleanup(oldRecord);

  armStateCleanup(normalRecord, hostedStateCleanupCallback, &normalItem);
  disarmStateCleanup(normalRecord);
  const bool normalPassed = order.count == 2 && !normalRecord.armed;

  const uint64_t originalSignalMask = getSignalMask();
  const uint64_t temporarySignalMask = originalSignalMask ^ (static_cast<uint64_t>(1) << 7);
  const size_t temporaryMaskCheckpoint = stateCleanupCheckpoint();
  bool temporaryMaskActive = false;
  {
    TemporarySignalMask signalMask(*this, temporarySignalMask);
    temporaryMaskActive = getSignalMask() == temporarySignalMask;
    retireDeferredScopesAfter(temporaryMaskCheckpoint);
  }
  const bool temporaryMaskCleanupPassed = temporaryMaskActive &&
                                          getSignalMask() == originalSignalMask &&
                                          !hasTemporarySignalWaitInterruption();

  armStateCleanup(baseRecord, hostedStateCleanupCallback, &baseItem);
  const bool pushed = pushState() != nullptr;
  if (pushed) {
    armAtomicStateCleanup(levelRecord, hostedStateCleanupCallback, &levelItem);
    registerFreshTerminationDeferral(levelTerminationRecord);
    abandonCurrentState(false);
  }
  const bool levelPassed = pushed && getStateLevel() == initialLevel && order.count == 3 &&
                           order.values[2] == 5 && baseRecord.armed && !levelRecord.armed &&
                           !levelTerminationRecord.armed && !isTerminationDeferred() &&
                           !userReturnWorkPending() && canSkipUserReturnWork();
  disarmStateCleanup(baseRecord);

  registerDeferredScope(terminationRecord, true, false);
  const bool explicitTerminationDefers = terminationWorkPending();
  unregisterDeferredScope(terminationRecord);
  const bool explicitTerminationRetired = !isTerminationDeferred() && !terminationRecord.armed &&
                                          !userReturnWorkPending() && canSkipUserReturnWork();

  bool pureScopesPassed = true;
  DeferredScopeRecord* initialHead =
      __atomic_load_n(&m_pDeferredScopes[initialLevel], __ATOMIC_ACQUIRE);
  const size_t pureCheckpoint = stateCleanupCheckpoint();
  alignas(TerminationDeferral) uint8_t scopeStorage[sizeof(TerminationDeferral)];
  ByteSet(scopeStorage, 0xa5, sizeof(scopeStorage));
  TerminationDeferral* fresh = new (scopeStorage) TerminationDeferral();
  DeferredScopeRecord* freshRecord =
      __atomic_load_n(&m_pDeferredScopes[initialLevel], __ATOMIC_ACQUIRE);
  const size_t freshSequence = freshRecord ? freshRecord->sequence : 0;
  pureScopesPassed &= freshRecord && freshRecord != initialHead && freshRecord->armed &&
                      freshRecord->next == initialHead && freshRecord->stateLevel == initialLevel &&
                      freshSequence > pureCheckpoint && freshRecord->defersTermination &&
                      !freshRecord->defersEvents && !freshRecord->cleanup &&
                      !freshRecord->context && terminationWorkPending();
  {
    TerminationDeferral moved(pedigree_std::move(*fresh));
    fresh->~TerminationDeferral();
    DeferredScopeRecord* movedRecord =
        __atomic_load_n(&m_pDeferredScopes[initialLevel], __ATOMIC_ACQUIRE);
    pureScopesPassed &= movedRecord && movedRecord != freshRecord &&
                        movedRecord->sequence == freshSequence && terminationWorkPending();

    // Reuse poisoned storage so adoption cannot rely on an accidentally zero stack.
    ByteSet(scopeStorage, 0x5a, sizeof(scopeStorage));
    TerminationDeferral* disabled = new (scopeStorage) TerminationDeferral(false);
    pureScopesPassed &=
        __atomic_load_n(&m_pDeferredScopes[initialLevel], __ATOMIC_ACQUIRE) == movedRecord;
    *disabled = pedigree_std::move(moved);
    DeferredScopeRecord* adoptedRecord =
        __atomic_load_n(&m_pDeferredScopes[initialLevel], __ATOMIC_ACQUIRE);
    pureScopesPassed &=
        adoptedRecord && adoptedRecord->sequence == freshSequence && terminationWorkPending();

    armStateCleanup(normalRecord, hostedStateCleanupCallback, &normalItem);
    {
      TerminationDeferral newer;
      *disabled = pedigree_std::move(newer);
      pureScopesPassed &=
          __atomic_load_n(&m_pDeferredScopes[initialLevel], __ATOMIC_ACQUIRE) == &normalRecord &&
          normalRecord.next == adoptedRecord && adoptedRecord &&
          adoptedRecord->sequence == freshSequence && terminationWorkPending();
    }
    disarmStateCleanup(normalRecord);
    *disabled = TerminationDeferral(false);
    pureScopesPassed &=
        !isTerminationDeferred() && !userReturnWorkPending() && canSkipUserReturnWork() &&
        __atomic_load_n(&m_pDeferredScopes[initialLevel], __ATOMIC_ACQUIRE) == initialHead;
    disabled->~TerminationDeferral();
  }

  bool nestedWorkPassed = true;
  DeferredScopeRecord eventOnlyRecord;
  {
    TerminationDeferral outer;
    nestedWorkPassed &= terminationWorkPending();
    registerDeferredScope(eventOnlyRecord, false, true);
    {
      TerminationDeferral inner;
      outer = TerminationDeferral(false);
      nestedWorkPassed &= terminationWorkPending();
    }
    nestedWorkPassed &= !isTerminationDeferred() && eventsDeferred() && userReturnWorkPending() &&
                        !canSkipUserReturnWork() && !clearUserReturnWorkIfIdle();
  }
  unregisterDeferredScope(eventOnlyRecord);
  nestedWorkPassed &= !userReturnWorkPending() && canSkipUserReturnWork();

  return cleanupDoesNotDeferTermination && checkpointPassed && normalPassed &&
         temporaryMaskCleanupPassed && levelPassed && explicitTerminationDefers &&
         explicitTerminationRetired && pureScopesPassed && nestedWorkPassed && order.count == 3;
}

bool Thread::runHostedExecStackOwnershipRegression() {
  if (Processor::information().getCurrentThread() != this || getStateLevel() != 0) {
    return false;
  }

  for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level) {
    if (m_StateLevels[level].m_pUserStack) {
      return false;
    }
  }

  constexpr size_t FakeStackSize = 4 * 4096;
  VirtualAddressSpace::Stack* oldBase =
      new VirtualAddressSpace::Stack(reinterpret_cast<void*>(0x100000), FakeStackSize);
  VirtualAddressSpace::Stack* oldNested =
      new VirtualAddressSpace::Stack(reinterpret_cast<void*>(0x200000), FakeStackSize);
  m_StateLevels[0].m_pUserStack = oldBase;

  if (!pushState()) {
    m_StateLevels[0].m_pUserStack = nullptr;
    delete oldBase;
    delete oldNested;
    return false;
  }

  const size_t nestedLevel = getStateLevel();
  m_StateLevels[nestedLevel].m_pUserStack = oldNested;
  // Exercise defensive duplicate handling in an otherwise unused level.
  m_StateLevels[MAX_NESTED_EVENTS - 1].m_pUserStack = oldBase;

  discardUserStackMetadataForExec();
  bool allOldMetadataDiscarded = true;
  for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level) {
    allOldMetadataDiscarded &= m_StateLevels[level].m_pUserStack == nullptr;
  }

  VirtualAddressSpace::Stack* replacement =
      new VirtualAddressSpace::Stack(reinterpret_cast<void*>(0x300000), FakeStackSize);
  adoptInitialUserStackForExec(replacement);
  const bool replacementOwnedByBase = m_StateLevels[0].m_pUserStack == replacement &&
                                      m_StateLevels[nestedLevel].m_pUserStack == nullptr;

  // This regression uses synthetic descriptors with no mappings; detach the
  // replacement before ordinary state cleanup asks the address space to free it.
  m_StateLevels[0].m_pUserStack = nullptr;
  delete replacement;
  popState();

  return allOldMetadataDiscarded && replacementOwnedByBase && getStateLevel() == 0;
}

void Thread::withDeferredScopeLockForTest(DeferredScopeLockHook hook) {
  m_DeferredScopeRegressionLock.acquire();
  if (hook) {
    hook();
  }
  m_DeferredScopeRegressionLock.release();
}

bool Thread::runHostedEventDeliveryLeaseRegression() {
  if (Processor::information().getCurrentThread() != this) {
    return false;
  }

  HostedPrequeuedEvent event;
  if (!sendEvent(&event)) {
    return false;
  }

  Event::Delivery delivery = getNextEvent();
  if (!delivery || delivery.get() != &event || hasEvent(&event) || event.pendingCount() != 1) {
    delivery.reset();
    cullEvent(&event);
    return false;
  }

  HostedDeliveryLeaseContext context(&event);
  Thread* waiterA = new Thread(Scheduler::instance().getKernelProcess(), hostedDeliveryLeaseWaiter,
                               &context, nullptr, false, true);
  waiterA->setName("hosted event-delivery lease waiter A");

  while (context.entered != static_cast<size_t>(1)) {
    Scheduler::instance().yield();
  }

  for (size_t i = 0; i < 4; ++i) {
    Scheduler::instance().yield();
  }
  Thread::WaitDebugInfo waiterInfo = {};
  const bool closePublished = waiterA->getWaitDebugInfo(waiterInfo) &&
                              waiterInfo.channelOwner == &event && waiterInfo.queued;
  const bool rejectedAfterClose = closePublished && !sendEvent(&event);
  const bool leaseHeld = context.completed == 0 && event.pendingCount() == 1;

  delivery.reset();
  while (context.completed != static_cast<size_t>(1)) {
    Scheduler::instance().yield();
  }

  const bool waiterAJoined = waiterA->join();
  const bool deliveryLeasePassed = closePublished && rejectedAfterClose && leaseHeld &&
                                   waiterAJoined && event.pendingCount() == 0;

  HostedPrequeuedEvent deferredEvent;
  const size_t callsBefore = g_HostedPrequeuedEventCalls;
  bool nestedDeferralPassed = false;
  {
    Uninterruptible outer;
    {
      Uninterruptible inner;
      if (!sendEvent(&deferredEvent)) {
        return false;
      }
      Processor::information().getScheduler().checkEventState(0);
      nestedDeferralPassed = g_HostedPrequeuedEventCalls == callsBefore &&
                             hasEvent(&deferredEvent) && deferredEvent.pendingCount() == 1;
    }

    Processor::information().getScheduler().checkEventState(0);
    nestedDeferralPassed = nestedDeferralPassed && g_HostedPrequeuedEventCalls == callsBefore &&
                           hasEvent(&deferredEvent) && deferredEvent.pendingCount() == 1;
  }

  Processor::information().getScheduler().checkEventState(0);
  nestedDeferralPassed = nestedDeferralPassed && g_HostedPrequeuedEventCalls == (callsBefore + 1) &&
                         !hasEvent(&deferredEvent) && deferredEvent.pendingCount() == 0;

  const size_t retireCallsBefore = g_HostedSelfRetireCalls;
  const size_t retireDestructionsBefore = g_HostedSelfRetireDestructions;
  HostedSelfRetireEvent* retiringEvent = new HostedSelfRetireEvent;
  g_pHostedSelfRetireEvent = retiringEvent;
  const bool retireQueued = sendEvent(retiringEvent);
  if (retireQueued) {
    waitForEvent();
  } else {
    g_pHostedSelfRetireEvent = nullptr;
    delete retiringEvent;
  }
  const bool selfRetirePassed = retireQueued && !g_pHostedSelfRetireEvent &&
                                g_HostedSelfRetireCalls == (retireCallsBefore + 1) &&
                                g_HostedSelfRetireDestructions == (retireDestructionsBefore + 1);

  HostedAdmissionRetireEvent* admissionEvent = new HostedAdmissionRetireEvent;
  HostedAdmissionRetireContext admissionContext(
      admissionEvent, static_cast<size_t>(g_HostedAdmissionRetireDestructions));
  g_pHostedAdmissionRetireContext = &admissionContext;
  __atomic_store_n(&g_EventAdmissionTarget, this, __ATOMIC_RELEASE);
  __atomic_store_n(&g_EventAdmissionHook, &hostedAdmissionRetireHook, __ATOMIC_RELEASE);
  const bool rejectedByConcurrentRetire = !sendEvent(admissionEvent);
  __atomic_store_n(&g_EventAdmissionHook, static_cast<EventAdmissionHook>(nullptr),
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_EventAdmissionTarget, static_cast<Thread*>(nullptr), __ATOMIC_RELEASE);
  g_pHostedAdmissionRetireContext = nullptr;
  const bool admissionRetirePassed =
      rejectedByConcurrentRetire && admissionContext.calls == 1 &&
      admissionContext.destroyedInsideHook == 0 &&
      g_HostedAdmissionRetireDestructions == (admissionContext.destructionsBefore + 1);

  return deliveryLeasePassed && nestedDeferralPassed && selfRetirePassed && admissionRetirePassed;
}

bool Thread::runHostedEventShutdownRegression() {
  if (Processor::information().getCurrentThread() != this) {
    return false;
  }

  HostedShutdownStableEvent stableEvent;
  HostedShutdownStableEvent racingEvent;
  HostedShutdownStableEvent postShutdownEvent;
  const size_t eventCallsBefore = g_HostedShutdownEventCalls;
  const size_t destructionsBefore = g_HostedShutdownEventDestructions;
  const size_t threadCallsBefore = g_HostedShutdownThreadCalls;

  Thread* target = new Thread(Scheduler::instance().getKernelProcess(), hostedShutdownThread,
                              nullptr, nullptr, false, true, true);
  target->setName("hosted event-queue shutdown regression");

  const bool stableQueued = target->sendEvent(&stableEvent);
  HostedShutdownDeletableEvent* deletableEvent = new HostedShutdownDeletableEvent;
  const bool deletableQueued = stableQueued && target->sendEvent(deletableEvent);
  if (!deletableQueued) {
    delete deletableEvent;
  }

  __atomic_store_n(&g_EventAdmissionTarget, target, __ATOMIC_RELEASE);
  __atomic_store_n(
      &g_EventAdmissionHook,
      +[](Thread* admissionTarget) {
        admissionTarget->setUnwindState(Thread::TerminateThread);
        while (true) {
          {
            LockGuard<Spinlock> guard(admissionTarget->m_Lock);
            if (admissionTarget->m_bShutdown) {
              break;
            }
          }
          Scheduler::instance().yield();
        }
      },
      __ATOMIC_RELEASE);
  const bool rejectedDuringShutdown = !target->sendEvent(&racingEvent);
  __atomic_store_n(&g_EventAdmissionHook, static_cast<EventAdmissionHook>(nullptr),
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_EventAdmissionTarget, static_cast<Thread*>(nullptr), __ATOMIC_RELEASE);

  bool shutdownObserved = false;
  constexpr size_t ShutdownAttempts = 10000;
  for (size_t attempt = 0; attempt < ShutdownAttempts; ++attempt) {
    {
      LockGuard<Spinlock> guard(target->m_Lock);
      shutdownObserved = target->m_bShutdown;
    }
    if (shutdownObserved) {
      break;
    }
    Scheduler::instance().yield();
  }

  bool rejectedAfterShutdown = false;
  if (shutdownObserved) {
    rejectedAfterShutdown = !target->sendEvent(&postShutdownEvent);
    if (!rejectedAfterShutdown) {
      target->cullEvent(&postShutdownEvent);
    }
  }

  const bool joined = target->join();
  return stableQueued && deletableQueued && rejectedDuringShutdown && shutdownObserved &&
         rejectedAfterShutdown && joined && stableEvent.pendingCount() == 0 &&
         racingEvent.pendingCount() == 0 && postShutdownEvent.pendingCount() == 0 &&
         g_HostedShutdownEventCalls == eventCallsBefore &&
         g_HostedShutdownEventDestructions == (destructionsBefore + 1) &&
         g_HostedShutdownThreadCalls == threadCallsBefore;
}
#endif

void Thread::inhibitEvent(size_t eventNumber, bool bInhibit) {
  LockGuard<Spinlock> guard(m_Lock);
  if (bInhibit)
    m_StateLevels[m_nStateLevel].m_InhibitMask->set(eventNumber);
  else
    m_StateLevels[m_nStateLevel].m_InhibitMask->clear(eventNumber);
}

uint64_t Thread::getSignalMask() {
  LockGuard<Spinlock> guard(m_Lock);
  return m_StateLevels[m_nStateLevel].m_SignalMask;
}

void Thread::setSignalMask(uint64_t mask) {
  LockGuard<Spinlock> guard(m_Lock);
  m_StateLevels[m_nStateLevel].m_SignalMask = mask;
}

uint64_t Thread::getSignalMaskForReturnFrame() {
  LockGuard<Spinlock> guard(m_Lock);
  const StateLevel& state = m_StateLevels[m_nStateLevel];
  return state.m_DeferredSignalMaskRestore ? state.m_SavedSignalMask : state.m_SignalMask;
}

void Thread::commitSignalHandlerMask(uint64_t mask) {
  LockGuard<Spinlock> guard(m_Lock);
  StateLevel& state = m_StateLevels[m_nStateLevel];
  state.m_SignalMask = mask;
  if (state.m_DeferredSignalMaskRestore) {
    state.m_SavedSignalMask = 0;
    state.m_DeferredSignalMaskRestore = false;
  }
}

void Thread::restoreDeferredSignalMask(size_t stateLevel) {
  LockGuard<Spinlock> guard(m_Lock);
  if (stateLevel >= MAX_NESTED_EVENTS) {
    FATAL("Deferred signal mask restored from an invalid Thread state level.");
  }

  StateLevel& state = m_StateLevels[stateLevel];
  if (state.m_DeferredSignalMaskRestore) {
    state.m_SignalMask = state.m_SavedSignalMask;
    state.m_SavedSignalMask = 0;
    state.m_DeferredSignalMaskRestore = false;
  }
}

void Thread::setCurrentSignalDelivery(size_t signalNumber, size_t continuationEpoch) {
  LockGuard<Spinlock> guard(m_Lock);
  StateLevel& state = m_StateLevels[m_nStateLevel];
  state.m_DispatchedSignalNumber = signalNumber;
  state.m_DispatchedSignalContinuationEpoch = continuationEpoch;
}

bool Thread::getCurrentSignalDelivery(size_t& signalNumber, size_t& continuationEpoch) {
  LockGuard<Spinlock> guard(m_Lock);
  const StateLevel& state = m_StateLevels[m_nStateLevel];
  if (!state.m_DispatchedSignalNumber) {
    return false;
  }

  signalNumber = state.m_DispatchedSignalNumber;
  continuationEpoch = state.m_DispatchedSignalContinuationEpoch;
  return true;
}

void Thread::prepareSignalStateForExec() {
  const size_t execStateLevel = getStateLevel();
  uint64_t effectiveSignalMask = 0;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_nStateLevel != execStateLevel) {
      FATAL("Thread state changed during exec signal preparation.");
    }
    const StateLevel& state = m_StateLevels[execStateLevel];
    effectiveSignalMask =
        state.m_DeferredSignalMaskRestore ? state.m_SavedSignalMask : state.m_SignalMask;
  }

  // JumpToUserspace collapses nested states but deliberately retains level
  // zero. Every lower physical stack is nevertheless abandoned by exec, so
  // retire records from deepest to shallowest before their storage vanishes.
  // The exec Uninterruptible lives at execStateLevel and remains armed here.
  for (size_t level = execStateLevel; level > 0; --level) {
    retireDeferredScopes(false, level - 1);
  }

  LockGuard<Spinlock> guard(m_Lock);
  if (m_nStateLevel != execStateLevel) {
    FATAL("Thread state changed during exec signal preparation.");
  }

  StateLevel& base = m_StateLevels[0];
  base.m_SignalMask = effectiveSignalMask;
  base.m_SavedSignalMask = 0;
  base.m_TemporarySignalMaskActive = false;
  base.m_TemporarySignalWaitInterrupted = false;
  base.m_DeferredSignalMaskRestore = false;
  base.m_DispatchedSignalNumber = 0;
  base.m_DispatchedSignalContinuationEpoch = 0;
  base.m_bOwnsAlternateSignalStack = false;
  base.m_InterruptionReason = NotInterrupted;
  base.m_bDispatchingWaitEvent = false;
  m_AlternateSignalStack = AlternateSignalStack();
}

size_t Thread::beginTemporarySignalMask(uint64_t signalMask) {
  if (this != Processor::information().getCurrentThread()) {
    FATAL("Temporary signal mask armed for a non-current Thread.");
  }

  LockGuard<Spinlock> guard(m_Lock);
  const size_t stateLevel = m_nStateLevel;
  if (stateLevel >= MAX_NESTED_EVENTS) {
    FATAL("Temporary signal mask armed on an invalid Thread state level.");
  }

  StateLevel& state = m_StateLevels[stateLevel];
  if (state.m_TemporarySignalMaskActive || state.m_DeferredSignalMaskRestore) {
    FATAL("Thread state already owns a temporary signal mask.");
  }

  state.m_SavedSignalMask = state.m_SignalMask;
  state.m_SignalMask = signalMask;
  state.m_InterruptionReason = NotInterrupted;
  state.m_TemporarySignalWaitInterrupted = false;
  state.m_TemporarySignalMaskActive = true;
  return stateLevel;
}

bool Thread::finishTemporarySignalMask(size_t stateLevel, bool deferForUserReturn) {
  LockGuard<Spinlock> guard(m_Lock);
  if (stateLevel >= MAX_NESTED_EVENTS) {
    FATAL("Temporary signal mask restored from an invalid Thread state level.");
  }

  StateLevel& state = m_StateLevels[stateLevel];
  if (!state.m_TemporarySignalMaskActive) {
    FATAL("Thread state has no temporary signal mask to restore.");
  }

  const bool interrupted = state.m_TemporarySignalWaitInterrupted;
  bool deferRestore = false;
  if (deferForUserReturn && interrupted && stateLevel == m_nStateLevel) {
    for (List<Event*>::Iterator it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
      Event* event = *it;
      if (event->isSignalEvent() && eventNeedsUserReturnFrameUnlocked(event) &&
          eventIsDeliverableUnlocked(event, EventSelection::AnyDeliverable)) {
        deferRestore = true;
        break;
      }
    }
  }
  if (deferRestore)
    markUserReturnWorkPending();
  // Keep a temporarily unblocked signal eligible until the syscall boundary
  // can save the original mask in its handler's return frame.
  state.m_DeferredSignalMaskRestore = deferRestore;
  if (!deferRestore) {
    state.m_SignalMask = state.m_SavedSignalMask;
    state.m_SavedSignalMask = 0;
  }
  state.m_TemporarySignalMaskActive = false;
  state.m_TemporarySignalWaitInterrupted = false;
  if (interrupted && state.m_InterruptionReason == InterruptedBySignal) {
    state.m_InterruptionReason = NotInterrupted;
  }
  return interrupted;
}

bool Thread::hasTemporarySignalWaitInterruption() {
  LockGuard<Spinlock> guard(m_Lock);
  const StateLevel& state = m_StateLevels[m_nStateLevel];
  return state.m_TemporarySignalMaskActive && state.m_TemporarySignalWaitInterrupted &&
         state.m_InterruptionReason == InterruptedBySignal;
}

bool Thread::hasActiveTemporarySignalMask() {
  LockGuard<Spinlock> guard(m_Lock);
  return m_StateLevels[m_nStateLevel].m_TemporarySignalMaskActive;
}

bool Thread::retainTemporarySignalWaitInterruptionOrClear() {
  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  m_Lock.acquire();
  StateLevel& state = m_StateLevels[m_nStateLevel];
  const bool retained = state.m_TemporarySignalMaskActive &&
                        state.m_TemporarySignalWaitInterrupted &&
                        state.m_InterruptionReason == InterruptedBySignal;
  if (!retained) {
    state.m_InterruptionReason = NotInterrupted;
  }
  m_Lock.release();
  Processor::setInterrupts(interruptsWereEnabled);
  return retained;
}

void Thread::cullEvent(Event* pEvent) {
  size_t removed = 0;
  {
    LockGuard<Spinlock> guard(m_Lock);

    for (List<Event*>::Iterator it = m_EventQueue.begin(); it != m_EventQueue.end();) {
      if (*it == pEvent) {
        it = m_EventQueue.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
  }

  // The caller retains ownership of an exact-event cull. Account for every
  // enqueue independently, including duplicate enqueues of the same object.
  while (removed--) {
    pEvent->deregisterThread(this);
  }
}

void Thread::cullEvent(size_t eventNumber) {
  Vector<Event*> deregisterEvents;

  {
    LockGuard<Spinlock> guard(m_Lock);

    for (List<Event*>::Iterator it = m_EventQueue.begin(); it != m_EventQueue.end();) {
      if ((*it)->getNumber() == eventNumber) {
        Event* pEvent = *it;
        it = m_EventQueue.erase(it);
        deregisterEvents.pushBack(pEvent);
      } else
        ++it;
    }
  }

  // clean up events now that we're no longer locked
  for (auto it : deregisterEvents) {
    it->completeDelivery(this);
  }
}

void Thread::cullSignalEvent(size_t signalNumber) {
  Vector<Event*> deregisterEvents;

  {
    LockGuard<Spinlock> guard(m_Lock);

    for (List<Event*>::Iterator it = m_EventQueue.begin(); it != m_EventQueue.end();) {
      if ((*it)->isSignalEvent() && (*it)->getNumber() == signalNumber) {
        Event* pEvent = *it;
        it = m_EventQueue.erase(it);
        deregisterEvents.pushBack(pEvent);
      } else {
        ++it;
      }
    }
  }

  for (auto it : deregisterEvents) {
    it->completeDelivery(this);
  }
}

bool Thread::transferProcessSignalsTo(Thread& target) {
  if (&target == this) {
    return true;
  }
  TerminationDeferral transferDeferral;
  while (true) {
    Event* pending = nullptr;
    {
      LockGuard<Spinlock> guard(m_Lock);
      for (auto it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
        if ((*it)->isSignalEvent() && static_cast<SignalEvent*>(*it)->isProcessDirected()) {
          pending = *it;
          m_EventQueue.erase(it);
          break;
        }
      }
    }
    if (!pending) {
      return true;
    }

    // The old registration pins the event through destination admission and
    // coalescing, including when the destination already holds this signal.
    if (!target.sendEvent(pending)) {
      LockGuard<Spinlock> guard(m_Lock);
      m_EventQueue.pushFront(pending);
      return false;
    }
    pending->completeDelivery(this);
  }
}

bool Thread::replaceSignalEvent(size_t signalNumber, Event* replacement, int processDirected,
                                uint64_t rebindGeneration) {
  if (!replacement || !replacement->isSignalEvent() || replacement->getNumber() != signalNumber) {
    return false;
  }

  Event::SendLease eventSendLease = replacement->beginSend();
  if (!eventSendLease) {
    return false;
  }

  {
    auto senderGuard = m_EventSenderDrainWaiters.acquire();
    LockGuard<Spinlock> guard(m_Lock);
    if (m_bShutdown || m_Status == Zombie) {
      return false;
    }
    ++m_EventSendersInFlight;
  }

  const bool eventRegistered = replacement->registerThread(this);
  Event* previous = nullptr;
  if (eventRegistered) {
    auto eventWaitGuard = m_EventWaiters.acquire();
    LockGuard<Spinlock> guard(m_Lock);
    if (!m_bShutdown && m_Status != Zombie) {
      for (List<Event*>::Iterator it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
        if ((*it)->isSignalEvent() && (*it)->getNumber() == signalNumber &&
            (!rebindGeneration ||
             static_cast<SignalEvent*>(*it)->rebindGeneration() != rebindGeneration) &&
            (processDirected < 0 ||
             static_cast<SignalEvent*>(*it)->isProcessDirected() == (processDirected != 0))) {
          previous = *it;
          SignalEvent* oldSignal = static_cast<SignalEvent*>(previous);
          SignalEvent* newSignal = static_cast<SignalEvent*>(replacement);
          newSignal->setProcessDirected(oldSignal->isProcessDirected());
          newSignal->setSignalOrigin(oldSignal->getSignalCode(), oldSignal->getSenderProcess(),
                                     oldSignal->getSenderUser());
          newSignal->setContinuationEpoch(oldSignal->getContinuationEpoch());
          newSignal->setSignalValue(oldSignal->getSignalValue());
          oldSignal->transferDeliveryStateTo(*newSignal);
          newSignal->setQueueSequence(oldSignal->queueSequence());
          newSignal->setChildStatus(oldSignal->childStatus(), oldSignal->childUserTime(),
                                    oldSignal->childSystemTime());
          newSignal->setRebindGeneration(rebindGeneration);
          *it = replacement;
          break;
        }
      }
    }
  }

  if (eventRegistered && !previous) {
    replacement->deregisterThread(this);
  }
  if (previous) {
    previous->completeDelivery(this);
  }

  {
    auto senderGuard = m_EventSenderDrainWaiters.acquire();
    bool drained = false;
    {
      LockGuard<Spinlock> guard(m_Lock);
      assert(m_EventSendersInFlight);
      drained = !--m_EventSendersInFlight;
    }
    if (drained) {
      senderGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
    }
  }

  return previous != nullptr;
}

bool Thread::hasSignalEvent(size_t signalNumber, int processDirected) {
  LockGuard<Spinlock> guard(m_Lock);

  for (List<Event*>::Iterator it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
    if ((*it)->isSignalEvent() && (*it)->getNumber() == signalNumber &&
        (processDirected < 0 ||
         static_cast<SignalEvent*>(*it)->isProcessDirected() == (processDirected != 0))) {
      return true;
    }
  }
  return false;
}

bool Thread::acceptingEvents() {
  LockGuard<Spinlock> guard(m_Lock);
  return !m_bShutdown && m_Status != Zombie;
}

void Thread::setSynchronousSignalMask(uint64_t mask) {
  LockGuard<Spinlock> guard(m_Lock);
  m_SynchronousSignalMask = mask;
}

uint64_t Thread::getSynchronousSignalMask() {
  LockGuard<Spinlock> guard(m_Lock);
  return m_SynchronousSignalMask;
}

uint64_t Thread::pendingSignalMask(bool processOnly) {
  LockGuard<Spinlock> guard(m_Lock);
  uint64_t mask = 0;
  for (Event* event : m_EventQueue) {
    if (!event->isSignalEvent() || !event->getNumber() || event->getNumber() > 64) {
      continue;
    }
    auto* signal = static_cast<SignalEvent*>(event);
    if (signal->deliveryActive() && (!processOnly || signal->isProcessDirected())) {
      mask |= uint64_t(1) << (signal->getNumber() - 1);
    }
  }
  return mask;
}

uint64_t Thread::pendingSignalOrder(size_t number, bool processOnly) {
  LockGuard<Spinlock> guard(m_Lock);
  uint64_t sequence = ~uint64_t(0);
  for (Event* event : m_EventQueue) {
    if (!event->isSignalEvent() || event->getNumber() != number)
      continue;
    auto* signal = static_cast<SignalEvent*>(event);
    if (signal->deliveryActive() && (!processOnly || signal->isProcessDirected()) &&
        signal->queueSequence() < sequence)
      sequence = signal->queueSequence();
  }
  return sequence;
}

Event::Delivery Thread::reservePendingSignal(uint64_t mask, bool processOnly,
                                             uint64_t expectedSequence) {
  LockGuard<Spinlock> guard(m_Lock);
  auto selected = m_EventQueue.end();
  for (auto it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
    Event* event = *it;
    if (!event->isSignalEvent() || !event->getNumber() || event->getNumber() > 64) {
      continue;
    }
    auto* signal = static_cast<SignalEvent*>(event);
    if (signal->deliveryActive() && (!processOnly || signal->isProcessDirected()) &&
        (mask & (uint64_t(1) << (signal->getNumber() - 1))) &&
        (selected == m_EventQueue.end() || event->getNumber() < (*selected)->getNumber() ||
         (event->getNumber() == (*selected)->getNumber() &&
          signal->queueSequence() < static_cast<SignalEvent*>(*selected)->queueSequence()))) {
      selected = it;
    }
  }
  if (selected == m_EventQueue.end()) {
    return Event::Delivery();
  }
  Event* event = *selected;
  if (expectedSequence != ~uint64_t(0) &&
      static_cast<SignalEvent*>(event)->queueSequence() != expectedSequence)
    return Event::Delivery();
  m_EventQueue.erase(selected);
  return Event::Delivery(event, this);
}

bool Thread::restorePendingSignal(Event::Delivery& delivery) {
  if (!delivery || delivery.m_pThread != this || delivery.m_bActive) {
    return false;
  }
  {
    auto waitGuard = m_EventWaiters.acquire();
    LockGuard<Spinlock> guard(m_Lock);
    if (m_bShutdown || m_Status == Zombie)
      return false;
    m_EventQueue.pushFront(delivery.m_pEvent);
    delivery.m_pEvent = nullptr;
    delivery.m_pThread = nullptr;
  }
  wakeForDeliverableEvents();
  return true;
}

void Thread::cullSignalSource(const void* source) {
  Vector<Event*> retiring;
  {
    LockGuard<Spinlock> guard(m_Lock);
    for (auto it = m_EventQueue.begin(); it != m_EventQueue.end();) {
      if ((*it)->isSignalEvent() && static_cast<SignalEvent*>(*it)->deliverySource() == source) {
        retiring.pushBack(*it);
        it = m_EventQueue.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (Event* event : retiring) {
    event->completeDelivery(this);
  }
}

Event::Delivery Thread::getNextEvent(EventSelection selection) {
  Event* pResult = nullptr;

  {
    LockGuard<Spinlock> guard(m_Lock);

    if (__atomic_load_n(&m_EventDeferralDepth, __ATOMIC_ACQUIRE)) {
      return Event::Delivery();
    }

    auto selected = m_EventQueue.end();
    for (auto it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
      Event* event = *it;
      if (!eventIsDeliverableUnlocked(event, selection)) {
        continue;
      }
      if (selected == m_EventQueue.end()) {
        selected = it;
      } else if (event->isSignalEvent() && (*selected)->isSignalEvent() &&
                 (event->getNumber() < (*selected)->getNumber() ||
                  (event->getNumber() == (*selected)->getNumber() &&
                   static_cast<SignalEvent*>(event)->queueSequence() <
                       static_cast<SignalEvent*>(*selected)->queueSequence()))) {
        selected = it;
      }
      if (!event->isSignalEvent()) {
        break;
      }
    }
    if (selected != m_EventQueue.end()) {
      pResult = *selected;
      m_EventQueue.erase(selected);
    }
  }

  return pResult ? Event::Delivery(pResult, this) : Event::Delivery();
}

bool Thread::hasEvents() {
  LockGuard<Spinlock> guard(m_Lock);

  return hasEventsUnlocked();
}

bool Thread::eventIsDeliverableUnlocked(Event* event, EventSelection selection) {
  const size_t eventNumber = event->getNumber();
  if (m_UserReturnSignalParked && event->isSignalEvent() && eventNumber != 9)
    return false;
  if (event->isSignalEvent() &&
      (!static_cast<SignalEvent*>(event)->deliveryActive() ||
       (eventNumber > 0 && eventNumber <= 64 &&
        (m_SynchronousSignalMask & (uint64_t(1) << (eventNumber - 1)))))) {
    return false;
  }
  const bool signalInhibited =
      event->isSignalEvent() && eventNumber > 0 && eventNumber <= 64 &&
      (m_StateLevels[m_nStateLevel].m_SignalMask & (static_cast<uint64_t>(1) << (eventNumber - 1)));
  const bool exactUserReturnUnavailable =
      (selection == EventSelection::WithoutExactUserReturn &&
       eventNeedsUserReturnFrameUnlocked(event)) ||
      (selection != EventSelection::AnyDeliverable && m_SignalFramesRequired &&
       event->isSignalEvent() && eventNumber != 9 &&
       !m_StateLevels[m_nStateLevel].m_UserReturnFrame);
  bool processBlocksEvent = false;
  if (selection == EventSelection::StoppedProcessKernel) {
    // This policy is selected only after the caller has observed Suspended
    // under the event-wait handshake. Do not re-read Process state here: a
    // concurrent resume must not broaden this dequeue to a user handler.
    processBlocksEvent = event->getHandlerPrivilege() != Event::HandlerPrivilege::Kernel ||
                         !event->isDeliverableWhileProcessSuspended();
  } else {
    const Process::ProcessState processState = m_pParent ? m_pParent->getState() : Process::Active;
    processBlocksEvent =
        (selection == EventSelection::KernelDeliverable &&
         event->getHandlerPrivilege() != Event::HandlerPrivilege::Kernel) ||
        (processState != Process::Active &&
         event->getHandlerPrivilege() == Event::HandlerPrivilege::User) ||
        (processState == Process::Suspended && !event->isDeliverableWhileProcessSuspended());
  }
  return !m_StateLevels[m_nStateLevel].m_InhibitMask->test(eventNumber) && !signalInhibited &&
         !exactUserReturnUnavailable && !processBlocksEvent &&
         (event->getSpecificNestingLevel() == ~0UL ||
          event->getSpecificNestingLevel() == m_nStateLevel);
}

void Thread::markDeferredUserReturnSignalInterruption() {
  auto eventWaitGuard = m_EventWaiters.acquire();
  LockGuard<Spinlock> guard(m_Lock);

  bool caughtSignalDeferred = false;
  for (List<Event*>::Iterator it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
    Event* event = *it;
    if (event->isSignalEvent() && eventNeedsUserReturnFrameUnlocked(event) &&
        eventIsDeliverableUnlocked(event, EventSelection::AnyDeliverable)) {
      caughtSignalDeferred = true;
      break;
    }
  }
  if (!caughtSignalDeferred) {
    return;
  }

  StateLevel& current = m_StateLevels[m_nStateLevel];
  if (current.m_bDispatchingWaitEvent) {
    current.m_InterruptionReason = InterruptedBySignal;
  }

  // Unlike the generic event trampoline, this path does not push a state
  // level. Include the current level when locating a temporary-mask owner.
  for (size_t level = m_nStateLevel + 1; level > 0; --level) {
    StateLevel& owner = m_StateLevels[level - 1];
    if (!owner.m_TemporarySignalMaskActive) {
      continue;
    }

    owner.m_InterruptionReason = InterruptedBySignal;
    owner.m_TemporarySignalWaitInterrupted = true;
    break;
  }
}

bool Thread::hasEventsUnlocked(EventSelection selection) {
  for (List<Event*>::Iterator it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
    if (eventIsDeliverableUnlocked(*it, selection)) {
      return true;
    }
  }

  return false;
}

bool Thread::hasDeliverableEventsUnlocked(EventSelection selection) {
  return !__atomic_load_n(&m_EventDeferralDepth, __ATOMIC_ACQUIRE) && hasEventsUnlocked(selection);
}

void Thread::wakeForDeliverableEvents() {
  bool wakeThread = false;
  PerProcessorScheduler* readyScheduler = nullptr;
  {
    auto eventWaitGuard = m_EventWaiters.acquire();
    LockGuard<Spinlock> guard(m_Lock);
    if (!m_bShutdown && m_Status != Zombie && hasDeliverableEventsUnlocked()) {
      wakeThread = interruptWaitUnlocked(WaitQueue::WakeReason::Event, readyScheduler);
    }
  }

  if (wakeThread) {
    assert(readyScheduler);
    readyScheduler->publishReadyFromWait(this);
  }
}

bool Thread::hasEvent(Event* pEvent) {
  LockGuard<Spinlock> guard(m_Lock);

  for (List<Event*>::Iterator it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
    if ((*it) == pEvent) {
      return true;
    }
  }

  return false;
}

bool Thread::hasEvent(size_t eventNumber) {
  LockGuard<Spinlock> guard(m_Lock);

  for (List<Event*>::Iterator it = m_EventQueue.begin(); it != m_EventQueue.end(); ++it) {
    if ((*it)->getNumber() == eventNumber) {
      return true;
    }
  }

  return false;
}

void Thread::unexpectedExit() {}

uintptr_t Thread::getTlsBase() {
  if (!m_StateLevels[0].m_pKernelStack)
    return 0;

  // Solves a problem where threads are created pointing to different address
  // spaces than the process that creates them (for whatever reason). Because
  // this is usually only called right after the address space switch in
  // PerProcessorScheduler, the address space is set properly.
  if (!m_pTlsBase && !m_bTlsBaseOverride) {
    // Get ourselves some space.
    uintptr_t base = 0;
    if (m_pParent->getAddressSpace()->getDynamicStart())
      m_pParent->allocateUserRange(Process::UserRegion::Dynamic, THREAD_TLS_SIZE, base);
    else
      m_pParent->allocateUserRange(Process::UserRegion::Normal, THREAD_TLS_SIZE, base);

    if (!base) {
      // Failed to allocate space.
      NOTICE("Thread [" << Dec << m_pParent->getId() << ":" << m_Id << Hex
                        << "]: failed to allocate TLS area.");
      return base;
    }

    // Map.
    physical_uintptr_t phys = PhysicalMemoryManager::instance().allocatePage();
    m_pParent->getAddressSpace()->map(
        phys, reinterpret_cast<void*>(base),
        VirtualAddressSpace::Write | VirtualAddressSpace::RuntimeMapping);

    // Set up our thread ID to start with in the TLS region, now that it's
    // actually mapped into the address space.
    m_pTlsBase = reinterpret_cast<void*>(base);
    uint32_t* tlsBase = reinterpret_cast<uint32_t*>(m_pTlsBase);
#if BITS_64
    *tlsBase = static_cast<uint32_t>(m_Id);
#else
    *tlsBase = m_Id;
#endif

#if VERBOSE_KERNEL
    NOTICE("Thread [" << Dec << m_pParent->getId() << ":" << m_Id << Hex
                      << "]: allocated TLS area at " << m_pTlsBase << ".");
#endif
  }
  return reinterpret_cast<uintptr_t>(m_pTlsBase);
}

void Thread::resetTlsBase() {
  // The scheduler queries the base while restoring an interrupted Thread, so
  // the old value must not be unpublished until remapping is non-preemptible.
  EnsureInterrupts interrupts(false);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  TlsResetHook hook = __atomic_load_n(&g_TlsResetHook, __ATOMIC_ACQUIRE);
  Thread* hookTarget = __atomic_load_n(&g_TlsResetTarget, __ATOMIC_ACQUIRE);
  if (hook && hookTarget == this) {
    hook(this, TlsResetBeforeClear, 0);
  }
#endif
  m_pTlsBase = 0;
  m_bTlsBaseOverride = false;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (hook && hookTarget == this) {
    hook(this, TlsResetCleared, 0);
  }
#endif
  const uintptr_t tlsBase = getTlsBase();
  Processor::setTlsBase(tlsBase);
#if X64 && !HOSTED
  m_UserGsBase = 0;
  Processor::setUserGsBase(0);
#endif
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (hook && hookTarget == this) {
    hook(this, TlsResetRemapped, tlsBase);
  }
#endif
}

void Thread::setTlsBase(uintptr_t base) {
  /// \todo clean up old base
  m_bTlsBaseOverride = true;
  m_pTlsBase = reinterpret_cast<void*>(base);

  if (Processor::information().getCurrentThread() == this) {
    Processor::setTlsBase(getTlsBase());
  }
}

#if X64 && !HOSTED
void Thread::setUserGsBase(uintptr_t base) {
  EnsureInterrupts interrupts(false);
  m_UserGsBase = base;
  if (Processor::information().getCurrentThread() == this)
    Processor::setUserGsBase(base);
}

void Thread::saveUserGsBase() {
  // A userspace selector load can change the base without arch_prctl.
  m_UserGsBase = Processor::getUserGsBase();
}
#endif

bool Thread::join() {
  return joinInternal(false);
}

bool Thread::joinForCompletion() {
  TerminationDeferral terminationDeferral;
  return joinInternal(true);
}

bool Thread::joinInternal(bool completion) {
  Thread* pThisThread = Processor::information().getCurrentThread();
  if (pThisThread == this) {
    return false;
  }

  Process* pParent = nullptr;
  {
    auto guard = m_JoinWaiters.acquire();
    if (m_bDetached || m_bJoinClaimed) {
      return false;
    }
    pParent = m_pParent;
    if (!pParent->beginThreadJoin()) {
      return false;
    }
    m_bJoinClaimed = true;
  }

  JoinDiscardContext discard = {this, pParent, true};
  StackDiscardScope discardScope(&Thread::discardJoin, &discard);
  while (true) {
    bool reapable = false;
    {
      auto guard = m_JoinWaiters.acquire();
      if (m_bReapable) {
        reapable = true;
      } else {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        WaitQueue::WakeReason reason =
            completion
                ? guard.waitForCompletion(WaitQueue::Channel(), Thread::Joining, returnAddress)
                : guard.wait(WaitQueue::Channel(), Thread::Joining, returnAddress);
        if (reason == WaitQueue::WakeReason::Unwinding ||
            reason == WaitQueue::WakeReason::Terminating) {
          if (completion) {
            continue;
          }
          discardScope.disarm();
          discard.claimed = false;
          {
            auto claimGuard = m_JoinWaiters.acquire();
            m_bJoinClaimed = false;
          }
          pParent->endThreadJoin();
          return false;
        }
      }
    }

    if (!reapable) {
      continue;
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    JoinOperationHook hook = __atomic_load_n(&g_JoinOperationHook, __ATOMIC_ACQUIRE);
    if (hook) {
      hook(this, pParent);
    }
#endif

    // No new external inspector may enter once final retirement begins.
    // Existing inspectors finish before the target can be deleted.
    closeExternalLeaseAdmissionAndDrain();

    // Serialise the final ownership decision with Process::kill. The active
    // join operation pins the parent while deletion runs after this lock.
    bool processOwnsTarget = false;
    {
      RecursingLockGuard<Spinlock> processGuard(pParent->m_Lock);
      {
        auto claimGuard = m_JoinWaiters.acquire();
        if (!m_bReapable) {
          continue;
        }
        if (m_bProcessExitOwned) {
          m_bJoinClaimed = false;
          processOwnsTarget = true;
        }
      }

      // No callback can observe the target after the ownership decision
      // below deletes it. From here, ordinary code owns the claim release.
      discardScope.disarm();
      discard.claimed = false;
    }

    if (!processOwnsTarget) {
      requireThreadDestructionContext();
      delete this;
    }

    pParent->endThreadJoin();
    return !processOwnsTarget;
  }
}

bool Thread::beginExternalLease() {
  LockGuard<Spinlock> guard(m_ExternalLeaseLock);
  if (m_bExternalLeaseAdmissionClosed) {
    return false;
  }

  ++m_nExternalLeases;
  return true;
}

void Thread::endExternalLease() {
  bool wake = false;
  bool finalRelease = false;
  bool finishDetachedRetirement = false;
  {
    LockGuard<Spinlock> guard(m_ExternalLeaseLock);
    if (!m_nExternalLeases) {
      FATAL("Thread external lease underflow.");
    }

    --m_nExternalLeases;
    finalRelease = !m_nExternalLeases;
    finishDetachedRetirement = finalRelease && m_bExternalLeaseAdmissionClosed;
    if (finishDetachedRetirement) {
      m_bExternalLeaseReleaseInProgress = true;
    } else {
      // Open admission means no drainer can have enrolled yet.
      wake = false;
    }
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  Thread* hookTarget = __atomic_load_n(&g_ExternalLeaseReleaseTarget, __ATOMIC_ACQUIRE);
  if (finalRelease && hookTarget == this) {
    ExternalLeaseReleaseHook hook = __atomic_load_n(&g_ExternalLeaseReleaseHook, __ATOMIC_ACQUIRE);
    if (hook) {
      hook(this, ExternalLeaseFinalReleaseUnlocked);
    }
  }
#endif

  if (wake) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    ExternalLeaseReleaseHook hook = __atomic_load_n(&g_ExternalLeaseReleaseHook, __ATOMIC_ACQUIRE);
    if (hookTarget == this && hook) {
      hook(this, ExternalLeaseBeforeWaiterWake);
    }
#endif
    m_ExternalLeaseWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
  }

  if (!finishDetachedRetirement) {
    // A drainer closes admission under m_ExternalLeaseLock before testing
    // this count. An open final release therefore has nobody to wake, and
    // must not touch this Thread after the predicate lock is released.
    return;
  }

  // Process::ThreadLease keeps the parent pinned until this method returns.
  // Serialising with Process teardown lets the final lease perform deferred
  // detached deletion after the scheduler has already switched off-stack.
  Process* parent = m_pParent;
  bool deleteNow = false;
  {
    RecursingLockGuard<Spinlock> processGuard(parent->m_Lock);
    {
      auto joinGuard = m_JoinWaiters.acquire();
      deleteNow =
          m_bReapable && m_bDetached && !m_bProcessExitOwned && !m_bDetachedRetirementClaimed;
      if (deleteNow) {
        m_bDetachedRetirementClaimed = true;
      }
    }

    {
      LockGuard<Spinlock> leaseGuard(m_ExternalLeaseLock);
      m_bExternalLeaseReleaseInProgress = false;
    }

    if (!deleteNow) {
      // Keep the Process lock until the final queue access is complete.
      // Scheduler-side retirement takes the same lock, so it cannot
      // delete this Thread between clearing the handoff bit and waking
      // a completion waiter.
      m_ExternalLeaseWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
    }
  }

  if (deleteNow) {
    requireThreadDestructionContext();
    delete this;
  }
}

void Thread::closeExternalLeaseAdmission() {
  LockGuard<Spinlock> guard(m_ExternalLeaseLock);
  m_bExternalLeaseAdmissionClosed = true;
}

void Thread::closeExternalLeaseAdmissionAndDrain() {
  TerminationDeferral terminationDeferral;
  while (true) {
    auto guard = m_ExternalLeaseWaiters.acquire();
    {
      LockGuard<Spinlock> stateGuard(m_ExternalLeaseLock);
      m_bExternalLeaseAdmissionClosed = true;
      if (!m_nExternalLeases && !m_bExternalLeaseReleaseInProgress) {
        return;
      }
    }

    const WaitQueue::WakeReason reason = guard.waitForCompletion(
        WaitQueue::Channel(this), Thread::Joining, reinterpret_cast<uintptr_t>(this));
    (void)reason;
  }
}

void Thread::discardJoin(void* context) {
  JoinDiscardContext* discard = reinterpret_cast<JoinDiscardContext*>(context);
  if (!discard->claimed) {
    return;
  }

  Thread* target = discard->target;
  Process* parent = discard->parent;
  discard->claimed = false;
  discard->target = nullptr;
  discard->parent = nullptr;
  {
    auto guard = target->m_JoinWaiters.acquire();
    target->m_bJoinClaimed = false;
  }
  parent->endThreadJoin();
}

bool Thread::detach() {
  Process* pParent = m_pParent;
  if (!pParent->beginThreadJoin()) {
    return false;
  }

  bool deleteNow = false;
  bool joinInProgress = false;
  {
    RecursingLockGuard<Spinlock> processGuard(pParent->m_Lock);
    {
      auto guard = m_JoinWaiters.acquire();
      if (m_bJoinClaimed) {
        ERROR(
            "Thread::detach() called while other threads are "
            "joining.");
        joinInProgress = true;
      } else {
        m_bDetached = true;
        deleteNow = m_bReapable && !m_bProcessExitOwned && !m_bDetachedRetirementClaimed;
        if (deleteNow) {
          m_bDetachedRetirementClaimed = true;
        }
      }
    }
  }

  if (joinInProgress) {
    pParent->endThreadJoin();
    return false;
  }

  if (deleteNow) {
    closeExternalLeaseAdmissionAndDrain();
    {
      RecursingLockGuard<Spinlock> processGuard(pParent->m_Lock);
      {
        auto guard = m_JoinWaiters.acquire();
        deleteNow = deleteNow && m_bDetached && m_bReapable && !m_bProcessExitOwned &&
                    m_bDetachedRetirementClaimed;
      }
    }
    if (deleteNow) {
      requireThreadDestructionContext();
      delete this;
    }
  }

  pParent->endThreadJoin();
  return true;
}

Thread::StateLevel::StateLevel()
    : m_State(),
      m_pKernelStack(0),
      m_pUserStack(0),
      m_pAuxillaryStack(0),
      m_InhibitMask(),
      m_SignalMask(0),
      m_SavedSignalMask(0),
      m_TemporarySignalMaskActive(false),
      m_TemporarySignalWaitInterrupted(false),
      m_DeferredSignalMaskRestore(false),
      m_DispatchedSignalNumber(0),
      m_DispatchedSignalContinuationEpoch(0),
      m_bOwnsAlternateSignalStack(false),
      m_Errno(0),
      m_InterruptionReason(NotInterrupted),
      m_bDispatchingWaitEvent(false),
      m_ExecutionContext(ExecutionContext::WaitableThread),
      m_pRequestQueueCallback(nullptr),
      m_bTerminalWaitCancelledBeforeBlock(false)
#if HOSTED
      ,
      m_HostedSignalDepth(0)
#endif
{
  m_State = new SchedulerState;
  ByteSet(m_State, 0, sizeof(SchedulerState));
  m_InhibitMask = SharedPointer<ExtensibleBitmap>::allocate();
}

Thread::StateLevel::~StateLevel() {
  delete m_State;
}

Thread::StateLevel::StateLevel(const Thread::StateLevel& s)
    : m_State(),
      m_pKernelStack(s.m_pKernelStack),
      m_pUserStack(s.m_pUserStack),
      m_pAuxillaryStack(s.m_pAuxillaryStack),
      m_InhibitMask(),
      m_SignalMask(s.m_SignalMask),
      m_SavedSignalMask(0),
      m_TemporarySignalMaskActive(false),
      m_TemporarySignalWaitInterrupted(false),
      m_DeferredSignalMaskRestore(false),
      m_DispatchedSignalNumber(0),
      m_DispatchedSignalContinuationEpoch(0),
      m_bOwnsAlternateSignalStack(false),
      m_Errno(s.m_Errno),
      m_InterruptionReason(s.m_InterruptionReason),
      m_bDispatchingWaitEvent(false),
      m_ExecutionContext(s.m_ExecutionContext),
      m_pRequestQueueCallback(nullptr),
      m_bTerminalWaitCancelledBeforeBlock(false)
#if HOSTED
      ,
      m_HostedSignalDepth(0)
#endif
{
  m_State = new SchedulerState(*(s.m_State));
  m_InhibitMask = SharedPointer<ExtensibleBitmap>::allocate(*(s.m_InhibitMask));
}

Thread::StateLevel& Thread::StateLevel::operator=(const Thread::StateLevel& s) {
  m_State = new SchedulerState(*(s.m_State));
  m_InhibitMask = SharedPointer<ExtensibleBitmap>::allocate(*(s.m_InhibitMask));
  m_SignalMask = s.m_SignalMask;
  m_SavedSignalMask = 0;
  m_TemporarySignalMaskActive = false;
  m_TemporarySignalWaitInterrupted = false;
  m_DeferredSignalMaskRestore = false;
  m_DispatchedSignalNumber = 0;
  m_DispatchedSignalContinuationEpoch = 0;
  m_bOwnsAlternateSignalStack = false;
  m_Errno = s.m_Errno;
  m_InterruptionReason = s.m_InterruptionReason;
  m_bDispatchingWaitEvent = false;
  m_ExecutionContext = s.m_ExecutionContext;
  m_pRequestQueueCallback = nullptr;
  m_bTerminalWaitCancelledBeforeBlock = false;
#if HOSTED
  m_HostedSignalDepth = 0;
#endif
  m_pKernelStack = s.m_pKernelStack;
  return *this;
}

void Thread::markTimeoutInterruptedWait() {
  const size_t interruptedLevel = m_nStateLevel ? m_nStateLevel - 1 : 0;
  m_StateLevels[interruptedLevel].m_InterruptionReason = InterruptedByTimeout;
}

void Thread::markSignalInterruptedWait() {
  auto eventWaitGuard = m_EventWaiters.acquire();
  LockGuard<Spinlock> guard(m_Lock);
  if (!m_nStateLevel) {
    return;
  }

  StateLevel& interrupted = m_StateLevels[m_nStateLevel - 1];
  if (interrupted.m_bDispatchingWaitEvent) {
    interrupted.m_InterruptionReason = InterruptedBySignal;
  }

  // A non-signal kernel handler can nest between the blocking syscall and the
  // signal handler. The inherited effective mask still belongs to the nearest
  // armed ancestor, so retain the interruption there rather than on every
  // suspended temporary wait.
  for (size_t level = m_nStateLevel; level > 0; --level) {
    StateLevel& owner = m_StateLevels[level - 1];
    if (!owner.m_TemporarySignalMaskActive) {
      continue;
    }

    owner.m_InterruptionReason = InterruptedBySignal;
    owner.m_TemporarySignalWaitInterrupted = true;
    break;
  }
}

bool Thread::eventsDeferred() const {
  return __atomic_load_n(&m_EventDeferralDepth, __ATOMIC_ACQUIRE) != 0;
}

bool Thread::getWaitDebugInfo(WaitDebugInfo& info) {
  const size_t level = __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
  if (level >= MAX_NESTED_EVENTS) {
    return false;
  }

  WaitQueue::Waiter& waiter = m_StateLevels[level].m_Waiter;
  WaitQueue* queue = waiter.loadQueue();
  if (!queue) {
    return false;
  }

  info.queue = queue;
  WaitQueue::Channel channel;
  if (!waiter.snapshotChannel(channel)) {
    return false;
  }
  info.channelOwner = channel.owner;
  info.channelValue = channel.value;
  info.reason = waiter.loadReason();
  info.stateLevel = waiter.stateLevel;
  info.queued = waiter.isQueued();

  // A concurrent wake may unpublish this persistent record. Reject a torn
  // snapshot rather than taking the target lock, which may be frozen by the
  // kernel debugger.
  return waiter.loadQueue() == queue && __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE) == level;
}

void Thread::deferEvents() {
  __atomic_add_fetch(&m_EventDeferralDepth, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
  markUserReturnWorkFlag(UserReturnEventsDeferred);
}

void Thread::resumeEvents() {
  const size_t depth = __atomic_load_n(&m_EventDeferralDepth, __ATOMIC_ACQUIRE);
  if (!depth) {
    FATAL("Unbalanced event-delivery deferral.");
  }
  if (__atomic_sub_fetch(&m_EventDeferralDepth, static_cast<size_t>(1), __ATOMIC_ACQ_REL) == 0) {
    clearUserReturnWorkFlag(UserReturnEventsDeferred);
  }
}

void Thread::deferTermination() {
  __atomic_add_fetch(&m_TerminationDeferralDepth, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
}

void Thread::resumeTermination() {
  const size_t depth = __atomic_load_n(&m_TerminationDeferralDepth, __ATOMIC_ACQUIRE);
  if (!depth) {
    FATAL("Unbalanced terminal-teardown deferral.");
  }
  __atomic_sub_fetch(&m_TerminationDeferralDepth, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
}

void Thread::registerFreshTerminationDeferral(DeferredScopeRecord& record) {
  // Only TerminationDeferral's constructor calls this with unpublished storage.
  // Keep the normal record and publication protocol without zeroing it first.
  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);

  const size_t level = __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
  const size_t sequence =
      __atomic_add_fetch(&m_NextStateCleanupSequence, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
  if (!sequence) {
    FATAL("Thread state cleanup sequence exhausted.");
  }

  record.stateLevel = level;
  record.sequence = sequence;
  record.defersTermination = true;
  record.defersEvents = false;
  record.cleanup = nullptr;
  record.context = nullptr;
  record.armed = true;
  deferTermination();

  DeferredScopeRecord* head = __atomic_load_n(&m_pDeferredScopes[level], __ATOMIC_ACQUIRE);
  do {
    record.next = head;
  } while (!__atomic_compare_exchange_n(&m_pDeferredScopes[level], &head, &record, false,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));

  Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::registerDeferredScope(DeferredScopeRecord& record, bool termination, bool events) {
  if (record.armed || record.next || record.defersTermination || record.defersEvents ||
      record.sequence || record.cleanup || record.context) {
    FATAL("Deferred scope registered more than once.");
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);

  const size_t level = __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
  const size_t sequence =
      __atomic_add_fetch(&m_NextStateCleanupSequence, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
  if (!sequence) {
    FATAL("Thread state cleanup sequence exhausted.");
  }

  record.stateLevel = level;
  record.sequence = sequence;
  record.defersTermination = termination;
  record.defersEvents = events;
  record.cleanup = nullptr;
  record.context = nullptr;
  record.armed = true;

  if (termination) {
    deferTermination();
  }
  if (events) {
    deferEvents();
  }

  DeferredScopeRecord* head = __atomic_load_n(&m_pDeferredScopes[level], __ATOMIC_ACQUIRE);
  do {
    record.next = head;
  } while (!__atomic_compare_exchange_n(&m_pDeferredScopes[level], &head, &record, false,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));

  Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::armStateCleanup(DeferredScopeRecord& record, DeferredScopeRecord::Cleanup cleanup,
                             void* context) {
  if (!cleanup) {
    FATAL("State cleanup armed without a callback.");
  }

  if (record.armed || record.next || record.defersTermination || record.defersEvents ||
      record.sequence || record.cleanup || record.context) {
    FATAL("State cleanup record armed more than once.");
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);

  const size_t level = __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
  const size_t sequence =
      __atomic_add_fetch(&m_NextStateCleanupSequence, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
  if (!sequence) {
    FATAL("Thread state cleanup sequence exhausted.");
  }

  record.stateLevel = level;
  record.sequence = sequence;
  record.cleanup = cleanup;
  record.context = context;
  record.armed = true;

  DeferredScopeRecord* head = __atomic_load_n(&m_pDeferredScopes[level], __ATOMIC_ACQUIRE);
  do {
    record.next = head;
  } while (!__atomic_compare_exchange_n(&m_pDeferredScopes[level], &head, &record, false,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));

  Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::unregisterDeferredScope(DeferredScopeRecord& record) {
  if (!record.armed || record.stateLevel >= MAX_NESTED_EVENTS) {
    FATAL("Deferred scope was not registered on this Thread.");
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);

  DeferredScopeRecord* expected = &record;
  if (!__atomic_compare_exchange_n(&m_pDeferredScopes[record.stateLevel], &expected, record.next,
                                   false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    FATAL("Deferred scopes were not released in LIFO order.");
  }

  if (record.defersTermination) {
    resumeTermination();
  }
  if (record.defersEvents) {
    resumeEvents();
  }
  record = DeferredScopeRecord();

  Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::disarmStateCleanup(DeferredScopeRecord& record) {
  if (!record.armed || record.stateLevel >= MAX_NESTED_EVENTS) {
    FATAL("State cleanup record was not armed on this Thread.");
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);

  DeferredScopeRecord* expected = &record;
  if (!__atomic_compare_exchange_n(&m_pDeferredScopes[record.stateLevel], &expected, record.next,
                                   false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    FATAL("State cleanup records were not disarmed in LIFO order.");
  }

  record = DeferredScopeRecord();

  Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::unregisterTerminationDeferral(DeferredScopeRecord& record) {
  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  if (!record.armed || !record.defersTermination || record.defersEvents || record.cleanup ||
      record.stateLevel >= MAX_NESTED_EVENTS) {
    FATAL("Invalid termination deferral retirement.");
  }

  DeferredScopeRecord* previous = nullptr;
  DeferredScopeRecord* current =
      __atomic_load_n(&m_pDeferredScopes[record.stateLevel], __ATOMIC_ACQUIRE);
  while (current && current != &record) {
    previous = current;
    current = current->next;
  }
  if (!current) {
    FATAL("Termination deferral was absent from its Thread.");
  }

  if (previous) {
    previous->next = record.next;
  } else {
    __atomic_store_n(&m_pDeferredScopes[record.stateLevel], record.next, __ATOMIC_RELEASE);
  }
  resumeTermination();
  record = DeferredScopeRecord();

  Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::moveTerminationDeferral(DeferredScopeRecord& from, DeferredScopeRecord& to) {
  if (&from == &to || !from.armed || from.stateLevel >= MAX_NESTED_EVENTS) {
    FATAL("Moved termination deferral was not registered.");
  }
  if (!from.defersTermination || from.defersEvents || from.cleanup || to.armed || to.next ||
      to.defersTermination || to.defersEvents || to.sequence || to.cleanup || to.context) {
    FATAL("Invalid termination deferral move.");
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);

  DeferredScopeRecord* previous = nullptr;
  DeferredScopeRecord* current =
      __atomic_load_n(&m_pDeferredScopes[from.stateLevel], __ATOMIC_ACQUIRE);
  while (current && current != &from) {
    previous = current;
    current = current->next;
  }
  if (!current) {
    FATAL("Moved termination deferral was absent from its Thread.");
  }

  to = from;
  if (previous) {
    previous->next = &to;
  } else {
    __atomic_store_n(&m_pDeferredScopes[from.stateLevel], &to, __ATOMIC_RELEASE);
  }
  from = DeferredScopeRecord();

  Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::retireDeferredScopes(bool allStateLevels, size_t stateLevel) {
  retireDeferredScopesMatching(allStateLevels, stateLevel, false, 0);
}

size_t Thread::stateCleanupCheckpoint() {
  return __atomic_load_n(&m_NextStateCleanupSequence, __ATOMIC_ACQUIRE);
}

void Thread::retireDeferredScopesAfter(size_t checkpoint) {
  retireDeferredScopesMatching(true, 0, true, checkpoint);
}

void Thread::retireDeferredScopesMatching(bool allStateLevels, size_t stateLevel,
                                          bool newerThanCheckpoint, size_t checkpoint) {
  DeferredScopeRecord* retired = nullptr;
  DeferredScopeRecord* retiredTail = nullptr;

  if (!allStateLevels && stateLevel >= MAX_NESTED_EVENTS) {
    FATAL("State cleanup retirement has an invalid level.");
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);

  while (true) {
    DeferredScopeRecord* candidate = nullptr;
    size_t candidateLevel = 0;

    for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level) {
      if (!allStateLevels && level != stateLevel) {
        continue;
      }

      DeferredScopeRecord* head = __atomic_load_n(&m_pDeferredScopes[level], __ATOMIC_ACQUIRE);
      if (!head || (newerThanCheckpoint && head->sequence <= checkpoint)) {
        continue;
      }
      if (!head->armed || head->stateLevel != level || !head->sequence) {
        FATAL("Corrupt Thread state cleanup publication.");
      }
      if (!candidate || head->sequence > candidate->sequence) {
        candidate = head;
        candidateLevel = level;
      }
    }

    if (!candidate) {
      break;
    }

    DeferredScopeRecord* expected = candidate;
    if (!__atomic_compare_exchange_n(&m_pDeferredScopes[candidateLevel], &expected, candidate->next,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      continue;
    }

    if (candidate->defersTermination) {
      resumeTermination();
    }
    if (candidate->defersEvents) {
      resumeEvents();
    }
    candidate->armed = false;
    candidate->next = nullptr;
    if (retiredTail) {
      retiredTail->next = candidate;
    } else {
      retired = candidate;
    }
    retiredTail = candidate;
  }

  Processor::setInterrupts(interruptsWereEnabled);

  while (retired) {
    DeferredScopeRecord* next = retired->next;
    DeferredScopeRecord::Cleanup cleanup = retired->cleanup;
    void* context = retired->context;
    *retired = DeferredScopeRecord();
    if (cleanup) {
      cleanup(context);
    }
    retired = next;
  }
}

void Thread::armAtomicStateCleanup(AtomicStateCleanupRecord& record,
                                   AtomicStateCleanupRecord::Cleanup cleanup, void* context) {
  if (Processor::information().getCurrentThread() != this) {
    FATAL("Interrupt/exception cleanup armed for a non-current Thread.");
  }
  armStateCleanup(record, cleanup, context);
}

#if HOSTED
size_t Thread::enterHostedSignalHandler() {
  const size_t level = __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
  __atomic_add_fetch(&m_StateLevels[level].m_HostedSignalDepth, 1, __ATOMIC_ACQ_REL);
  return level;
}

void Thread::leaveHostedSignalHandler(size_t stateLevel) {
  if (UNLIKELY(stateLevel >= MAX_NESTED_EVENTS)) {
    FATAL_NOLOCK("Hosted signal frame recorded an invalid state level");
    return;
  }

  const size_t previous =
      __atomic_fetch_sub(&m_StateLevels[stateLevel].m_HostedSignalDepth, 1, __ATOMIC_ACQ_REL);
  if (UNLIKELY(!previous)) {
    __atomic_store_n(&m_StateLevels[stateLevel].m_HostedSignalDepth, 0, __ATOMIC_RELEASE);
    FATAL_NOLOCK("Hosted Thread signal-frame depth underflowed");
  }
}
#endif

void Thread::disarmAtomicStateCleanup(AtomicStateCleanupRecord& record) {
  if (Processor::information().getCurrentThread() != this) {
    FATAL("Interrupt/exception cleanup disarmed for a non-current Thread.");
  }
  disarmStateCleanup(record);
}

void Thread::setScheduler(class PerProcessorScheduler* pScheduler) {
  __atomic_store_n(&m_pScheduler, pScheduler, __ATOMIC_RELEASE);
}

void Thread::cleanStateLevel(size_t level) {
  if (__atomic_load_n(&m_pDeferredScopes[level], __ATOMIC_ACQUIRE)) {
    FATAL("Thread state stack freed with an armed cleanup record.");
  }
  if (m_StateLevels[level].m_TemporarySignalMaskActive) {
    FATAL("Thread state stack freed with an active temporary signal mask.");
  }
  restoreDeferredSignalMask(level);

#if HOSTED
  if (__atomic_load_n(&m_StateLevels[level].m_HostedSignalDepth, __ATOMIC_ACQUIRE)) {
    FATAL("Thread state stack freed with a live hosted signal frame.");
  }
#endif

  if (m_StateLevels[level].m_Waiter.loadQueue()) {
    FATAL("Thread state stack was cleaned while still in a wait queue.");
  }

  if (m_StateLevels[level].m_pKernelStack) {
    VirtualAddressSpace::getKernelAddressSpace().freeStack(m_StateLevels[level].m_pKernelStack);
    m_StateLevels[level].m_pKernelStack = 0;
  } else if (m_StateLevels[level].m_pAuxillaryStack) {
    VirtualAddressSpace::getKernelAddressSpace().freeStack(m_StateLevels[level].m_pAuxillaryStack);
    m_StateLevels[level].m_pAuxillaryStack = 0;
  }

  if (m_StateLevels[level].m_pUserStack && m_pParent) {
    // Can't use Processor::getCurrent.. as by the time we're called
    // we may have switched address spaces to allow the thread to die.
    if (m_StateLevels[level].m_pUserStack != m_pInputUserStack)
      m_pParent->getAddressSpace()->freeStack(m_StateLevels[level].m_pUserStack);
    m_StateLevels[level].m_pUserStack = 0;
  }

  m_StateLevels[level].m_InhibitMask.reset();
  m_StateLevels[level].m_SavedSignalMask = 0;
  m_StateLevels[level].m_TemporarySignalWaitInterrupted = false;
  m_StateLevels[level].m_DispatchedSignalNumber = 0;
  m_StateLevels[level].m_DispatchedSignalContinuationEpoch = 0;
  m_StateLevels[level].m_bOwnsAlternateSignalStack = false;
  m_StateLevels[level].m_ExecutionContext.reset();
  m_StateLevels[level].m_pRequestQueueCallback = nullptr;
  m_StateLevels[level].m_bTerminalWaitCancelledBeforeBlock = false;
}

void Thread::setUnwindState(UnwindType ut) {
  bool becameReady = false;
  bool queuedBeforeStart = false;
  PerProcessorScheduler* readyScheduler = nullptr;
  {
    LockGuard<Spinlock> guard(m_Lock);
    __atomic_store_n(&m_UnwindState, ut, __ATOMIC_RELEASE);
    queuedBeforeStart = m_Status == Created && ut == TerminateThread;
    if (ut != Continue) {
      markUserReturnWorkPending();
      const bool terminating = ut == TerminateThread;
      becameReady = interruptWaitUnlocked(
          terminating ? WaitQueue::WakeReason::Terminating : WaitQueue::WakeReason::Unwinding,
          readyScheduler);
    }
  }

  if (becameReady) {
    assert(readyScheduler);
    readyScheduler->publishReadyFromWait(this);
  } else if (queuedBeforeStart) {
    Scheduler::instance().threadStatusChanged(this);
  }
}

void Thread::deferProcessExit(int code) {
  // Exec can forward a sibling's request. Publish status and cause together
  // so a concurrent local failure cannot create a mixed exit reason.
  __atomic_store_n(&m_DeferredProcessExitRequest, static_cast<uint32_t>(code), __ATOMIC_RELEASE);
  setUnwindState(Exit);
}

void Thread::deferSignalExit(int signal) {
  const uint64_t request = static_cast<uint32_t>(signal) | (static_cast<uint64_t>(1) << 32);
  __atomic_store_n(&m_DeferredProcessExitRequest, request, __ATOMIC_RELEASE);
  setUnwindState(Exit);
}

Thread::DeferredProcessExit Thread::takeDeferredProcessExit() {
  const uint64_t value =
      __atomic_exchange_n(&m_DeferredProcessExitRequest, uint64_t(0), __ATOMIC_ACQ_REL);
  DeferredProcessExit request = {static_cast<int32_t>(value), (value >> 32)
                                                                  ? Subsystem::ExitCause::Signal
                                                                  : Subsystem::ExitCause::Normal};
  return request;
}

bool Thread::deferSubsystemException(size_t type, uintptr_t faultAddress, uintptr_t errorCode) {
  size_t expected = 0;
  if (!__atomic_compare_exchange_n(&m_DeferredSubsystemExceptionState, &expected, 1, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    FATAL_NOLOCK("Nested subsystem exception reached an occupied deferred slot");
    return false;
  }

  m_DeferredSubsystemExceptionType = type;
  m_DeferredSubsystemExceptionFaultAddress = faultAddress;
  m_DeferredSubsystemExceptionErrorCode = errorCode;
  __atomic_store_n(&m_DeferredSubsystemExceptionState, 2, __ATOMIC_RELEASE);
  markUserReturnWorkFlag(UserReturnDeferredException);
  return true;
}

bool Thread::takeDeferredSubsystemException(size_t& type, uintptr_t& faultAddress,
                                            uintptr_t& errorCode) {
  if (__atomic_load_n(&m_DeferredSubsystemExceptionState, __ATOMIC_ACQUIRE) != 2) {
    return false;
  }

  type = m_DeferredSubsystemExceptionType;
  faultAddress = m_DeferredSubsystemExceptionFaultAddress;
  errorCode = m_DeferredSubsystemExceptionErrorCode;
  __atomic_store_n(&m_DeferredSubsystemExceptionState, 0, __ATOMIC_RELEASE);
  clearUserReturnWorkFlag(UserReturnDeferredException);
  return true;
}

bool Thread::interruptWaitUnlocked(WaitQueue::WakeReason reason,
                                   PerProcessorScheduler*& readyScheduler) {
  readyScheduler = nullptr;
  WaitQueue::Waiter& waiter = m_StateLevels[m_nStateLevel].m_Waiter;
  if (!waiter.loadQueue() || waiter.loadReason() != WaitQueue::WakeReason::Waiting) {
    return false;
  }

  waiter.storeReason(reason);

  if (m_Status == Sleeping) {
    m_Status = Ready;
    __atomic_store_n(&m_ReadyPublicationPending, true, __ATOMIC_RELEASE);
    readyScheduler = waiter.scheduler;
    assert(readyScheduler);
    return true;
  }
  return false;
}

bool Thread::hasActiveWaitUnlocked() const {
  return m_StateLevels[m_nStateLevel].m_Waiter.loadQueue() != nullptr;
}

bool Thread::hasActiveWaitAtAnyLevel() const {
  for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level) {
    if (m_StateLevels[level].m_Waiter.loadQueue()) {
      return true;
    }
  }
  return false;
}

bool Thread::activeWaitPendingUnlocked() const {
  const WaitQueue::Waiter& waiter = m_StateLevels[m_nStateLevel].m_Waiter;
  return waiter.loadQueue() && waiter.loadReason() == WaitQueue::WakeReason::Waiting;
}

void Thread::markTerminalWaitCancelledBeforeBlockUnlocked(size_t level) {
  assert(level < MAX_NESTED_EVENTS);
  StateLevel& state = m_StateLevels[level];
  assert(!state.m_Waiter.loadQueue());
  assert(state.m_Waiter.loadReason() == WaitQueue::WakeReason::Terminating);
  state.m_bTerminalWaitCancelledBeforeBlock = true;
}

bool Thread::consumeTerminalWaitCancelledBeforeBlockUnlocked() {
  StateLevel& state = m_StateLevels[m_nStateLevel];
  if (!state.m_bTerminalWaitCancelledBeforeBlock) {
    return false;
  }

  // A new enrolment always clears the marker. Treat any other mismatch as
  // stale rather than allowing it to suppress an unrelated sleep.
  state.m_bTerminalWaitCancelledBeforeBlock = false;
  return !state.m_Waiter.loadQueue() &&
         state.m_Waiter.loadReason() == WaitQueue::WakeReason::Terminating;
}

void Thread::clearTerminalWaitCancelledBeforeBlockUnlocked(size_t level) {
  assert(level < MAX_NESTED_EVENTS);
  m_StateLevels[level].m_bTerminalWaitCancelledBeforeBlock = false;
}

bool Thread::markReapable() {
  auto guard = m_JoinWaiters.acquire();
  m_bReapable = true;
  if (!m_bDetached) {
    guard.wakeAll();
  }

  bool externalLeasesDrained = false;
  {
    LockGuard<Spinlock> leaseGuard(m_ExternalLeaseLock);
    externalLeasesDrained =
        m_bExternalLeaseAdmissionClosed && !m_nExternalLeases && !m_bExternalLeaseReleaseInProgress;
  }
  const bool deleteNow =
      m_bDetached && !m_bProcessExitOwned && externalLeasesDrained && !m_bDetachedRetirementClaimed;
  if (deleteNow) {
    m_bDetachedRetirementClaimed = true;
  }
  return deleteNow;
}

#endif  // THREADS
