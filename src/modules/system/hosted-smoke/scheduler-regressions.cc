/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "system/kernel/machine/hosted/SchedulerTimer.h"

#include <signal.h>

namespace
{
class HostedAccountingProcess : public Process
{
  public:
    explicit HostedAccountingProcess(Thread *driver)
        : Process(DeferredPublication(), driver->getParent()), m_Driver(driver),
          calls(0), user(0), profile(0), failures(0)
    {
        enableTimeAccountingReports();
        description() += "hosted deferred accounting probe";
        publish();
    }

    ~HostedAccountingProcess() override
    {
        prepareForDestruction();
    }

    Thread *m_Driver;
    Atomic<size_t> calls;
    Atomic<size_t> user;
    Atomic<size_t> profile;
    Atomic<size_t> failures;

  private:
    void reportTimesUpdated(
        Time::Timestamp userTotal,
        Time::Timestamp profileTotal) override
    {
        Thread *current = Processor::information().getCurrentThread();
        const size_t processCount =
            Scheduler::instance().getNumProcesses();
        const size_t threadCount = getNumThreads();
        (void) threadCount;
        if (
            !Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
            !current || current == m_Driver || !processCount ||
            userTotal < user || profileTotal < profile)
        {
            failures += 1;
        }
        user = static_cast<size_t>(userTotal);
        profile = static_cast<size_t>(profileTotal);
        calls += 1;
    }
};

struct AccountingThreadContext
{
    AccountingThreadContext(Process *process, Time::Timestamp kernelBefore)
        : process(process), kernelBefore(kernelBefore), ran(0),
          firstSliceAccounted(0)
    {
    }

    Process *process;
    Time::Timestamp kernelBefore;
    Atomic<size_t> ran;
    Atomic<size_t> firstSliceAccounted;
};

int accountedKernelThread(void *parameter)
{
    AccountingThreadContext *context =
        reinterpret_cast<AccountingThreadContext *>(parameter);
    Scheduler::instance().yield();
    context->firstSliceAccounted =
        context->process->getKernelTime() > context->kernelBefore;
    context->ran = 1;
    return 0;
}

bool deferredTimeAccountingWorker()
{
    Thread *driver = Processor::information().getCurrentThread();
    HostedAccountingProcess *process = new HostedAccountingProcess(driver);

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    process->publishTimeAccountingForHostedTest(13, 7);
    Processor::setInterrupts(interruptsWereEnabled);

    constexpr size_t Attempts = 10000;
    for (size_t attempt = 0; !process->calls && attempt < Attempts; ++attempt)
    {
        Scheduler::instance().yield();
    }

    const bool exactWorkerBatch =
        process->calls == 1 && process->user == 13 &&
        process->profile == 20 && !process->failures;

    AccountingThreadContext threadContext(
        process, process->getKernelTime());
    Thread *accountedThread = new Thread(
        process, accountedKernelThread, &threadContext, nullptr, false, true,
        true);
    accountedThread->setName("hosted accounting first-slice probe");
    const bool accountedThreadStarted = accountedThread->start();
    const bool accountedThreadJoined =
        accountedThreadStarted && accountedThread->joinForCompletion();
    if (!accountedThreadStarted)
    {
        delete accountedThread;
    }

    for (size_t attempt = 0;
         process->profile == 20 && attempt < Attempts; ++attempt)
    {
        Scheduler::instance().yield();
    }
    const bool firstKernelSliceAccounted =
        accountedThreadStarted && accountedThreadJoined &&
        threadContext.ran && threadContext.firstSliceAccounted &&
        process->user == 13 && process->profile > 20 && !process->failures;

    process->closeTimeAccountingForHostedTest();
    const size_t callsBeforeLatePublication = process->calls;
    process->publishTimeAccountingForHostedTest(101, 211);
    for (size_t attempt = 0; attempt < 32; ++attempt)
    {
        Scheduler::instance().yield();
    }
    const bool latePublicationDiscarded =
        process->calls == callsBeforeLatePublication;
    delete process;

    const bool passed =
        exactWorkerBatch && firstKernelSliceAccounted &&
        latePublicationDiscarded;
    if (!passed)
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL deferred-time-accounting-worker: "
            "accounting escaped its ordinary worker, first-slice, or "
            "teardown boundary");
    }
    else
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS deferred-time-accounting-worker");
    }
    return passed;
}

struct ContextSwitchContext
{
    explicit ContextSwitchContext(Thread *driver)
        : driver(driver), phase(0), switchReturns(0), bookkeepingCalls(0),
          restoreBoundaries(0), tickCalls(0), targetCalls(0), failures(0)
    {
    }

    Thread *driver;
    Atomic<size_t> phase;
    Atomic<size_t> switchReturns;
    Atomic<size_t> bookkeepingCalls;
    Atomic<size_t> restoreBoundaries;
    Atomic<size_t> tickCalls;
    Atomic<size_t> targetCalls;
    Atomic<size_t> failures;
};

ContextSwitchContext *g_ContextSwitchContext = nullptr;

struct SchedulerTimerContext
{
    SchedulerTimerContext() : calls(0), failures(0)
    {
    }

    Atomic<size_t> calls;
    Atomic<size_t> failures;
};

SchedulerTimerContext *g_SchedulerTimerContext = nullptr;

struct HostedSignalSwitchContext
{
    HostedSignalSwitchContext()
        : armed(0), waiting(0), ran(0), failures(0)
    {
    }

    Atomic<size_t> armed;
    Atomic<size_t> waiting;
    Atomic<size_t> ran;
    Atomic<size_t> failures;
};

int hostedSignalSwitchTarget(void *parameter)
{
    HostedSignalSwitchContext *context =
        reinterpret_cast<HostedSignalSwitchContext *>(parameter);
    context->waiting = 1;
    while (!context->armed)
    {
        Scheduler::instance().yield();
    }

    // This continuation is selected by the real scheduler signal while its
    // frame remains live on another Pedigree stack.
    Processor::setInterrupts(true);
    __pedigree_hosted::sigset_t mask;
    __pedigree_hosted::sigprocmask(0, nullptr, &mask);
    Thread *current = Processor::information().getCurrentThread();
    if (
        !current || current->getHostedSignalDepth() ||
        Processor::hostedSignalFrameDepthForTest() != 1 ||
        !Processor::getInterrupts() ||
        !__pedigree_hosted::sigismember(&mask, SIGUSR1) ||
        !__pedigree_hosted::sigismember(&mask, SIGUSR2))
    {
        context->failures += 1;
    }
    context->ran = 1;
    return 0;
}

bool hostedSignalMaskSpansContextSwitch()
{
    constexpr const char *Test = "hosted-signal-mask-context-switch";
    HostedSignalSwitchContext context;
    Thread *target = new Thread(
        Scheduler::instance().getKernelProcess(), hostedSignalSwitchTarget,
        &context, nullptr, false, true, true);
    target->setName("hosted signal-mask context-switch probe");
    const bool started = target->start();

    constexpr size_t Attempts = 10000;
    for (size_t attempt = 0;
         started && !context.waiting && attempt < Attempts; ++attempt)
    {
        Scheduler::instance().yield();
    }

    context.armed = 1;
    const Time::Timestamp deadline =
        Time::getTicks() + (3 * Time::Multiplier::Second);
    while (!context.ran && Time::getTicks() < deadline)
    {
        Processor::pause();
    }

    // A failed preemption must not leave the probe stack live while its
    // context record goes out of scope.
    for (size_t attempt = 0;
         started && !context.ran && attempt < Attempts; ++attempt)
    {
        Scheduler::instance().yield();
    }

    const bool joined = context.ran && target->joinForCompletion();
    if (!started)
    {
        delete target;
    }

    const bool passed =
        started && context.waiting && context.ran && joined &&
        !context.failures;
    if (!passed)
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL " << Test
                                       << ": a switched-in thread unmasked "
                                          "a live hosted IRQ signal frame");
    }
    else
    {
        NOTICE("HOSTED-WAIT-TEST: PASS hosted-signal-mask-context-switch");
    }
    return passed;
}

void observeSchedulerTimerHardContext(uint64_t delta, InterruptState &state)
{
    SchedulerTimerContext *context = __atomic_load_n(
        &g_SchedulerTimerContext, __ATOMIC_ACQUIRE);
    if (!context)
    {
        return;
    }

    Thread *current = Processor::information().getCurrentThread();
    const uint64_t interval = 100 * Time::Multiplier::Millisecond;
    if (delta < interval || (delta % interval) || !current ||
        !current->getHostedSignalDepth() ||
        !Processor::onHostedExecutionThread() ||
        Processor::inDeviceHardIrq() ||
        Processor::deviceHardIrqDepthForTest() != 0 ||
        state.getInterruptNumber() != SIGUSR2 ||
        state.getInterruptSource() != HostedSchedulerTimer::sourceForTest())
    {
        context->failures += 1;
    }
    context->calls += 1;
}

bool schedulerTimerHardContext()
{
    constexpr const char *Test = "hosted-scheduler-timer-hard-context";
    SchedulerTimerContext context;

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    __atomic_store_n(
        &g_SchedulerTimerContext, &context, __ATOMIC_RELEASE);
    HostedSchedulerTimer::setHardContextHookForTest(
        observeSchedulerTimerHardContext);
    Processor::setInterrupts(interruptsWereEnabled);

    const Time::Timestamp deadline =
        Time::getTicks() + (2 * Time::Multiplier::Second);
    while (!context.calls && Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }

    Processor::setInterrupts(false);
    HostedSchedulerTimer::setHardContextHookForTest(nullptr);
    __atomic_store_n(
        &g_SchedulerTimerContext,
        static_cast<SchedulerTimerContext *>(nullptr), __ATOMIC_RELEASE);
    Processor::setInterrupts(interruptsWereEnabled);

    const bool passed = context.calls && !context.failures;
    if (!passed)
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL " << Test
                                      << ": the scheduler callback did not "
                                         "suspend only its device-hard marker");
    }
    else
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "hosted-scheduler-timer-hard-context");
    }
    return passed;
}

class QueuedTickHandler : public HardIrqHandler
{
  public:
    explicit QueuedTickHandler(ContextSwitchContext &context)
        : m_Context(context)
    {
    }

    bool irq(irq_id_t, InterruptState &) override
    {
        const size_t phase = m_Context.phase;
        if (phase && phase < 4)
        {
            m_Context.tickCalls += 1;
            if (phase != 3 || Processor::getInterrupts() ||
                !Processor::inDeviceHardIrq() ||
                Processor::deviceHardIrqDepthForTest() != 1)
            {
                m_Context.failures += 1;
            }
            m_Context.phase = 4;
        }
        return true;
    }

  private:
    ContextSwitchContext &m_Context;
};

void contextSwitchHook(ProcessorBase::HostedContextSwitchStage stage)
{
    ContextSwitchContext *context =
        __atomic_load_n(&g_ContextSwitchContext, __ATOMIC_ACQUIRE);
    if (!context ||
        Processor::information().getCurrentThread() != context->driver)
    {
        return;
    }

    switch (stage)
    {
        case ProcessorBase::HostedContextSwitchStage::SwitchStateReturnedMasked:
            if (!context->phase.compareAndSwap(0, 1))
            {
                return;
            }
            context->switchReturns += 1;
            if (Processor::getInterrupts() ||
                !Processor::queueHostedSchedulerTickForTest())
            {
                context->failures += 1;
            }
            break;
        case ProcessorBase::HostedContextSwitchStage::
            SchedulerBookkeepingComplete:
            if (context->phase == static_cast<size_t>(1))
            {
                context->bookkeepingCalls += 1;
                if (Processor::getInterrupts() ||
                    !context->phase.compareAndSwap(1, 2))
                {
                    context->failures += 1;
                }
            }
            break;
        case ProcessorBase::HostedContextSwitchStage::
            SchedulerRestoringInterrupts:
            if (context->phase == static_cast<size_t>(2))
            {
                context->restoreBoundaries += 1;
                if (Processor::getInterrupts() ||
                    !context->phase.compareAndSwap(2, 3))
                {
                    context->failures += 1;
                }
            }
            break;
    }
}

int contextSwitchTarget(void *parameter)
{
    ContextSwitchContext *context =
        reinterpret_cast<ContextSwitchContext *>(parameter);
    context->targetCalls += 1;
    return 0;
}

bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR(
        "HOSTED-WAIT-TEST: FAIL context-switch-interrupt-restore: " << detail);
    return false;
}
}  // namespace

bool runHostedSchedulerRegressions()
{
    if (
        !hostedSignalMaskSpansContextSwitch() ||
        !schedulerTimerHardContext() || !deferredTimeAccountingWorker())
    {
        return false;
    }

    Thread *driver = Processor::information().getCurrentThread();
    ContextSwitchContext context(driver);
    QueuedTickHandler tickHandler(context);
    IrqManager *irqManager = Machine::instance().getIrqManager();
    const irq_id_t tickId =
        irqManager->registerHardIsaIrqHandler(
            1, &tickHandler, IrqPolicy::syntheticHard());

    Thread *target = new Thread(
        Scheduler::instance().getKernelProcess(), contextSwitchTarget, &context,
        nullptr, false, true, true);
    target->setName("hosted context-switch IRQ target");

    __atomic_store_n(&g_ContextSwitchContext, &context, __ATOMIC_RELEASE);
    Processor::setHostedContextSwitchHook(contextSwitchHook);
    const bool started = target->start();

    constexpr size_t Attempts = 10000;
    bool completed = false;
    for (size_t attempt = 0; started && attempt < Attempts; ++attempt)
    {
        if (context.phase == static_cast<size_t>(4) &&
            target->isReapableForHostedTest())
        {
            completed = true;
            break;
        }
        Scheduler::instance().yield();
    }

    Processor::setHostedContextSwitchHook(nullptr);
    __atomic_store_n(
        &g_ContextSwitchContext, static_cast<ContextSwitchContext *>(nullptr),
        __ATOMIC_RELEASE);

    const bool targetJoined =
        target->isReapableForHostedTest() && target->joinForCompletion();
    const bool handlerRemoved =
        tickId && irqManager->unregisterHandler(tickId, &tickHandler);

    const bool passed = check(
        tickId && started && completed && targetJoined && handlerRemoved &&
            context.switchReturns == 1 && context.bookkeepingCalls == 1 &&
            context.restoreBoundaries == 1 && context.tickCalls == 1 &&
            context.targetCalls == 1 && context.failures == 0,
        "the queued scheduler IRQ escaped the masked post-switch boundary");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS context-switch-interrupt-restore");
    }
    return passed;
}
