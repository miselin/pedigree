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

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/MemoryAllocator.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/ZombieQueue.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/users/Group.h"
#include "modules/system/users/User.h"
#include "modules/system/vfs/File.h"

namespace {
SharedPointer<UserspacePidNamespace> g_DefaultUserspacePidNamespace;

SharedPointer<UserspacePidNamespace> defaultUserspacePidNamespace() {
  if (!g_DefaultUserspacePidNamespace) {
    g_DefaultUserspacePidNamespace = SharedPointer<UserspacePidNamespace>::tryAllocate();
  }
  return g_DefaultUserspacePidNamespace;
}
}  // namespace

Process* Process::m_pInitProcess = 0;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
Process::TerminationElectionHook Process::m_TerminationElectionHook = nullptr;
Process::ExternalLeaseReleaseHook Process::m_ExternalLeaseReleaseHook = nullptr;
Process* Process::m_ExternalLeaseReleaseTarget = nullptr;
Process::OrphanPublicationHook Process::m_OrphanPublicationHook = nullptr;
#endif

namespace {
bool canAdoptChildren(Process* pProcess, Process* pExcluded = 0) {
  if (!pProcess || pProcess == pExcluded) {
    return false;
  }

  const Process::ProcessState state = pProcess->getState();
  return state == Process::Active || state == Process::Suspended;
}

}  // namespace

Process::ExecScope::ExecScope(Process& process, bool active)
    : m_pProcess(nullptr), m_bAdmitted(!active), m_TerminationDeferral(active) {
  if (!active) {
    return;
  }
  Thread* current = Processor::information().getCurrentThread();
  LockGuard<Spinlock> guard(process.m_Lock);
  if (current && current->getParent() == &process && !process.m_pExecOwner &&
      process.getState() == Active && !process.m_bTerminalOwnerReserved &&
      current->getUnwindState() == Thread::Continue) {
    process.m_pExecOwner = current;
    m_pProcess = &process;
    m_bAdmitted = true;
  }
}

Process::ExecScope::~ExecScope() {
  if (!m_pProcess) {
    return;
  }
  {
    LockGuard<Spinlock> guard(m_pProcess->m_Lock);
    if (m_pProcess->m_bExecCommitted) {
      auto joinGuard = m_pProcess->m_ThreadJoinWaiters.acquire();
      m_pProcess->m_bThreadJoinAdmissionClosed = false;
    }
    m_pProcess->m_bExecCommitted = false;
    m_pProcess->m_bExecExitForwarded = false;
    m_pProcess->m_pExecOwner = nullptr;
  }
  m_pProcess->m_ExecWaiters.wakeAll();
}

Process::ThreadCreationScope::ThreadCreationScope(Process& process)
    : m_pProcess(nullptr), m_TerminationDeferral(true) {
  LockGuard<Spinlock> guard(process.m_Lock);
  if (!process.m_pExecOwner && process.getState() == Active && !process.m_bTerminalOwnerReserved) {
    ++process.m_nThreadCreations;
    m_pProcess = &process;
  }
}

Process::ThreadCreationScope::~ThreadCreationScope() {
  if (!m_pProcess) {
    return;
  }
  {
    LockGuard<Spinlock> guard(m_pProcess->m_Lock);
    --m_pProcess->m_nThreadCreations;
  }
  m_pProcess->m_ExecWaiters.wakeAll();
}

bool Process::ExecScope::commit() {
  if (!m_pProcess) {
    return m_bAdmitted;
  }
  Process& process = *m_pProcess;
  Thread* current = Processor::information().getCurrentThread();
  Vector<Thread*> peers;
  while (true) {
    auto progress = process.m_ExecWaiters.acquire();
    bool creatorsFinished = false;
    {
      LockGuard<Spinlock> guard(process.m_Lock);
      if (process.getState() != Active || process.m_bTerminalOwnerReserved ||
          current->getUnwindState() != Thread::Continue) {
        return false;
      }
      creatorsFinished = process.m_nThreadCreations == 0;
      if (creatorsFinished) {
        // Retain peers before waking them. Detached retirement and kernel
        // joins must not free a pointer in this teardown snapshot.
        process.m_bExecCommitted = true;
        for (Thread* thread : process.m_Threads) {
          auto exitGuard = thread->m_JoinWaiters.acquire();
          if (thread != current && thread->m_bReapable &&
              (thread->m_bDetachedRetirementClaimed || thread->m_bJoinClaimed)) {
            continue;
          }
          thread->m_bProcessExitOwned = true;
          if (!thread->m_bReapable) {
            thread->m_bProcessExitParticipant = true;
            ++process.m_nTerminationParticipants;
          }
          if (thread != current) {
            peers.pushBack(thread);
          }
        }
        auto joinGuard = process.m_ThreadJoinWaiters.acquire();
        process.m_bThreadJoinAdmissionClosed = true;
      }
    }
    if (creatorsFinished) {
      break;
    }
    const WaitQueue::WakeReason reason = progress.waitForCompletion(
        WaitQueue::Channel(), Thread::ProcessWait, reinterpret_cast<uintptr_t>(&process));
    (void)reason;
  }

  if (process.m_pSubsystem) {
    process.m_pSubsystem->prepareThreadsForExec(current);
  }

  for (Thread* peer : peers) {
    peer->setUnwindState(Thread::TerminateThread);
  }
  while (true) {
    auto progress = process.m_ExecWaiters.acquire();
    bool peersOffStack = false;
    {
      LockGuard<Spinlock> guard(process.m_Lock);
      peersOffStack = process.m_nTerminationParticipants == 1;
    }
    if (peersOffStack) {
      break;
    }
    const WaitQueue::WakeReason reason = progress.waitForCompletion(
        WaitQueue::Channel(), Thread::ProcessWait, reinterpret_cast<uintptr_t>(&process));
    (void)reason;
  }

  // A join can outlive the target's final context switch. Drain its ownership
  // decision before deleting retained threads or reopening join admission.
  while (true) {
    auto joins = process.m_ThreadJoinWaiters.acquire();
    if (!process.m_nThreadJoinOperations) {
      break;
    }
    const WaitQueue::WakeReason reason = joins.waitForCompletion(
        WaitQueue::Channel(&process), Thread::ProcessWait, reinterpret_cast<uintptr_t>(&process));
    (void)reason;
  }
  for (Thread* peer : peers) {
    peer->closeExternalLeaseAdmissionAndDrain();
    delete peer;
  }
  while (true) {
    auto progress = process.m_ExecWaiters.acquire();
    {
      LockGuard<Spinlock> guard(process.m_Lock);
      if (process.m_Threads.count() == 1) {
        current->m_bProcessExitOwned = false;
        current->m_bProcessExitParticipant = false;
        process.m_nTerminationParticipants = 0;
        return true;
      }
    }
    // A detached deletion claimed before our snapshot owns its remaining
    // TLS and stack cleanup; removeThread publishes its final completion.
    const WaitQueue::WakeReason reason = progress.waitForCompletion(
        WaitQueue::Channel(), Thread::ProcessWait, reinterpret_cast<uintptr_t>(&process));
    (void)reason;
  }
}

void Process::ExecScope::adoptLeaderIdentity() {
  if (m_pProcess) {
    LockGuard<Spinlock> guard(m_pProcess->m_Lock);
    __atomic_store_n(&m_pProcess->m_pExecOwner->m_TaskId, m_pProcess->m_Id, __ATOMIC_RELEASE);
  }
}

Process::ThreadLease::ThreadLease()
    : m_pProcess(nullptr), m_pThread(nullptr), m_TerminationDeferral(false) {}

Process::ReaperClaim::ReaperClaim() : m_pProcess(nullptr), m_TerminationDeferral(false) {}

Process::ReaperClaim::ReaperClaim(Process* process)
    : m_pProcess(process), m_TerminationDeferral(process != nullptr) {}

Process::ReaperClaim::ReaperClaim(ReaperClaim&& other) noexcept
    : m_pProcess(other.m_pProcess),
      m_TerminationDeferral(pedigree_std::move(other.m_TerminationDeferral)) {
  other.m_pProcess = nullptr;
}

Process::ReaperClaim::~ReaperClaim() {
  if (m_pProcess) {
    FATAL("Process reaper ownership left scope without publication");
  }
}

Process::ReaperClaim& Process::ReaperClaim::operator=(ReaperClaim&& other) noexcept {
  if (this != &other) {
    if (m_pProcess) {
      FATAL("Process reaper ownership overwritten before publication");
    }
    m_pProcess = other.m_pProcess;
    m_TerminationDeferral = pedigree_std::move(other.m_TerminationDeferral);
    other.m_pProcess = nullptr;
  }
  return *this;
}

void Process::ReaperClaim::publish() {
  if (!m_pProcess) {
    FATAL("Invalid Process reaper publication");
  }

  Process* process = m_pProcess;
  m_pProcess = nullptr;
  process->publishReaperClaim();
  m_TerminationDeferral = TerminationDeferral(false);
}

Process::TerminalOwnerReservation::TerminalOwnerReservation()
    : m_pProcess(nullptr), m_TerminationDeferral(false) {}

Process::TerminalOwnerReservation::TerminalOwnerReservation(
    TerminalOwnerReservation&& other) noexcept
    : m_pProcess(other.m_pProcess),
      m_TerminationDeferral(pedigree_std::move(other.m_TerminationDeferral)) {
  other.m_pProcess = nullptr;
}

Process::TerminalOwnerReservation::~TerminalOwnerReservation() {
  if (m_pProcess) {
    FATAL("Process terminal-owner reservation left scope without installation");
  }
}

Process::TerminalOwnerReservation& Process::TerminalOwnerReservation::operator=(
    TerminalOwnerReservation&& other) noexcept {
  if (this != &other) {
    if (m_pProcess) {
      FATAL("Process terminal-owner reservation overwritten before installation");
    }
    m_pProcess = other.m_pProcess;
    m_TerminationDeferral = pedigree_std::move(other.m_TerminationDeferral);
    other.m_pProcess = nullptr;
  }
  return *this;
}

void Process::TerminalOwnerReservation::install(Thread* owner) {
  if (!m_pProcess || !owner) {
    FATAL("Invalid Process terminal-owner installation");
  }

  Process* process = m_pProcess;
  process->installTerminalOwner(owner);
  m_pProcess = nullptr;
  m_TerminationDeferral = TerminationDeferral(false);
}

Process::ThreadLease::ThreadLease(Process* process, Thread* thread)
    : m_pProcess(process), m_pThread(thread), m_TerminationDeferral(process && thread) {}

Process::ThreadLease::ThreadLease(ThreadLease&& other) noexcept
    : m_pProcess(other.m_pProcess),
      m_pThread(other.m_pThread),
      m_TerminationDeferral(pedigree_std::move(other.m_TerminationDeferral)) {
  other.m_pProcess = nullptr;
  other.m_pThread = nullptr;
}

Process::ThreadLease::~ThreadLease() {
  reset();
}

Process::ThreadLease& Process::ThreadLease::operator=(ThreadLease&& other) noexcept {
  if (this != &other) {
    if (other.m_pProcess && other.m_pThread) {
      m_TerminationDeferral = pedigree_std::move(other.m_TerminationDeferral);

      Process* previousProcess = m_pProcess;
      Thread* previousThread = m_pThread;
      m_pProcess = other.m_pProcess;
      m_pThread = other.m_pThread;
      other.m_pProcess = nullptr;
      other.m_pThread = nullptr;
      if (previousProcess) {
        previousProcess->releaseThreadLease(previousThread);
      }
    } else {
      reset();
    }
  }
  return *this;
}

void Process::ThreadLease::reset() {
  Process* process = m_pProcess;
  Thread* thread = m_pThread;
  m_pProcess = nullptr;
  m_pThread = nullptr;
  if (process) {
    process->releaseThreadLease(thread);
  }
  m_TerminationDeferral = TerminationDeferral(false);
}

Process::FileContextLease::FileContextLease()
    : m_pFile(nullptr), m_bVfsReference(false), m_TerminationDeferral(true) {}

Process::FileContextLease::~FileContextLease() {
  reset();
}

void Process::FileContextLease::reset() {
  File* file = m_pFile;
  const bool release = m_bVfsReference;
  m_pFile = nullptr;
  m_bVfsReference = false;
  if (release) {
    file->releaseVfsReference();
  }
}

void Process::FileContextLease::adopt(File* file, bool vfsReference) {
  if (m_pFile) {
    FATAL("Process FileContextLease adopted over an active reference");
  }
  m_pFile = file;
  m_bVfsReference = vfsReference;
}

void Process::FileContextLease::swap(FileContextLease& other) {
  File* file = m_pFile;
  const bool vfsReference = m_bVfsReference;
  m_pFile = other.m_pFile;
  m_bVfsReference = other.m_bVfsReference;
  other.m_pFile = file;
  other.m_bVfsReference = vfsReference;
}

Process::Process() : Process(DeferredPublication()) {
  publish();
}

Process::Process(DeferredPublication, ProcessType type)
    : m_Threads(),
      m_NextTid(0),
      m_Id(Scheduler::instance().reserveProcessId()),
      m_UserspaceNamespace(),
      m_UserspaceId(0),
      str(),
      m_pParent(0),
      m_pAddressSpace(&VirtualAddressSpace::getKernelAddressSpace()),
      m_ExitStatus(0),
      m_FilesystemContextLock(),
      m_FilesystemContext(),
      m_bFilesystemContextReady(true),
      m_Ctty(),
      m_SpaceAllocator(false),
      m_DynamicSpaceAllocator(false),
      m_UserReservationLock(false),
      m_UserReservationGeneration(0),
      m_pUser(0),
      m_pGroup(0),
      m_pEffectiveUser(0),
      m_pEffectiveGroup(0),
      m_pDynamicLinker(0),
      m_pSubsystem(0),
      m_ChildStateWaiters(),
      m_TerminationWaiters(),
      m_SuspensionWaiters(),
      m_ContinuationEpoch(0),
      m_ThreadJoinWaiters(),
      m_nThreadJoinOperations(0),
      m_bThreadJoinAdmissionClosed(false),
      m_ExternalLeaseLock(false),
      m_ExternalLeaseWaiters(),
      m_nExternalLeases(0),
      m_bExternalLeaseAdmissionClosed(false),
      m_bExternalLeaseReleaseInProgress(false),
      m_PendingChildTransition(),
      m_State(Active),
      m_Type(type),
      m_bDestroying(false),
      m_bPublished(false),
      m_bUnregistered(false),
      m_pTerminatingThread(0),
      m_nTerminationParticipants(0),
      m_bTerminationRendezvousStarted(false),
      m_bTerminalOwnerReserved(false),
      m_pReservedTerminalOwner(nullptr),
      m_bTerminationCleanupStarted(false),
      m_bTerminationSealed(false),
      m_bTerminationReapable(false),
      m_ReaperState(ReaperUnclaimed),
      m_Lock(false),
      m_Metadata(),
      m_PerCpuTimeAccounting(
          PEDIGREE_TIME_ACCOUNTING && Processor::isInitialised() >= 2 ? Processor::getCount() : 0),
      m_DeferredTimeAccounting(),
      m_TimeAccountingReports(),
      m_bTimeAccountingReportsEnabled(false),
      m_TimeAccountingReportInterest(0),
      m_bSharedAddressSpace(false) {
  resetCounts();
  m_Metadata.startTime = Time::getTimeNanoseconds();

  resetUserReservations();
}

Process::Process(Process* pParent, bool bCopyOnWrite)
    : Process(DeferredPublication(), pParent, bCopyOnWrite) {
  publish();
}

Process::Process(DeferredPublication, Process* pParent, bool bCopyOnWrite,
                 FilesystemContextMode filesystemContext, bool emptyAddressSpace, ProcessType type)
    : m_Threads(),
      m_NextTid(0),
      m_Id(Scheduler::instance().reserveProcessId()),
      m_UserspaceNamespace(pParent->m_UserspaceNamespace ? pParent->m_UserspaceNamespace
                                                         : defaultUserspacePidNamespace()),
      m_UserspaceId(m_UserspaceNamespace ? m_UserspaceNamespace->allocate() : 0),
      str(),
      m_pParent(pParent),
      m_pAddressSpace(0),
      m_ExitStatus(0),
      m_FilesystemContextLock(),
      m_FilesystemContext(),
      m_bFilesystemContextReady(true),
      m_Ctty(),
      m_SpaceAllocator(false),
      m_DynamicSpaceAllocator(false),
      m_UserReservationLock(false),
      m_UserReservationGeneration(0),
      m_pUser(pParent->m_pUser),
      m_pGroup(pParent->m_pGroup),
      m_pEffectiveUser(pParent->m_pEffectiveUser),
      m_pEffectiveGroup(pParent->m_pEffectiveGroup),
      m_pDynamicLinker(pParent->m_pDynamicLinker),
      m_pSubsystem(0),
      m_ChildStateWaiters(),
      m_TerminationWaiters(),
      m_SuspensionWaiters(),
      m_ContinuationEpoch(0),
      m_ThreadJoinWaiters(),
      m_nThreadJoinOperations(0),
      m_bThreadJoinAdmissionClosed(false),
      m_ExternalLeaseLock(false),
      m_ExternalLeaseWaiters(),
      m_nExternalLeases(0),
      m_bExternalLeaseAdmissionClosed(false),
      m_bExternalLeaseReleaseInProgress(false),
      m_PendingChildTransition(),
      m_State(Active),
      m_Type(type),
      m_bDestroying(false),
      m_bPublished(false),
      m_bUnregistered(false),
      m_pTerminatingThread(0),
      m_nTerminationParticipants(0),
      m_bTerminationRendezvousStarted(false),
      m_bTerminalOwnerReserved(false),
      m_pReservedTerminalOwner(nullptr),
      m_bTerminationCleanupStarted(false),
      m_bTerminationSealed(false),
      m_bTerminationReapable(false),
      m_ReaperState(ReaperUnclaimed),
      m_Lock(false),
      m_Metadata(),
      m_PerCpuTimeAccounting(
          PEDIGREE_TIME_ACCOUNTING && Processor::isInitialised() >= 2 ? Processor::getCount() : 0),
      m_DeferredTimeAccounting(),
      m_TimeAccountingReports(),
      m_bTimeAccountingReportsEnabled(false),
      m_TimeAccountingReportInterest(0),
      m_bSharedAddressSpace(!bCopyOnWrite) {
  UserReservationSnapshot inheritedReservations;
  if (!pParent->snapshotUserReservations(inheritedReservations)) {
    FATAL("Cannot snapshot parent process reservations");
  }
  m_SpaceAllocator.swap(inheritedReservations.normal);
  m_DynamicSpaceAllocator.swap(inheritedReservations.dynamic);

  {
    TerminationDeferral filesystemContextDeferral;
    LockGuard<Mutex> guard(pParent->m_FilesystemContextLock);
    m_Ctty = pParent->m_Ctty;
  }
  if (filesystemContext == FilesystemContextMode::Inherit) {
    auto parentContext = pParent->acquireFilesystemContext();
    if (parentContext)
      m_bFilesystemContextReady = parentContext->forkForProcess(m_FilesystemContext);
  } else {
    m_bFilesystemContextReady = false;
  }

  // Resource counters describe the inherited address space, but forked CPU
  // time and process age start at zero for the child. Individual atomic
  // loads avoid racing a whole-struct copy with the running parent.
  m_Metadata.heapUsage = pParent->getHeapUsage();
  m_Metadata.virtualPages = pParent->getVirtualPageCount();
  m_Metadata.physicalPages = pParent->getPhysicalPageCount();
  m_Metadata.sharedPages = pParent->getSharedPageCount();
  m_Metadata.startTime = Time::getTimeNanoseconds();

#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
  m_BenchmarkSyscallTiming = __atomic_load_n(&pParent->m_BenchmarkSyscallTiming, __ATOMIC_ACQUIRE);
#endif
#if PEDIGREE_BENCHMARK_SYSCALL_TRACE
  m_BenchmarkSyscallTrace = __atomic_load_n(&pParent->m_BenchmarkSyscallTrace, __ATOMIC_ACQUIRE);
#endif
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  m_BenchmarkVmDiagnostics = __atomic_load_n(&pParent->m_BenchmarkVmDiagnostics, __ATOMIC_ACQUIRE);
#endif

  m_pAddressSpace = emptyAddressSpace ? VirtualAddressSpace::create()
                                      : pParent->m_pAddressSpace->clone(bCopyOnWrite);
  if (emptyAddressSpace && m_pAddressSpace)
    resetUserReservations();
  str = pParent->str;

  // Annotate the temporary description.
  if (m_bSharedAddressSpace) {
    str += "<C>";  // C for cloned (i.e. shared address space)
  } else {
    str += "<F>";  // F for forked.
  }
}

void Process::VforkCompletion::wait() {
  Uninterruptible events;
  TerminationDeferral termination;
  for (;;) {
    auto guard = m_Waiters.acquire();
    if (m_Complete)
      return;
    const auto reason = guard.waitForCompletion(WaitQueue::Channel(), Thread::ProcessWait,
                                                reinterpret_cast<uintptr_t>(this));
    (void)reason;
  }
}

void Process::VforkCompletion::complete() {
  auto guard = m_Waiters.acquire();
  m_Complete = true;
  guard.wakeAll();
}

void Process::borrowVforkAddressSpace(Process& parent,
                                      const SharedPointer<VforkCompletion>& completion) {
  assert(!m_bPublished && !m_pVforkOwner && m_pAddressSpace && completion);
  m_pVforkPrivateAddressSpace = m_pAddressSpace;
  m_pVforkOwner = parent.addressSpaceOwner();
  m_pAddressSpace = parent.getAddressSpace();
  m_VforkCompletion = completion;
}

void Process::releaseVforkAddressSpace() {
  if (!m_pVforkOwner)
    return;
  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  m_pAddressSpace = m_pVforkPrivateAddressSpace;
  m_pVforkPrivateAddressSpace = nullptr;
  m_pVforkOwner = nullptr;
  Thread* current = Processor::information().getCurrentThread();
  if (current && current->getParent() == this)
    Processor::switchAddressSpace(*m_pAddressSpace);
  Processor::setInterrupts(interrupts);
  // The child can be reaped before its creator runs, so the waiter owns an
  // independent reference. No CPU may still execute this child in the old VM.
  auto completion = pedigree_std::move(m_VforkCompletion);
  completion->complete();
}

FilesystemContextRef Process::acquireFilesystemContext() const {
  LockGuard<Mutex> guard(m_FilesystemContextLock);
  return m_FilesystemContext.reference();
}

bool Process::installFilesystemContext(FilesystemContextOwner&& context) {
  if (!context)
    return false;
  LockGuard<Mutex> guard(m_FilesystemContextLock);
  if (m_FilesystemContext)
    return false;
  // The old slot is empty, so moving ownership cannot invoke provider code.
  m_FilesystemContext = pedigree_std::move(context);
  m_bFilesystemContextReady = true;
  return true;
}

bool Process::filesystemContextReady() const {
  LockGuard<Mutex> guard(m_FilesystemContextLock);
  return m_bFilesystemContextReady;
}

void Process::releaseFilesystemContext() {
  FilesystemContextOwner retired;
  {
    LockGuard<Mutex> guard(m_FilesystemContextLock);
    retired = pedigree_std::move(m_FilesystemContext);
    m_bFilesystemContextReady = false;
  }
  retired.reset();
}

namespace {
class CoreControllingTerminal : public Process::ControllingTerminal {
 public:
  explicit CoreControllingTerminal(File* file) : m_File(file) {}
  ~CoreControllingTerminal() override {
    m_File->releaseVfsReference();
  }
  File* file() const override {
    return m_File;
  }

 private:
  File* m_File;
};
}  // namespace

SharedPointer<Process::ControllingTerminal> Process::acquireCttyContext() const {
  LockGuard<Mutex> guard(m_FilesystemContextLock);
  return m_Ctty;
}

File* Process::acquireCtty(FileContextLease& lease) const {
  auto context = acquireCttyContext();
  FileContextLease replacement;
  File* file = context ? context->file() : nullptr;
  if (file && file->retainVfsReference())
    replacement.adopt(file, true);
  lease.swap(replacement);
  return lease.get();
}

void Process::setCttyContext(const SharedPointer<ControllingTerminal>& context) {
  TerminationDeferral deferral;
  SharedPointer<ControllingTerminal> retired;
  {
    LockGuard<Mutex> guard(m_FilesystemContextLock);
    retired = pedigree_std::move(m_Ctty);
    m_Ctty = context;
  }
}

bool Process::setCtty(File* file) {
  if (!file) {
    setCttyContext(SharedPointer<ControllingTerminal>());
    return true;
  }
  if (!file->retainVfsReference())
    return false;
  auto* raw = new CoreControllingTerminal(file);
  if (!raw) {
    file->releaseVfsReference();
    return false;
  }
  auto context = SharedPointer<ControllingTerminal>::tryAdopt(raw);
  if (!context)
    return false;
  setCttyContext(context);
  return true;
}

void Process::enableTimeAccountingReports(size_t initialInterest) {
  __atomic_store_n(&m_TimeAccountingReportInterest, initialInterest, __ATOMIC_RELEASE);
  __atomic_store_n(&m_bTimeAccountingReportsEnabled, true, __ATOMIC_RELEASE);
}

void Process::setTimeAccountingReportInterest(size_t interest, bool enabled) {
  const size_t previous =
      enabled ? __atomic_fetch_or(&m_TimeAccountingReportInterest, interest, __ATOMIC_ACQ_REL)
              : __atomic_fetch_and(&m_TimeAccountingReportInterest, ~interest, __ATOMIC_ACQ_REL);
  // Catch CPU time published between a timer's baseline snapshot and arming,
  // even if no later mode transition publishes another batch.
  if (enabled && interest && !previous &&
      __atomic_load_n(&m_bTimeAccountingReportsEnabled, __ATOMIC_ACQUIRE) &&
      m_DeferredTimeAccounting.publish(1)) {
    Processor::information().getScheduler().publishDeferredTimeAccounting();
  }
}

void Process::accountReapedChild(const Process* child, Time::Timestamp& user,
                                 Time::Timestamp& kernel) {
  if (!child || child == this || child->getState() != Reaped) {
    FATAL("Process child CPU accounting requires sole ownership of a reaped child");
  }

  user = child->getUserTime() + child->getReapedChildrenUserTime();
  kernel = child->getKernelTime() + child->getReapedChildrenKernelTime();
  __atomic_fetch_add(&m_Metadata.reapedChildrenUserTime, user, __ATOMIC_RELAXED);
  __atomic_fetch_add(&m_Metadata.reapedChildrenKernelTime, kernel, __ATOMIC_RELAXED);
#if PEDIGREE_SYSCALL_COUNTER
  const uint64_t syscalls = child->getSyscallCount() + child->getReapedChildrenSyscallCount();
  __atomic_fetch_add(&m_Metadata.reapedChildrenSyscallCount, syscalls, __ATOMIC_RELAXED);
  SyscallLatencySnapshot latency;
  child->getSyscallLatencySnapshot(latency);
  for (size_t i = 0; i < SyscallLatencyBucketCount; ++i) {
    __atomic_fetch_add(&m_Metadata.reapedChildrenSyscallLatencyBuckets[i], latency.buckets[i],
                       __ATOMIC_RELAXED);
  }
#endif
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
  for (size_t i = 0; i < SyscallTimingSlotCount; ++i) {
    const uint64_t calls = __atomic_load_n(&child->m_SyscallTimingCalls[i], __ATOMIC_ACQUIRE);
    const uint64_t kernelNanoseconds =
        __atomic_load_n(&child->m_SyscallTimingKernelNanoseconds[i], __ATOMIC_ACQUIRE);
    __atomic_fetch_add(&m_SyscallTimingCalls[i], calls, __ATOMIC_RELAXED);
    __atomic_fetch_add(&m_SyscallTimingKernelNanoseconds[i], kernelNanoseconds, __ATOMIC_RELAXED);
  }
#endif
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  for (size_t i = 0; i < BenchmarkVmCounterCount; ++i) {
    const uint64_t value = __atomic_load_n(&child->m_BenchmarkVmCounters[i], __ATOMIC_ACQUIRE);
    __atomic_fetch_add(&m_BenchmarkVmCounters[i], value, __ATOMIC_RELAXED);
  }
#endif
}

void Process::queueTimeAccountingReport(Time::Timestamp elapsed) {
  if (m_DeferredTimeAccounting.publish(elapsed)) {
    Processor::information().getScheduler().publishDeferredTimeAccounting();
  }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Process::publishTimeAccountingForHostedTest(Time::Timestamp user, Time::Timestamp system) {
  __atomic_fetch_add(&m_Metadata.userTime, user, __ATOMIC_RELAXED);
  __atomic_fetch_add(&m_Metadata.kernelTime, system, __ATOMIC_RELAXED);
  reportTimeAccounting(user ? user : system);
}

void Process::closeTimeAccountingForHostedTest() {
  closeDeferredTimeAccounting();
}
#endif

void Process::drainDeferredTimeAccounting() {
  OperationBarrier::Lease report;
  if (!m_TimeAccountingReports.tryAcquire(report)) {
    m_DeferredTimeAccounting.take();
    return;
  }

  if (m_DeferredTimeAccounting.take()) {
    const Time::Timestamp user = getUserTime();
    reportTimesUpdated(user, user + getKernelTime());
  }
}

void Process::closeDeferredTimeAccounting() {
  __atomic_store_n(&m_bTimeAccountingReportsEnabled, false, __ATOMIC_RELEASE);
  m_TimeAccountingReports.closeAndWait();
  m_DeferredTimeAccounting.take();
}

void Process::publish() {
  if (!filesystemContextReady())
    FATAL("Process published before filesystem-context preparation completed");
  if (m_bPublished) {
    FATAL("Process::publish() called more than once.");
  }

  Process* pRequestedParent = getParent();
  if (!pRequestedParent) {
    Scheduler::instance().addProcess(this);
    m_bPublished = true;
    return;
  }

  Scheduler::ProcessLease requestedParent;
  if (!Scheduler::instance().acquireProcess(requestedParent, pRequestedParent)) {
    Scheduler::ProcessLease init;
    const bool initAcquired = Scheduler::instance().acquireProcess(init, Process::getInit());
    Process* publishParent =
        initAcquired && canAdoptChildren(init.get(), this) ? init.get() : nullptr;
    if (publishParent) {
      auto publishGuard = publishParent->m_ChildStateWaiters.acquire();
      if (canAdoptChildren(publishParent, this)) {
        __atomic_store_n(&m_pParent, publishParent, __ATOMIC_RELEASE);
        Scheduler::instance().addProcess(this);
        m_bPublished = true;
        publishGuard.wakeAll();
        return;
      }
    }
    __atomic_store_n(&m_pParent, static_cast<Process*>(nullptr), __ATOMIC_RELEASE);
    Scheduler::instance().addProcess(this);
    m_bPublished = true;
    return;
  }

  // Publication and parent teardown use the same child-state guard. If the
  // requested parent has already begun exiting, publish under a live init or
  // as an orphan instead of appearing after the parent's reparenting scan.
  auto parentGuard = requestedParent->m_ChildStateWaiters.acquire();
  Process* pPublishParent = pRequestedParent;
  if (!canAdoptChildren(pPublishParent)) {
    pPublishParent = nullptr;
  }

  if (!pPublishParent) {
    Scheduler::ProcessLease publishParent;
    const bool publishParentAcquired =
        Scheduler::instance().acquireProcess(publishParent, Process::getInit());
    if (publishParentAcquired && publishParent.get() != pRequestedParent &&
        canAdoptChildren(publishParent.get(), this)) {
      auto publishGuard = publishParent->m_ChildStateWaiters.acquire();
      pPublishParent = publishParent.get();
      __atomic_store_n(&m_pParent, pPublishParent, __ATOMIC_RELEASE);
      Scheduler::instance().addProcess(this);
      m_bPublished = true;
      parentGuard.wakeAll();
      publishGuard.wakeAll();
      return;
    }
  }

  if (!canAdoptChildren(pPublishParent, this)) {
    pPublishParent = 0;
  }
  __atomic_store_n(&m_pParent, pPublishParent, __ATOMIC_RELEASE);
  Scheduler::instance().addProcess(this);
  m_bPublished = true;
  parentGuard.wakeAll();
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Process::makeOrphanBeforePublicationForHostedTest() {
  if (m_bPublished) {
    FATAL(
        "Process test fixture attempted to become an orphan after "
        "publication.");
  }
  __atomic_store_n(&m_pParent, static_cast<Process*>(nullptr), __ATOMIC_RELEASE);
}
#endif

void Process::prepareForDestruction() {
  // A dying process does not receive a fresh virtual/profile timer signal.
  // Close worker admission, let an already-running report leave scope, and
  // discard time published by peers while their termination was in flight.
  closeDeferredTimeAccounting();

  Process* expectedInit = this;
  __atomic_compare_exchange_n(&m_pInitProcess, &expectedInit, static_cast<Process*>(0), false,
                              __ATOMIC_RELEASE, __ATOMIC_RELAXED);

  if (!m_bPublished || m_bUnregistered) {
    closeExternalLeaseAdmission();
    drainExternalLeases();
    return;
  }

  // Remove ourselves from enumeration while holding the same parent-owned
  // guard waitpid uses to inspect child pointers. Once this releases, a new
  // scan cannot discover this object and an existing scan has finished.
  bool unregistered = false;
  while (!unregistered) {
    Process* pParent = getParent();
    if (pParent) {
      Scheduler::ProcessLease parent;
      if (!Scheduler::instance().acquireProcess(parent, pParent)) {
        if (getParent() != pParent) {
          continue;
        }
        FATAL(
            "Process retained an unpinned parent during "
            "destruction.");
      }
      auto guard = parent->m_ChildStateWaiters.acquire();
      if (getParent() != pParent) {
        continue;
      }

      Scheduler::instance().removeProcess(this);
      m_bUnregistered = true;
      guard.wakeAll();
      unregistered = true;
    } else {
      Scheduler::instance().removeProcess(this);
      m_bUnregistered = true;
      unregistered = true;
    }
  }

  drainExternalLeases();
}

Process::~Process() {
  TerminationDeferral terminationDeferral;
  prepareForDestruction();

  // A scheduler callback can publish a reapable detached Thread immediately
  // before process termination observes it. Keep the Process and its address
  // space alive until the ordinary retirement worker has removed that Thread.
  m_DeferredThreadReaps.closeAndWait();

  // A joiner releases this lease only after it has stopped using both the
  // target Thread and this Process. Closing admission first makes the drain
  // a one-way lifetime barrier.
  while (true) {
    auto joinGuard = m_ThreadJoinWaiters.acquire();
    m_bThreadJoinAdmissionClosed = true;
    if (!m_nThreadJoinOperations) {
      break;
    }

    const WaitQueue::WakeReason reason = joinGuard.waitForCompletion(
        WaitQueue::Channel(this), Thread::ProcessWait, reinterpret_cast<uintptr_t>(this));
    (void)reason;
  }

  // Make sure we have full mutual exclusion on the Subsystem before we lock
  // here. This ensures we have full access to the subsystem and avoids a case
  // where we lock here but the subsystem destruction needs to reschedule to
  // acquire the subsystem locks.
  if (m_pSubsystem) {
    m_pSubsystem->acquire();
  }

  // Close topology mutation while holding the vector lock, then drop the
  // spinlock before any lease drain or destructor can sleep. A remover that
  // entered earlier finishes before this transition; later add/remove calls
  // observe m_bDestroying and cannot mutate m_Threads.
  {
    LockGuard<Spinlock> guard(m_Lock);
    m_bDestroying = true;
  }

  for (Vector<Thread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); ++it) {
    Thread* pThread = *it;
    pThread->closeExternalLeaseAdmissionAndDrain();
    auto threadExitGuard = pThread->m_JoinWaiters.acquire();
    if (!pThread->m_bReapable) {
      FATAL("Process::~Process invariant failed for pid "
            << Dec << m_Id << ", tid " << pThread->getId() << ": thread status "
            << static_cast<size_t>(pThread->getStatus()) << " is not off-stack/reapable.");
    }
    if (pThread->m_bJoinClaimed) {
      FATAL("Process::~Process invariant failed for pid "
            << Dec << m_Id << ", tid " << pThread->getId()
            << ": a join claim escaped the lifetime barrier.");
    }
    if (pThread == Processor::information().getCurrentThread()) {
      FATAL("Process::~Process invariant failed for pid "
            << Dec << m_Id << ": destructor is running on a thread stack it would free.");
    }
  }

  // Reapable threads have no live stack users. Process destruction owns any
  // objects retained for join, including process-exit participants.
  for (Vector<Thread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); ++it) {
    delete *it;
  }

  if (m_pSubsystem)
    delete m_pSubsystem;

  releaseFilesystemContext();

  VirtualAddressSpace& VAddressSpace = Processor::information().getVirtualAddressSpace();

  bool bInterrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);

  if (m_pAddressSpace) {
    Processor::switchAddressSpace(*m_pAddressSpace);
    m_pAddressSpace->revertToKernelAddressSpace();
    Processor::switchAddressSpace(VAddressSpace);
  }

  delete m_pAddressSpace;

  str.append("<Z>");

  Processor::setInterrupts(bInterrupts);
}

bool Process::beginThreadJoin() {
  auto guard = m_ThreadJoinWaiters.acquire();
  if (m_bThreadJoinAdmissionClosed) {
    return false;
  }

  ++m_nThreadJoinOperations;
  return true;
}

void Process::endThreadJoin() {
  auto guard = m_ThreadJoinWaiters.acquire();
  if (!m_nThreadJoinOperations) {
    FATAL("Process::endThreadJoin underflow for pid " << Dec << m_Id << ".");
  }

  --m_nThreadJoinOperations;
  if (!m_nThreadJoinOperations) {
    guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
  }
}

bool Process::beginExternalLease() {
  LockGuard<Spinlock> guard(m_ExternalLeaseLock);
  if (m_bExternalLeaseAdmissionClosed) {
    return false;
  }

  ++m_nExternalLeases;
  return true;
}

void Process::endExternalLease() {
  bool wake = false;
  bool finalRelease = false;
  bool finishClosedRelease = false;
  {
    LockGuard<Spinlock> guard(m_ExternalLeaseLock);
    if (!m_nExternalLeases) {
      FATAL("Process external lease underflow for pid " << Dec << m_Id << ".");
    }

    --m_nExternalLeases;
    finalRelease = !m_nExternalLeases;
    finishClosedRelease = finalRelease && m_bExternalLeaseAdmissionClosed;
    wake = finishClosedRelease;
    if (finishClosedRelease) {
      m_bExternalLeaseReleaseInProgress = true;
    }
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  Process* hookTarget = __atomic_load_n(&m_ExternalLeaseReleaseTarget, __ATOMIC_ACQUIRE);
  if (finalRelease && hookTarget == this) {
    ExternalLeaseReleaseHook hook = __atomic_load_n(&m_ExternalLeaseReleaseHook, __ATOMIC_ACQUIRE);
    if (hook) {
      hook(this, ExternalLeaseFinalReleaseUnlocked);
    }
  }
#endif

  if (!finishClosedRelease) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    ExternalLeaseReleaseHook hook = __atomic_load_n(&m_ExternalLeaseReleaseHook, __ATOMIC_ACQUIRE);
    if (wake && hookTarget == this && hook) {
      hook(this, ExternalLeaseBeforeWaiterWake);
    }
#endif
    if (wake) {
      m_ExternalLeaseWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
    }

    // A drainer closes admission under m_ExternalLeaseLock before testing
    // this count. An open final release therefore has nobody to wake, and
    // must not touch this Process after the predicate lock is released.
    return;
  }

  auto waiterGuard = m_ExternalLeaseWaiters.acquire();
  {
    LockGuard<Spinlock> guard(m_ExternalLeaseLock);
    m_bExternalLeaseReleaseInProgress = false;
  }

  if (wake) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    ExternalLeaseReleaseHook hook = __atomic_load_n(&m_ExternalLeaseReleaseHook, __ATOMIC_ACQUIRE);
    if (hookTarget == this && hook) {
      hook(this, ExternalLeaseBeforeWaiterWake);
    }
#endif
    waiterGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
  }
}

void Process::closeExternalLeaseAdmission() {
  LockGuard<Spinlock> guard(m_ExternalLeaseLock);
  m_bExternalLeaseAdmissionClosed = true;
}

void Process::drainExternalLeases() {
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
        WaitQueue::Channel(this), Thread::ProcessWait, reinterpret_cast<uintptr_t>(this));
    (void)reason;
  }
}

void Process::releaseThreadLease(Thread* thread) {
  if (thread) {
    thread->endExternalLease();
  }
  endExternalLease();
}

size_t Process::addThread(Thread* pThread) {
  LockGuard<Spinlock> guard(m_Lock);
  if (!pThread)
    return ~0;
  const ProcessState state = getState();
  if (m_bDestroying || m_bTerminationSealed || m_bExecCommitted || state == Terminated ||
      state == Reaped) {
    FATAL("Process::addThread invariant failed for pid "
          << Dec << m_Id << ": a thread cannot be published after exit rendezvous seals.");
  }
  if (state == Terminating) {
    // A create-vs-exit race joins the rendezvous but never runs its entry
    // point. The add worker retires this Created thread off-stack.
    pThread->m_bProcessExitOwned = true;
    pThread->m_bProcessExitParticipant = true;
    __atomic_store_n(&pThread->m_UnwindState, Thread::TerminateThread, __ATOMIC_RELEASE);
    ++m_nTerminationParticipants;
  }
  m_Threads.pushBack(pThread);
  const size_t localId = m_NextTid += 1;
  __atomic_store_n(&pThread->m_TaskId,
                   localId == 1 ? m_Id : Scheduler::instance().reserveProcessId(),
                   __ATOMIC_RELEASE);
  return localId;
}

void Process::threadExiting(Thread* pThread) {
  if (m_pSubsystem) {
    m_pSubsystem->threadExiting(pThread);
  }
}

void Process::transferExecProcessSignals(Thread* pThread) {
  // Final process exit holds m_Lock through its last scheduler handoff.
  // Signals have no surviving recipient once that terminal phase begins.
  const ProcessState state = getState();
  if (m_pSubsystem && (state == Active || state == Suspended)) {
    m_pSubsystem->preserveProcessSignalsForThreadExit(pThread);
  }
}

void Process::removeThread(Thread* pThread) {
  {
    RecursingLockGuard<Spinlock> guard(m_Lock);

    // The destructor owns its vector iteration and deliberately leaves removal
    // until the whole Process object disappears. Logical process termination,
    // however, can outlive detached Thread destruction while waitpid waits.
    if (m_bDestroying)
      return;
    for (Vector<Thread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); it++) {
      if (*it == pThread) {
        m_Threads.erase(it);
        break;
      }
    }

    if (m_pSubsystem)
      m_pSubsystem->threadRemoved(pThread);
  }
  m_ExecWaiters.wakeAll();
}

size_t Process::getNumThreads() {
  LockGuard<Spinlock> guard(m_Lock);
  return m_Threads.count();
}

bool Process::acquireThread(ThreadLease& lease, size_t n) {
  Thread* thread = nullptr;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (n < m_Threads.count() && beginExternalLease()) {
      thread = m_Threads[n];
      if (!thread || !thread->beginExternalLease()) {
        endExternalLease();
        thread = nullptr;
      }
    }
  }

  if (!thread) {
    lease.reset();
    return false;
  }

  lease = ThreadLease(this, thread);
  return true;
}

bool Process::acquireProcessSignalThread(ThreadLease& lease) {
  Thread* target = nullptr;
  {
    LockGuard<Spinlock> guard(m_Lock);
    const ProcessState state = getState();
    if ((state == Active || state == Suspended) && beginExternalLease()) {
      auto acquire = [&](Thread* thread) {
        if (!thread) {
          return false;
        }
        LockGuard<Spinlock> threadGuard(thread->m_Lock);
        if (thread->m_bShutdown || thread->m_Status == Thread::Zombie ||
            thread->getUnwindState() == Thread::TerminateThread || !thread->beginExternalLease()) {
          return false;
        }
        target = thread;
        return true;
      };
      if (!acquire(m_pExecOwner)) {
        for (Thread* thread : m_Threads) {
          if (acquire(thread)) {
            break;
          }
        }
      }
      if (!target) {
        endExternalLease();
      }
    }
  }

  if (!target) {
    lease.reset();
    return false;
  }
  lease = ThreadLease(this, target);
  return true;
}

bool Process::acquireThreadById(ThreadLease& lease, size_t id) {
  Thread* thread = nullptr;
  {
    LockGuard<Spinlock> guard(m_Lock);
    for (Vector<Thread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); ++it) {
      if (*it && (*it)->getId() == id) {
        thread = *it;
        break;
      }
    }
    if (!thread || !beginExternalLease()) {
      thread = nullptr;
    } else if (!thread->beginExternalLease()) {
      endExternalLease();
      thread = nullptr;
    }
  }

  if (!thread) {
    lease.reset();
    return false;
  }

  lease = ThreadLease(this, thread);
  return true;
}

bool Process::acquireThreadByTaskId(ThreadLease& lease, size_t id) {
  Thread* thread = nullptr;
  {
    LockGuard<Spinlock> guard(m_Lock);
    for (Vector<Thread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); ++it) {
      if (*it && (*it)->getTaskId() == id) {
        thread = *it;
        break;
      }
    }
    if (!thread || !beginExternalLease()) {
      thread = nullptr;
    } else if (!thread->beginExternalLease()) {
      endExternalLease();
      thread = nullptr;
    }
  }

  if (!thread) {
    lease.reset();
    return false;
  }

  lease = ThreadLease(this, thread);
  return true;
}

bool Process::acquireThread(ThreadLease& lease, Thread* expected) {
  if (!expected) {
    lease.reset();
    return false;
  }

  Thread* thread = nullptr;
  {
    LockGuard<Spinlock> guard(m_Lock);
    for (Vector<Thread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); ++it) {
      if (*it == expected) {
        thread = *it;
        break;
      }
    }
    if (!thread || !beginExternalLease()) {
      thread = nullptr;
    } else if (!thread->beginExternalLease()) {
      endExternalLease();
      thread = nullptr;
    }
  }

  if (!thread) {
    lease.reset();
    return false;
  }

  lease = ThreadLease(this, thread);
  return true;
}

Process::TerminalOwnerReservation Process::reserveTerminalOwner() {
  TerminalOwnerReservation reservation;
  while (true) {
    auto progress = m_ExecWaiters.acquire();
    m_Lock.acquire();
    if (m_bExecCommitted) {
      m_Lock.release();
      const WaitQueue::WakeReason reason = progress.waitForCompletion(
          WaitQueue::Channel(), Thread::ProcessWait, reinterpret_cast<uintptr_t>(this));
      (void)reason;
      continue;
    }
    const ProcessState state = getState();
    if (m_bDestroying || m_bTerminationRendezvousStarted || m_bTerminationSealed ||
        state == Terminating || state == Terminated || state == Reaped) {
      m_Lock.release();
      return reservation;
    }
    if (m_bTerminalOwnerReserved) {
      FATAL("Process terminal owner reserved more than once for pid " << Dec << m_Id << ".");
    }

    m_bTerminalOwnerReserved = true;
    m_pReservedTerminalOwner = nullptr;
    reservation.m_pProcess = this;
    m_Lock.release();
    break;
  }
  reservation.m_TerminationDeferral = TerminationDeferral(true);
  return reservation;
}

void Process::installTerminalOwner(Thread* owner) {
  LockGuard<Spinlock> guard(m_Lock);
  if (!m_bTerminalOwnerReserved || m_pReservedTerminalOwner) {
    FATAL("Process terminal owner installed without a reservation for pid " << Dec << m_Id << ".");
  }

  bool found = false;
  for (Vector<Thread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); ++it) {
    if (*it == owner) {
      found = true;
      break;
    }
  }
  if (!found) {
    FATAL("Process terminal owner was not published to pid " << Dec << m_Id << ".");
  }

  {
    LockGuard<Spinlock> ownerGuard(owner->m_Lock);
    if (owner->m_Status != Thread::Created || owner->m_bStartRequested ||
        owner->getUnwindState() != Thread::Continue) {
      FATAL("Process terminal owner was not installed before startup for pid " << Dec << m_Id
                                                                               << ".");
    }
  }

  m_pReservedTerminalOwner = owner;
}

bool Process::transitionState(ProcessState expected, ProcessState desired) {
  return __atomic_compare_exchange_n(&m_State, &expected, desired, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE);
}

void Process::transitionToTerminating() {
  while (true) {
    const ProcessState state = getState();
    if (state == Terminating) {
      return;
    }
    if (state != Active && state != Suspended) {
      FATAL("Process state cannot transition to Terminating from "
            << Dec << static_cast<size_t>(state) << " for pid " << m_Id << ".");
    }
    if (transitionState(state, Terminating)) {
      return;
    }
  }
}

void Process::reap() {
  if (!transitionState(Terminated, Reaped) && getState() != Reaped) {
    FATAL("Process state cannot transition to Reaped from "
          << Dec << static_cast<size_t>(getState()) << " for pid " << m_Id << ".");
  }
}

void Process::markTerminating() {
  closeDeferredTimeAccounting();
  transitionToTerminating();
}

bool Process::prepareThreadExit() {
  Thread* current = Processor::information().getCurrentThread();
  if (!current || current->getParent() != this) {
    FATAL("Thread exit intent must come from the current process.");
  }

  LockGuard<Spinlock> guard(m_Lock);
  current->m_bThreadExitRequested = true;
  if (m_bTerminalOwnerReserved || m_bTerminationRendezvousStarted || getState() == Terminating) {
    return false;
  }

  for (Thread* thread : m_Threads) {
    if (thread == current || thread->m_bThreadExitRequested ||
        thread->getUnwindState() == Thread::TerminateThread) {
      continue;
    }
    auto exitGuard = thread->m_JoinWaiters.acquire();
    if (!thread->m_bExitStarted && !thread->m_bReapable) {
      return false;
    }
  }

  // Reserve the running owner before dropping the lock. Concurrent exits
  // cannot elect another owner, and late clone publication joins termination.
  m_bTerminalOwnerReserved = true;
  m_pReservedTerminalOwner = current;
  transitionToTerminating();
  return true;
}

bool Process::beginTermination(int code, Subsystem::ExitCause cause) {
  // Do not hold m_Lock while an admitted accounting report drains: POSIX
  // signal publication may itself need to inspect this Process's threads.
  closeDeferredTimeAccounting();

  m_Lock.acquire();
  Thread* pCurrentThread = Processor::information().getCurrentThread();
  if (!pCurrentThread || pCurrentThread->getParent() != this) {
    FATAL("Process::beginTermination invariant failed for pid "
          << Dec << m_Id << ": exit must be initiated by a thread in the target process.");
  }

  // Election and exec commit share m_Lock: neither may begin a peer
  // rendezvous between this check and publication of its ownership.
  if (m_bExecCommitted && m_pExecOwner != pCurrentThread) {
    if (m_bExecExitForwarded) {
      m_Lock.release();
      return false;
    }
    m_bExecExitForwarded = true;
    Thread* execOwner = m_pExecOwner;
    if (!beginExternalLease() || !execOwner->beginExternalLease()) {
      FATAL("Exec owner lost its lifetime during exit forwarding.");
    }
    m_Lock.release();
    ThreadLease owner(this, execOwner);
    if (cause == Subsystem::ExitCause::Signal) {
      owner->deferSignalExit(code);
    } else {
      owner->deferProcessExit(code);
    }
    return false;
  }

  if (m_bTerminalOwnerReserved) {
    if (pCurrentThread != m_pReservedTerminalOwner) {
      m_Lock.release();
      return false;
    }
    m_bTerminalOwnerReserved = false;
    m_pReservedTerminalOwner = nullptr;
  }

  if (m_bTerminationRendezvousStarted) {
    const bool isOwner = m_pTerminatingThread == pCurrentThread;
    if (!isOwner && !pCurrentThread->m_bProcessExitParticipant && !pCurrentThread->m_bReapable) {
      FATAL(
          "Process exit competitor was absent from the rendezvous for "
          "pid "
          << Dec << m_Id << ", tid " << pCurrentThread->getId() << ".");
    }
    m_Lock.release();
    return isOwner;
  }
  if (m_bTerminationSealed || m_bTerminationReapable) {
    FATAL(
        "Process::beginTermination observed an invalid completed "
        "rendezvous for pid "
        << Dec << m_Id << ".");
  }
  m_bTerminationRendezvousStarted = true;

  Vector<Thread*> peersToTerminate;
  bool currentThreadParticipates = false;
  for (Vector<Thread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); ++it) {
    Thread* pThread = *it;
    auto threadExitGuard = pThread->m_JoinWaiters.acquire();
    if (pThread->m_bReapable) {
      continue;
    }
    if (pThread->m_bProcessExitOwned != pThread->m_bProcessExitParticipant) {
      FATAL("Process::beginTermination invariant failed for pid "
            << Dec << m_Id << ", tid " << pThread->getId()
            << ": thread has inconsistent process-exit ownership.");
    }

    if (!pThread->m_bProcessExitParticipant) {
      pThread->m_bProcessExitOwned = true;
      pThread->m_bProcessExitParticipant = true;
      ++m_nTerminationParticipants;
    }

    if (pThread == pCurrentThread) {
      currentThreadParticipates = true;
    } else if (!pThread->m_bExitStarted) {
      peersToTerminate.pushBack(pThread);
    }
  }

  if (!currentThreadParticipates) {
    FATAL("Process::beginTermination invariant failed for pid "
          << Dec << m_Id << ": initiating thread is absent or already off-stack.");
  }

  {
    auto completionGuard = m_TerminationWaiters.acquire();
    m_pTerminatingThread = pCurrentThread;
    m_bTerminationReapable = false;
  }
  transitionToTerminating();

  // Init cannot accept any more orphans once its own teardown starts.
  Process* expectedInit = this;
  __atomic_compare_exchange_n(&m_pInitProcess, &expectedInit, static_cast<Process*>(0), false,
                              __ATOMIC_RELEASE, __ATOMIC_RELAXED);

#if VERBOSE_KERNEL
  Scheduler::ProcessLease logParent;
  if (Scheduler::instance().acquireProcess(logParent, getParent()))
    NOTICE("Kill: " << m_Id << " (parent: " << logParent->getId() << ")");
  else
    NOTICE("Kill: " << m_Id << " (parent: <orphan>)");
#endif

  // Reparent every child, including terminated-but-unreaped children. Direct
  // deletion here races both waitpid and a child still switching off-stack.
  Scheduler::ProcessLease newParent;
  const bool newParentAcquired =
      Scheduler::instance().acquireProcess(newParent, Process::getInit());
  Process* pNewParent = newParentAcquired ? newParent.get() : nullptr;
  auto moveChildren = [this](Process* pParent) {
    while (true) {
      Process* pChild = Scheduler::instance().getChildProcess(this, 0);
      if (!pChild) {
        break;
      }

      __atomic_store_n(&pChild->m_pParent, pParent, __ATOMIC_RELEASE);
    }
  };

  {
    auto oldParentGuard = m_ChildStateWaiters.acquire();
    if (pNewParent) {
      auto newParentGuard = pNewParent->m_ChildStateWaiters.acquire();
      if (!canAdoptChildren(pNewParent, this)) {
        pNewParent = 0;
      }
      moveChildren(pNewParent);
      oldParentGuard.wakeAll();
      newParentGuard.wakeAll();
    } else {
      moveChildren(0);
      oldParentGuard.wakeAll();
    }
  }

  // Peer wakeup can make another same-core thread runnable immediately.
  // The current thread remains a rendezvous participant, so dropping m_Lock
  // here cannot make Process destruction observable.
  m_Lock.release();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (m_TerminationElectionHook) {
    m_TerminationElectionHook(this, pCurrentThread);
  }
#endif

  // Peers take a thread-only exit path. Re-entering Subsystem::exit() here
  // would rerun process teardown and recreate the historical deadlocks.
  for (Vector<Thread*>::Iterator it = peersToTerminate.begin(); it != peersToTerminate.end();
       ++it) {
    (*it)->setUnwindState(Thread::TerminateThread);
  }

  return true;
}

bool Process::quiesceTermination() {
  Thread* pCurrentThread = Processor::information().getCurrentThread();
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (!m_bTerminationRendezvousStarted || m_pTerminatingThread != pCurrentThread) {
      return false;
    }
    if (m_bTerminationCleanupStarted) {
      return false;
    }

    m_bTerminationCleanupStarted = true;
  }

  while (true) {
    auto progress = m_ExecWaiters.acquire();
    {
      LockGuard<Spinlock> guard(m_Lock);
      // Admitted creators may still publish a Created peer into this
      // rendezvous. Terminating rejects new admissions; seal only after
      // the existing creation scopes have finished publication.
      if (m_nThreadCreations == 0) {
        m_bTerminationSealed = true;
        break;
      }
    }
    const WaitQueue::WakeReason reason = progress.waitForCompletion(
        WaitQueue::Channel(), Thread::ProcessWait, reinterpret_cast<uintptr_t>(this));
    (void)reason;
  }

  while (true) {
    auto completionGuard = m_TerminationWaiters.acquire();
    m_Lock.acquire();
    const size_t participants = m_nTerminationParticipants;
    m_Lock.release();
    if (participants == 0) {
      FATAL("Process exit owner disappeared before teardown for pid " << Dec << m_Id << ".");
    }
    if (participants == 1) {
      return true;
    }

    const WaitQueue::WakeReason reason =
        completionGuard.waitForCompletion(WaitQueue::Channel(), Thread::ProcessWait,
                                          reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    (void)reason;
  }
}

void Process::finishTermination(bool notifyParent) {
  finishTermination(false, notifyParent);
}

void Process::finishTermination(bool abandonStack, bool notifyParent) {
  Thread* pCurrentThread = Processor::information().getCurrentThread();
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (!m_bTerminationCleanupStarted || m_pTerminatingThread != pCurrentThread ||
        m_nTerminationParticipants != 1 || !pCurrentThread->m_bProcessExitParticipant) {
      FATAL(
          "Process::finishTermination called without exclusive "
          "off-stack teardown ownership for pid "
          << Dec << m_Id << ".");
    }
  }

  // Derived cleanup may acquire blocking locks without coupling them to the
  // Process lock.
  processTerminated();

  // Wait status must be visible before an ordinary parent notification can
  // dispatch a handler which immediately calls waitpid(WNOHANG).
  publishTerminationStatus(notifyParent);

  // Add to the zombie queue if the process is an orphan.
  if (!getParent()) {
    NOTICE("Process::kill() - process is an orphan, adding to ZombieQueue.");

    ReaperClaim reaper = tryClaimReaper();
    if (reaper) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      OrphanPublicationHook publicationHook =
          __atomic_load_n(&m_OrphanPublicationHook, __ATOMIC_ACQUIRE);
      if (publicationHook) {
        publicationHook(this, OrphanPublicationPhase::Preparing, Processor::getInterrupts(),
                        m_Lock.acquired());
      }
#endif
      reaper.publish();
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      if (publicationHook) {
        publicationHook(this, OrphanPublicationPhase::Published, Processor::getInterrupts(),
                        m_Lock.acquired());
      }
#endif
    }
  }

  m_Lock.acquire();
  if (!m_bTerminationCleanupStarted || m_pTerminatingThread != pCurrentThread ||
      m_nTerminationParticipants != 1 || !pCurrentThread->m_bProcessExitParticipant) {
    FATAL(
        "Process::finishTermination lost exclusive off-stack teardown "
        "ownership for pid "
        << Dec << m_Id << ".");
  }

  // Parent-owned processes are reaped by waitpid; orphan publication above
  // may already have blocked its worker on the off-stack completion.
#if VERBOSE_KERNEL
  if (getParent()) {
    NOTICE(
        "Process::kill() - not adding to ZombieQueue, process has a "
        "parent.");
  }
#endif
  if (abandonStack) {
    Processor::information().getScheduler().abandonCurrentThreadStack(
        PerProcessorScheduler::StackDiscardReason::EmergencyProcessKill, &m_Lock);
  }
  Processor::information().getScheduler().commitCurrentThreadExit(&m_Lock);

  FATAL("Should never get here");
}

Process::ReaperClaim Process::tryClaimReaper() {
  size_t expected = ReaperUnclaimed;
  if (!__atomic_compare_exchange_n(&m_ReaperState, &expected, ReaperClaimed, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return ReaperClaim();
  }
  return ReaperClaim(this);
}

void Process::publishReaperClaim() {
  size_t expected = ReaperClaimed;
  if (!__atomic_compare_exchange_n(&m_ReaperState, &expected, ReaperPublished, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    FATAL("Process reaper ownership published more than once");
  }
  ZombieQueue::instance().addObject(new ZombieProcess(this));
}

void Process::kill() {
  if (!beginTermination()) {
    Processor::information().getScheduler().abandonCurrentThreadStack(
        PerProcessorScheduler::StackDiscardReason::EmergencyProcessKill);
  }
  if (!quiesceTermination()) {
    FATAL("Process::kill failed to claim teardown for pid " << Dec << m_Id << ".");
  }
  finishTermination(true, false);
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Process::setTerminationElectionHook(TerminationElectionHook hook) {
  m_TerminationElectionHook = hook;
}

void Process::setExternalLeaseReleaseHookForHostedTest(Process* target,
                                                       ExternalLeaseReleaseHook hook) {
  __atomic_store_n(&m_ExternalLeaseReleaseTarget, target, __ATOMIC_RELEASE);
  __atomic_store_n(&m_ExternalLeaseReleaseHook, hook, __ATOMIC_RELEASE);
}

bool Process::isTerminationReapableForHostedTest() {
  auto guard = m_TerminationWaiters.acquire();
  return m_bTerminationReapable;
}

void Process::setOrphanPublicationHook(OrphanPublicationHook hook) {
  __atomic_store_n(&m_OrphanPublicationHook, hook, __ATOMIC_RELEASE);
}
#endif

void Process::suspend(int stopSignal) {
  suspendInternal(stopSignal, false, 0);
}

void Process::suspendIfContinuationEpoch(int stopSignal, size_t continuationEpoch) {
  suspendInternal(stopSignal, true, continuationEpoch);
}

void Process::suspendInternal(int stopSignal, bool checkContinuationEpoch,
                              size_t continuationEpoch) {
  bool published = false;
  bool enteredSuspended = false;
  while (!published) {
    Process* pParent = getParent();
    if (pParent) {
      Scheduler::ProcessLease parent;
      if (!Scheduler::instance().acquireProcess(parent, pParent)) {
        if (getParent() != pParent) {
          continue;
        }
        FATAL("Process::suspend retained an unpinned parent.");
      }
      auto guard = parent->m_ChildStateWaiters.acquire();
      if (getParent() != pParent) {
        continue;
      }

      auto suspensionGuard = m_SuspensionWaiters.acquire();
      const bool epochMatches = !checkContinuationEpoch || continuationEpoch == m_ContinuationEpoch;
      if (epochMatches && transitionState(Active, Suspended)) {
        m_PendingChildTransition.kind = ChildTransitionKind::Stopped;
        m_PendingChildTransition.stopSignal = stopSignal;
        enteredSuspended = true;
        guard.wakeAll();
      }
      published = true;
    } else {
      auto suspensionGuard = m_SuspensionWaiters.acquire();
      const bool epochMatches = !checkContinuationEpoch || continuationEpoch == m_ContinuationEpoch;
      if (epochMatches && transitionState(Active, Suspended)) {
        m_PendingChildTransition.kind = ChildTransitionKind::Stopped;
        m_PendingChildTransition.stopSignal = stopSignal;
        enteredSuspended = true;
      }
      published = true;
    }
  }

  if (!enteredSuspended && getState() != Suspended) {
    return;
  }

  // Notify the parent only for the thread that won the state transition.
  if (enteredSuspended) {
    Scheduler::ProcessLease parent;
    if (Scheduler::instance().acquireProcess(parent, getParent()) && parent->getSubsystem()) {
      Process::ThreadLease parentThread;
      bool parentThreadAcquired = false;
      {
        auto relationGuard = parent->m_ChildStateWaiters.acquire();
        if (getParent() == parent.get()) {
          parentThreadAcquired = parent->acquireProcessSignalThread(parentThread);
        }
      }
      if (parentThreadAcquired) {
        parent->getSubsystem()->threadException(parentThread.get(), Subsystem::Child);
      }
    }
  }

  Thread* current = Processor::information().getCurrentThread();
  if (current && current->getParent() == this) {
    // The stop-owning thread uses the same event-publication handshake as
    // return-bound peers. This retains a terminal event queued after the
    // Suspended transition but before the owner publishes its wait.
    if (current->getScheduler()->serviceProcessStopAtUserReturn()) {
      return;
    }
    return;
  }

  // Kernel callers may synchronously suspend another Process. Preserve that
  // control-plane wait without applying the caller's own event policy.
  while (true) {
    auto guard = m_SuspensionWaiters.acquire();
    if (getState() != Suspended) {
      return;
    }

    const WaitQueue::WakeReason wakeReason =
        guard.wait(WaitQueue::Channel(), Thread::ProcessWait,
                   reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (wakeReason == WaitQueue::WakeReason::Terminating ||
        wakeReason == WaitQueue::WakeReason::Unwinding) {
      return;
    }
  }
}

size_t Process::getContinuationEpoch() {
  auto suspensionGuard = m_SuspensionWaiters.acquire();
  return m_ContinuationEpoch;
}

void Process::resume() {
  bool published = false;
  bool resumed = false;
  while (!published) {
    Process* pParent = getParent();
    if (pParent) {
      Scheduler::ProcessLease parent;
      if (!Scheduler::instance().acquireProcess(parent, pParent)) {
        if (getParent() != pParent) {
          continue;
        }
        FATAL("Process::resume retained an unpinned parent.");
      }
      auto guard = parent->m_ChildStateWaiters.acquire();
      if (getParent() != pParent) {
        continue;
      }

      auto suspensionGuard = m_SuspensionWaiters.acquire();
      ++m_ContinuationEpoch;
      if (transitionState(Suspended, Active)) {
        m_PendingChildTransition.kind = ChildTransitionKind::Continued;
        m_PendingChildTransition.stopSignal = 0;
        resumed = true;
        suspensionGuard.wakeAll();
        guard.wakeAll();
      }
      published = true;
    } else {
      auto suspensionGuard = m_SuspensionWaiters.acquire();
      ++m_ContinuationEpoch;
      if (transitionState(Suspended, Active)) {
        m_PendingChildTransition.kind = ChildTransitionKind::Continued;
        m_PendingChildTransition.stopSignal = 0;
        resumed = true;
        suspensionGuard.wakeAll();
      }
      published = true;
    }
  }

  if (!resumed) {
    return;
  }

  // Wake return-boundary gates through the queue shared with event
  // publication, then recheck events that became eligible at Active.
  for (size_t i = getNumThreads(); i > 0; --i) {
    ThreadLease thread;
    if (acquireThread(thread, i - 1)) {
      {
        auto eventGuard = thread->m_EventWaiters.acquire();
        eventGuard.wakeAll(WaitQueue::WakeReason::Signalled,
                           WaitQueue::Channel(this, static_cast<uintptr_t>(Thread::ProcessWait)));
      }
      thread->wakeForDeliverableEvents();
    }
  }
}

bool Process::selectPendingChildTransition(bool includeStopped, bool includeContinued, bool consume,
                                           ChildTransition& transition) {
  auto suspensionGuard = m_SuspensionWaiters.acquire();
  const bool selected =
      (includeStopped && m_PendingChildTransition.kind == ChildTransitionKind::Stopped) ||
      (includeContinued && m_PendingChildTransition.kind == ChildTransitionKind::Continued);
  if (!selected) {
    transition = ChildTransition();
    return false;
  }

  transition = m_PendingChildTransition;
  if (consume) {
    m_PendingChildTransition = ChildTransition();
  }
  return true;
}

bool Process::takePendingChildTransition(bool includeStopped, bool includeContinued,
                                         ChildTransition& transition) {
  return selectPendingChildTransition(includeStopped, includeContinued, true, transition);
}

int64_t Process::getUserId() const {
  User* identity = getUser();
  return identity ? static_cast<int64_t>(identity->getId()) : -1;
}

int64_t Process::getGroupId() const {
  Group* identity = getGroup();
  return identity ? static_cast<int64_t>(identity->getId()) : -1;
}

int64_t Process::getEffectiveUserId() const {
  User* identity = getEffectiveUser();
  return identity ? static_cast<int64_t>(identity->getId()) : -1;
}

int64_t Process::getEffectiveGroupId() const {
  Group* identity = getEffectiveGroup();
  return identity ? static_cast<int64_t>(identity->getId()) : -1;
}

void Process::getSupplementalGroupIds(Vector<int64_t>& vec) const {
  // no-op
}

void Process::setUserId(int64_t) {}

void Process::setGroupId(int64_t) {}

void Process::setEffectiveUserId(int64_t) {}

void Process::setEffectiveGroupId(int64_t) {}

bool Process::waitUntilTerminationReapable() {
  while (true) {
    auto guard = m_TerminationWaiters.acquire();
    if (m_bTerminationReapable) {
      return true;
    }
    if (Processor::information().getCurrentThread() == m_pTerminatingThread) {
      return false;
    }

    const WaitQueue::WakeReason wakeReason =
        guard.waitForCompletion(WaitQueue::Channel(), Thread::ProcessWait,
                                reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (wakeReason == WaitQueue::WakeReason::Terminating ||
        wakeReason == WaitQueue::WakeReason::Unwinding) {
      // Reaper cancellation cannot make deleting a live Process safe.
      // Keep the wrapper alive until the off-stack completion arrives.
      continue;
    }
  }
}

bool Process::waitUntilTerminationReapableForTerminalCoordinator() {
  while (true) {
    {
      auto guard = m_TerminationWaiters.acquire();
      if (m_bTerminationReapable) {
        return true;
      }
      if (Processor::information().getCurrentThread() == m_pTerminatingThread) {
        return false;
      }
    }

    Scheduler::instance().yield();
  }
}

bool Process::terminatingThreadReapable(Thread* pThread, bool& wakeOwner) {
  wakeOwner = false;
  if (!pThread->m_bProcessExitParticipant) {
    return false;
  }
  if (!m_nTerminationParticipants) {
    FATAL("Process exit rendezvous underflow for pid " << Dec << m_Id << ", tid "
                                                       << pThread->getId() << ".");
  }

  pThread->m_bProcessExitParticipant = false;
  --m_nTerminationParticipants;
  if (m_bTerminationRendezvousStarted && m_nTerminationParticipants == 0) {
    m_bTerminationSealed = true;
    return true;
  }

  wakeOwner = m_bTerminationRendezvousStarted || m_bExecCommitted;
  return false;
}

void Process::publishTerminationStatus(bool notifyParent) {
  bool published = false;
  while (!published) {
    Process* pParent = getParent();
    if (pParent) {
      Scheduler::ProcessLease parent;
      if (!Scheduler::instance().acquireProcess(parent, pParent)) {
        if (getParent() != pParent) {
          continue;
        }
        FATAL(
            "Process::publishTerminationStatus retained an unpinned "
            "parent.");
      }

      // Pin the notification target before taking the child-state guard. Event
      // allocation and delivery happen after that guard is dropped.
      Process::ThreadLease parentThread;
      const bool parentThreadAcquired =
          notifyParent && parent->acquireProcessSignalThread(parentThread);
      bool parentAcceptsSignal = false;
      {
        auto guard = parent->m_ChildStateWaiters.acquire();
        if (getParent() != pParent) {
          continue;
        }

        const ProcessState parentState = parent->getState();
        parentAcceptsSignal = parentState == Active || parentState == Suspended;
        {
          auto suspensionGuard = m_SuspensionWaiters.acquire();
          m_PendingChildTransition = ChildTransition();
          if (!transitionState(Terminating, Terminated)) {
            FATAL("Process state was not Terminating while publishing pid " << Dec << m_Id << ".");
          }
        }
        guard.wakeAll();
        published = true;
      }

      if (parentThreadAcquired && parentAcceptsSignal && parent->getSubsystem()) {
        parent->getSubsystem()->threadException(parentThread.get(), Subsystem::Child);
      }
    } else {
      auto suspensionGuard = m_SuspensionWaiters.acquire();
      m_PendingChildTransition = ChildTransition();
      if (!transitionState(Terminating, Terminated)) {
        FATAL("Process state was not Terminating while publishing pid " << Dec << m_Id << ".");
      }
      published = true;
    }
  }
}

void Process::publishTerminationReapable() {
  {
    RecursingLockGuard<Spinlock> processGuard(m_Lock);
    if (m_nTerminationParticipants) {
      FATAL("Process termination published with "
            << Dec << m_nTerminationParticipants << " live rendezvous participants for pid " << m_Id
            << ".");
    }
    for (Vector<Thread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); ++it) {
      Thread* pThread = *it;
      auto threadExitGuard = pThread->m_JoinWaiters.acquire();
      if (!pThread->m_bReapable) {
        FATAL("Process termination published before tid " << Dec << pThread->getId() << " of pid "
                                                          << m_Id << " switched off-stack.");
      }
    }
    const ProcessState state = getState();
    if (state != Terminated && state != Reaped) {
      FATAL("Process off-stack completion observed unpublished wait status for pid " << Dec << m_Id
                                                                                     << ".");
    }
  }

  // This is deliberately the final Process access in the scheduler callback.
  // A ZombieQueue worker may begin destruction as soon as it observes this
  // completion.
  auto guard = m_TerminationWaiters.acquire();
  m_pTerminatingThread = 0;
  m_bTerminationReapable = true;
  guard.wakeAll();
}

Process* Process::getInit() {
  return __atomic_load_n(&m_pInitProcess, __ATOMIC_ACQUIRE);
}

void Process::setInit(Process* pProcess) {
  Process* expected = 0;
  __atomic_compare_exchange_n(&m_pInitProcess, &expected, pProcess, false, __ATOMIC_RELEASE,
                              __ATOMIC_RELAXED);
}

#endif  // THREADS
