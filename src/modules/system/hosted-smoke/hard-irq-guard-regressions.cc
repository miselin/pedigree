/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/Processor.h"
#include "system/kernel/machine/hosted/IrqManager.h"

namespace
{
constexpr size_t ExpectedDenials = 7;

struct GuardHookContext
{
    GuardHookContext() : calls(0), overflow(0), operations()
    {
    }

    size_t calls;
    size_t overflow;
    DeviceHardIrqOperation operations[ExpectedDenials];
};

GuardHookContext *g_GuardHookContext = nullptr;

bool denyGuardedOperation(DeviceHardIrqOperation operation)
{
    GuardHookContext *context =
        __atomic_load_n(&g_GuardHookContext, __ATOMIC_ACQUIRE);
    if (!context)
    {
        return false;
    }

    const size_t call = __atomic_fetch_add(
        &context->calls, static_cast<size_t>(1), __ATOMIC_RELAXED);
    if (call < ExpectedDenials)
    {
        context->operations[call] = operation;
    }
    else
    {
        __atomic_store_n(
            &context->overflow, static_cast<size_t>(1), __ATOMIC_RELAXED);
    }
    return true;
}

class HardIrqGuardProbe : public HardIrqHandler
{
  public:
    HardIrqGuardProbe(Semaphore &semaphore, WaitQueue &waitQueue)
        : m_Semaphore(semaphore), m_WaitQueue(waitQueue), calls(0), depth(0),
          marked(false), interruptsDisabled(false), scheduleReturned(false),
          semaphoreDenied(false), semaphoreReleaseDenied(false),
          waitDenied(false), wakeDenied(false), heapAllocateDenied(false),
          heapFreeDenied(false)
    {
    }

    bool irq(irq_id_t, InterruptState &) override
    {
        ++calls;
        depth = Processor::deviceHardIrqDepthForTest();
        marked = Processor::inDeviceHardIrq();
        interruptsDisabled = !Processor::getInterrupts();

        Scheduler::instance().yield();
        scheduleReturned = true;

        Semaphore::SemaphoreError error = Semaphore::NoError;
        semaphoreDenied = !m_Semaphore.acquireWithError(1, 0, 0, error) &&
                          error == Semaphore::Interrupted;
        m_Semaphore.release(1);
        semaphoreReleaseDenied = m_Semaphore.getValue() == 0;

        {
            auto guard = m_WaitQueue.acquire();
            waitDenied = guard.wait() == WaitQueue::WakeReason::Spurious;
        }
        wakeDenied =
            m_WaitQueue.wakeAll(WaitQueue::WakeReason::Signalled) == 0;

        heapAllocateDenied = SlamAllocator::guardedAllocateForTest(64) == 0;
        SlamAllocator::guardedFreeForTest(0);
        heapFreeDenied = true;
        return true;
    }

    Semaphore &m_Semaphore;
    WaitQueue &m_WaitQueue;
    size_t calls;
    size_t depth;
    bool marked;
    bool interruptsDisabled;
    bool scheduleReturned;
    bool semaphoreDenied;
    bool semaphoreReleaseDenied;
    bool waitDenied;
    bool wakeDenied;
    bool heapAllocateDenied;
    bool heapFreeDenied;
};
}  // namespace

bool runHostedHardIrqGuardRegressions()
{
    constexpr const char *Test = "hard-irq-operation-deny-guard";
    GuardHookContext context;
    Semaphore semaphore(0);
    WaitQueue waitQueue;
    HardIrqGuardProbe handler(semaphore, waitQueue);
    IrqManager *manager = Machine::instance().getIrqManager();
    const irq_id_t id = manager->registerHardIsaIrqHandler(
        2, &handler, IrqPolicy::syntheticHard());

    __atomic_store_n(&g_GuardHookContext, &context, __ATOMIC_RELEASE);
    Processor::setDeviceHardIrqOperationHookForTest(denyGuardedOperation);
    const size_t denialsBefore =
        Processor::deviceHardIrqOperationDenialsForTest();
    const bool ordinaryAllowed = Processor::guardDeviceHardIrqOperation(
        DeviceHardIrqOperation::Schedule);

    bool handled = false;
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    const bool admitted =
        id && HostedIrqManager::dispatchHandlerForTest(2, &handler, handled);
    Processor::setInterrupts(interruptsWereEnabled);

    Processor::setDeviceHardIrqOperationHookForTest(nullptr);
    __atomic_store_n(
        &g_GuardHookContext, static_cast<GuardHookContext *>(nullptr),
        __ATOMIC_RELEASE);
    const size_t denialsAfter =
        Processor::deviceHardIrqOperationDenialsForTest();
    const bool removed = id && manager->unregisterHandler(id, &handler);
    const bool handlerAbsent =
        !HostedIrqManager::containsHandlerForTest(2, &handler);

    const DeviceHardIrqOperation expected[ExpectedDenials] = {
        DeviceHardIrqOperation::Schedule,
        DeviceHardIrqOperation::SemaphoreAcquire,
        DeviceHardIrqOperation::SemaphoreRelease,
        DeviceHardIrqOperation::WaitQueueAccess,
        DeviceHardIrqOperation::WaitQueueAccess,
        DeviceHardIrqOperation::HeapAllocate,
        DeviceHardIrqOperation::HeapFree,
    };
    bool operationsMatch =
        context.calls == ExpectedDenials && !context.overflow;
    for (size_t i = 0; i < ExpectedDenials && operationsMatch; ++i)
    {
        operationsMatch = context.operations[i] == expected[i];
    }

    const bool passed =
        id && ordinaryAllowed && admitted && handled && removed &&
        handlerAbsent && handler.calls == 1 && handler.marked &&
        handler.depth == 1 && handler.interruptsDisabled &&
        handler.scheduleReturned &&
        handler.semaphoreDenied && handler.semaphoreReleaseDenied &&
        handler.waitDenied && handler.wakeDenied && handler.heapAllocateDenied &&
        handler.heapFreeDenied &&
        operationsMatch && denialsAfter - denialsBefore == ExpectedDenials &&
        waitQueue.waiterCount() == 0 && semaphore.getValue() == 0 &&
        !Processor::inDeviceHardIrq() &&
        Processor::deviceHardIrqDepthForTest() == 0;

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS hard-irq-operation-deny-guard");
    }
    else
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL "
            << Test << ": a forbidden hard-IRQ operation escaped its guard");
    }
    return passed;
}
