/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/SchedulerTimer.h"
#include "pedigree/kernel/machine/SchedulerTimerDispatchCleanup.h"
#include "pedigree/kernel/machine/SchedulerTimerHandler.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"

#include <signal.h>
#include <time.h>

#include "system/kernel/machine/hosted/IrqManager.h"
#include "system/kernel/machine/hosted/SchedulerTimer.h"
#include "system/kernel/machine/hosted/Timer.h"

#if !PEDIGREE_HOSTED_CORE_SMOKE
extern "C" int hostedSchedulerExitUserProbe(void* parameter);
extern "C" void hostedSchedulerExitUserProbeTimedOut(void* parameter);

#if HOSTED && BITS_64
asm(".text\n"
    ".globl hostedSchedulerExitUserProbe\n"
    ".type hostedSchedulerExitUserProbe,@function\n"
    "hostedSchedulerExitUserProbe:\n"
    "movq %rdi,%r12\n"
    "movq $1,0(%r12)\n"
    "subq $16,%rsp\n"
    "movl $228,%eax\n"
    "movl $1,%edi\n"
    "movq %rsp,%rsi\n"
    "syscall\n"
    "testq %rax,%rax\n"
    "js 2f\n"
    "movq 0(%rsp),%r13\n"
    "addq $3,%r13\n"
    "movq $100000,%r14\n"
    "1:\n"
    "pause\n"
    "decq %r14\n"
    "jnz 1b\n"
    "movq $100000,%r14\n"
    "movl $228,%eax\n"
    "movl $1,%edi\n"
    "movq %rsp,%rsi\n"
    "syscall\n"
    "testq %rax,%rax\n"
    "js 2f\n"
    "cmpq %r13,0(%rsp)\n"
    "jl 1b\n"
    "2:\n"
    "movq $1,8(%r12)\n"
    "subq $16,%rsp\n"
    "movq $0xa00,0(%rsp)\n"
    "movl $14,%eax\n"
    "xorl %edi,%edi\n"
    "movq %rsp,%rsi\n"
    "xorl %edx,%edx\n"
    "movl $8,%r10d\n"
    "syscall\n"
    "call hostedSetKernelFs@PLT\n"
    "movq 24(%r12),%rsp\n"
    "andq $-16,%rsp\n"
    "movq %r12,%rdi\n"
    "call hostedSchedulerExitUserProbeTimedOut@PLT\n"
    "ud2\n"
    ".size hostedSchedulerExitUserProbe,.-hostedSchedulerExitUserProbe\n");
#endif
#endif

namespace {
class HostedAccountingProcess : public Process {
 public:
  explicit HostedAccountingProcess(Thread* driver)
      : Process(DeferredPublication(), driver->getParent()),
        m_Driver(driver),
        calls(0),
        user(0),
        profile(0),
        failures(0) {
    enableTimeAccountingReports();
    description() += "hosted deferred accounting probe";
    publish();
  }

  ~HostedAccountingProcess() override {
    prepareForDestruction();
  }

  Thread* m_Driver;
  Atomic<size_t> calls;
  Atomic<size_t> user;
  Atomic<size_t> profile;
  Atomic<size_t> failures;

 private:
  void reportTimesUpdated(Time::Timestamp userTotal, Time::Timestamp profileTotal) override {
    Thread* current = Processor::information().getCurrentThread();
    const size_t processCount = Scheduler::instance().getNumProcesses();
    const size_t threadCount = getNumThreads();
    (void)threadCount;
    if (!Processor::getInterrupts() || Processor::inDeviceHardIrq() || !current ||
        current == m_Driver || !processCount || userTotal < user || profileTotal < profile) {
      failures += 1;
    }
    user = static_cast<size_t>(userTotal);
    profile = static_cast<size_t>(profileTotal);
    calls += 1;
  }
};

struct AccountingThreadContext {
  AccountingThreadContext(Process* process, Time::Timestamp kernelBefore)
      : process(process), kernelBefore(kernelBefore), ran(0), firstSliceAccounted(0) {}

  Process* process;
  Time::Timestamp kernelBefore;
  Atomic<size_t> ran;
  Atomic<size_t> firstSliceAccounted;
};

int accountedKernelThread(void* parameter) {
  AccountingThreadContext* context = reinterpret_cast<AccountingThreadContext*>(parameter);
  Scheduler::instance().yield();
  context->firstSliceAccounted = context->process->getKernelTime() > context->kernelBefore;
  context->ran = 1;
  return 0;
}

bool deferredTimeAccountingWorker() {
  Thread* driver = Processor::information().getCurrentThread();
  HostedAccountingProcess* process = new HostedAccountingProcess(driver);

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  process->publishTimeAccountingForHostedTest(13, 7);
  Processor::setInterrupts(interruptsWereEnabled);

  constexpr size_t Attempts = 10000;
  for (size_t attempt = 0; !process->calls && attempt < Attempts; ++attempt) {
    Scheduler::instance().yield();
  }

  const bool exactWorkerBatch =
      process->calls == 1 && process->user == 13 && process->profile == 20 && !process->failures;

  AccountingThreadContext threadContext(process, process->getKernelTime());
  Thread* accountedThread =
      new Thread(process, accountedKernelThread, &threadContext, nullptr, false, true, true);
  accountedThread->setName("hosted accounting first-slice probe");
  const bool accountedThreadStarted = accountedThread->start();
  const bool accountedThreadJoined = accountedThreadStarted && accountedThread->joinForCompletion();
  if (!accountedThreadStarted) {
    delete accountedThread;
  }

  for (size_t attempt = 0; process->profile == 20 && attempt < Attempts; ++attempt) {
    Scheduler::instance().yield();
  }
  const bool firstKernelSliceAccounted = accountedThreadStarted && accountedThreadJoined &&
                                         threadContext.ran && threadContext.firstSliceAccounted &&
                                         process->user == 13 && process->profile > 20 &&
                                         !process->failures;

  process->closeTimeAccountingForHostedTest();
  const size_t callsBeforeLatePublication = process->calls;
  process->publishTimeAccountingForHostedTest(101, 211);
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    Scheduler::instance().yield();
  }
  const bool latePublicationDiscarded = process->calls == callsBeforeLatePublication;
  delete process;

  const bool passed = exactWorkerBatch && firstKernelSliceAccounted && latePublicationDiscarded;
  if (!passed) {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL deferred-time-accounting-worker: "
        "accounting escaped its ordinary worker, first-slice, or "
        "teardown boundary");
  } else {
    NOTICE("HOSTED-WAIT-TEST: PASS deferred-time-accounting-worker");
  }
  return passed;
}

struct ContextSwitchContext {
  explicit ContextSwitchContext(Thread* driver)
      : driver(driver),
        phase(0),
        switchReturns(0),
        bookkeepingCalls(0),
        restoreBoundaries(0),
        tickCalls(0),
        targetCalls(0),
        failures(0) {}

  Thread* driver;
  Atomic<size_t> phase;
  Atomic<size_t> switchReturns;
  Atomic<size_t> bookkeepingCalls;
  Atomic<size_t> restoreBoundaries;
  Atomic<size_t> tickCalls;
  Atomic<size_t> targetCalls;
  Atomic<size_t> failures;
};

ContextSwitchContext* g_ContextSwitchContext = nullptr;

struct SchedulerTimerContext {
  SchedulerTimerContext() : calls(0), failures(0) {}

  Atomic<size_t> calls;
  Atomic<size_t> failures;
};

SchedulerTimerContext* g_SchedulerTimerContext = nullptr;

constexpr int DeferredTimerExitCode = 73;

#if !PEDIGREE_HOSTED_CORE_SMOKE
struct SchedulerExitContext;

struct SchedulerExitUserProbeState {
  uintptr_t ready;
  uintptr_t timedOut;
  SchedulerExitContext* context;
  uintptr_t kernelStackTop;
};

static_assert(__builtin_offsetof(SchedulerExitUserProbeState, ready) == 0 &&
                  __builtin_offsetof(SchedulerExitUserProbeState, timedOut) == 8 &&
                  __builtin_offsetof(SchedulerExitUserProbeState, kernelStackTop) == 24,
              "hosted scheduler user probe assembly layout changed");

struct SchedulerExitContext {
  SchedulerExitContext()
      : user{0, 0, this, 0},
        target(nullptr),
        event(nullptr),
        userStackBase(0),
        userStackTop(0),
        hookCalls(0),
        targetHookCalls(0),
        userHookCalls(0),
        tickCalls(0),
        queued(0),
        eventCalls(0),
        exitCalls(0),
        failures(0) {}

  SchedulerExitUserProbeState user;
  Thread* target;
  Event* event;
  uintptr_t userStackBase;
  uintptr_t userStackTop;
  Atomic<size_t> hookCalls;
  Atomic<size_t> targetHookCalls;
  Atomic<size_t> userHookCalls;
  Atomic<size_t> tickCalls;
  Atomic<size_t> queued;
  Atomic<size_t> eventCalls;
  Atomic<size_t> exitCalls;
  Atomic<size_t> failures;
};

SchedulerExitContext* g_SchedulerExitContext = nullptr;

void schedulerExitEventHandler(size_t) {
  SchedulerExitContext* context = __atomic_load_n(&g_SchedulerExitContext, __ATOMIC_ACQUIRE);
  if (!context) {
    return;
  }
  Thread* current = Processor::information().getCurrentThread();
  if (current != context->target || !Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
      current->getHostedSignalDepth() ||
      current->currentTimeAccountingMode() != CpuTimeMode::Kernel) {
    context->failures += 1;
  }
  context->eventCalls += 1;
  current->deferProcessExit(DeferredTimerExitCode);
}

class SchedulerExitEvent : public Event {
 public:
  SchedulerExitEvent() : Event(reinterpret_cast<uintptr_t>(&schedulerExitEventHandler), false) {}

  size_t serialize(uint8_t*) override {
    return 0;
  }

  size_t getNumber() override {
    return 0x45584954;
  }
};

class SchedulerExitSubsystem : public Subsystem {
 public:
  explicit SchedulerExitSubsystem(SchedulerExitContext& context)
      : Subsystem(None), m_Context(context) {}

  void exit(int code) override {
    Thread* current = Processor::information().getCurrentThread();
    if (code != DeferredTimerExitCode || current != m_Context.target || m_Context.eventCalls != 1 ||
        !Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
        current->getHostedSignalDepth() ||
        current->currentTimeAccountingMode() != CpuTimeMode::Kernel) {
      m_Context.failures += 1;
    }
    m_Context.exitCalls += 1;
    Thread::threadExited();
  }

  bool kill(KillReason, Thread*) override {
    return false;
  }

  bool invoke(const char*, Vector<String>&, Vector<String>&) override {
    return false;
  }

  bool invoke(const char*, Vector<String>&, Vector<String>&, SyscallState&) override {
    return false;
  }

  bool invoke(File*, const String&, Vector<String>&, Vector<String>&) override {
    return false;
  }

  bool invoke(File*, const String&, Vector<String>&, Vector<String>&, SyscallState&) override {
    return false;
  }

  File* findFile(const String&, File*) override {
    return nullptr;
  }

 private:
  SchedulerExitContext& m_Context;
};

class SchedulerExitProcess : public Process {
 public:
  SchedulerExitProcess(SchedulerExitContext& context, Process* parent)
      : Process(DeferredPublication(), parent) {
    setSubsystem(new SchedulerExitSubsystem(context));
    description() += "hosted scheduler return-tail exit probe";
    publish();
  }

  ~SchedulerExitProcess() override {
    prepareForDestruction();
  }
};

void queueExitEventFromSchedulerTick(uint64_t delta, InterruptState& state) {
  SchedulerExitContext* context = __atomic_load_n(&g_SchedulerExitContext, __ATOMIC_ACQUIRE);
  Thread* current = Processor::information().getCurrentThread();
  if (!context) {
    return;
  }
  context->hookCalls += 1;
  if (!__atomic_load_n(&context->user.ready, __ATOMIC_ACQUIRE)) {
    return;
  }
  if (current != context->target) {
    return;
  }
  context->targetHookCalls += 1;
  if (state.kernelMode() || current->getStateLevel() ||
      state.getStackPointer() < context->userStackBase ||
      state.getStackPointer() > context->userStackTop) {
    return;
  }
  context->userHookCalls += 1;
  if (!context->queued.compareAndSwap(0, 1)) {
    return;
  }

  const uint64_t interval = 100 * Time::Multiplier::Millisecond;
  if (delta < interval || (delta % interval) || Processor::getInterrupts() ||
      current->getHostedSignalDepth() != 1 || Processor::inDeviceHardIrq() || state.kernelMode() ||
      state.getStackPointer() < context->userStackBase ||
      state.getStackPointer() > context->userStackTop ||
      current->currentTimeAccountingMode() != CpuTimeMode::Kernel ||
      state.getInterruptNumber() != SIGUSR2 ||
      state.getInterruptSource() != HostedSchedulerTimer::sourceForTest()) {
    context->failures += 1;
  }

  // Test-only injection at the exact historical window: the old timer path
  // dispatched this preallocated Event before returning from the hard IRQ.
  if (!current->sendEvent(context->event)) {
    context->failures += 1;
  }
  context->tickCalls += 1;
}

bool schedulerTimerExitDeferral() {
  constexpr const char* Test = "scheduler-timer-exit-return-tail";
  SchedulerExitContext context;
  SchedulerExitEvent event;
  context.event = &event;
  const bool timerSlowed = HostedTimer::setSignalIntervalForTest(4 * Time::Multiplier::Second);
  Thread* driver = Processor::information().getCurrentThread();
  SchedulerExitProcess* process =
      driver ? new SchedulerExitProcess(context, driver->getParent()) : nullptr;
  VirtualAddressSpace::Stack* userStack =
      process ? process->getAddressSpace()->allocateStack() : nullptr;
  Thread* target = nullptr;
  if (userStack) {
    context.userStackBase = reinterpret_cast<uintptr_t>(userStack->getBase());
    context.userStackTop = reinterpret_cast<uintptr_t>(userStack->getTop());
    target = new Thread(process, hostedSchedulerExitUserProbe, &context.user, userStack->getTop(),
                        false, true, true);
    context.target = target;
    context.user.kernelStackTop = reinterpret_cast<uintptr_t>(target->getKernelStack());
    target->setName("hosted scheduler exit return-tail probe");
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  __atomic_store_n(&g_SchedulerExitContext, &context, __ATOMIC_RELEASE);
  HostedSchedulerTimer::setHardContextHookForTest(queueExitEventFromSchedulerTick);
  Processor::setInterrupts(interruptsWereEnabled);

  const bool started = target && target->start();
  bool reapable = false;
  timespec startedAt = {};
  timespec now = {};
  clock_gettime(CLOCK_MONOTONIC, &startedAt);
  while (started && !reapable) {
    reapable = target->isReapableForHostedTest();
    if (reapable) {
      break;
    }
    Scheduler::instance().yield();
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - startedAt.tv_sec >= 6) {
      break;
    }
  }
  if (started && !reapable) {
    ERROR("HOSTED-WAIT-TEST: DETAIL "
          << Test << ": ready=" << context.user.ready << " timed-out=" << context.user.timedOut
          << " hooks=" << context.hookCalls.value() << " target-hooks="
          << context.targetHookCalls.value() << " user-hooks=" << context.userHookCalls.value()
          << " queued=" << context.queued.value() << " event-calls=" << context.eventCalls.value()
          << " exit-calls=" << context.exitCalls.value() << " target-status="
          << static_cast<size_t>(target->getStatus()) << " target-level=" << target->getStateLevel()
          << " target-signal-depth=" << target->getHostedSignalDepth()
          << " timer-admissions=" << HostedSchedulerTimer::activeDispatchesForTest());
    FATAL("Hosted scheduler exit probe made no bounded progress");
  }
  const bool joined = reapable && target->joinForCompletion();
  if (target && !started) {
    delete target;
  }

  Processor::setInterrupts(false);
  HostedSchedulerTimer::setHardContextHookForTest(nullptr);
  __atomic_store_n(&g_SchedulerExitContext, static_cast<SchedulerExitContext*>(nullptr),
                   __ATOMIC_RELEASE);
  const bool timerRestored = HostedTimer::setSignalIntervalForTest(Time::Multiplier::Millisecond);
  Processor::setInterrupts(interruptsWereEnabled);

  event.waitForDeliveries();
  if (userStack) {
    process->getAddressSpace()->freeStack(userStack);
  }
  if (process) {
    delete process;
  }

  const bool passed = timerSlowed && timerRestored && started && joined && context.user.ready &&
                      !context.user.timedOut && context.tickCalls == 1 && context.queued == 1 &&
                      context.eventCalls == 1 && context.exitCalls == 1 && !context.failures;
  if (!passed) {
    ERROR("HOSTED-WAIT-TEST: FAIL " << Test
                                    << ": process exit ran before the "
                                       "IRQ return-to-user tail");
  } else {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "scheduler-timer-exit-return-tail");
  }
  return passed;
}
#endif

struct HostedSignalSwitchContext {
  explicit HostedSignalSwitchContext(Thread* driver)
      : driver(driver),
        target(nullptr),
        armed(0),
        waiting(0),
        computing(0),
        driverTicks(0),
        targetTicks(0),
        ran(0),
        failures(0),
        failureMask(0) {}

  Thread* driver;
  Thread* target;
  Atomic<size_t> armed;
  Atomic<size_t> waiting;
  Atomic<size_t> computing;
  Atomic<size_t> driverTicks;
  Atomic<size_t> targetTicks;
  Atomic<size_t> ran;
  Atomic<size_t> failures;
  Atomic<size_t> failureMask;
};

HostedSignalSwitchContext* g_HostedSignalSwitchContext = nullptr;

void observeHostedAutodisarmTick(uint64_t delta, InterruptState& state) {
  HostedSignalSwitchContext* context =
      __atomic_load_n(&g_HostedSignalSwitchContext, __ATOMIC_ACQUIRE);
  if (!context || !context->armed) {
    return;
  }

  Thread* current = Processor::information().getCurrentThread();
  if (current == context->driver) {
    context->driverTicks += 1;
    return;
  }
  if (current != context->target || !context->computing ||
      !context->targetTicks.compareAndSwap(0, 1)) {
    return;
  }

  const uint64_t interval = 100 * Time::Multiplier::Millisecond;
  size_t failureMask = 0;
  failureMask |= delta < interval ? 1 : 0;
  failureMask |= (delta % interval) ? 2 : 0;
  failureMask |= !state.kernelMode() ? 4 : 0;
  failureMask |= !current->getHostedSignalDepth() ? 8 : 0;
  failureMask |= Processor::hostedSignalFrameDepthForTest() < 2 ? 16 : 0;
  failureMask |= Processor::getInterrupts() ? 32 : 0;
  failureMask |= Processor::inDeviceHardIrq() ? 64 : 0;
  failureMask |= current->currentTimeAccountingMode() != CpuTimeMode::Kernel ? 128 : 0;
  failureMask |= state.getInterruptNumber() != SIGUSR2 ? 256 : 0;
  failureMask |= state.getInterruptSource() != HostedSchedulerTimer::sourceForTest() ? 512 : 0;
  failureMask |= Processor::executionContext() != ExecutionContext::SchedulerIrq ? 1024 : 0;
  if (failureMask) {
    context->failureMask |= failureMask;
    context->failures += 1;
  }
}

int hostedSignalSwitchTarget(void* parameter) {
  HostedSignalSwitchContext* context = reinterpret_cast<HostedSignalSwitchContext*>(parameter);
  context->waiting = 1;
  while (!context->armed) {
    Scheduler::instance().yield();
  }

  // This continuation is selected by the real scheduler signal while its
  // frame remains live on another Pedigree stack.
  Processor::setInterrupts(true);
  sigset_t mask;
  sigprocmask(0, nullptr, &mask);
  Thread* current = Processor::information().getCurrentThread();
  size_t failureMask = 0;
  failureMask |= !current ? 1024 : 0;
  failureMask |= current && current->getHostedSignalDepth() ? 2048 : 0;
  failureMask |= !Processor::hostedSignalFrameDepthForTest() ? 4096 : 0;
  failureMask |= !Processor::getInterrupts() ? 8192 : 0;
  failureMask |= sigismember(&mask, SIGUSR1) ? 16384 : 0;
  failureMask |= sigismember(&mask, SIGUSR2) ? 32768 : 0;
  failureMask |= Processor::executionContext() != ExecutionContext::WaitableThread ? 131072 : 0;
  if (failureMask) {
    context->failureMask |= failureMask;
    context->failures += 1;
  }

  context->computing = 1;
  timespec startedAt = {};
  timespec now = {};
  clock_gettime(CLOCK_MONOTONIC, &startedAt);
  while (!context->targetTicks) {
    Processor::pause();
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - startedAt.tv_sec >= 3) {
      context->failureMask |= 65536;
      context->failures += 1;
      break;
    }
  }
  context->ran = 1;
  return 0;
}

bool hostedSignalMaskSpansContextSwitch() {
  constexpr const char* Test = "hosted-signal-autodisarm-preemption";
  HostedSignalSwitchContext context(Processor::information().getCurrentThread());
  Thread* target = new Thread(Scheduler::instance().getKernelProcess(), hostedSignalSwitchTarget,
                              &context, nullptr, false, true, true);
  target->setName("hosted signal-mask context-switch probe");
  context.target = target;
  const bool started = target->start();

  constexpr size_t Attempts = 10000;
  for (size_t attempt = 0; started && !context.waiting && attempt < Attempts; ++attempt) {
    Scheduler::instance().yield();
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  __atomic_store_n(&g_HostedSignalSwitchContext, &context, __ATOMIC_RELEASE);
  HostedSchedulerTimer::setHardContextHookForTest(observeHostedAutodisarmTick);
  context.armed = 1;
  Processor::setInterrupts(interruptsWereEnabled);
  const Time::Timestamp deadline = Time::getTicks() + (3 * Time::Multiplier::Second);
  while (!context.ran && Time::getTicks() < deadline) {
    Processor::pause();
  }

  Processor::setInterrupts(false);
  HostedSchedulerTimer::setHardContextHookForTest(nullptr);
  __atomic_store_n(&g_HostedSignalSwitchContext, static_cast<HostedSignalSwitchContext*>(nullptr),
                   __ATOMIC_RELEASE);
  Processor::setInterrupts(interruptsWereEnabled);

  // A failed preemption must not leave the probe stack live while its
  // context record goes out of scope.
  for (size_t attempt = 0; started && !context.ran && attempt < Attempts; ++attempt) {
    Scheduler::instance().yield();
  }

  const bool joined = context.ran && target->joinForCompletion();
  if (!started) {
    delete target;
  }

  const bool passed = started && context.waiting && context.computing && context.driverTicks &&
                      context.targetTicks == 1 && context.ran && joined && !context.failures;
  if (!passed) {
    ERROR("HOSTED-WAIT-TEST: FAIL "
          << Test << ": s=" << started << " w=" << static_cast<size_t>(context.waiting)
          << " c=" << static_cast<size_t>(context.computing)
          << " d=" << static_cast<size_t>(context.driverTicks) << " t="
          << static_cast<size_t>(context.targetTicks) << " r=" << static_cast<size_t>(context.ran)
          << " j=" << joined << " f=" << static_cast<size_t>(context.failures)
          << " m=" << context.failureMask.value());
  } else {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "hosted-signal-autodisarm-preemption");
  }
  return passed;
}

void observeSchedulerTimerHardContext(uint64_t delta, InterruptState& state) {
  SchedulerTimerContext* context = __atomic_load_n(&g_SchedulerTimerContext, __ATOMIC_ACQUIRE);
  if (!context) {
    return;
  }

  Thread* current = Processor::information().getCurrentThread();
  const uint64_t interval = 100 * Time::Multiplier::Millisecond;
  if (delta < interval || (delta % interval) || !current || !current->getHostedSignalDepth() ||
      !Processor::onHostedExecutionThread() ||
      Processor::executionContext() != ExecutionContext::SchedulerIrq ||
      Processor::inDeviceHardIrq() || Processor::deviceHardIrqDepthForTest() != 0 ||
      state.getInterruptNumber() != SIGUSR2 ||
      state.getInterruptSource() != HostedSchedulerTimer::sourceForTest()) {
    context->failures += 1;
  }
  context->calls += 1;
}

bool schedulerTimerHardContext() {
  constexpr const char* Test = "hosted-scheduler-timer-hard-context";
  SchedulerTimerContext context;
  const bool directRoute = HostedSchedulerTimer::directRoutePublishedForTest();

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  __atomic_store_n(&g_SchedulerTimerContext, &context, __ATOMIC_RELEASE);
  HostedSchedulerTimer::setHardContextHookForTest(observeSchedulerTimerHardContext);
  Processor::setInterrupts(interruptsWereEnabled);

  const Time::Timestamp deadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (!context.calls && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }

  Processor::setInterrupts(false);
  HostedSchedulerTimer::setHardContextHookForTest(nullptr);
  __atomic_store_n(&g_SchedulerTimerContext, static_cast<SchedulerTimerContext*>(nullptr),
                   __ATOMIC_RELEASE);
  Processor::setInterrupts(interruptsWereEnabled);

  const bool passed = directRoute && context.calls && !context.failures;
  if (!passed) {
    ERROR("HOSTED-WAIT-TEST: FAIL " << Test
                                    << ": the scheduler callback did not "
                                       "use its dedicated controller route");
  } else {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "hosted-scheduler-timer-hard-context");
  }
  return passed;
}

class ConflictingSchedulerTimerHandler : public SchedulerTimerHandler {
 public:
  ConflictingSchedulerTimerHandler() : calls(0) {}

  void timer(uint64_t, InterruptState&) override {
    calls += 1;
  }

  Atomic<size_t> calls;
};

class SelfRemovingSchedulerTimerHandler : public SchedulerTimerHandler {
 public:
  explicit SelfRemovingSchedulerTimerHandler(SchedulerTimer* timer)
      : m_Timer(timer), calls(0), removalSucceeded(0), continuedAfterRemoval(0), wrongContext(0) {}

  void timer(uint64_t, InterruptState&) override {
    if (Processor::executionContext() != ExecutionContext::SchedulerIrq) {
      wrongContext += 1;
    }
    calls += 1;
    if (m_Timer && m_Timer->removeHandler(this)) {
      removalSucceeded = 1;
    }
    // A true removal must be impossible here: this statement is still in
    // the callback whose lifetime removeHandler promises to drain.
    continuedAfterRemoval = 1;
  }

  SchedulerTimer* m_Timer;
  Atomic<size_t> calls;
  Atomic<size_t> removalSucceeded;
  Atomic<size_t> continuedAfterRemoval;
  Atomic<size_t> wrongContext;
};

bool schedulerTimerAbandonedAdmissionCleanup() {
  constexpr const char* Test = "hosted-scheduler-timer-abandoned-admission-cleanup";
  Thread* current = Processor::information().getCurrentThread();
  ConflictingSchedulerTimerHandler handler;
  SchedulerTimerHandlerSlot slot;
  const size_t owner = Processor::id();
  const size_t initialLevel = current ? current->getStateLevel() : 0;
  const bool published = current && slot.publish(owner, &handler);
  SchedulerState* previous = published ? current->pushState() : nullptr;
  const bool pushed = previous && current->getStateLevel() == initialLevel + 1;

  bool admitted = false;
  bool counted = false;
  bool abandoned = false;
  if (pushed) {
    SchedulerTimerHandlerSlot::DispatchGuard dispatch;
    admitted = slot.beginDispatch(owner, dispatch);
    if (admitted) {
      // Models the old leak: the scheduler frame owns an admission, but
      // its Thread state is discarded without C++ stack unwinding.
      {
        SchedulerTimerDispatchCleanup cleanup(dispatch);
        counted = slot.activeDispatches() == 1;
        current->abandonCurrentState(false);
        abandoned = current->getStateLevel() == initialLevel && slot.activeDispatches() == 0;
      }
      // Both cleanup and the raw guard destruct after the modelled
      // abandonment; idempotent release must prevent an underflow.
    } else {
      current->popState(false);
    }
  }

  const bool removed = abandoned && slot.unpublish(owner, &handler);
  const bool republished = removed && slot.publish(owner, &handler);
  bool readmitted = false;
  if (republished) {
    SchedulerTimerHandlerSlot::DispatchGuard dispatch;
    readmitted = slot.beginDispatch(owner, dispatch);
    dispatch.release();
  }
  const bool drained = slot.activeDispatches() == 0;
  const bool finallyRemoved = republished && drained && slot.unpublish(owner, &handler);

  const bool passed = published && pushed && admitted && counted && abandoned && removed &&
                      republished && readmitted && drained && finallyRemoved;
  if (!passed) {
    ERROR("HOSTED-WAIT-TEST: FAIL " << Test
                                    << ": an abandoned scheduler frame "
                                       "stranded its callback admission");
  } else {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "hosted-scheduler-timer-abandoned-admission-cleanup");
  }
  return passed;
}

bool schedulerTimerSingleOwner() {
  constexpr const char* Test = "hosted-scheduler-timer-single-owner";
  SchedulerTimer* timer = Machine::instance().getSchedulerTimer();
  SchedulerTimerHandler* owner = HostedSchedulerTimer::publishedHandlerForTest();
  ConflictingSchedulerTimerHandler conflicting;

  const bool ownerPublished = owner != nullptr;
  const bool nullRegistrationRejected = timer && !timer->registerHandler(nullptr);
  const bool duplicateRejected = timer && !timer->registerHandler(owner);
  const bool conflictRejected = timer && !timer->registerHandler(&conflicting);
  const bool nullRemovalRejected = timer && !timer->removeHandler(nullptr);
  const bool wrongOwnerRejected = timer && !timer->removeHandler(&conflicting);
  const bool ownerPreserved = HostedSchedulerTimer::publishedHandlerForTest() == owner;

  const bool passed = timer && ownerPublished && nullRegistrationRejected && duplicateRejected &&
                      conflictRejected && nullRemovalRejected && wrongOwnerRejected &&
                      ownerPreserved && !conflicting.calls;
  if (!passed) {
    ERROR("HOSTED-WAIT-TEST: FAIL " << Test
                                    << ": handler ownership was replaced "
                                       "or removed by a non-owner");
  } else {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "hosted-scheduler-timer-single-owner");
  }
  return passed;
}

bool schedulerTimerSelfRemovalRejected() {
  constexpr const char* Test = "hosted-scheduler-timer-self-removal-rejected";
  SchedulerTimer* timer = Machine::instance().getSchedulerTimer();
  SchedulerTimerHandler* owner = HostedSchedulerTimer::publishedHandlerForTest();
  SelfRemovingSchedulerTimerHandler probe(timer);

  auto removeWithRetry = [timer](SchedulerTimerHandler* handler, size_t& attempts) {
    constexpr size_t RemovalAttemptLimit = 256;
    while (timer && handler && attempts < RemovalAttemptLimit) {
      ++attempts;
      if (timer->removeHandler(handler)) {
        return true;
      }
      // The regression driver can resume inside the scheduler tick whose
      // dispatch admission makes removal retryable. Yield until that older
      // hard frame returns instead of treating a live admission as failure.
      Scheduler::instance().yield();
    }
    return false;
  };

  size_t ownerRemovalAttempts = 0;
  const bool ownerRemoved = removeWithRetry(owner, ownerRemovalAttempts);
  const bool probeRegistered = ownerRemoved && timer->registerHandler(&probe);
  const Time::Timestamp deadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (probeRegistered && !probe.calls && Time::getTicks() < deadline) {
    Processor::pause();
  }

  size_t probeRemovalAttempts = 0;
  const bool probeRemoved =
      probeRegistered && !probe.removalSucceeded && removeWithRetry(&probe, probeRemovalAttempts);
  if (probeRegistered && !probeRemoved && !probe.removalSucceeded) {
    FATAL("Hosted scheduler-timer regression could not retire its stack probe");
  }

  const bool probeQuiesced = !probeRegistered || probeRemoved || probe.removalSucceeded;
  const bool ownerRestored = ownerRemoved && probeQuiesced && timer->registerHandler(owner);
  if (ownerRemoved && !ownerRestored) {
    FATAL("Hosted scheduler-timer regression could not restore the real owner");
  }
  const bool passed = ownerRemoved && probeRegistered && probe.calls.value() >= 1 &&
                      !probe.removalSucceeded && probe.continuedAfterRemoval == 1 &&
                      !probe.wrongContext && probeRemoved && ownerRestored &&
                      HostedSchedulerTimer::publishedHandlerForTest() == owner;
  if (!passed) {
    ERROR("HOSTED-WAIT-TEST: FAIL " << Test << ": owner-removed=" << ownerRemoved << " registered="
                                    << probeRegistered << " calls=" << probe.calls.value());
    ERROR("HOSTED-WAIT-TEST: DETAIL "
          << Test << ": callback-remove=" << probe.removalSucceeded.value()
          << " continued=" << probe.continuedAfterRemoval.value()
          << " wrong-context=" << probe.wrongContext.value() << " probe-removed=" << probeRemoved);
    ERROR("HOSTED-WAIT-TEST: DETAIL "
          << Test << ": owner-restored=" << ownerRestored
          << " published-owner=" << (HostedSchedulerTimer::publishedHandlerForTest() == owner)
          << " owner-remove-attempts=" << ownerRemovalAttempts
          << " probe-remove-attempts=" << probeRemovalAttempts
          << " active-dispatches=" << HostedSchedulerTimer::activeDispatchesForTest());
  } else {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "hosted-scheduler-timer-self-removal-rejected");
  }
  return passed;
}

class RejectedHostedDeviceHandler : public HardIrqHandler {
 public:
  HardIrqDisposition irq(irq_id_t, InterruptState&) override {
    return HardIrqDisposition::Handled;
  }
};

bool schedulerRouteIsDedicated() {
  constexpr const char* Test = "hosted-scheduler-route-dedicated";
  IrqManager* manager = Machine::instance().getIrqManager();
  SchedulerIrqHandler* handler = HostedIrqManager::schedulerIrqHandlerForTest(1);
  RejectedHostedDeviceHandler deviceHandler;
  const irq_id_t rejected =
      manager ? manager->registerHardIsaIrqHandler(1, &deviceHandler, IrqPolicy::syntheticHard())
              : 0;
  const bool passed =
      manager && handler && !rejected && HostedIrqManager::schedulerIrqHandlerForTest(1) == handler;
  if (!passed) {
    ERROR("HOSTED-WAIT-TEST: FAIL " << Test
                                    << ": hosted admitted a device handler "
                                       "on its dedicated scheduler line");
  } else {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "hosted-scheduler-route-dedicated");
  }
  return passed;
}

void observeQueuedSchedulerTick(uint64_t, InterruptState&) {
  ContextSwitchContext* context = __atomic_load_n(&g_ContextSwitchContext, __ATOMIC_ACQUIRE);
  if (!context || Processor::information().getCurrentThread() != context->driver) {
    return;
  }

  const size_t phase = context->phase;
  if (phase && phase < 4) {
    context->tickCalls += 1;
    if (phase != 3 || Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
        Processor::executionContext() != ExecutionContext::SchedulerIrq) {
      context->failures += 1;
    }
    context->phase = 4;
  }
}

void contextSwitchHook(ProcessorBase::HostedContextSwitchStage stage) {
  ContextSwitchContext* context = __atomic_load_n(&g_ContextSwitchContext, __ATOMIC_ACQUIRE);
  if (!context || Processor::information().getCurrentThread() != context->driver) {
    return;
  }

  switch (stage) {
    case ProcessorBase::HostedContextSwitchStage::SwitchStateReturnedMasked:
      if (!context->phase.compareAndSwap(0, 1)) {
        return;
      }
      context->switchReturns += 1;
      if (Processor::getInterrupts() || !HostedSchedulerTimer::queueTickForTest()) {
        context->failures += 1;
      }
      break;
    case ProcessorBase::HostedContextSwitchStage::SchedulerBookkeepingComplete:
      if (context->phase == static_cast<size_t>(1)) {
        context->bookkeepingCalls += 1;
        if (Processor::getInterrupts() || !context->phase.compareAndSwap(1, 2)) {
          context->failures += 1;
        }
      }
      break;
    case ProcessorBase::HostedContextSwitchStage::SchedulerRestoringInterrupts:
      if (context->phase == static_cast<size_t>(2)) {
        context->restoreBoundaries += 1;
        if (Processor::getInterrupts() || !context->phase.compareAndSwap(2, 3)) {
          context->failures += 1;
        }
      }
      break;
  }
}

int contextSwitchTarget(void* parameter) {
  ContextSwitchContext* context = reinterpret_cast<ContextSwitchContext*>(parameter);
  context->targetCalls += 1;
  return 0;
}

bool check(bool condition, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL context-switch-interrupt-restore: " << detail);
  return false;
}
}  // namespace

#if !PEDIGREE_HOSTED_CORE_SMOKE
extern "C" void hostedSchedulerExitUserProbeTimedOut(void* parameter) {
  SchedulerExitUserProbeState* state = reinterpret_cast<SchedulerExitUserProbeState*>(parameter);
  if (state && state->context) {
    state->context->failures += 1;
  }
  Thread::threadExited();
}
#endif

bool runHostedSchedulerRegressions() {
  if (!hostedSignalMaskSpansContextSwitch() || !schedulerTimerSingleOwner() ||
      !schedulerTimerAbandonedAdmissionCleanup() || !schedulerTimerSelfRemovalRejected() ||
      !schedulerRouteIsDedicated() || !schedulerTimerHardContext()) {
    return false;
  }

#if !PEDIGREE_HOSTED_CORE_SMOKE
  // This probe intentionally enters Linux userspace and exercises the
  // syscall-return tail. Darwin hosted execution is kernel-only.
  if (!schedulerTimerExitDeferral()) {
    return false;
  }
#endif

  if (!deferredTimeAccountingWorker()) {
    return false;
  }

  Thread* driver = Processor::information().getCurrentThread();
  ContextSwitchContext context(driver);
  const bool directRoutePreserved = HostedSchedulerTimer::directRoutePublishedForTest();

  Thread* target = new Thread(Scheduler::instance().getKernelProcess(), contextSwitchTarget,
                              &context, nullptr, false, true, true);
  target->setName("hosted context-switch IRQ target");

  __atomic_store_n(&g_ContextSwitchContext, &context, __ATOMIC_RELEASE);
  Processor::setHostedContextSwitchHook(contextSwitchHook);
  HostedSchedulerTimer::setHardContextHookForTest(observeQueuedSchedulerTick);
  const bool started = target->start();

  constexpr size_t Attempts = 10000;
  bool completed = false;
  for (size_t attempt = 0; started && attempt < Attempts; ++attempt) {
    if (context.phase == static_cast<size_t>(4) && target->isReapableForHostedTest()) {
      completed = true;
      break;
    }
    Scheduler::instance().yield();
  }

  HostedSchedulerTimer::setHardContextHookForTest(nullptr);
  Processor::setHostedContextSwitchHook(nullptr);
  __atomic_store_n(&g_ContextSwitchContext, static_cast<ContextSwitchContext*>(nullptr),
                   __ATOMIC_RELEASE);

  const bool targetJoined = target->isReapableForHostedTest() && target->joinForCompletion();
  const bool directRouteAfterRemoval = HostedSchedulerTimer::directRoutePublishedForTest();

  const bool passed =
      check(directRoutePreserved && started && completed && targetJoined &&
                directRouteAfterRemoval && context.switchReturns == 1 &&
                context.bookkeepingCalls == 1 && context.restoreBoundaries == 1 &&
                context.tickCalls == 1 && context.targetCalls == 1 && context.failures == 0,
            "the queued scheduler IRQ escaped the masked post-switch boundary");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS context-switch-interrupt-restore");
  }
  return passed;
}
