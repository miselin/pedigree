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

#include "SchedulerTimer.h"
#include "IrqManager.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/SchedulerTimerDispatchCleanup.h"
#include "pedigree/kernel/machine/SchedulerTimerHandler.h"
#include "pedigree/kernel/processor/Processor.h"

/** 10 hertz frequency. */
#define ONE_SECOND 1000000000
#define HZ 10

HostedSchedulerTimer HostedSchedulerTimer::m_Instance;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
HostedSchedulerTimer::HardContextHook HostedSchedulerTimer::m_HardContextHook =
    nullptr;

void HostedSchedulerTimer::setHardContextHookForTest(HardContextHook hook)
{
    __atomic_store_n(&m_HardContextHook, hook, __ATOMIC_RELEASE);
}

uintptr_t HostedSchedulerTimer::sourceForTest()
{
    return reinterpret_cast<uintptr_t>(&m_Instance);
}

SchedulerTimerHandler *HostedSchedulerTimer::publishedHandlerForTest()
{
    return m_Instance.m_Handler.publishedHandlerForTest(Processor::id());
}

size_t HostedSchedulerTimer::activeDispatchesForTest()
{
    return m_Instance.m_Handler.activeDispatches();
}

bool HostedSchedulerTimer::directRoutePublishedForTest()
{
    return HostedIrqManager::schedulerIrqHandlerForTest(1) == &m_Instance;
}

bool HostedSchedulerTimer::queueTickForTest()
{
    return m_Instance.m_TickSource.queueExpirationForTest();
}
#endif

HostedSchedulerTimer::~HostedSchedulerTimer()
{
    uninitialise();
}

bool HostedSchedulerTimer::registerHandler(SchedulerTimerHandler *handler)
{
    return m_Handler.publish(Processor::id(), handler);
}

bool HostedSchedulerTimer::removeHandler(SchedulerTimerHandler *handler)
{
    return canRemoveHandlerInCurrentContext() &&
           m_Handler.unpublish(Processor::id(), handler);
}

bool HostedSchedulerTimer::initialise()
{
    if (!m_TickSource.prepare(SIGUSR2, this))
    {
        return false;
    }

    IrqManager &irqManager = *Machine::instance().getIrqManager();
    m_IrqId = irqManager.registerSchedulerIrqHandler(
        1, this, IrqPolicy::syntheticHard());
    if (m_IrqId == 0)
    {
        m_TickSource.destroy();
        return false;
    }

    // Publish the direct route before arming the signal source so the first
    // scheduler tick cannot arrive on an unregistered line.
    if (!m_TickSource.arm(ONE_SECOND / HZ))
    {
        if (!irqManager.unregisterSchedulerIrqHandler(m_IrqId, this))
        {
            FATAL(
                "HostedSchedulerTimer could not roll back its IRQ "
                "registration");
        }
        m_IrqId = 0;
        m_TickSource.destroy();
        return false;
    }

    m_bInitialized = true;
    return true;
}
void HostedSchedulerTimer::uninitialise()
{
    if (!m_bInitialized)
    {
        return;
    }
    m_bInitialized = false;

    if (!m_TickSource.disarm())
    {
        FATAL("HostedSchedulerTimer could not disarm its signal source");
    }

    // Free the IRQ
    if (m_IrqId != 0)
    {
        IrqManager &irqManager = *Machine::instance().getIrqManager();
        if (!irqManager.unregisterSchedulerIrqHandler(m_IrqId, this))
        {
            FATAL(
                "HostedSchedulerTimer teardown could not remove its direct "
                "IRQ route");
        }
        m_IrqId = 0;
    }
    m_TickSource.destroy();

    // Source teardown does not confer ownership of the scheduler callback.
    // Its exact owner remains responsible for unpublishing it.
}

HostedSchedulerTimer::HostedSchedulerTimer()
    : m_IrqId(0), m_TickSource(), m_Handler(), m_bInitialized(false)
{
}

void HostedSchedulerTimer::schedulerIrq(
    irq_id_t number, InterruptState &state)
{
    const siginfo_t *signalInfo =
        reinterpret_cast<const siginfo_t *>(state.getRegister(1));
    if (number != 1 || state.getInterruptNumber() != SIGUSR2)
    {
        return;
    }

    size_t expirations = 0;
    const HostedTickSource::TakeResult result =
        m_TickSource.takeExpirations(signalInfo, expirations);
    if (result == HostedTickSource::TakeResult::NotSource)
    {
        return;
    }
    if (result == HostedTickSource::TakeResult::Invalid)
    {
        FATAL_NOLOCK(
            "HostedSchedulerTimer received invalid tick-source metadata");
        return;
    }
    if (!expirations)
    {
        return;
    }

    constexpr uint64_t Interval = ONE_SECOND / HZ;
    if (expirations > (~static_cast<uint64_t>(0) / Interval))
    {
        FATAL_NOLOCK("HostedSchedulerTimer elapsed-time batch overflowed");
        return;
    }
    const uint64_t delta = static_cast<uint64_t>(expirations) * Interval;

    SchedulerTimerHandlerSlot::DispatchGuard dispatch;
    if (LIKELY(m_Handler.beginDispatch(Processor::id(), dispatch)))
    {
        SchedulerTimerDispatchCleanup dispatchCleanup(dispatch);
        ExecutionContextGuard schedulerContext(ExecutionContext::SchedulerIrq);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        // Test only admitted callbacks. Keeping this hook outside the slot
        // previously let a dropped scheduler tick look like real progress.
        HardContextHook hook =
            __atomic_load_n(&m_HardContextHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(delta, state);
        }
#endif
        dispatch.handler()->timer(delta, state);
    }
}
