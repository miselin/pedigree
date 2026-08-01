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

namespace
{
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
            if (phase != 3 || !Processor::getInterrupts())
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
    Thread *driver = Processor::information().getCurrentThread();
    ContextSwitchContext context(driver);
    QueuedTickHandler tickHandler(context);
    IrqManager *irqManager = Machine::instance().getIrqManager();
    const irq_id_t tickId =
        irqManager->registerHardIsaIrqHandler(1, &tickHandler);

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
