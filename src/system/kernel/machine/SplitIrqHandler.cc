/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/machine/SplitIrqHandler.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/String.h"

SplitIrqHandler::SplitIrqHandler(const String &name)
    : HardIrqHandler(), RequestQueue(name), m_Registrations(),
      m_RegistrationCount(0), m_WorkRequest(&workReleased, this),
      m_StateLock(false), m_Quiescing(true), m_Stopping(1),
      m_PublicationFailures(0), m_DeferredIrqs(0), m_CompletedBatches(0),
      m_PendingWork(0), m_Started(false)
{
}

SplitIrqHandler::~SplitIrqHandler()
{
    if (m_Started || m_RegistrationCount || !m_WorkRequest.isAvailable())
    {
        FATAL(
            "A split IRQ handler reached its base destructor while "
            "active; the most-derived destructor must disable its device "
            "source and call shutdownSplitIrq().");
    }
}

bool SplitIrqHandler::initialiseSplitIrq()
{
#if THREADS
    if (m_Started || m_RegistrationCount)
    {
        return false;
    }

    __atomic_store_n(&m_PendingWork, static_cast<size_t>(0), __ATOMIC_RELEASE);
    m_PublicationFailures = 0;
    m_DeferredIrqs = 0;
    m_CompletedBatches = 0;
    {
        LockGuard<Spinlock> guard(m_StateLock);
        m_Quiescing = false;
    }
    m_Stopping = 0;
    RequestQueue::initialise();
    m_Started = getLifecycleState() == LifecycleState::Accepting;
    if (!m_Started)
    {
        m_Stopping = 1;
    }
    return m_Started;
#else
    return false;
#endif
}

irq_id_t SplitIrqHandler::registerIsaSplitIrq(
    IrqManager &manager, uint8_t irq, bool edge)
{
    if (!m_Started || m_Stopping || m_RegistrationCount >= MaxRegistrations)
    {
        return 0;
    }

    const irq_id_t id = manager.registerHardIsaIrqHandler(irq, this, edge);
    if (!id)
    {
        return 0;
    }

    Registration &registration = m_Registrations[m_RegistrationCount++];
    registration.manager = &manager;
    registration.id = id;
    return id;
}

irq_id_t
SplitIrqHandler::registerPciSplitIrq(IrqManager &manager, Device &device)
{
    if (!m_Started || m_Stopping || m_RegistrationCount >= MaxRegistrations)
    {
        return 0;
    }

    const irq_id_t id = manager.registerHardPciIrqHandler(this, &device);
    if (!id)
    {
        return 0;
    }

    Registration &registration = m_Registrations[m_RegistrationCount++];
    registration.manager = &manager;
    registration.id = id;
    return id;
}

bool SplitIrqHandler::shutdownSplitIrq()
{
    if (!m_Started)
    {
        return m_RegistrationCount == 0 && m_WorkRequest.isAvailable();
    }

#if THREADS
    Thread *current = Processor::information().getCurrentThread();
    if (!current || !Processor::getInterrupts())
    {
        return false;
    }
#if HOSTED
    if (current->getHostedSignalDepth())
    {
        return false;
    }
#endif

    {
        auto guard = m_RequestQueueWaiters.acquire();
        if (m_pThread == current)
        {
            return false;
        }
    }
#endif

    // A bottom half which wins m_StateLock first may rearm, but hardware
    // quiescence then masks it. A bottom half which loses observes the state
    // transition and cannot rearm after the hardware has been masked.
    {
        LockGuard<Spinlock> guard(m_StateLock);
        m_Quiescing = true;
    }
    if (!quiesceIrqSources())
    {
        return false;
    }

    // Keep queue admission open until unregister has drained hard callbacks
    // admitted before device quiescence became visible.
    while (m_RegistrationCount)
    {
        Registration &registration = m_Registrations[m_RegistrationCount - 1];
        if (!registration.manager || !registration.id ||
            !registration.manager->unregisterHandler(registration.id, this))
        {
            // A retry remains safe. Successfully removed registrations stay
            // closed, while any remaining admitted callback can still publish.
            return false;
        }

        registration.manager = nullptr;
        registration.id = 0;
        --m_RegistrationCount;
    }

    if (!drain())
    {
        return false;
    }

    // An admitted hard callback may have run after the first quiesce while
    // unregister waited for its callback pin. Reassert the device boundary
    // after all work published by those callbacks has drained.
    if (!quiesceIrqSources())
    {
        return false;
    }

    m_Stopping = 1;
    RequestQueue::destroy();
    if (getLifecycleState() != LifecycleState::Stopped)
    {
        return false;
    }

    __atomic_store_n(&m_PendingWork, static_cast<size_t>(0), __ATOMIC_RELEASE);
    m_Started = false;
    return m_WorkRequest.isAvailable();
}

bool SplitIrqHandler::irq(irq_id_t number, InterruptState &state)
{
    size_t work = 0;
    const HardIrqDisposition disposition = hardIrq(number, state, work);
    if (disposition == HardIrqDisposition::NotHandled)
    {
        return false;
    }
    if (disposition == HardIrqDisposition::Deferred)
    {
        publishWork(work ? work : 1);
    }
    return true;
}

uint64_t SplitIrqHandler::executeRequest(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t)
{
    const size_t work = __atomic_exchange_n(
        &m_PendingWork, static_cast<size_t>(0), __ATOMIC_ACQ_REL);
    if (work)
    {
        threadedIrq(work);
        {
            LockGuard<Spinlock> guard(m_StateLock);
            if (!m_Quiescing)
            {
                rearmIrqSources(work);
            }
        }
        m_CompletedBatches += 1;
    }
    return 0;
}

void SplitIrqHandler::publishWork(size_t work)
{
    if (m_Stopping)
    {
        return;
    }

    __atomic_or_fetch(&m_PendingWork, work, __ATOMIC_ACQ_REL);
    m_DeferredIrqs += 1;
    const InterruptEnqueueResult result = tryPublishWork();
    if (result != InterruptEnqueueResult::Accepted &&
        result != InterruptEnqueueResult::TokenBusy && !m_Stopping)
    {
        m_PublicationFailures += 1;
    }
}

RequestQueue::InterruptEnqueueResult SplitIrqHandler::tryPublishWork()
{
    InterruptEnqueueResult result = republishWhileReleasing(m_WorkRequest, 0);
    if (result == InterruptEnqueueResult::TokenBusy)
    {
        result = enqueueFromInterrupt(m_WorkRequest, 0);
    }
    return result;
}

void SplitIrqHandler::workReleased(void *context)
{
    reinterpret_cast<SplitIrqHandler *>(context)->workReleased();
}

void SplitIrqHandler::workReleased()
{
    if (m_Stopping || !__atomic_load_n(&m_PendingWork, __ATOMIC_ACQUIRE))
    {
        return;
    }

    const InterruptEnqueueResult result =
        republishWhileReleasing(m_WorkRequest, 0);
    if (result != InterruptEnqueueResult::Accepted &&
        result != InterruptEnqueueResult::TokenBusy && !m_Stopping)
    {
        m_PublicationFailures += 1;
    }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
HardIrqHandler *SplitIrqHandler::hardHandlerForTest()
{
    return this;
}

WaitQueue *SplitIrqHandler::workerWaitQueueForTest()
{
    return &m_RequestQueueWaiters;
}

size_t SplitIrqHandler::publicationFailuresForTest() const
{
    return m_PublicationFailures;
}

size_t SplitIrqHandler::pendingWorkForTest() const
{
    return __atomic_load_n(&m_PendingWork, __ATOMIC_ACQUIRE);
}

size_t SplitIrqHandler::deferredIrqsForTest() const
{
    return m_DeferredIrqs;
}

size_t SplitIrqHandler::completedBatchesForTest() const
{
    return m_CompletedBatches;
}
#endif
