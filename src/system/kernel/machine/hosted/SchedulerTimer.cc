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
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/SchedulerTimerHandler.h"

using namespace __pedigree_hosted;

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
#endif

HostedSchedulerTimer::~HostedSchedulerTimer()
{
    uninitialise();
}

bool HostedSchedulerTimer::registerHandler(SchedulerTimerHandler *handler)
{
    if (UNLIKELY(handler == 0 && m_Handler != 0))
        return false;

    m_Handler = handler;
    return true;
}

void HostedSchedulerTimer::removeHandler(SchedulerTimerHandler *handler)
{
    if (m_Handler == handler)
    {
        m_Handler = nullptr;
    }
}

bool HostedSchedulerTimer::initialise()
{
    struct sigevent sv;
    ByteSet(&sv, 0, sizeof(sv));
    sv.sigev_notify = SIGEV_SIGNAL;
    sv.sigev_signo = SIGUSR2;
    sv.sigev_value.sival_ptr = this;
    int r = timer_create(CLOCK_MONOTONIC, &sv, &m_Timer);
    if (r != 0)
    {
        /// \todo error message or something
        return false;
    }

    IrqManager &irqManager = *Machine::instance().getIrqManager();
    m_IrqId = irqManager.registerHardIsaIrqHandler(
        1, this, IrqPolicy::syntheticHard());
    if (m_IrqId == 0)
    {
        timer_delete(m_Timer);
        return false;
    }

    // Publish the hard handler before arming the signal source so the first
    // scheduler tick cannot arrive on an unregistered line.
    struct itimerspec interval;
    ByteSet(&interval, 0, sizeof(interval));
    interval.it_interval.tv_nsec = ONE_SECOND / HZ;
    interval.it_value.tv_nsec = ONE_SECOND / HZ;
    r = timer_settime(m_Timer, 0, &interval, 0);
    if (r != 0)
    {
        if (!irqManager.unregisterHandler(m_IrqId, this))
        {
            FATAL(
                "HostedSchedulerTimer could not roll back its IRQ "
                "registration");
        }
        m_IrqId = 0;
        timer_delete(m_Timer);
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

    struct itimerspec disarmed;
    ByteSet(&disarmed, 0, sizeof(disarmed));
    if (timer_settime(m_Timer, 0, &disarmed, nullptr) != 0)
    {
        FATAL("HostedSchedulerTimer could not disarm its signal source");
    }

    // Free the IRQ
    if (m_IrqId != 0)
    {
        IrqManager &irqManager = *Machine::instance().getIrqManager();
        if (!irqManager.unregisterHandler(m_IrqId, this))
        {
            FATAL(
                "HostedSchedulerTimer teardown could not synchronously "
                "unregister its IRQ callback");
        }
        m_IrqId = 0;
    }
    timer_delete(m_Timer);
    m_Handler = nullptr;
}

HostedSchedulerTimer::HostedSchedulerTimer()
    : m_IrqId(0), m_Handler(0), m_bInitialized(false)
{
}

bool HostedSchedulerTimer::irq(irq_id_t number, InterruptState &state)
{
    const siginfo_t *signalInfo =
        reinterpret_cast<const siginfo_t *>(state.getRegister(1));
    if (
        number != 1 || state.getInterruptNumber() != SIGUSR2 || !signalInfo ||
        signalInfo->si_signo != SIGUSR2 || signalInfo->si_code != SI_TIMER ||
        signalInfo->si_value.sival_ptr != this)
    {
        return false;
    }

    if (signalInfo->si_overrun < 0)
    {
        FATAL_NOLOCK(
            "HostedSchedulerTimer received invalid POSIX timer metadata");
        return true;
    }

    const uint64_t expirations =
        static_cast<uint64_t>(signalInfo->si_overrun) + 1;
    const uint64_t delta = expirations * (ONE_SECOND / HZ);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HardContextHook hook =
        __atomic_load_n(&m_HardContextHook, __ATOMIC_ACQUIRE);
    if (hook)
    {
        hook(delta, state);
    }
#endif

    if (LIKELY(m_Handler != 0))
        m_Handler->timer(delta, state);

    return true;
}
