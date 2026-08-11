/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "modules/Module.h"
#include "modules/subsys/pedigree-c/pedigreecSyscallNumbers.h"
#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/net-syscalls.h"
#include "modules/subsys/posix/poll-syscalls.h"
#include "modules/subsys/posix/system-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#undef PEDIGREE_INIT_SIGRET
#undef PEDIGREE_SIGRET
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/subsys/posix/syscalls/posixSyscallNumbers.h"

extern void system_reset();
extern "C" bool posixDuplicateInitRollbackPreservesProcessForTest(Process* processIdentity);

namespace {
constexpr size_t HostedAttempts = 10000;
constexpr int PollCloseReuseTimeoutMilliseconds = 5000;
size_t g_RuntimePinnedLifecycleCalls = 0;

struct TerminalBlockedHandlerContext {
  TerminalBlockedHandlerContext()
      : blocker(0, true),
        thread(nullptr),
        hookEntered(0),
        exitStaged(0),
        releasedByTermination(0),
        unexpectedRelease(0),
        syscallReturned(0) {}

  Semaphore blocker;
  Thread* thread;
  Atomic<size_t> hookEntered;
  Atomic<size_t> exitStaged;
  Atomic<size_t> releasedByTermination;
  Atomic<size_t> unexpectedRelease;
  Atomic<size_t> syscallReturned;
};

TerminalBlockedHandlerContext* g_TerminalBlockedHandlerContext = nullptr;

int terminalCreatedFixtureEntry(void*) {
  FATAL("HOSTED-SYSCALL-TEST: FAIL posix-terminal-drain-created-entry-ran");
  return 0;
}

void terminalBlockedHandlerPin(Service_t service, SyscallHandler*) {
  TerminalBlockedHandlerContext* context = g_TerminalBlockedHandlerContext;
  Thread* current = Processor::information().getCurrentThread();
  if (!context || service != posix || current != context->thread) {
    return;
  }

  context->hookEntered += 1;
  if (SyscallManager::instance().requestProcessExit(73)) {
    context->exitStaged += 1;
  }

  const bool acquired = context->blocker.acquire();
  if (!acquired && current->getUnwindState() == Thread::TerminateThread) {
    context->releasedByTermination += 1;
  } else {
    context->unexpectedRelease += 1;
  }
}

int terminalBlockedHandlerEntry(void* parameter) {
  TerminalBlockedHandlerContext* context =
      reinterpret_cast<TerminalBlockedHandlerContext*>(parameter);
  SyscallManager::instance().syscall(posix, POSIX_GETPID);
  context->syscallReturned += 1;
  return 1;
}

void runtimePinnedLifecycleProbe() {
  ++g_RuntimePinnedLifecycleCalls;
}

class DescriptorRetirementProbe : public FileDescriptor {
 public:
  explicit DescriptorRetirementProbe(Atomic<size_t>& destructions)
      : FileDescriptor(), m_Destructions(destructions) {}

  ~DescriptorRetirementProbe() override {
    m_Destructions += 1;
  }

 private:
  Atomic<size_t>& m_Destructions;
};

class PollGenerationProbe : public NetworkSyscalls {
 public:
  PollGenerationProbe(Atomic<size_t>& registrations, Atomic<size_t>& unpolls,
                      Atomic<size_t>& destructions)
      : NetworkSyscalls(AF_UNSPEC, SOCK_STREAM, 0),
        m_Registrations(registrations),
        m_Unpolls(unpolls),
        m_Destructions(destructions),
        m_Ready(0),
        m_Waiter(nullptr) {}

  ~PollGenerationProbe() override {
    m_Destructions += 1;
  }

  int connect(const struct sockaddr_storage*, socklen_t) override {
    return -1;
  }

  ssize_t sendto_msg(const struct msghdr*) override {
    return -1;
  }

  ssize_t recvfrom_msg(struct msghdr*) override {
    return -1;
  }

  int listen(int) override {
    return -1;
  }

  int bind(const struct sockaddr_storage*, socklen_t) override {
    return -1;
  }

  int accept(struct sockaddr_storage*, socklen_t*, int) override {
    return -1;
  }

  int getpeername(struct sockaddr_storage*, socklen_t*) override {
    return -1;
  }

  int getsockname(struct sockaddr_storage*, socklen_t*) override {
    return -1;
  }

  int setsockopt(int, int, const void*, socklen_t) override {
    return -1;
  }

  int getsockopt(int, int, void*, socklen_t*) override {
    return -1;
  }

  bool canPoll() const override {
    return true;
  }

  bool poll(bool& read, bool& write, bool& error, Semaphore* waiter) override {
    const bool readable = read && m_Ready;
    read = readable;
    write = false;
    error = false;
    if (waiter && !readable) {
      m_Waiter = waiter;
      m_Registrations += 1;
    }
    return readable;
  }

  void unPoll(Semaphore* waiter) override {
    m_Unpolls += 1;
    if (m_Waiter == waiter) {
      m_Waiter = nullptr;
    }
  }

  void makeReadable() {
    m_Ready = 1;
    Semaphore* waiter = m_Waiter;
    if (waiter) {
      waiter->release();
    }
  }

  const void* waiterAddress() const {
    return m_Waiter;
  }

 private:
  Atomic<size_t>& m_Registrations;
  Atomic<size_t>& m_Unpolls;
  Atomic<size_t>& m_Destructions;
  Atomic<size_t> m_Ready;
  Atomic<Semaphore*> m_Waiter;
};

struct DescriptorCloseContext {
  DescriptorCloseContext(PosixSubsystem* subsystem, size_t fd)
      : subsystem(subsystem),
        fd(fd),
        release(0, false),
        entered(0),
        acquired(0),
        usedAfterClose(0),
        returned(0) {}

  PosixSubsystem* subsystem;
  size_t fd;
  Semaphore release;
  Atomic<size_t> entered;
  Atomic<size_t> acquired;
  Atomic<size_t> usedAfterClose;
  Atomic<size_t> returned;
};

int holdDescriptorAcrossBlock(void* parameter) {
  DescriptorCloseContext* context = reinterpret_cast<DescriptorCloseContext*>(parameter);
  DescriptorLease descriptor;
  context->acquired = context->subsystem->acquireFileDescriptor(context->fd, descriptor) ? 1 : 0;
  context->entered += 1;

  if (!context->release.acquireForCompletion()) {
    context->returned += 1;
    return 1;
  }

  if (descriptor && descriptor->fd == context->fd) {
    context->usedAfterClose += 1;
  }
  context->returned += 1;
  return 0;
}

bool descriptorClosePinning(Process* kernelProcess) {
  constexpr size_t DescriptorNumber = 37;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  Atomic<size_t> destructions(0);
  DescriptorRetirementProbe* probe = new DescriptorRetirementProbe(destructions);
  probe->fd = DescriptorNumber;
  subsystem->addFileDescriptor(DescriptorNumber, probe);

  DescriptorCloseContext context(subsystem, DescriptorNumber);
  Thread* worker =
      new Thread(kernelProcess, holdDescriptorAcrossBlock, &context, nullptr, false, true, true);
  worker->setName("hosted descriptor pin holder");

  bool passed = worker->start();
  bool blocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && passed; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (context.entered && worker->getWaitDebugInfo(info) && info.queue && info.queued &&
        info.channelOwner == &context.release && worker->getStatus() == Thread::Sleeping) {
      blocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  passed = passed && blocked && context.acquired == 1;
  DescriptorLease closing;
  const bool closeAcquired = subsystem->acquireFileDescriptor(DescriptorNumber, closing);
  const bool closed = closeAcquired && subsystem->closeFileDescriptor(DescriptorNumber, closing);
  closing.reset();
  passed = passed && closed;

  DescriptorLease unpublished;
  passed = passed && !subsystem->acquireFileDescriptor(DescriptorNumber, unpublished) &&
           destructions == 0;

  context.release.release();
  passed = worker->join() && passed;
  passed = passed && context.returned == 1 && context.usedAfterClose == 1 && destructions == 1;

  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-close-pinning: "
        "close did not unpublish immediately while retaining the active "
        "operation");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-close-pinning");
  return true;
}

bool descriptorCloseGeneration(Process* kernelProcess) {
  constexpr size_t DescriptorNumber = 38;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  Atomic<size_t> oldDestructions(0);
  Atomic<size_t> replacementDestructions(0);
  DescriptorRetirementProbe* oldDescriptor = new DescriptorRetirementProbe(oldDestructions);
  oldDescriptor->fd = DescriptorNumber;
  oldDescriptor->offset = 1;
  subsystem->addFileDescriptor(DescriptorNumber, oldDescriptor);

  DescriptorLease oldLease;
  bool passed = subsystem->acquireFileDescriptor(DescriptorNumber, oldLease);

  DescriptorRetirementProbe* replacement = new DescriptorRetirementProbe(replacementDestructions);
  replacement->fd = DescriptorNumber;
  replacement->offset = 2;
  subsystem->addFileDescriptor(DescriptorNumber, replacement);

  // An in-flight close of the old generation must not remove a descriptor
  // which has since reused the same numeric fd.
  passed = passed && !subsystem->closeFileDescriptor(DescriptorNumber, oldLease) &&
           oldDestructions == 0 && replacementDestructions == 0;

  DescriptorLease replacementLease;
  passed = passed && subsystem->acquireFileDescriptor(DescriptorNumber, replacementLease) &&
           replacementLease->offset == 2;

  oldLease.reset();
  passed = passed && oldDestructions == 1 && replacementDestructions == 0;

  const bool replacementClosed = subsystem->closeFileDescriptor(DescriptorNumber, replacementLease);
  DescriptorLease unpublished;
  passed = passed && replacementClosed &&
           !subsystem->acquireFileDescriptor(DescriptorNumber, unpublished) &&
           replacementDestructions == 0;
  replacementLease.reset();
  passed = passed && replacementDestructions == 1;

  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-close-generation: "
        "an old close removed a reused descriptor generation");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-close-generation");
  return true;
}

struct PollCloseReuseContext {
  explicit PollCloseReuseContext(size_t fd)
      : descriptor{static_cast<int>(fd), POLLIN, 0}, result(-2), entered(0), returned(0) {}

  struct pollfd descriptor;
  Atomic<int> result;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
};

int pollAcrossCloseReuse(void* parameter) {
  PollCloseReuseContext* context = reinterpret_cast<PollCloseReuseContext*>(parameter);
  context->entered += 1;
  // A missed wakeup must report a bounded regression failure. The old
  // infinite poll made the outer harness timeout the only evidence.
  context->result = posix_poll_safe(&context->descriptor, 1, PollCloseReuseTimeoutMilliseconds);
  context->returned += 1;
  return context->result == 1 ? 0 : 1;
}

bool pollCloseReuseCleanup(Process* kernelProcess) {
  constexpr size_t DescriptorNumber = 39;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  Atomic<size_t> aRegistrations(0);
  Atomic<size_t> aUnpolls(0);
  Atomic<size_t> aNetworkDestructions(0);
  Atomic<size_t> aDescriptorDestructions(0);
  PollGenerationProbe* aNetwork =
      new PollGenerationProbe(aRegistrations, aUnpolls, aNetworkDestructions);
  SharedPointer<NetworkSyscalls> aNetworkKeepalive(aNetwork);
  DescriptorRetirementProbe* aDescriptor = new DescriptorRetirementProbe(aDescriptorDestructions);
  aDescriptor->fd = DescriptorNumber;
  aDescriptor->offset = 1;
  aDescriptor->networkImpl = aNetworkKeepalive;
  subsystem->addFileDescriptor(DescriptorNumber, aDescriptor);

  Atomic<size_t> bRegistrations(0);
  Atomic<size_t> bUnpolls(0);
  Atomic<size_t> bNetworkDestructions(0);
  Atomic<size_t> bDescriptorDestructions(0);
  PollGenerationProbe* bNetwork =
      new PollGenerationProbe(bRegistrations, bUnpolls, bNetworkDestructions);
  SharedPointer<NetworkSyscalls> bNetworkKeepalive(bNetwork);
  DescriptorRetirementProbe* bDescriptor = new DescriptorRetirementProbe(bDescriptorDestructions);
  bDescriptor->fd = DescriptorNumber;
  bDescriptor->offset = 2;
  bDescriptor->networkImpl = bNetworkKeepalive;

  PollCloseReuseContext context(DescriptorNumber);
  Thread* worker = new Thread(process, pollAcrossCloseReuse, &context, nullptr, false, true, true);
  worker->setName("hosted poll close-reuse worker");
  const bool started = worker->start();
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "worker-started");
  bool blockedOnA = false;
  for (size_t attempt = 0; attempt < HostedAttempts && started; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (context.entered && aRegistrations && worker->getWaitDebugInfo(info) && info.queue &&
        info.queued && info.channelOwner == aNetwork->waiterAddress() &&
        worker->getStatus() == Thread::Sleeping) {
      blockedOnA = true;
      break;
    }
    Scheduler::instance().yield();
  }

  bool passed = started && blockedOnA && aRegistrations == 1;
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "waiter-published-a blocked="
      << blockedOnA << " registrations=" << aRegistrations.value());
  DescriptorLease closingA;
  const bool acquiredA = subsystem->acquireFileDescriptor(DescriptorNumber, closingA);
  const bool closedA = acquiredA && subsystem->closeFileDescriptor(DescriptorNumber, closingA);
  closingA.reset();
  subsystem->addFileDescriptor(DescriptorNumber, bDescriptor);
  passed = passed && closedA && aDescriptorDestructions == 0 && aNetworkDestructions == 0;
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "closed-a-published-b");

  if (aRegistrations) {
    aNetwork->makeReadable();
  } else {
    // Failure cleanup: if the worker did not pin A, allow any lookup of B
    // to finish rather than leaving the hosted smoke run blocked.
    bNetwork->makeReadable();
  }
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "release-published");

  const bool joined = started && worker->joinForCompletion();
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "worker-returned joined="
      << joined << " returned=" << context.returned.value()
      << " result=" << context.result.value());
  passed = passed && joined && context.returned == 1 && context.result == 1 &&
           (context.descriptor.revents & POLLIN) && aUnpolls == 1 && bUnpolls == 0 &&
           bRegistrations == 0 && aDescriptorDestructions == 1 && aNetworkDestructions == 0;
  aNetworkKeepalive.reset();
  passed = passed && aNetworkDestructions == 1;

  DescriptorLease closingB;
  const bool acquiredB = subsystem->acquireFileDescriptor(DescriptorNumber, closingB);
  const bool closedB = acquiredB && subsystem->closeFileDescriptor(DescriptorNumber, closingB);
  closingB.reset();
  passed = passed && closedB && bDescriptorDestructions == 1 && bNetworkDestructions == 0;
  bNetworkKeepalive.reset();
  passed = passed && bNetworkDestructions == 1;

  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL poll-close-reuse-cleanup: "
        "poll cleanup followed the reused fd instead of its registered "
        "descriptor generation");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS poll-close-reuse-cleanup");
  return true;
}

struct PosixTeardownContext {
  explicit PosixTeardownContext(Process* process)
      : process(process),
        releaseGate(0, false),
        holderEntered(0),
        holderReturned(0),
        reaperEntered(0),
        processDeleted(0) {}

  Process* process;
  Semaphore releaseGate;
  Atomic<size_t> holderEntered;
  Atomic<size_t> holderReturned;
  Atomic<size_t> reaperEntered;
  Atomic<size_t> processDeleted;
};

int holdMemoryMapLifecycleGate(void* parameter) {
  PosixTeardownContext* context = reinterpret_cast<PosixTeardownContext*>(parameter);
  MemoryMapManager::instance().acquireLifecycleGateForHostedTest();
  context->holderEntered += 1;
  const bool released = context->releaseGate.acquireForCompletion();
  MemoryMapManager::instance().releaseLifecycleGateForHostedTest();
  context->holderReturned += 1;
  return released ? 0 : 1;
}

int deletePosixProcess(void* parameter) {
  PosixTeardownContext* context = reinterpret_cast<PosixTeardownContext*>(parameter);
  context->reaperEntered += 1;
  delete context->process;
  context->processDeleted += 1;
  return 0;
}

bool posixTeardownContention(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  PosixTeardownContext context(process);

  Thread* holder =
      new Thread(kernelProcess, holdMemoryMapLifecycleGate, &context, nullptr, false, true, true);
  holder->setName("hosted mmap lifecycle holder");
  bool passed = holder->start();

  for (size_t attempt = 0; attempt < HostedAttempts && passed && !context.holderEntered;
       ++attempt) {
    Scheduler::instance().yield();
  }
  passed = passed && context.holderEntered == 1;

  Thread* reaper = nullptr;
  bool blocked = false;
  if (passed) {
    reaper = new Thread(kernelProcess, deletePosixProcess, &context, nullptr, false, true, true);
    reaper->setName("hosted POSIX process reaper");
    passed = reaper->start();

    for (size_t attempt = 0; attempt < HostedAttempts && passed; ++attempt) {
      Thread::WaitDebugInfo info = {};
      if (context.reaperEntered && reaper->getWaitDebugInfo(info) && info.queue && info.queued &&
          info.channelOwner == MemoryMapManager::instance().lifecycleGateAddressForHostedTest() &&
          reaper->getStatus() == Thread::Sleeping) {
        blocked = true;
        break;
      }
      Scheduler::instance().yield();
    }
    passed = passed && blocked && context.processDeleted == 0;
  }

  context.releaseGate.release();
  passed = holder->join() && passed;
  if (reaper) {
    passed = reaper->join() && passed;
    passed = passed && context.processDeleted == 1;
  } else {
    delete process;
  }

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-teardown-contention: "
        "real PosixSubsystem destruction did not sleep and resume on "
        "the memory-map lifecycle gate");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-teardown-contention");
  return true;
}

bool zeroResultWinsSignal(Thread* thread) {
  thread->setErrno(0);
  thread->setInterruptionReason(Thread::InterruptedBySignal);
  const bool completed = finishInterruptibleSocketCall(thread, static_cast<ssize_t>(0));
  const bool passed =
      completed && thread->getInterruptionReason() == Thread::NotInterrupted && !thread->getErrno();
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL socket-zero-result-signal: "
        "EOF or zero-length success was replaced with EINTR");
    thread->clearInterruption();
    thread->setErrno(0);
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS socket-zero-result-signal");
  return true;
}

bool cloneStateDropsParentErrnoDestination() {
  long error = 0;
  SyscallState parent = {};
  parent.error_ptr = reinterpret_cast<uintptr_t>(&error);
  parent.result = 37;

  const SyscallState child = posix_copy_clone_state(parent);
  const bool passed = !child.error_ptr && child.result == parent.result &&
                      parent.error_ptr == reinterpret_cast<uintptr_t>(&error);
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL clone-errno-lifetime: "
        "the child retained its parent's stack-local errno destination");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS clone-errno-lifetime");
  return true;
}

bool failedPinnedModuleRejectsUnload() {
  Module module;
  module.name.assign("hosted-failed-pinned-probe");
  module.unloadable = false;
  module.status = Module::Failed;

  if (KernelElf::claimModuleUnloadForTest(&module) != KernelElf::TestUnloadPinned ||
      module.status != Module::Failed || module.unloadComplete) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL failed-pinned-module: "
        "failed initialisation did not preserve its pinned module image");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS failed-pinned-module");
  return true;
}

ModuleInfo* findStaticModuleInfo(const char* name, size_t& matches) {
  ModuleInfo* match = nullptr;
  matches = 0;
  for (size_t i = 0; i < g_StaticDriverN; ++i) {
    ModuleInfo* info = g_StaticDrivers[i];
    if (info && info->name && !StringCompare(info->name, name)) {
      match = info;
      ++matches;
    }
  }
  return match;
}

bool moduleInfoDependsOn(ModuleInfo* info, const char* dependency, bool optional = false) {
  const char** dependencies = optional ? info->opt_dependencies : info->dependencies;
  if (!dependencies) {
    return false;
  }
  for (size_t i = 0; dependencies[i]; ++i) {
    if (!StringCompare(dependencies[i], dependency)) {
      return true;
    }
  }
  return false;
}

bool linkerModuleMetadataIsPinned() {
  size_t matches = 0;
  ModuleInfo* linker = findStaticModuleInfo("linker", matches);

  if (matches != 1 || !linker || linker->unloadable || linker->runtimeUnloadable ||
      !moduleInfoDependsOn(linker, "vfs")) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL linker-pinned-metadata: "
        "the real linker ModuleInfo did not pin its dependency closure");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS linker-pinned-metadata");
  return true;
}

bool filesystemModuleUnloadPolicyIsCorrect() {
  size_t posixMatches = 0;
  size_t mountrootMatches = 0;
  size_t ramfsMatches = 0;
  size_t rawfsMatches = 0;
  ModuleInfo* posix = findStaticModuleInfo("posix", posixMatches);
  ModuleInfo* mountroot = findStaticModuleInfo("mountroot", mountrootMatches);
  ModuleInfo* ramfs = findStaticModuleInfo("ramfs", ramfsMatches);
  ModuleInfo* rawfs = findStaticModuleInfo("rawfs", rawfsMatches);

  const bool metadataValid =
      posixMatches == 1 && mountrootMatches == 1 && ramfsMatches == 1 && rawfsMatches == 1 &&
      posix && mountroot && ramfs && rawfs && posix->unloadable && !posix->runtimeUnloadable &&
      mountroot->unloadable && !mountroot->runtimeUnloadable && !ramfs->unloadable &&
      !ramfs->runtimeUnloadable && rawfs->unloadable && !rawfs->runtimeUnloadable &&
      moduleInfoDependsOn(posix, "mountroot") && moduleInfoDependsOn(posix, "ramfs") &&
      moduleInfoDependsOn(mountroot, "vfs") && moduleInfoDependsOn(mountroot, "rawfs") &&
      moduleInfoDependsOn(mountroot, "ramfs") && moduleInfoDependsOn(mountroot, "fat", true) &&
      moduleInfoDependsOn(mountroot, "ext2", true) &&
      moduleInfoDependsOn(mountroot, "iso9660", true) && moduleInfoDependsOn(ramfs, "vfs") &&
      moduleInfoDependsOn(rawfs, "vfs");
  if (!metadataValid) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL filesystem-unload-policy-metadata: "
        "the real filesystem owner metadata did not encode the expected unload policy or "
        "dependency closure");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS filesystem-unload-policy-metadata");
  return true;
}

bool runtimePinnedModuleAllowsLifecycleCleanup() {
  g_RuntimePinnedLifecycleCalls = 0;

  Module active;
  active.name.assign("hosted-runtime-pinned-active-probe");
  active.exit = runtimePinnedLifecycleProbe;
  active.runtimeUnloadable = false;
  active.status = Module::Active;

  bool runLifecycle = true;
  const KernelElf::TestModuleUnloadClaim explicitActive =
      KernelElf::claimModuleUnloadForTest(&active, false, &runLifecycle);
  const bool explicitActiveValid = explicitActive == KernelElf::TestUnloadRuntimePinned &&
                                   !runLifecycle && active.status == Module::Active &&
                                   !active.unloadComplete && !g_RuntimePinnedLifecycleCalls;
  if (explicitActive == KernelElf::TestUnloadClaimed) {
    KernelElf::completeModuleUnloadForTest(&active);
  }
  if (!explicitActiveValid) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL runtime-pinned-cleanup: "
        "explicit unload escaped the runtime-only lifetime boundary");
    return false;
  }

  const KernelElf::TestModuleUnloadClaim shutdownActive =
      KernelElf::claimModuleUnloadForTest(&active, true, &runLifecycle);
  const bool shutdownActiveValid = shutdownActive == KernelElf::TestUnloadClaimed && runLifecycle &&
                                   active.status == Module::Unloading;
  if (shutdownActive == KernelElf::TestUnloadClaimed) {
    KernelElf::completeModuleUnloadForTest(&active, false, runLifecycle);
  }
  if (!shutdownActiveValid) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL runtime-pinned-cleanup: "
        "shutdown could not claim an active runtime-pinned module");
    return false;
  }

  Module failed;
  failed.name.assign("hosted-runtime-pinned-failed-probe");
  failed.exit = runtimePinnedLifecycleProbe;
  failed.runtimeUnloadable = false;
  failed.status = Module::Failed;
  runLifecycle = true;
  const KernelElf::TestModuleUnloadClaim explicitFailed =
      KernelElf::claimModuleUnloadForTest(&failed, false, &runLifecycle);
  const bool explicitFailedValid = explicitFailed == KernelElf::TestUnloadRuntimePinned &&
                                   !runLifecycle && failed.status == Module::Failed &&
                                   !failed.unloadComplete;
  if (explicitFailed == KernelElf::TestUnloadClaimed) {
    KernelElf::completeModuleUnloadForTest(&failed, true);
  }
  if (!explicitFailedValid) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL runtime-pinned-cleanup: "
        "explicit unload escaped a failed runtime-pinned module");
    return false;
  }

  const KernelElf::TestModuleUnloadClaim failureCleanup =
      KernelElf::claimModuleUnloadForTest(&failed, true, &runLifecycle);
  const bool failureCleanupValid = failureCleanup == KernelElf::TestUnloadClaimed && runLifecycle &&
                                   failed.status == Module::Unloading;
  if (failureCleanup == KernelElf::TestUnloadClaimed) {
    KernelElf::completeModuleUnloadForTest(&failed, true, runLifecycle);
  }
  if (!failureCleanupValid) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL runtime-pinned-cleanup: "
        "failed initialisation could not claim lifecycle cleanup");
    return false;
  }

  if (!active.isUnloaded() || !active.unloadComplete || failed.status != Module::Failed ||
      !failed.unloadComplete || g_RuntimePinnedLifecycleCalls != 2) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL runtime-pinned-cleanup: "
        "shutdown or failure cleanup did not publish completion");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS runtime-pinned-cleanup");
  return true;
}

bool moduleUnloadOwnershipIsRetryable() {
  Module module;
  module.name.assign("hosted-unload-owner-probe");
  module.status = Module::Active;
  Module* fixtures[] = {&module};

  const KernelElf::TestModuleUnloadClaim first =
      KernelElf::claimNamedModuleUnloadForTest(fixtures, 1, "hosted-unload-owner-probe");
  const KernelElf::TestModuleUnloadClaim concurrent =
      KernelElf::claimNamedModuleUnloadForTest(fixtures, 1, "hosted-unload-owner-probe");
  KernelElf::completeModuleUnloadForTest(&module);
  const KernelElf::TestModuleUnloadClaim repeat =
      KernelElf::claimNamedModuleUnloadForTest(fixtures, 1, "hosted-unload-owner-probe");
  const KernelElf::TestModuleUnloadClaim missing =
      KernelElf::claimNamedModuleUnloadForTest(fixtures, 1, "hosted-unload-missing-probe");

  if (first != KernelElf::TestUnloadClaimed || concurrent != KernelElf::TestUnloadBusy ||
      repeat != KernelElf::TestUnloadComplete || missing != KernelElf::TestUnloadUnknown ||
      !module.isUnloaded() || !module.unloadComplete) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL module-unload-ownership: "
        "the first owner, concurrent retry, or completed tombstone was lost");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS module-unload-ownership");
  return true;
}

bool moduleShutdownOrderIsDependencySafe() {
  const char* nicsOptional[] = {"ne2k", nullptr};
  const char* ne2kDependencies[] = {"network-stack", nullptr};

  Module networkStack;
  networkStack.name.assign("network-stack");
  networkStack.status = Module::Active;

  Module nics;
  nics.name.assign("nics");
  nics.depends_opt = nicsOptional;
  nics.runtimeUnloadable = false;
  nics.status = Module::Active;

  Module ne2k;
  ne2k.name.assign("ne2k");
  ne2k.depends = ne2kDependencies;
  ne2k.status = Module::Active;

  Module* modules[] = {&networkStack, &nics, &ne2k};
  Module* order[3] = {};
  const size_t planned = KernelElf::planModuleUnloadOrderForTest(modules, 3, order, 3);
  const size_t repeated = KernelElf::planModuleUnloadOrderForTest(modules, 3, order, 3);

  const char* cycleADependencies[] = {"cycle-b", nullptr};
  const char* cycleBDependencies[] = {"cycle-a", nullptr};
  Module cycleA;
  cycleA.name.assign("cycle-a");
  cycleA.depends = cycleADependencies;
  cycleA.status = Module::Active;
  Module cycleB;
  cycleB.name.assign("cycle-b");
  cycleB.depends = cycleBDependencies;
  cycleB.status = Module::Active;
  Module* cycle[] = {&cycleA, &cycleB};
  Module* cycleOrder[2] = {};
  const size_t cyclicPlanned = KernelElf::planModuleUnloadOrderForTest(cycle, 2, cycleOrder, 2);

  if (planned != 3 || order[0] != &nics || order[1] != &ne2k ||
      order[2] != &networkStack || repeated != 0 || cyclicPlanned != 0 ||
      cycleA.unloadComplete || cycleB.unloadComplete) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL module-shutdown-order: "
        "optional/mandatory dependents were not retired first or a cycle was torn down");
    return false;
  }

  Module permanent;
  permanent.name.assign("permanent-shutdown-probe");
  permanent.unloadable = false;
  permanent.runtimeUnloadable = false;
  permanent.status = Module::Active;
  Module runtimePinned;
  runtimePinned.name.assign("runtime-pinned-shutdown-probe");
  runtimePinned.runtimeUnloadable = false;
  runtimePinned.status = Module::Active;
  Module* retentionModules[] = {&permanent, &runtimePinned};
  Module* retentionOrder[2] = {};
  const size_t retentionPlanned =
      KernelElf::planModuleUnloadOrderForTest(retentionModules, 2, retentionOrder, 2);

  if (retentionPlanned != 1 || retentionOrder[0] != &runtimePinned ||
      !runtimePinned.unloadComplete || permanent.unloadComplete) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL module-shutdown-retention-policy: "
        "a runtime-only module was retained or a permanent pin was retired");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS module-shutdown-order");
  NOTICE("HOSTED-SYSCALL-TEST: PASS module-shutdown-retention-policy");
  return true;
}

bool publishTerminalBlockedHandlerFixture(Process* kernelProcess) {
  TerminalBlockedHandlerContext* context = new TerminalBlockedHandlerContext;
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  process->description() = "hosted blocked POSIX handler shutdown fixture";
  Thread* thread =
      new Thread(process, terminalBlockedHandlerEntry, context, nullptr, false, true, true);
  thread->setName("hosted blocked POSIX handler fixture");
  context->thread = thread;
  process->publish();

  g_TerminalBlockedHandlerContext = context;
  SyscallManager::instance().setHandlerPinHook(terminalBlockedHandlerPin);
  const bool started = thread->start();

  bool blocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && started; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (context->hookEntered == static_cast<size_t>(1) && thread->getWaitDebugInfo(info) &&
        info.queue && info.queued && info.channelOwner == &context->blocker &&
        thread->getStatus() == Thread::Sleeping) {
      blocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  SyscallManager::instance().setHandlerPinHook(nullptr);

  if (!started || !blocked || context->exitStaged != static_cast<size_t>(1) ||
      context->releasedByTermination || context->unexpectedRelease || context->syscallReturned) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-terminal-blocked-handler-fixture: "
        "the POSIX handler was not admitted and blocked with a staged process exit");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-terminal-blocked-handler-fixture-published");
  return true;
}

bool runRegressions() {
  NOTICE("HOSTED-SYSCALL-TEST: BEGIN real-event-boundaries");
  Thread* thread = Processor::information().getCurrentThread();
  if (!thread || thread->getStateLevel()) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL real-event-boundaries: "
        "module initialisation was not at base state");
    return false;
  }

  Process* kernelProcess = Scheduler::instance().getKernelProcess();
  if (!kernelProcess) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-close-pinning");
  if (!descriptorClosePinning(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-close-generation");
  if (!descriptorCloseGeneration(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN poll-close-reuse-cleanup");
  if (!pollCloseReuseCleanup(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN posix-teardown-contention");
  if (!posixTeardownContention(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN socket-zero-result-signal");
  if (!zeroResultWinsSignal(thread)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN clone-errno-lifetime");
  if (!cloneStateDropsParentErrnoDestination()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN failed-pinned-module");
  if (!failedPinnedModuleRejectsUnload()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN linker-pinned-metadata");
  if (!linkerModuleMetadataIsPinned()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN filesystem-unload-policy-metadata");
  if (!filesystemModuleUnloadPolicyIsCorrect()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN runtime-pinned-cleanup");
  if (!runtimePinnedModuleAllowsLifecycleCleanup()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN module-unload-ownership");
  if (!moduleUnloadOwnershipIsRetryable()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN module-shutdown-order");
  if (!moduleShutdownOrderIsDependencySafe()) {
    return false;
  }

  SyscallManager& manager = SyscallManager::instance();
  static char pedigreeCModule[] = "pedigree-c";
  static char lwipModule[] = "lwip";
  static char networkStackModule[] = "network-stack";
  const uintptr_t sigretResult = manager.syscall(posix, PEDIGREE_SIGRET);
  const uintptr_t unwindResult = manager.syscall(posix, PEDIGREE_UNWIND_SIGNAL);
  const uintptr_t eventReturnResult = manager.syscall(pedigree_c, PEDIGREE_EVENT_RETURN);
  const uintptr_t selfUnloadResult = manager.syscall(pedigree_c, PEDIGREE_MODULE_UNLOAD,
                                                     reinterpret_cast<uintptr_t>(pedigreeCModule));
  const uintptr_t stillLoadedResult = manager.syscall(pedigree_c, PEDIGREE_MODULE_IS_LOADED,
                                                      reinterpret_cast<uintptr_t>(pedigreeCModule));
  const uintptr_t lwipUnloadResult =
      manager.syscall(pedigree_c, PEDIGREE_MODULE_UNLOAD,
                      reinterpret_cast<uintptr_t>(lwipModule));
  const uintptr_t lwipStillLoadedResult =
      manager.syscall(pedigree_c, PEDIGREE_MODULE_IS_LOADED,
                      reinterpret_cast<uintptr_t>(lwipModule));
  const uintptr_t networkStackUnloadResult =
      manager.syscall(pedigree_c, PEDIGREE_MODULE_UNLOAD,
                      reinterpret_cast<uintptr_t>(networkStackModule));
  const uintptr_t networkStackStillLoadedResult =
      manager.syscall(pedigree_c, PEDIGREE_MODULE_IS_LOADED,
                      reinterpret_cast<uintptr_t>(networkStackModule));

  if (sigretResult != static_cast<uintptr_t>(-1) || unwindResult != static_cast<uintptr_t>(-1) ||
      eventReturnResult != static_cast<uintptr_t>(-1) ||
      selfUnloadResult != static_cast<uintptr_t>(-1) || stillLoadedResult != 1 ||
      lwipUnloadResult != static_cast<uintptr_t>(-1) || lwipStillLoadedResult != 1 ||
      networkStackUnloadResult != static_cast<uintptr_t>(-1) ||
      networkStackStillLoadedResult != 1 ||
      thread->getStateLevel() || thread->getErrno()) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL real-event-boundaries: "
        "a public misuse path escaped its lifetime boundary");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS real-event-boundaries");

  PosixProcess* terminalFixture = new PosixProcess(kernelProcess);
  terminalFixture->setSubsystem(new PosixSubsystem);
  terminalFixture->description() = "hosted zero-thread POSIX shutdown fixture";
  terminalFixture->publish();
  if (terminalFixture->getNumThreads() != 0 || terminalFixture->getType() != Process::Posix) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-terminal-drain-fixture: "
        "fixture was not published as an ownerless POSIX process");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-terminal-drain-fixture-published");

  if (!posixDuplicateInitRollbackPreservesProcessForTest(terminalFixture)) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-duplicate-init-rollback: "
        "an unowned duplicate initialisation retired an existing POSIX process");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-duplicate-init-rollback-preserved-process");

  PosixProcess* createdFixture = new PosixProcess(kernelProcess);
  createdFixture->setSubsystem(new PosixSubsystem);
  createdFixture->description() = "hosted Created-thread POSIX shutdown fixture";
  Thread* createdThread =
      new Thread(createdFixture, terminalCreatedFixtureEntry, nullptr, nullptr, false, true, true);
  createdThread->setName("hosted terminal Created-thread fixture");
  createdFixture->publish();
  if (createdFixture->getNumThreads() != 1 || createdThread->getStatus() != Thread::Created ||
      createdFixture->getType() != Process::Posix) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-terminal-drain-created-fixture: "
        "fixture did not retain its unstarted ordinary entry");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-terminal-drain-created-fixture-published");

  if (!publishTerminalBlockedHandlerFixture(kernelProcess)) {
    return false;
  }
  return true;
}

bool entry() {
  const bool passed = runRegressions();
  system_reset();
  return passed;
}

void exit() {
  TerminalBlockedHandlerContext* context = g_TerminalBlockedHandlerContext;
  g_TerminalBlockedHandlerContext = nullptr;
  if (!context) {
    return;
  }

  if (context->hookEntered != static_cast<size_t>(1) ||
      context->exitStaged != static_cast<size_t>(1) ||
      context->releasedByTermination != static_cast<size_t>(1) || context->unexpectedRelease ||
      context->syscallReturned) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-terminal-blocked-handler-release: "
        "terminal process teardown did not release the admitted handler before module exit");
  } else {
    NOTICE("HOSTED-SYSCALL-TEST: PASS posix-terminal-blocked-handler-released-by-process-exit");
  }
  delete context;
}
}  // namespace

MODULE_INFO("hosted-syscall-smoke", &entry, &exit, "posix", "pedigree-c");
