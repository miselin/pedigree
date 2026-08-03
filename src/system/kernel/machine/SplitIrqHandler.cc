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
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/String.h"

namespace
{
class SplitLifecycleGuard
{
  public:
    explicit SplitLifecycleGuard(Atomic<size_t> &busy)
        : m_Busy(busy), m_Owned(m_Busy.compareAndSwap(0, 1))
    {
    }

    ~SplitLifecycleGuard()
    {
        if (m_Owned)
        {
            m_Busy = 0;
        }
    }

    bool owned() const
    {
        return m_Owned;
    }

  private:
    Atomic<size_t> &m_Busy;
    bool m_Owned;
};
}  // namespace

SplitIrqHandler::SplitIrqHandler(const String &name)
    : HardIrqHandler(), m_Registrations(), m_RegistrationCount(0),
      m_LifecycleBusy(0), m_AcceptingRegistrations(0),
      m_Dispatcher(name, 1, dispatchThreaded, this), m_StateLock(false),
      m_Quiescing(true), m_Stopping(1), m_PublicationFailures(0),
      m_DeferredIrqs(0), m_CompletedBatches(0), m_PendingWork(0),
      m_Started(false)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_RegistrationPublishedHook(nullptr)
#endif
{
}

SplitIrqHandler::~SplitIrqHandler()
{
    if (m_Started || m_RegistrationCount || m_LifecycleBusy ||
        m_AcceptingRegistrations || m_Dispatcher.isInitialised() ||
        __atomic_load_n(&m_PendingWork, __ATOMIC_ACQUIRE))
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
    TerminationDeferral lifecycleTermination;
    SplitLifecycleGuard lifecycle(m_LifecycleBusy);
    if (!lifecycle.owned())
    {
        return false;
    }
    if (m_Started || m_RegistrationCount)
    {
        return false;
    }

    __atomic_store_n(&m_PendingWork, static_cast<size_t>(0), __ATOMIC_RELEASE);
    m_PublicationFailures = 0;
    m_DeferredIrqs = 0;
    m_CompletedBatches = 0;
    m_Stopping = 1;
    if (!m_Dispatcher.initialise())
    {
        return false;
    }
    {
        LockGuard<Spinlock> guard(m_StateLock);
        m_Quiescing = false;
    }
    m_Stopping = 0;
    m_Started = true;
    m_AcceptingRegistrations = 1;
    return true;
#else
    return false;
#endif
}

irq_id_t SplitIrqHandler::registerIsaSplitIrq(
    IrqManager &manager, uint8_t irq, const IrqPolicy &policy)
{
    TerminationDeferral lifecycleTermination;
    SplitLifecycleGuard lifecycle(m_LifecycleBusy);
    if (!lifecycle.owned() || !m_AcceptingRegistrations || !m_Started ||
        m_Stopping || m_RegistrationCount >= MaxRegistrations)
    {
        return 0;
    }

    const irq_id_t id =
        manager.registerHardIsaIrqHandler(irq, this, policy);
    if (!id)
    {
        return 0;
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (m_RegistrationPublishedHook)
    {
        m_RegistrationPublishedHook(this);
    }
#endif

    Registration &registration = m_Registrations[m_RegistrationCount++];
    registration.manager = &manager;
    registration.id = id;
    return id;
}

irq_id_t
SplitIrqHandler::registerPciSplitIrq(
    IrqManager &manager, Device &device, const IrqPolicy &policy)
{
    TerminationDeferral lifecycleTermination;
    SplitLifecycleGuard lifecycle(m_LifecycleBusy);
    if (!lifecycle.owned() || !m_AcceptingRegistrations || !m_Started ||
        m_Stopping || m_RegistrationCount >= MaxRegistrations)
    {
        return 0;
    }

    const irq_id_t id =
        manager.registerHardPciIrqHandler(this, &device, policy);
    if (!id)
    {
        return 0;
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (m_RegistrationPublishedHook)
    {
        m_RegistrationPublishedHook(this);
    }
#endif

    Registration &registration = m_Registrations[m_RegistrationCount++];
    registration.manager = &manager;
    registration.id = id;
    return id;
}

bool SplitIrqHandler::shutdownSplitIrq()
{
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

    // Derived quiesce operations and callback drains may reach interruptible
    // waits. Keep teardown alive until the lifecycle token has been released;
    // declaration order makes the guard unwind before termination is enabled.
    TerminationDeferral lifecycleTermination;
    SplitLifecycleGuard lifecycle(m_LifecycleBusy);
    if (!lifecycle.owned())
    {
        return false;
    }

    if (!m_Started)
    {
        return m_RegistrationCount == 0 && !m_AcceptingRegistrations &&
               !m_Dispatcher.isInitialised() &&
               !__atomic_load_n(&m_PendingWork, __ATOMIC_ACQUIRE);
    }

    if (m_Dispatcher.isCurrentWorker())
    {
        return false;
    }

    // Once teardown has won lifecycle ownership, failed retries may continue
    // but no later registration can reopen a source on the draining worker.
    m_AcceptingRegistrations = 0;
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

    // Keep dispatcher admission open until unregister has drained hard
    // callbacks admitted before device quiescence became visible.
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

    m_Stopping = 1;
    if (!m_Dispatcher.shutdown())
    {
        return false;
    }

    // A failed hard publication records work before its doorbell is rejected.
    // Once hardware is quiesced and every hard callback is drained, no new
    // producer can race this ordinary-context orphan drain.
    if (__atomic_load_n(&m_PendingWork, __ATOMIC_ACQUIRE))
    {
        dispatchThreaded();
    }

    // An admitted hard callback may have run after the first quiesce while
    // unregister waited for its callback pin. Reassert the device boundary
    // after all work published by those callbacks has drained.
    if (!quiesceIrqSources())
    {
        return false;
    }

    if (__atomic_load_n(&m_PendingWork, __ATOMIC_ACQUIRE))
    {
        return false;
    }

    m_Started = false;
    return true;
}

HardIrqDisposition
SplitIrqHandler::irq(irq_id_t number, InterruptState &state)
{
    size_t work = 0;
    const HardStageDisposition disposition = hardIrq(number, state, work);
    if (disposition == HardStageDisposition::NotHandled)
    {
        return HardIrqDisposition::NotHandled;
    }
    if (disposition == HardStageDisposition::Deferred)
    {
        return publishWork(work ? work : 1) ? HardIrqDisposition::Handled :
                                             HardIrqDisposition::KeepMasked;
    }
    return HardIrqDisposition::Handled;
}

void SplitIrqHandler::dispatchThreaded(
    void *context, uint8_t, size_t)
{
    reinterpret_cast<SplitIrqHandler *>(context)->dispatchThreaded();
}

void SplitIrqHandler::dispatchThreaded()
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
}

bool SplitIrqHandler::publishWork(size_t work)
{
    __atomic_or_fetch(&m_PendingWork, work, __ATOMIC_ACQ_REL);
    m_DeferredIrqs += 1;
    if (m_Stopping || !m_Dispatcher.publishFromInterrupt(0, 1))
    {
        m_PublicationFailures += 1;
        return false;
    }
    return true;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
HardIrqHandler *SplitIrqHandler::hardHandlerForTest()
{
    return this;
}

void SplitIrqHandler::setRegistrationPublishedHookForTest(
    RegistrationPublishedHook hook)
{
    m_RegistrationPublishedHook = hook;
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

void SplitIrqHandler::rejectNextPublicationForTest()
{
    m_Dispatcher.rejectNextPublicationForTest();
}
#endif
