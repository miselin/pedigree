/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/ExecutionContext.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

#include "modules/system/vfs/Filesystem.h"
#include "modules/system/vfs/VFS.h"

namespace {
constexpr size_t Attempts = 10000;
constexpr bool DontPickCore = HOSTED;

class TestDisk final : public Disk {
 public:
  uintptr_t read(uint64_t) override {
    return 0;
  }

  size_t getSize() const override {
    return 0;
  }

  size_t getBlockSize() const override {
    return 512;
  }

  bool pin(uint64_t) override {
    return false;
  }

  void unpin(uint64_t) override {}
};

class TestFilesystem final : public Filesystem {
 public:
  explicit TestFilesystem(Atomic<size_t>* destructions)
      : m_Destructions(destructions), m_Label("vfs-callback-test") {}

  ~TestFilesystem() override {
    if (m_Destructions) {
      *m_Destructions += 1;
    }
  }

  bool initialise(Disk*) override {
    return true;
  }

  File* getRoot() const override {
    return nullptr;
  }

  const String& getVolumeLabel() const override {
    return m_Label;
  }

  bool remove(File*, File*) override {
    return false;
  }

 protected:
  bool createFile(File*, const String&, uint32_t) override {
    return false;
  }

  bool createDirectory(File*, const String&, uint32_t) override {
    return false;
  }

  bool createSymlink(File*, const String&, const String&) override {
    return false;
  }

 private:
  Atomic<size_t>* m_Destructions;
  String m_Label;
};

bool check(bool condition, const char* test, const char* detail) {
  if (condition) {
    return true;
  }
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  ERROR("QEMU-CONCURRENCY-TEST: FAIL " << test << ": " << detail);
#else
  ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
#endif
  return false;
}

bool waitForCallbackDrain(Thread* thread, uintptr_t callback) {
  for (size_t attempt = 0; attempt < Attempts; ++attempt) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (thread->getWaitDebugInfo(info) && info.queue && info.channelOwner && info.queued &&
        thread->getDebugState(debugAddress) == Thread::CallbackDrain && debugAddress == callback) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

struct ProbeDrainContext {
  ProbeDrainContext()
      : callbackEntered(0),
        callbackRelease(0),
        callbackCalls(0),
        callbackAfterRemoval(0),
        removalReturned(0),
        removalSucceeded(0),
        destroyedFilesystems(0),
        callbackProcessor(static_cast<size_t>(-1)),
        removerProcessor(static_cast<size_t>(-1)),
        failures(0),
        mountResult(false),
        mounted(nullptr) {}

  VFS registry;
  TestDisk disk;
  Semaphore callbackEntered;
  Semaphore callbackRelease;
  Atomic<size_t> callbackCalls;
  Atomic<size_t> callbackAfterRemoval;
  Atomic<size_t> removalReturned;
  Atomic<size_t> removalSucceeded;
  Atomic<size_t> destroyedFilesystems;
  Atomic<size_t> callbackProcessor;
  Atomic<size_t> removerProcessor;
  Atomic<size_t> failures;
  bool mountResult;
  Filesystem* mounted;
};

ProbeDrainContext* g_ProbeDrainContext = nullptr;

Filesystem* blockingProbe(Disk*) {
  ProbeDrainContext* context = g_ProbeDrainContext;
  context->callbackCalls += 1;
  context->callbackProcessor = Processor::id();
  context->callbackEntered.release();
  if (!context->callbackRelease.acquireForCompletion()) {
    context->failures += 1;
  }
  if (context->removalReturned) {
    context->callbackAfterRemoval += 1;
  }
  return new TestFilesystem(&context->destroyedFilesystems);
}

int dispatchBlockingProbe(void* parameter) {
  ProbeDrainContext* context = reinterpret_cast<ProbeDrainContext*>(parameter);
  String stableName;
  context->mountResult = context->registry.mount(&context->disk, stableName, &context->mounted);
  return 0;
}

int removeBlockingProbe(void* parameter) {
  ProbeDrainContext* context = reinterpret_cast<ProbeDrainContext*>(parameter);
  context->removerProcessor = Processor::id();
  context->removalSucceeded = context->registry.removeProbeCallback(blockingProbe) ? 1 : 0;
  context->removalReturned = 1;
  return 0;
}

bool probeCallbackDrain(size_t& callbackProcessor, size_t& removerProcessor) {
  ProbeDrainContext context;
  g_ProbeDrainContext = &context;
  context.registry.addProbeCallback(blockingProbe);

  Process* process = Scheduler::instance().getKernelProcess();
  Thread* dispatcher =
      new Thread(process, dispatchBlockingProbe, &context, nullptr, false, DontPickCore);
  dispatcher->setName("VFS blocking probe dispatcher");
  const bool callbackEntered = context.callbackEntered.acquireForCompletion();
  const bool interruptsWereEnabled = Processor::getInterrupts();
  bool nonWaitableRemoval = false;
  bool nonWaitableContextObserved = false;
  {
    ExecutionContextGuard nonWaitable(ExecutionContext::HardDeviceIrq);
    nonWaitableRemoval = context.registry.removeProbeCallback(blockingProbe);
    nonWaitableContextObserved = Processor::getInterrupts() &&
                                 Processor::executionContext() == ExecutionContext::HardDeviceIrq;
  }
  const bool waitableContextRestored =
      Processor::executionContext() == ExecutionContext::WaitableThread;
  Processor::setInterrupts(false);
  const bool nonYieldingRemoval = context.registry.removeProbeCallback(blockingProbe);
  const bool interruptsStayedDisabled = !Processor::getInterrupts();
  Processor::setInterrupts(interruptsWereEnabled);
  Thread* remover =
      new Thread(process, removeBlockingProbe, &context, nullptr, false, DontPickCore);
  remover->setName("VFS blocking probe remover");
  const bool observedDrain =
      callbackEntered && waitForCallbackDrain(remover, reinterpret_cast<uintptr_t>(blockingProbe));
  const bool returnedEarly = context.removalReturned != static_cast<size_t>(0);

  context.callbackRelease.release();
  const bool dispatcherJoined = dispatcher->joinForCompletion();
  const bool removerJoined = remover->joinForCompletion();
  const size_t callsAfterRemoval = context.callbackCalls;
  String stableName;
  Filesystem* unexpected = nullptr;
  const bool mountedAfterRemoval = context.registry.mount(&context.disk, stableName, &unexpected);
  g_ProbeDrainContext = nullptr;

  if (unexpected) {
    context.registry.unregisterFilesystem(unexpected);
  }
  callbackProcessor = context.callbackProcessor;
  removerProcessor = context.removerProcessor;

  const bool passed =
      check(callbackEntered && interruptsWereEnabled && !nonWaitableRemoval &&
                nonWaitableContextObserved && waitableContextRestored && !nonYieldingRemoval &&
                interruptsStayedDisabled && observedDrain && !returnedEarly && dispatcherJoined &&
                removerJoined && !context.failures &&
                context.removalSucceeded == static_cast<size_t>(1),
            "vfs-probe-callback-lifetime",
            "probe removal blocked without yield permission or did not drain on retry") &&
      check(!context.mountResult && !context.mounted && !mountedAfterRemoval && !unexpected &&
                callsAfterRemoval == static_cast<size_t>(1) &&
                context.callbackCalls == callsAfterRemoval && !context.callbackAfterRemoval &&
                context.destroyedFilesystems == static_cast<size_t>(1),
            "vfs-probe-callback-lifetime",
            "a closed probe published or leaked its returned filesystem");
  if (passed) {
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
    NOTICE("QEMU-CONCURRENCY-TEST: PASS vfs-probe-callback-lifetime");
#else
    NOTICE("HOSTED-WAIT-TEST: PASS vfs-probe-callback-lifetime");
#endif
  }
  return passed;
}

struct MountDrainContext {
  MountDrainContext()
      : callbackEntered(0),
        callbackRelease(0),
        callbackCalls(0),
        callbackAfterRemoval(0),
        removalReturned(0),
        removalSucceeded(0),
        probeRemovalReturned(0),
        probeRemovalSucceeded(0),
        destroyedFilesystems(0),
        callbackProcessor(static_cast<size_t>(-1)),
        removerProcessor(static_cast<size_t>(-1)),
        failures(0),
        mountResult(false),
        mounted(nullptr) {}

  VFS registry;
  TestDisk disk;
  Semaphore callbackEntered;
  Semaphore callbackRelease;
  Atomic<size_t> callbackCalls;
  Atomic<size_t> callbackAfterRemoval;
  Atomic<size_t> removalReturned;
  Atomic<size_t> removalSucceeded;
  Atomic<size_t> probeRemovalReturned;
  Atomic<size_t> probeRemovalSucceeded;
  Atomic<size_t> destroyedFilesystems;
  Atomic<size_t> callbackProcessor;
  Atomic<size_t> removerProcessor;
  Atomic<size_t> failures;
  bool mountResult;
  Filesystem* mounted;
};

MountDrainContext* g_MountDrainContext = nullptr;

Filesystem* successfulProbe(Disk*) {
  return new TestFilesystem(&g_MountDrainContext->destroyedFilesystems);
}

void blockingMountCallback() {
  MountDrainContext* context = g_MountDrainContext;
  context->callbackCalls += 1;
  context->callbackProcessor = Processor::id();
  context->callbackEntered.release();
  if (!context->callbackRelease.acquireForCompletion()) {
    context->failures += 1;
  }
  if (context->removalReturned || context->probeRemovalReturned) {
    context->callbackAfterRemoval += 1;
  }
}

int dispatchBlockingMount(void* parameter) {
  MountDrainContext* context = reinterpret_cast<MountDrainContext*>(parameter);
  String stableName;
  context->mountResult = context->registry.mount(&context->disk, stableName, &context->mounted);
  return 0;
}

int removeBlockingMount(void* parameter) {
  MountDrainContext* context = reinterpret_cast<MountDrainContext*>(parameter);
  context->removerProcessor = Processor::id();
  context->removalSucceeded = context->registry.removeMountCallback(blockingMountCallback) ? 1 : 0;
  context->removalReturned = 1;
  return 0;
}

int removeSuccessfulProbe(void* parameter) {
  MountDrainContext* context = reinterpret_cast<MountDrainContext*>(parameter);
  context->probeRemovalSucceeded = context->registry.removeProbeCallback(successfulProbe) ? 1 : 0;
  context->probeRemovalReturned = 1;
  return 0;
}

bool mountCallbackDrain(size_t& callbackProcessor, size_t& removerProcessor) {
  MountDrainContext context;
  g_MountDrainContext = &context;
  context.registry.addProbeCallback(successfulProbe);
  context.registry.addMountCallback(blockingMountCallback);
  context.registry.addMountCallback(blockingMountCallback);

  Process* process = Scheduler::instance().getKernelProcess();
  Thread* dispatcher =
      new Thread(process, dispatchBlockingMount, &context, nullptr, false, DontPickCore);
  dispatcher->setName("VFS blocking mount dispatcher");
  const bool callbackEntered = context.callbackEntered.acquireForCompletion();
  Thread* remover =
      new Thread(process, removeBlockingMount, &context, nullptr, false, DontPickCore);
  remover->setName("VFS blocking mount remover");
  Thread* probeRemover =
      new Thread(process, removeSuccessfulProbe, &context, nullptr, false, DontPickCore);
  probeRemover->setName("VFS committed probe remover");
  const bool observedMountDrain =
      callbackEntered &&
      waitForCallbackDrain(remover, reinterpret_cast<uintptr_t>(blockingMountCallback));
  const bool observedProbeDrain =
      callbackEntered &&
      waitForCallbackDrain(probeRemover, reinterpret_cast<uintptr_t>(successfulProbe));
  const bool returnedEarly = context.removalReturned || context.probeRemovalReturned;

  context.callbackRelease.release();
  const bool dispatcherJoined = dispatcher->joinForCompletion();
  const bool removerJoined = remover->joinForCompletion();
  const bool probeRemoverJoined = probeRemover->joinForCompletion();
  if (context.mounted) {
    context.registry.unregisterFilesystem(context.mounted);
    context.mounted = nullptr;
  }

  const size_t callsAfterRemoval = context.callbackCalls;
  context.registry.addProbeCallback(successfulProbe);
  String stableName;
  Filesystem* secondMounted = nullptr;
  const bool mountedAfterRemoval =
      context.registry.mount(&context.disk, stableName, &secondMounted);
  if (secondMounted) {
    context.registry.unregisterFilesystem(secondMounted);
  }
  const bool probeRemoved = context.registry.removeProbeCallback(successfulProbe);
  g_MountDrainContext = nullptr;

  callbackProcessor = context.callbackProcessor;
  removerProcessor = context.removerProcessor;
  const bool passed =
      check(callbackEntered && observedMountDrain && observedProbeDrain && !returnedEarly &&
                dispatcherJoined && removerJoined && probeRemoverJoined && !context.failures &&
                context.removalSucceeded == static_cast<size_t>(1) &&
                context.probeRemovalSucceeded == static_cast<size_t>(1),
            "vfs-mount-callback-lifetime",
            "mount or committed probe removal did not drain through publication") &&
      check(context.mountResult && mountedAfterRemoval && probeRemoved &&
                callsAfterRemoval == static_cast<size_t>(1) &&
                context.callbackCalls == callsAfterRemoval && !context.callbackAfterRemoval &&
                context.destroyedFilesystems == static_cast<size_t>(2),
            "vfs-mount-callback-lifetime",
            "a retired mount callback ran again or leaked its test filesystem");
  if (passed) {
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
    NOTICE("QEMU-CONCURRENCY-TEST: PASS vfs-mount-callback-lifetime");
#else
    NOTICE("HOSTED-WAIT-TEST: PASS vfs-mount-callback-lifetime");
#endif
  }
  return passed;
}

struct SelfRemovalContext {
  SelfRemovalContext() : calls(0), deferred(0), idleCalls(0), destroyedFilesystems(0) {}

  VFS registry;
  TestDisk disk;
  Atomic<size_t> calls;
  Atomic<size_t> deferred;
  Atomic<size_t> idleCalls;
  Atomic<size_t> destroyedFilesystems;
};

SelfRemovalContext* g_SelfRemovalContext = nullptr;

Filesystem* selfRemovingProbe(Disk*) {
  SelfRemovalContext* context = g_SelfRemovalContext;
  const size_t call = context->calls += 1;
  if (!context->registry.removeProbeCallback(selfRemovingProbe)) {
    context->deferred += 1;
  }
  if (call == static_cast<size_t>(1)) {
    context->registry.addProbeCallback(selfRemovingProbe);
  }
  return new TestFilesystem(&context->destroyedFilesystems);
}

Filesystem* idleProbe(Disk*) {
  g_SelfRemovalContext->idleCalls += 1;
  return nullptr;
}

bool probeSelfRemoval() {
  SelfRemovalContext context;
  g_SelfRemovalContext = &context;
  context.registry.addProbeCallback(selfRemovingProbe);
  context.registry.addProbeCallback(selfRemovingProbe);
  context.registry.addProbeCallback(idleProbe);

  bool idleNonWaitableRemoval = false;
  bool hardContextObserved = false;
  {
    ExecutionContextGuard hardContext(ExecutionContext::HardDeviceIrq);
    idleNonWaitableRemoval = context.registry.removeProbeCallback(idleProbe);
    hardContextObserved = Processor::executionContext() == ExecutionContext::HardDeviceIrq;
  }
  const bool idleExternallyRetired = context.registry.removeProbeCallback(idleProbe);

  String stableName;
  Filesystem* mounted = nullptr;
  const bool firstMounted = context.registry.mount(&context.disk, stableName, &mounted);
  if (mounted) {
    context.registry.unregisterFilesystem(mounted);
    mounted = nullptr;
  }
  const bool secondMounted = context.registry.mount(&context.disk, stableName, &mounted);
  const bool externallyRetired = context.registry.removeProbeCallback(selfRemovingProbe);
  const bool thirdMounted = context.registry.mount(&context.disk, stableName, &mounted);
  g_SelfRemovalContext = nullptr;

  const bool passed = check(
      firstMounted && !secondMounted && !thirdMounted && !mounted && !idleNonWaitableRemoval &&
          hardContextObserved && idleExternallyRetired && externallyRetired &&
          context.calls == static_cast<size_t>(2) && context.deferred == static_cast<size_t>(2) &&
          !context.idleCalls && context.destroyedFilesystems == static_cast<size_t>(2),
      "vfs-probe-self-removal",
      "deferred removal did not revive once or retire on a waitable retry");
  if (passed) {
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
    NOTICE("QEMU-CONCURRENCY-TEST: PASS vfs-probe-self-removal");
#else
    NOTICE("HOSTED-WAIT-TEST: PASS vfs-probe-self-removal");
#endif
  }
  return passed;
}
}  // namespace

bool runVfsCallbackLifetimeRegressions() {
  size_t probeCallbackProcessor = static_cast<size_t>(-1);
  size_t probeRemoverProcessor = static_cast<size_t>(-1);
  size_t mountCallbackProcessor = static_cast<size_t>(-1);
  size_t mountRemoverProcessor = static_cast<size_t>(-1);

  const bool probePassed = probeCallbackDrain(probeCallbackProcessor, probeRemoverProcessor);
  const bool mountPassed = mountCallbackDrain(mountCallbackProcessor, mountRemoverProcessor);
  const bool selfRemovalPassed = probeSelfRemoval();

#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  NOTICE("QEMU-CONCURRENCY-TEST: vfs-callback-cpus probe="
         << Dec << probeCallbackProcessor << "/" << probeRemoverProcessor
         << " mount=" << mountCallbackProcessor << "/" << mountRemoverProcessor);
  const bool cpuSpread =
      check(probeCallbackProcessor != probeRemoverProcessor &&
                mountCallbackProcessor != mountRemoverProcessor,
            "vfs-callback-cpu-spread", "callback retirement did not cross test CPUs");
#else
  const bool cpuSpread = true;
#endif

  const bool passed = probePassed && mountPassed && selfRemovalPassed && cpuSpread;
  if (passed) {
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
    NOTICE("QEMU-CONCURRENCY-TEST: PASS vfs-callback-lifetime-smp");
#else
    NOTICE("HOSTED-WAIT-TEST: PASS vfs-callback-lifetime");
#endif
  }
  return passed;
}
