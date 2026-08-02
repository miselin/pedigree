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

#if X86_COMMON

#include "Uhci.h"
#include "modules/system/usb/Usb.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#define INDEX_FROM_TD_VIRT(ptr)                  \
    (((reinterpret_cast<uintptr_t>((ptr)) -      \
       reinterpret_cast<uintptr_t>(m_pTDList)) / \
      sizeof(TD)))
#define INDEX_FROM_TD_PHYS(ptr) ((((ptr) -m_pTDListPhys) / sizeof(TD)))
#define PHYS_TD(idx) (m_pTDListPhys + ((idx) * sizeof(TD)))

#define QH_REGION_SIZE 0x4000
#define TD_REGION_SIZE 0x8000

#define TOTAL_MEM_PAGES \
    ((QH_REGION_SIZE / 0x1000) + (TD_REGION_SIZE / 0x1000) + 1)

static int threadStub(void *p)
{
    TerminationDeferral workerLifetime;
    Uhci *pUhci = reinterpret_cast<Uhci *>(p);
    pUhci->doDequeue();
    return 0;
}

Uhci::Uhci(Device *pDev)
    : UsbHub(pDev), RequestQueue(MakeConstantString("UHCI")), m_pBase(0),
      m_SubmissionOperations(), m_CancelOperations(), m_TransferOperations(),
      m_CallbackOperations(), m_IrqId(0), m_TimerRegistered(false),
      m_InterruptsClosing(false), m_nPorts(0), m_PortChangesClosing(false),
      m_PortChangeLock(), m_Mutex(), m_IrqProcessingLock(),
      m_CompletionDeliveries(), m_AsyncQueueListChangeLock(),
      m_pFrameList(nullptr), m_pFrameListPhys(0), m_pTDList(nullptr),
      m_pTDListPhys(0), m_pAsyncQH(nullptr), m_pPeriodicQH(nullptr),
      m_pQHList(nullptr), m_pQHListPhys(0), m_UhciMR("Uhci-MR"),
      m_pCurrentAsyncQueueTail(0), m_pCurrentAsyncQueueHead(0),
      m_AsyncSchedule(), m_DequeueList(), m_DequeueCount(0),
      m_DequeueOperations(), m_pDequeueThread(nullptr), m_nPortCheckTicks(0)
{
    setSpecificType(String("UHCI"));

    // Verify that IRQs are enabled - we need them!
    if (!Processor::getInterrupts())
        Processor::setInterrupts(true);

    // Grab the ports
    m_pBase = m_Addresses[0]->m_Io;
    m_Addresses[0]->map();

    // Allocate the memory region
    if (!PhysicalMemoryManager::instance().allocateRegion(
            m_UhciMR, TOTAL_MEM_PAGES, PhysicalMemoryManager::continuous,
            VirtualAddressSpace::Write | VirtualAddressSpace::KernelMode))
    {
        ERROR("USB: UHCI: Couldn't allocate memory region!");
        return;
    }

    uintptr_t pVirtBase =
        reinterpret_cast<uintptr_t>(m_UhciMR.virtualAddress());
    physical_uintptr_t pPhysBase = m_UhciMR.physicalAddress();

    m_pFrameList = reinterpret_cast<uint32_t *>(pVirtBase);
    m_pQHList = reinterpret_cast<QH *>(pVirtBase + 0x1000);
    m_pTDList = reinterpret_cast<TD *>(pVirtBase + 0x1000 + QH_REGION_SIZE);

    m_pFrameListPhys = pPhysBase;
    m_pQHListPhys = pPhysBase + 0x1000;
    m_pTDListPhys = m_pQHListPhys + QH_REGION_SIZE;

    // Allocate room for the Dummy QH
    m_QHBitmap.set(0);
    QH *pDummyQH = &m_pQHList[0];

    // Set all frame list entries to the dummy QH
    DoubleWordSet(m_pFrameList, m_pQHListPhys | 2, 0x400);

    ByteSet(pDummyQH, 0, sizeof(QH));

    pDummyQH->pNext = m_pQHListPhys >> 4;
    pDummyQH->bNextQH = 1;
    pDummyQH->bElemInvalid = 1;

    pDummyQH->pMetaData = new QH::MetaData;
    pDummyQH->pMetaData->pOwner = this;
    pDummyQH->pMetaData->pPeriodicCallback = nullptr;
    pDummyQH->pMetaData->pPeriodicParam = 0;
    pDummyQH->pMetaData->bPeriodic = false;
    pDummyQH->pMetaData->pFirstTD = nullptr;
    pDummyQH->pMetaData->pLastTD = nullptr;
    pDummyQH->pMetaData->nTotalBytes = 0;
    pDummyQH->pMetaData->bIgnore = true;
    pDummyQH->pMetaData->id = 0;
    pDummyQH->pMetaData->pPrev = pDummyQH->pMetaData->pNext = pDummyQH;

    m_pCurrentAsyncQueueTail = m_pCurrentAsyncQueueHead = pDummyQH;

    // Dequeue main thread
    m_pDequeueThread = new Thread(
        Processor::information().getCurrentThread()->getParent(), threadStub,
        reinterpret_cast<void *>(this));

    uint32_t nCommand = PciBus::instance().readConfigSpace(this, 1);
#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("USB: UHCI: Pci command+status: " << nCommand);
#endif
    PciBus::instance().writeConfigSpace(this, 1, nCommand | 0x6);

    // Disable legacy emulation and SMI generation
    setLegacySupportControl(0x8F00);

    // Stop a running controller (BIOS may have started it up). Unset the
    // configured flag, as we are no longer configured properly.
    m_pBase->write16(m_pBase->read16(UHCI_CMD) & ~0x40, UHCI_CMD);
    stop();
    m_pBase->write16(m_pBase->read16(UHCI_STS) & 0x1f, UHCI_STS);

    // Reset the host controller
    m_pBase->write16(m_pBase->read16(UHCI_CMD) | UHCI_CMD_HCRES, UHCI_CMD);
    constexpr size_t ResetPollLimit = 100;
    size_t resetPolls = ResetPollLimit;
    while (resetPolls-- && (m_pBase->read16(UHCI_CMD) & UHCI_CMD_HCRES))
        Time::delay(1 * Time::Multiplier::Millisecond);
    if (m_pBase->read16(UHCI_CMD) & UHCI_CMD_HCRES)
        panic("UHCI controller reset did not complete within 100 ms");

    // Write frame list pointer
    m_pBase->write32(m_pFrameListPhys, UHCI_FRLP);

    // Close and flush the controller source before registering with the PIC.
    // Registration unmasks the shared line immediately.
    m_pBase->write16(0, UHCI_INTR);
    (void) m_pBase->read16(UHCI_INTR);
    const uint16_t pendingStatus = m_pBase->read16(UHCI_STS);
    if (pendingStatus)
    {
        m_pBase->write16(pendingStatus, UHCI_STS);
        (void) m_pBase->read16(UHCI_STS);
    }

    m_IrqId = Machine::instance().getIrqManager()->registerPciIrqHandler(
        static_cast<IrqHandler *>(this), this, IrqPolicy::pciIntxThreaded());
    if (!m_IrqId)
    {
        panic("UHCI could not register its PCI IRQ handler");
    }
    Machine::instance().getIrqManager()->control(
        getInterruptNumber(), IrqManager::MitigationThreshold,
        (1500000 / 64));  // 12KB/ms (12Mbps) in bytes, divided by 64 bytes
                          // maximum per transfer/IRQ

    RequestQueue::initialise();
#if THREADS
    if (getLifecycleState() != RequestQueue::LifecycleState::Accepting)
    {
        panic("UHCI request queue did not enter the accepting state");
    }
#endif

    // Start the controller: 64-byte reclamation and CF set, as well as run bit.
    // Also, force a global resume of all ports out of any form of suspend state
    m_pBase->write16(0xC1 | 0x10, UHCI_CMD);
    Time::delay(10 * Time::Multiplier::Millisecond);
    m_pBase->write16(0xC1, UHCI_CMD);
    start();

#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("USB: UHCI: Reset complete");
#endif

    // Give time for ports to resume and stabilise.
    Time::delay(100 * Time::Multiplier::Millisecond);

    for (size_t i = 0; i < UsbHcd::UhciRootPortCount; i++)
    {
        uint16_t nPortStatus = m_pBase->read16(UHCI_PORTSC + (i * 2));
        NOTICE("Port status is " << nPortStatus);
        if ((!(nPortStatus & 0x80)) && (m_nPorts >= 2))
        {
            break;  // Controllers must have 2 ports, but can have up to 7 by
                    // the spec.
        }

        ++m_nPorts;
    }

    if (!UsbHcd::validUhciRootPortCount(m_nPorts))
    {
        panic("UHCI detected an unsupported root-port count");
    }

    for (size_t i = 0; i < m_nPorts; ++i)
    {
        if (!m_PortChanges[i].configure(*this, 0, i))
        {
            panic("UHCI could not configure a root-port publication token");
        }
    }

    // Every fallible publication setup is complete. The controller ran with
    // its source masked up to this point, so registration failures cannot leak
    // an unserviceable level interrupt onto the shared PCI line.
    m_pBase->write16(0xf, UHCI_INTR);
    (void) m_pBase->read16(UHCI_INTR);
    setLegacySupportControl(0x2000);

    for (size_t i = 0; i < m_nPorts; ++i)
    {
        if (!portReset(i))
        {
            continue;
        }

        const size_t portRegister = UHCI_PORTSC + (i * 2);
        {
            LockGuard<Spinlock> portChangeGuard(m_PortChangeLock);
            const uint16_t portStatus = m_pBase->read16(portRegister);
            if (portStatus & UHCI_PORTSC_CSCH)
            {
                constexpr uint16_t ChangeMask =
                    UHCI_PORTSC_CSCH | UHCI_PORTSC_EDCH;
                m_pBase->write16(
                    UsbHcd::selectiveW1cValue(
                        portStatus, ChangeMask,
                        static_cast<uint16_t>(UHCI_PORTSC_CSCH)),
                    portRegister);
                (void) m_pBase->read16(portRegister);
            }
        }
        executeRequest(i);
    }

    // Install the timer handler for the periodic port checks. A threadless
    // build cannot safely enumerate a device from timer interrupt context.
#if THREADS
    Timer *timer = Machine::instance().getTimer();
    if (timer)
    {
        m_TimerRegistered = timer->registerHandler(this);
    }
    if (!m_TimerRegistered)
    {
        ERROR("UHCI could not register its root-port polling callback");
    }
#endif
}

void Uhci::enqueueCompletedTransfer(void *context)
{
    QH *pQH = reinterpret_cast<QH *>(context);
    assert(pQH && pQH->pMetaData && pQH->pMetaData->pOwner);
    Uhci *pOwner = pQH->pMetaData->pOwner;
    {
        LockGuard<Mutex> queueGuard(pOwner->m_AsyncQueueListChangeLock);
        pOwner->m_DequeueList.pushBack(pQH);
    }
    pOwner->m_DequeueCount.release();
}

void Uhci::detachQueueHeadLocked(QH *pQH)
{
    assert(pQH && pQH->pMetaData);

    for (List<QH *>::Iterator it = m_AsyncSchedule.begin();
         it != m_AsyncSchedule.end();)
    {
        if (*it == pQH)
        {
            m_AsyncSchedule.erase(it);
            break;
        }
        ++it;
    }

    QH *pPrev = pQH->pMetaData->pPrev;
    QH *pNext = pQH->pMetaData->pNext;
    if (pPrev && pNext)
    {
        pPrev->pMetaData->pNext = pNext;
        pNext->pMetaData->pPrev = pPrev;
        pPrev->pNext = pQH->pNext;
        pPrev->bNextQH = 1;
        pPrev->bNextInvalid = 0;
        if (pQH == m_pCurrentAsyncQueueTail)
            m_pCurrentAsyncQueueTail = pPrev;
    }

    pQH->pMetaData->pPrev = nullptr;
    pQH->pMetaData->pNext = nullptr;
    pQH->pMetaData->bIgnore = true;
}

void Uhci::reclaimQueueHeadLocked(QH *pQH)
{
    assert(pQH && pQH->pMetaData);

    while (pQH->pMetaData->completedTdList.count())
    {
        TD *pTD = pQH->pMetaData->completedTdList.popFront();
        const size_t id = pTD->id;
        ByteSet(pTD, 0, sizeof(TD));
        m_TDBitmap.clear(id);
    }

    while (pQH->pMetaData->tdList.count())
    {
        TD *pTD = pQH->pMetaData->tdList.popFront();
        const size_t id = pTD->id;
        ByteSet(pTD, 0, sizeof(TD));
        m_TDBitmap.clear(id);
    }

    const size_t id = pQH->pMetaData->id;
    delete pQH->pMetaData;
    ByteSet(pQH, 0, sizeof(QH));
    m_QHBitmap.clear(id);
}

Uhci::~Uhci()
{
    List<UsbHcd::CallbackDeliveryQueue::Record *> teardownCompletions;
    List<QH *> periodicRetirements;

    // The timer is the only port-change producer, and unregistration drains
    // any callback already admitted by the timer registry.
    Timer *timer = Machine::instance().getTimer();
    if (m_TimerRegistered)
    {
        if (!timer || !timer->unregisterHandler(this))
        {
            panic(
                "UHCI teardown could not synchronously unregister its timer "
                "callback");
        }
        m_TimerRegistered = false;
    }

    {
        LockGuard<Spinlock> portChangeGuard(m_PortChangeLock);
        m_PortChangesClosing = true;
    }
    for (size_t i = 0; i < m_nPorts; ++i)
    {
        m_PortChanges[i].stopAfterQuiesce();
    }
    RequestQueue::destroy();

    // Port enumeration has drained. No new transaction may now cross the DMA
    // publication boundary, but accepted transfers retain ownership until the
    // dequeue worker has reclaimed their descriptors. Submission callers stay
    // admitted until after this boundary so an in-flight caller either linked
    // before close or observes the closed transfer admission and rolls back.
    m_TransferOperations.close();

    {
        LockGuard<Mutex> transactionGuard(m_Mutex);
        LockGuard<Mutex> irqGuard(m_IrqProcessingLock);
        m_InterruptsClosing = true;
        if (m_pBase)
        {
            m_pBase->write16(0, UHCI_INTR);
            (void) m_pBase->read16(UHCI_INTR);
        }
        setLegacySupportControl(0x8F00);
        // Close admission while the IRQ serialization lock keeps a level
        // interrupt from repeatedly entering the just-closed callback path.
        m_CallbackOperations.close();

        if (m_pBase)
        {
            // Fatal controller errors bypass USBINTR. USBPIRQDEN above keeps
            // them off the shared line while the callback admission is
            // closed and the synchronous DMA halt completes.
            stop();
            const uint16_t pending = m_pBase->read16(UHCI_STS) & 0x1f;
            if (pending)
            {
                m_pBase->write16(pending, UHCI_STS);
                (void) m_pBase->read16(UHCI_STS);
            }
        }

        if (m_pQHList)
        {
            LockGuard<Mutex> queueGuard(m_AsyncQueueListChangeLock);
            while (m_AsyncSchedule.count())
            {
                QH *pQH = m_AsyncSchedule.popFront();
                assert(pQH && pQH->pMetaData);

                if (pQH->pMetaData->bPeriodic)
                {
                    const bool dequeueAdmitted =
                        m_DequeueOperations.tryEnter();
                    assert(dequeueAdmitted);
                    if (dequeueAdmitted)
                        periodicRetirements.pushBack(pQH);
                }
                else
                {
                    UsbHcd::TransferCompletion::Claim claim;
                    const bool claimed =
                        pQH->pMetaData->completion.claimForTeardown(
                            -TransactionError, claim);
                    assert(claimed);
                    if (claimed)
                    {
                        const bool dequeueAdmitted =
                            m_DequeueOperations.tryEnter();
                        assert(dequeueAdmitted);
                        if (dequeueAdmitted)
                        {
                            teardownCompletions.pushBack(
                                m_CompletionDeliveries.create(
                                    {pQH->pMetaData->id, claim.generation},
                                    claim.callback, claim.parameter,
                                    claim.result, enqueueCompletedTransfer,
                                    pQH));
                        }
                    }
                }

                detachQueueHeadLocked(pQH);
            }
        }

        if (teardownCompletions.count())
            m_CompletionDeliveries.publish(teardownCompletions);

        if (m_pBase)
        {
            m_pBase->write32(0, UHCI_FRLP);
            (void) m_pBase->read32(UHCI_FRLP);
        }
    }

    m_SubmissionOperations.close();
    m_SubmissionOperations.wait();

    // These callbacks can release synchronous USB waits owned by a callback
    // which entered before callback admission closed.
    while (teardownCompletions.count())
    {
        auto *completion = teardownCompletions.popFront();
        m_CompletionDeliveries.deliver(completion);
    }

    if (m_IrqId)
    {
        if (!Machine::instance().getIrqManager()->unregisterHandler(
                m_IrqId, static_cast<IrqHandler *>(this)))
        {
            panic(
                "UHCI teardown could not synchronously unregister its IRQ "
                "callback");
        }
        m_IrqId = 0;
    }
    m_CallbackOperations.wait();

    // Teardown and already-admitted IRQ callbacks were allowed to
    // synchronously cancel or drain peer records from the fully-published
    // batch above.
    m_CancelOperations.close();
    m_CancelOperations.wait();

    // No producer remains. This is normally already empty, but draining makes
    // the teardown invariant explicit if a peer callback stole a batch record.
    (void) m_CompletionDeliveries.drainAll();

    // Existing periodic samples are covered by m_CallbackOperations. Retire
    // their QHs only after those samples have returned; the periodic API has
    // no terminal-callback contract and its current consumers ignore errors.
    size_t periodicCount = 0;
    {
        LockGuard<Mutex> queueGuard(m_AsyncQueueListChangeLock);
        while (periodicRetirements.count())
        {
            m_DequeueList.pushBack(periodicRetirements.popFront());
            ++periodicCount;
        }
    }
    while (periodicCount)
    {
        --periodicCount;
        m_DequeueCount.release();
    }

    m_DequeueOperations.close();
    m_DequeueCount.release();
    m_DequeueOperations.wait();
    m_TransferOperations.wait();
    if (m_pDequeueThread)
    {
        if (!m_pDequeueThread->joinForCompletion())
            panic("UHCI teardown could not join its dequeue worker");
        m_pDequeueThread = nullptr;
    }

    assert(m_CompletionDeliveries.empty());
    assert(m_SubmissionOperations.isClosedAndDrained());
    assert(m_CancelOperations.isClosedAndDrained());
    assert(m_CallbackOperations.isClosedAndDrained());
    assert(m_DequeueOperations.isClosedAndDrained());
    assert(m_TransferOperations.isClosedAndDrained());
    {
        LockGuard<Mutex> queueGuard(m_AsyncQueueListChangeLock);
        assert(!m_AsyncSchedule.count());
        assert(!m_DequeueList.count());
    }

    if (m_pQHList)
    {
        LockGuard<Mutex> transactionGuard(m_Mutex);
        constexpr size_t QhListCount = QH_REGION_SIZE / sizeof(QH);
        constexpr size_t TdListCount = TD_REGION_SIZE / sizeof(TD);

        // Transactions built but never accepted were never DMA-owned and do
        // not carry a retained transfer admission.
        for (size_t i = 1; i < QhListCount; ++i)
        {
            if (!m_QHBitmap.test(i))
                continue;
            QH *pQH = &m_pQHList[i];
            assert(pQH->pMetaData);
            assert(
                pQH->pMetaData->completion.state() ==
                UsbHcd::TransferCompletion::State::Idle);
            reclaimQueueHeadLocked(pQH);
        }

        for (size_t i = 1; i < QhListCount; ++i)
            assert(!m_QHBitmap.test(i));
        for (size_t i = 0; i < TdListCount; ++i)
            assert(!m_TDBitmap.test(i));

        QH *pDummyQH = &m_pQHList[0];
        assert(m_QHBitmap.test(0));
        delete pDummyQH->pMetaData;
        ByteSet(pDummyQH, 0, sizeof(QH));
        m_QHBitmap.clear(0);
        assert(!m_QHBitmap.test(0));
        m_pCurrentAsyncQueueHead = nullptr;
        m_pCurrentAsyncQueueTail = nullptr;
    }
}

void Uhci::doDequeue()
{
    while (true)
    {
        if (m_DequeueOperations.isClosedAndDrained())
        {
            return;
        }

        const bool dequeuing = m_DequeueCount.acquireForCompletion();
        (void) dequeuing;

        QH *pQH = 0;
        {
            LockGuard<Mutex> guard(m_AsyncQueueListChangeLock);
            pQH = m_DequeueList.popFront();
        }

        if (!pQH)
            continue;

        {
            LockGuard<Mutex> transactionGuard(m_Mutex);
            reclaimQueueHeadLocked(pQH);
        }
        m_DequeueOperations.leave();
        m_TransferOperations.leave();

#ifdef USB_VERBOSE_DEBUG
        DEBUG_LOG("Dequeue complete.");
#endif
    }
}

IrqDisposition Uhci::irq(irq_id_t number)
{
    (void) number;

    OperationBarrier::Lease callback;
    if (!m_CallbackOperations.tryAcquire(callback))
    {
        return IrqDisposition::Quiesced;
    }
    List<UsbHcd::CallbackDeliveryQueue::Record *> completions;
    {
        LockGuard<Mutex> transactionGuard(m_IrqProcessingLock);

        if (m_InterruptsClosing)
        {
            return IrqDisposition::Quiesced;
    }

    uint16_t nStatus = m_pBase->read16(UHCI_STS);

    if (!nStatus)
    {
            return IrqDisposition::NotHandled;  // Shared IRQ: another device
    }

        // Retire only the captured causes before scanning. A completion which
        // arrives after its QH has been visited then relatches status and
        // causes a fresh threaded pass instead of being erased by a trailing
        // W1C.
    m_pBase->write16(nStatus, UHCI_STS);
        (void) m_pBase->read16(UHCI_STS);

#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("UHCI IRQ " << nStatus);
#endif

    List<QH *> persistList;

    // Because there's no IOC for *every* transfer, we need to handle errors
        // that occur before the last transfer. These will create an error
        // status only.
    if (nStatus & (UHCI_STS_INT | UHCI_STS_ERR))
    {
            constexpr size_t QhListCount = QH_REGION_SIZE / sizeof(QH);
            constexpr size_t TdListCount = TD_REGION_SIZE / sizeof(TD);
            size_t qhBudget = QhListCount;
        QH *pQH = 0;
            while (qhBudget)
        {
                --qhBudget;
            {
                    LockGuard<Mutex> guard(m_AsyncQueueListChangeLock);
                if (m_AsyncSchedule.count())
                    pQH = m_AsyncSchedule.popFront();
                else
                    break;
            }

            {
                bool bPeriodic = pQH->pMetaData->bPeriodic;

                // Iterate the TD list
                TD *pTD = 0;
                    size_t tdBudget = TdListCount;
                    while (pQH->pMetaData->tdList.count() && tdBudget)
                {
                        --tdBudget;
                    pTD = pQH->pMetaData->tdList.popFront();

                    // If we've already detected that this TD was a short
                    // transfer, don't process any more TDs
                    if (pTD->bShortTransferTD)
                        break;

                    bool bEndOfTransfer = false;
                    if (pTD->nStatus == 0x80)
                    {
                        pQH->pMetaData->tdList.pushFront(pTD);
                        break;
                    }

                    ssize_t nResult;
                    if (((pTD->nErr == 0) && (pTD->nStatus & 0x7e)) ||
                        (nStatus & UHCI_STS_ERR))
                    {
                        // #ifdef USB_VERBOSE_DEBUG
                        ERROR_NOLOCK(
                            ((nStatus & UHCI_STS_ERR) ? "USB" : "TD")
                            << " ERROR!");
                        ERROR_NOLOCK(
                                "TD Status: " << pTD->nStatus << " ["
                                              << pTD->nErr
                                          << "], USB status: " << nStatus);
                        // #endif
                        nResult = -pTD->getError();
                    }
                    else
                    {
                        nResult = (pTD->nActLen + 1) % 0x800;
                        pQH->pMetaData->nTotalBytes += nResult;
                    }
#ifdef USB_VERBOSE_DEBUG
                    DEBUG_LOG_NOLOCK(
                        "TD #"
                        << Dec << pTD->id << " [QH #" << pQH->pMetaData->id
                        << Hex << "] DONE: " << Dec << pTD->nAddress << ":"
                        << pTD->nEndpoint << " "
                        << (pTD->nPid == UsbPidOut ?
                                "OUT" :
                                (pTD->nPid == UsbPidIn ?
                                     "IN" :
                                         (pTD->nPid == UsbPidSetup ? "SETUP" :
                                                                     "")))
                        << " " << nResult << Hex);
#endif

                    // Handle the "end of transfer" cases
                        bEndOfTransfer = (!bPeriodic &&
                                          ((nResult < 0) ||
                                           (pTD == pQH->pMetaData->pLastTD))) ||
                        (bPeriodic && (nResult >= 0));

                    // Some extra cases we need to handle
                    if ((pTD != pQH->pMetaData->pLastTD) && !bEndOfTransfer)
                    {
                        // Control endpoints are irrelevant here
                        if (pTD->nEndpoint)
                        {
                            if ((nResult >= 0) && (pTD->nPid == UsbPidIn))
                            {
                                // Check for a short read. There is no point
                                    // continuing to read from the device if
                                    // it's sent us less than we asked for
                                    // before the last TD. This stops the
                                    // transfer hanging on IN transactions that
                                    // return less data than expected.
                                if (nResult < pTD->nBufferSize)
                                {
                                    DEBUG_LOG_NOLOCK(
                                        "UHCI: Short read - got "
                                        << nResult << " bytes, wanted "
                                        << pTD->nBufferSize << " bytes");
                                    pTD->bShortTransferTD = true;
                                    bEndOfTransfer = true;
                                }
                            }
                        }
                    }

                    if (!bPeriodic)
                        pQH->pMetaData->completedTdList.pushBack(pTD);

                        // Last TD or error condition, if async, otherwise only
                        // when it gives no error
                    if (bEndOfTransfer)
                    {
                        const ssize_t completionResult =
                            nResult < 0 ? nResult :
                                          pQH->pMetaData->nTotalBytes;

                        if (!bPeriodic)
                        {
                            const bool captured =
                                pQH->pMetaData->completion.captureNatural(
                                    completionResult);
                            assert(captured);
                            if (captured)
                            {
                                // The completion lock excludes cancellation
                                // while this establishes the DMA boundary.
                                stop();

                                UsbHcd::TransferCompletion::Claim claim;
                                const bool claimed =
                                    pQH->pMetaData->completion.claimCaptured(
                                        claim);
                                const bool dequeueAdmitted =
                                    m_DequeueOperations.tryEnter();
                                assert(claimed && dequeueAdmitted);
                                if (claimed && dequeueAdmitted)
                                {
                                    {
                                        LockGuard<Mutex> queueGuard(
                                            m_AsyncQueueListChangeLock);
                                        detachQueueHeadLocked(pQH);
                                    }

                                    completions.pushBack(
                                        m_CompletionDeliveries.create(
                                            {pQH->pMetaData->id,
                                             claim.generation},
                                            claim.callback, claim.parameter,
                                            claim.result,
                                            enqueueCompletedTransfer, pQH));
                                }

                                start();
                            }
                        }
                        else
                        {
                            pTD->bDataToggle = !pTD->bDataToggle;
                            pQH->pMetaData->nTotalBytes = 0;

                            if (pQH->pMetaData->pPeriodicCallback)
                            {
                                completions.pushBack(
                                    m_CompletionDeliveries.create(
                                        {pQH->pMetaData->id,
                                         m_CompletionDeliveries
                                             .nextGeneration()},
                                        pQH->pMetaData->pPeriodicCallback,
                                        pQH->pMetaData->pPeriodicParam,
                                        completionResult));
                            }
                        }
                    }

                    // Interrupt TDs need to be always active
                    if (bPeriodic)
                    {
                        pQH->pMetaData->bIgnore = false;
                        pTD->nStatus = 0x80;
                        pTD->nActLen = 0;

                        // Modified by the host controller
                        pQH->pElem = PHYS_TD(pTD->id) >> 4;
                        pQH->bElemInvalid = 0;
                        pQH->bElemQH = 0;

                        pQH->pMetaData->tdList.pushBack(pTD);
                        break;  // Periodic QHs should only have one TD
                    }
                }

                    if (!tdBudget && pQH->pMetaData->tdList.count())
                    {
                        ERROR_NOLOCK(
                            "UHCI: QH #" << Dec << pQH->pMetaData->id << Hex
                                         << " exceeded the TD scan budget");
                    }
            }

            if (!pQH->pMetaData->bIgnore)
                persistList.pushBack(pQH);
            }

            {
                LockGuard<Mutex> guard(m_AsyncQueueListChangeLock);
                if (m_AsyncSchedule.count())
                {
                    ERROR_NOLOCK("UHCI: exceeded the QH scan budget");
                }
            }
    }

    if (persistList.count())
    {
            LockGuard<Mutex> guard(m_AsyncQueueListChangeLock);
        for (List<QH *>::Iterator it = persistList.begin();
             it != persistList.end();)
        {
            m_AsyncSchedule.pushBack(*it);
            it = persistList.erase(it);
        }
    }

        if (completions.count())
            m_CompletionDeliveries.publish(completions);
    }

    while (completions.count())
    {
        auto *completion = completions.popFront();
        m_CompletionDeliveries.deliver(completion);
    }

    return IrqDisposition::Handled;
}

void Uhci::addTransferToTransaction(
    uintptr_t pTransaction, bool bToggle, UsbPid pid, uintptr_t pBuffer,
    size_t nBytes)
{
    OperationBarrier::Lease submission;
    if (!m_SubmissionOperations.tryAcquire(submission))
        return;

    LockGuard<Mutex> transactionGuard(m_Mutex);
    size_t nIndex = 0;
    constexpr size_t QhListCount = QH_REGION_SIZE / sizeof(QH);
    if ((pTransaction == static_cast<uintptr_t>(-1)) ||
        (pTransaction >= QhListCount) || !m_pQHList ||
        !m_QHBitmap.test(pTransaction) ||
        !m_pQHList[pTransaction].pMetaData)
    {
        ERROR("USB: UHCI: invalid transaction for transfer");
        return;
    }
    QH *pQH = &m_pQHList[pTransaction];
    if (
        pQH->pMetaData->pPrev || pQH->pMetaData->pNext ||
        (!pQH->pMetaData->bPeriodic &&
         pQH->pMetaData->completion.state() !=
             UsbHcd::TransferCompletion::State::Idle))
    {
        ERROR("USB: UHCI: cannot extend an accepted transaction");
        return;
    }
    nIndex = m_TDBitmap.getFirstClear();
    if (nIndex >= (TD_REGION_SIZE / sizeof(TD)))
    {
        ERROR("USB: UHCI: TD space full");
        return;
    }
    m_TDBitmap.set(nIndex);

    // Grab the TD
    TD *pTD = &m_pTDList[nIndex];
    ByteSet(pTD, 0, sizeof(TD));
    pTD->bNextInvalid =
        1;  // Assume next is invalid, will be zeroed if another TD is linked
    pTD->id = nIndex;

    // Active, and only allow one retry
    pTD->nStatus = 0x80;
    pTD->bIoc = 0;  // Don't issue an interrupt on completion until the very
                    // last TD in the transaction.
    pTD->nErr = 3;

    // PID for this transfer
    pTD->nPid = pid;

    // Speed information
    pTD->bLoSpeed = pQH->pMetaData->endpointInfo.speed == LowSpeed;

    // Don't care about short packet detection
    pTD->bSpd = 0;

    // Endpoint information
    pTD->nAddress = pQH->pMetaData->endpointInfo.nAddress;
    pTD->nEndpoint = pQH->pMetaData->endpointInfo.nEndpoint;
    pTD->bDataToggle = bToggle;

    // Transfer information
    pTD->nMaxLen = nBytes ? nBytes - 1 : 0x7ff;
    pTD->nBufferSize = nBytes;
    if (nBytes)
    {
        VirtualAddressSpace &va =
            Processor::information().getVirtualAddressSpace();
        if (va.isMapped(reinterpret_cast<void *>(pBuffer)))
        {
            physical_uintptr_t phys = 0;
            size_t flags = 0;
            va.getMapping(reinterpret_cast<void *>(pBuffer), phys, flags);
            pTD->pBuff = phys + (pBuffer & 0xFFF);
        }
        else
        {
            ERROR(
                "UHCI: addTransferToTransaction: Buffer (page "
                << Dec << pBuffer << Hex << ") isn't mapped!");
            m_TDBitmap.clear(nIndex);
            return;
        }
    }

    // Link into the existing TD list
    if (pQH->pMetaData->pLastTD)
    {
        pQH->pMetaData->pLastTD->pNext = PHYS_TD(nIndex) >> 4;
        pQH->pMetaData->pLastTD->bNextInvalid = 0;
        pQH->pMetaData->pLastTD->bNextQH = 0;
        // pQH->pMetaData->pLastTD->bNextDepth = 1;
    }
    else
    {
        pQH->pMetaData->pFirstTD = pTD;
        pQH->pElem = PHYS_TD(nIndex) >> 4;
        pQH->bElemInvalid = 0;
        pQH->bElemQH = 0;
    }
    pQH->pMetaData->pLastTD = pTD;

    pQH->pMetaData->tdList.pushBack(pTD);
}

uintptr_t Uhci::createTransaction(UsbEndpoint endpointInfo)
{
    OperationBarrier::Lease submission;
    if (!m_SubmissionOperations.tryAcquire(submission))
        return static_cast<uintptr_t>(-1);

    LockGuard<Mutex> transactionGuard(m_Mutex);
    size_t nIndex = 0;
    if (!m_pQHList)
        return static_cast<uintptr_t>(-1);
    nIndex = m_QHBitmap.getFirstClear();
    if (nIndex >= (QH_REGION_SIZE / sizeof(QH)))
    {
        ERROR("USB: UHCI: QH space full");
        return static_cast<uintptr_t>(-1);
    }
    m_QHBitmap.set(nIndex);

    // Grab the QH
    QH *pQH = &m_pQHList[nIndex];
    ByteSet(pQH, 0, sizeof(QH));

    // Only need to configure metadata, everything else is set during linkage
    // and TD creation
    pQH->pMetaData = new QH::MetaData;
    pQH->pMetaData->pOwner = this;
    pQH->pMetaData->pPeriodicCallback = nullptr;
    pQH->pMetaData->pPeriodicParam = 0;
    pQH->pMetaData->endpointInfo = endpointInfo;
    pQH->pMetaData->bPeriodic = false;
    pQH->pMetaData->pFirstTD = pQH->pMetaData->pLastTD = 0;
    pQH->pMetaData->nTotalBytes = 0;
    pQH->pMetaData->pPrev = pQH->pMetaData->pNext = 0;
    pQH->pMetaData->bIgnore = false;
    pQH->pMetaData->id = nIndex;

    return nIndex;
}

bool Uhci::doAsync(
    uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t),
    uintptr_t pParam)
{
    OperationBarrier::Lease submission;
    if (!m_SubmissionOperations.tryAcquire(submission))
        return false;

    LockGuard<Mutex> transactionGuard(m_Mutex);
    constexpr size_t QhListCount = QH_REGION_SIZE / sizeof(QH);
    if ((pTransaction == static_cast<uintptr_t>(-1)) ||
        (pTransaction >= QhListCount) ||
        !m_QHBitmap.test(pTransaction))
    {
        ERROR(
            "UHCI: doAsync: didn't get a valid transaction id ["
            << pTransaction << "].");
        return false;
    }

    QH *pQH = &m_pQHList[pTransaction];
    if (!pQH->pMetaData)
    {
        ByteSet(pQH, 0, sizeof(QH));
        m_QHBitmap.clear(pTransaction);
        return false;
    }
    if (!pQH->pMetaData->pLastTD)
    {
        ERROR(
            "UHCI: doAsync: transaction has no transfers [" << pTransaction
                                                            << "].");
        reclaimQueueHeadLocked(pQH);
        return false;
    }
    if (
        pQH->pMetaData->pPrev || pQH->pMetaData->pNext ||
        (!pQH->pMetaData->bPeriodic &&
         pQH->pMetaData->completion.state() !=
             UsbHcd::TransferCompletion::State::Idle))
    {
        ERROR(
            "UHCI: doAsync: transaction is already owned [" << pTransaction
                                                             << "].");
        return false;
    }

    if (!m_TransferOperations.tryEnter())
    {
        reclaimQueueHeadLocked(pQH);
        return false;
    }

    LockGuard<Mutex> irqGuard(m_IrqProcessingLock);
    if (m_InterruptsClosing || !m_pCurrentAsyncQueueTail)
    {
        if (!m_pCurrentAsyncQueueTail)
            ERROR("UHCI: Queue tail is null!");
        reclaimQueueHeadLocked(pQH);
        m_TransferOperations.leave();
        return false;
    }

    // m_IrqProcessingLock is the controller-wide DMA publication lock.
    stop();
    {
        LockGuard<Mutex> queueGuard(m_AsyncQueueListChangeLock);

        const size_t queueHeadIndex =
            (reinterpret_cast<uintptr_t>(m_pCurrentAsyncQueueHead) -
             reinterpret_cast<uintptr_t>(m_pQHList)) /
            sizeof(QH);
        pQH->pNext =
            (m_pQHListPhys + (queueHeadIndex * sizeof(QH))) >> 4;
        pQH->bNextInvalid = 0;
        pQH->bNextQH = 1;
        pQH->pMetaData->bIgnore = true;

        QH *pOldTail = m_pCurrentAsyncQueueTail;
        m_pCurrentAsyncQueueTail = pQH;
        pOldTail->pNext =
            (m_pQHListPhys + (pTransaction * sizeof(QH))) >> 4;
        pOldTail->bNextInvalid = 0;
        pOldTail->bNextQH = 1;
        pOldTail->pMetaData->pNext = pQH;

        pQH->pMetaData->pNext = m_pCurrentAsyncQueueHead;
        pQH->pMetaData->pPrev = pOldTail;
        m_pCurrentAsyncQueueHead->pMetaData->pPrev = pQH;
        m_AsyncSchedule.pushBack(pQH);

        pQH->pMetaData->pLastTD->bIoc = 1;
        if (pQH->pMetaData->bPeriodic)
        {
            pQH->pMetaData->pPeriodicCallback = pCallback;
            pQH->pMetaData->pPeriodicParam = pParam;
        }
        else
        {
            pQH->pMetaData->completion.arm(
                pCallback, pParam, m_CompletionDeliveries.nextGeneration());
        }

        // Active is published only after both linked-list representations and
        // the callback obligation are complete.
        pQH->pMetaData->bIgnore = false;
    }

    start();
    return true;
}

void Uhci::cancelAsyncAndDrain(
    uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t),
    uintptr_t pParam)
{
    OperationBarrier::Lease cancellation;
    if (!m_CancelOperations.tryAcquire(cancellation))
        return;

    List<UsbHcd::CallbackDeliveryQueue::Record *> completions;
    bool drainDelivery = false;
    UsbHcd::CallbackDeliveryQueue::Key deliveryKey = {0, 0};

    {
        LockGuard<Mutex> transactionGuard(m_Mutex);
        constexpr size_t QhListCount = QH_REGION_SIZE / sizeof(QH);
        if ((pTransaction != static_cast<uintptr_t>(-1)) &&
            (pTransaction < QhListCount) &&
            m_QHBitmap.test(pTransaction))
        {
            QH *pQH = &m_pQHList[pTransaction];
            if (pQH->pMetaData && !pQH->pMetaData->bPeriodic)
            {
                LockGuard<Mutex> irqGuard(m_IrqProcessingLock);
                UsbHcd::TransferCompletion::Claim claim;
                const auto disposition =
                    pQH->pMetaData->completion.claimCancellation(
                        pCallback, pParam, -TransactionError, claim);

                if (
                    disposition == UsbHcd::TransferCompletion::
                                       CancellationDisposition::Claimed)
                {
                    const uint16_t interruptMask =
                        m_pBase->read16(UHCI_INTR);
                    m_pBase->write16(0, UHCI_INTR);
                    (void) m_pBase->read16(UHCI_INTR);
                    stop();

                    const bool dequeueAdmitted =
                        m_DequeueOperations.tryEnter();
                    assert(dequeueAdmitted);
                    if (dequeueAdmitted)
                    {
                        {
                            LockGuard<Mutex> queueGuard(
                                m_AsyncQueueListChangeLock);
                            detachQueueHeadLocked(pQH);
                        }

                        completions.pushBack(
                            m_CompletionDeliveries.create(
                                {pQH->pMetaData->id, claim.generation},
                                claim.callback, claim.parameter, claim.result,
                                enqueueCompletedTransfer, pQH));
                        m_CompletionDeliveries.publish(completions);
                    }

                    if (!m_InterruptsClosing)
                    {
                        start();
                        m_pBase->write16(interruptMask, UHCI_INTR);
                    }
                    else
                    {
                        m_pBase->write16(0, UHCI_INTR);
                    }
                    (void) m_pBase->read16(UHCI_INTR);
                }
                else if (
                    disposition ==
                    UsbHcd::TransferCompletion::CancellationDisposition::
                        DrainPublished)
                {
                    deliveryKey = {pTransaction, claim.generation};
                    drainDelivery = true;
                }
            }
        }
    }

    while (completions.count())
    {
        auto *completion = completions.popFront();
        m_CompletionDeliveries.deliver(completion);
    }

    if (drainDelivery)
        (void) m_CompletionDeliveries.drain(deliveryKey);
}

void Uhci::addInterruptInHandler(
    UsbEndpoint endpointInfo, uintptr_t pBuffer, uint16_t nBytes,
    void (*pCallback)(uintptr_t, ssize_t), uintptr_t pParam)
{
    OperationBarrier::Lease submission;
    if (!m_SubmissionOperations.tryAcquire(submission))
        return;

    // Create a new transaction
    uintptr_t nTransaction = createTransaction(endpointInfo);
    if (nTransaction == static_cast<uintptr_t>(-1))
    {
        ERROR("USB: UHCI: Couldn't create interrupt transaction!");
        return;
    }

    // Get the QH and set the periodic flag
    QH *pQH = &m_pQHList[nTransaction];
    pQH->pMetaData->bPeriodic = true;

    // Add a single transfer to the transaction
    addTransferToTransaction(nTransaction, false, UsbPidIn, pBuffer, nBytes);
    if (!pQH->pMetaData->pLastTD)
    {
        ERROR("USB: UHCI: Couldn't add transfer to transaction!");
        LockGuard<Mutex> transactionGuard(m_Mutex);
        if (m_QHBitmap.test(nTransaction) && pQH->pMetaData)
            reclaimQueueHeadLocked(pQH);
        return;
    }

    // Get the TD and set the error counter to "unlimited retries"
    TD *pTD = pQH->pMetaData->pLastTD;
    pTD->nErr = 0;

    // Let doAsync do the rest
    if (!doAsync(nTransaction, pCallback, pParam))
    {
        ERROR("USB: UHCI: Couldn't submit interrupt transaction!");
        LockGuard<Mutex> transactionGuard(m_Mutex);
        if (m_QHBitmap.test(nTransaction) && pQH->pMetaData)
            reclaimQueueHeadLocked(pQH);
    }
}

void Uhci::modifyPortControl(
    size_t portRegister, uint16_t clearMask, uint16_t setMask)
{
    constexpr uint16_t ChangeMask = UHCI_PORTSC_CSCH | UHCI_PORTSC_EDCH;
    LockGuard<Spinlock> portChangeGuard(m_PortChangeLock);
    uint16_t portControl = UsbHcd::selectiveW1cValue(
        m_pBase->read16(portRegister), ChangeMask, static_cast<uint16_t>(0));
    portControl &= ~clearMask;
    portControl |= setMask & ~ChangeMask;
    m_pBase->write16(portControl, portRegister);
}

bool Uhci::portReset(uint8_t nPort, bool bErrorResponse)
{
#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("USB: UHCI: Reset on port " << nPort);
#endif

    const size_t portRegister = UHCI_PORTSC + (nPort * 2);
    constexpr uint16_t ChangeMask = UHCI_PORTSC_CSCH | UHCI_PORTSC_EDCH;

    if (bErrorResponse)
    {
        // Before port reset, disable the port
        modifyPortControl(portRegister, UHCI_PORTSC_ENABLE, 0);
        constexpr size_t PortDisablePollLimit = 100;
        size_t disablePolls = PortDisablePollLimit;
        while (
            disablePolls-- &&
            (m_pBase->read16(portRegister) & UHCI_PORTSC_ENABLE))
        {
            Time::delay(1 * Time::Multiplier::Millisecond);
        }
        if (m_pBase->read16(portRegister) & UHCI_PORTSC_ENABLE)
        {
            ERROR(
                "USB: UHCI: Port " << Dec << nPort << Hex
                                    << " did not disable within 100 ms");
            return false;
        }
    }

    // Perform a reset of the port
    modifyPortControl(portRegister, 0, UHCI_PORTSC_PRES);
    Time::delay(50 * Time::Multiplier::Millisecond);
    modifyPortControl(portRegister, UHCI_PORTSC_PRES, 0);

    // Enable the port
    modifyPortControl(portRegister, 0, UHCI_PORTSC_ENABLE);
    Time::delay((bErrorResponse ? 500 : 100) * Time::Multiplier::Millisecond);

    // Check that the device is completely enabled
    if (!(m_pBase->read16(portRegister) & UHCI_PORTSC_ENABLE))
    {
        //#ifdef USB_VERBOSE_DEBUG
        DEBUG_LOG(
            "USB: UHCI: During reset, port "
            << nPort << " could not be enabled. It may become enabled soon.");
        //#endif
        return false;
    }

    // Retire the enable-change generated by reset. Leave CSC asserted for the
    // timer so a disconnect/reconnect during reset cannot be erased here.
    {
        LockGuard<Spinlock> portChangeGuard(m_PortChangeLock);
        const uint16_t portStatus = m_pBase->read16(portRegister);
        if (portStatus & UHCI_PORTSC_EDCH)
        {
            m_pBase->write16(
                UsbHcd::selectiveW1cValue(
                    portStatus, ChangeMask,
                    static_cast<uint16_t>(UHCI_PORTSC_EDCH)),
                portRegister);
            (void) m_pBase->read16(portRegister);
        }
    }

    // Verify that we have a device connected here
    if (!(m_pBase->read16(portRegister) & UHCI_PORTSC_CONN))
    {
        //#ifdef USB_VERBOSE_DEBUG
        DEBUG_LOG(
            "USB: UHCI: During reset, port "
            << nPort
            << " was enabled but had no device on it. A device may be detected "
               "shortly.");
        //#endif
        return false;
    }

#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("USB: Post-reset status is " << m_pBase->read16(portRegister));
#endif

    return true;
}

uint64_t Uhci::executeRequest(
    uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
    uint64_t p6, uint64_t p7, uint64_t p8)
{
    if (p1 >= m_nPorts)
    {
        return 0;
    }
    UsbHcd::PortChangeRequest::Completion completion(
        m_PortChanges[p1], static_cast<size_t>(p8));
    if (!completion)
    {
        return 0;
    }

    const uint16_t portStatus = m_pBase->read16(UHCI_PORTSC + (p1 * 2));

    // Check for a connected device
    if (portStatus & UHCI_PORTSC_CONN)
    {
        // Determine the speed of the attached device
        if (portStatus & UHCI_PORTSC_LOSPEED)
        {
            DEBUG_LOG(
                "USB: UHCI [" << this << "]: Port " << Dec << p1 << Hex
                              << " has a low-speed device connected to it");
            if (!deviceConnected(p1, LowSpeed))
                WARNING(
                    "USB: UHCI ["
                    << this << "]: Port " << Dec << p1 << Hex
                    << " appeared to be connected but could not be set up");
        }
        else
        {
            DEBUG_LOG(
                "USB: UHCI [" << this << "]: Port " << Dec << p1 << Hex
                              << " has a full-speed device connected to it");
            if (!deviceConnected(p1, FullSpeed))
                WARNING(
                    "USB UHCI ["
                    << this << "]: Port " << Dec << p1 << Hex
                    << " appeared to be connected but could not be set up");
        }
    }
    else
    {
        DEBUG_LOG(
            "USB: UHCI [" << this << "]: Device on port " << Dec << p1 << Hex
                          << " disconnected.");
        deviceDisconnected(p1);
    }
    return 0;
}

void Uhci::cancelRequest(const Request &request)
{
    if (request.p1 < m_nPorts)
    {
        m_PortChanges[request.p1].cancel(request.p8);
    }
}

void Uhci::timer(uint64_t delta)
{
#if !THREADS
    (void) delta;
    return;
#else
    OperationBarrier::Lease callback;
    if (!m_CallbackOperations.tryAcquire(callback))
    {
        return;
    }

    {
        LockGuard<Spinlock> portChangeGuard(m_PortChangeLock);
        if (m_PortChangesClosing)
        {
            return;
        }

        m_nPortCheckTicks += delta;
        if (m_nPortCheckTicks < 1000000)
        {
            return;
        }

        // We check the ports once in a Millisecond.
        m_nPortCheckTicks = 0;

        // Check every port for a change
        for (size_t i = 0; i < m_nPorts; i++)
        {
            const size_t portRegister = UHCI_PORTSC + (i * 2);
            const uint16_t portStatus = m_pBase->read16(portRegister);
            constexpr uint16_t ChangeMask = UHCI_PORTSC_CSCH | UHCI_PORTSC_EDCH;
            uint16_t acknowledgeMask = portStatus & UHCI_PORTSC_EDCH;
            size_t acknowledgeGeneration = 0;

            if (portStatus & UHCI_PORTSC_CSCH)
            {
                if (deferConnectionChangeIfSuppressed(i))
                {
                    acknowledgeMask |= UHCI_PORTSC_CSCH;
                }
                else
                {
                    const auto observation = m_PortChanges[i].observe();
                    if (UsbHcd::PortChangeRequest::canAcknowledge(
                            observation.result))
                    {
                        acknowledgeMask |= UHCI_PORTSC_CSCH;
                        acknowledgeGeneration = observation.generation;
                    }
                }
            }

            if (acknowledgeMask)
            {
                m_pBase->write16(
                    UsbHcd::selectiveW1cValue(
                        portStatus, ChangeMask, acknowledgeMask),
                    portRegister);
                (void) m_pBase->read16(portRegister);
            }
            if (acknowledgeGeneration)
            {
                m_PortChanges[i].acknowledge(acknowledgeGeneration);
            }
        }
    }
#endif
}

void Uhci::replaySuppressedConnectionChange(size_t port)
{
#if THREADS
    if (port >= m_nPorts)
    {
        ERROR("UHCI: invalid suppressed root-port replay " << Dec << port);
        return;
    }

    LockGuard<Spinlock> portChangeGuard(m_PortChangeLock);
    // Teardown stops publication before its active enumeration worker returns.
    if (m_PortChangesClosing)
    {
        return;
    }
    const auto observation = m_PortChanges[port].observe();
    const bool accepted =
        UsbHcd::PortChangeRequest::canAcknowledge(observation.result);
    if (accepted)
    {
        m_PortChanges[port].acknowledge(observation.generation);
        return;
    }

    m_PortChangesClosing = true;
    ERROR("UHCI: live suppressed root-port replay could not be published");
    assert(false);
#else
    (void) port;
#endif
}

void Uhci::stop()
{
    m_pBase->write16(m_pBase->read16(UHCI_CMD) & ~1, UHCI_CMD);
    constexpr size_t TransitionPollLimit = 100;
    size_t polls = TransitionPollLimit;
    while (polls-- && !(m_pBase->read16(UHCI_STS) & UHCI_STS_HALT))
        Time::delay(1 * Time::Multiplier::Millisecond);
    if (!(m_pBase->read16(UHCI_STS) & UHCI_STS_HALT))
        panic("UHCI controller did not halt within 100 ms");
}

void Uhci::start()
{
    m_pBase->write16(m_pBase->read16(UHCI_CMD) | 1, UHCI_CMD);
    constexpr size_t TransitionPollLimit = 100;
    size_t polls = TransitionPollLimit;
    while (polls-- && (m_pBase->read16(UHCI_STS) & UHCI_STS_HALT))
        Time::delay(1 * Time::Multiplier::Millisecond);
    if (m_pBase->read16(UHCI_STS) & UHCI_STS_HALT)
        panic("UHCI controller did not start within 100 ms");
}

void Uhci::setLegacySupportControl(uint16_t control)
{
    constexpr size_t LegacySupportDword = 0xC0 / 4;
    const uint32_t legacy =
        PciBus::instance().readConfigSpace(this, LegacySupportDword);
    PciBus::instance().writeConfigSpace(
        this, LegacySupportDword,
        (legacy & static_cast<uint32_t>(0xFFFF0000)) | control);
    (void) PciBus::instance().readConfigSpace(this, LegacySupportDword);
}

#endif
