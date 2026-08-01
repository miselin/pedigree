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

#include "Ohci.h"
#include "modules/system/usb/Usb.h"
#include "modules/system/usb/UsbHub.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/InterruptHandler.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#define INDEX_FROM_TD(ptr) \
    (((reinterpret_cast<uintptr_t>((ptr)) & 0xFFF) / sizeof(TD)))
#define PHYS_TD(idx) (m_pTDListPhys + ((idx) * sizeof(TD)))

Ohci::Ohci(Device *pDev)
    : UsbHub(pDev), RequestQueue(MakeConstantString("OHCI")), m_pBase(0),
      m_nPorts(0), m_Initialised(false), m_Mutex(), m_PortResetMutex(),
      m_IrqProcessingLock(), m_RootHubLock(),
      m_RootHubStatusChangeDesired(false), m_PortResetActive(false),
      m_TeardownPhase(0), m_ScheduleChangeLock(),
      m_PeriodicListChangeLock(), m_ControlListChangeLock(),
      m_BulkListChangeLock(), m_PeriodicEDBitmap(), m_ControlEDBitmap(),
      m_BulkEDBitmap(), m_pBulkQueueHead(0), m_pControlQueueHead(0),
      m_pBulkQueueTail(0), m_pControlQueueTail(0), m_pPeriodicQueueTail(0),
      m_DequeueListLock(), m_DequeueList(), m_DequeueCount(0),
      m_OhciMR("Ohci-MR"), m_CallbackOperations(), m_IrqId(0)
{
    setSpecificType(String("OHCI"));

#if !X86_COMMON
    // InterruptManager cannot synchronously unregister a raw handler. Refuse
    // publication until that platform can provide the same lifetime barrier
    // as IrqManager.
    ERROR("OHCI requires synchronous IRQ unregistration on this platform");
    return;
#endif

    // Allocate the memory region
    if (!PhysicalMemoryManager::instance().allocateRegion(
            m_OhciMR, 5, PhysicalMemoryManager::continuous,
            VirtualAddressSpace::Write | VirtualAddressSpace::KernelMode))
    {
        ERROR("USB: OHCI: Couldn't allocate memory region!");
        return;
    }

    uintptr_t virtualBase =
        reinterpret_cast<uintptr_t>(m_OhciMR.virtualAddress());
    uintptr_t physicalBase = m_OhciMR.physicalAddress();

    m_pHcca = reinterpret_cast<Hcca *>(virtualBase);
    m_pBulkEDList = reinterpret_cast<ED *>(virtualBase + 0x1000);
    m_pControlEDList = reinterpret_cast<ED *>(virtualBase + 0x2000);
    m_pPeriodicEDList = reinterpret_cast<ED *>(virtualBase + 0x3000);
    m_pTDList = reinterpret_cast<TD *>(virtualBase + 0x4000);

    m_pHccaPhys = physicalBase;
    m_pBulkEDListPhys = physicalBase + 0x1000;
    m_pControlEDListPhys = physicalBase + 0x2000;
    m_pPeriodicEDListPhys = physicalBase + 0x3000;
    m_pTDListPhys = physicalBase + 0x4000;

    // Clear out the HCCA block.
    ByteSet(m_pHcca, 0, 0x800);

    // Get an ED for the periodic list
    m_PeriodicEDBitmap.set(0);
    ED *pPeriodicED = m_pPeriodicEDList;
    ByteSet(pPeriodicED, 0, sizeof(ED));
    pPeriodicED->bSkip = true;
    pPeriodicED->pMetaData = new ED::MetaData;
    pPeriodicED->pMetaData->id = 0x2000;
    pPeriodicED->pMetaData->pPrev = pPeriodicED->pMetaData->pNext = pPeriodicED;

    // Set all HCCA interrupt ED entries to our periodic ED
    DoubleWordSet(m_pHcca->pInterruptEDList, m_pPeriodicEDListPhys, 3);

    // Every periodic ED will be added after this one
    m_pPeriodicQueueTail = pPeriodicED;

    m_pBulkQueueTail = m_pBulkQueueHead = 0;
    m_pControlQueueTail = m_pControlQueueHead = 0;

#if X86_COMMON
    // Make sure bus mastering and MMIO are enabled.
    uint32_t nPciCmdSts = PciBus::instance().readConfigSpace(this, 1);
    PciBus::instance().writeConfigSpace(this, 1, nPciCmdSts | 0x6);
#endif

    // Grab the ports
    m_pBase = m_Addresses[0]->m_Io;
    m_Addresses[0]->map();

    // Dump the version of the controller and a nice little banner.
    uint8_t version = m_pBase->read32(OhciVersion) & 0xFF;
    DEBUG_LOG(
        "USB: OHCI: starting up - controller is version "
        << Dec << ((version & 0xF0) >> 4) << "." << (version & 0xF) << Hex
        << ".");

    // Do not let a firmware-programmed source reach the PCI line while the
    // controller is being taken over and reset.
    m_pBase->write32(OhciInterruptAll, OhciInterruptDisable);
    (void) m_pBase->read32(OhciInterruptEnable);

    // Determine first of all if the HC is controlled by the BIOS.
    uint32_t control = m_pBase->read32(OhciControl);
    if (control & OhciControlInterruptRoute)
    {
        // SMM.
        DEBUG_LOG("USB: OHCI: currently in SMM!");
        uint32_t status = m_pBase->read32(OhciCommandStatus);
        m_pBase->write32(
            status | OhciCommandRequestOwnership, OhciCommandStatus);
        while ((control = m_pBase->read32(OhciControl)) &
               OhciControlInterruptRoute)
            Time::delay(1 * Time::Multiplier::Millisecond);
    }
    else
    {
        // Chances are good that the BIOS has the thing running.
        if (control & OhciControlStateFunctionalMask)
            DEBUG_LOG("USB: OHCI: BIOS is currently in charge.");
        else
            DEBUG_LOG("USB: OHCI: not yet operational.");

        // Throw the controller into operational mode if it isn't.
        if (!(control & OhciControlStateRunning))
            m_pBase->write32(OhciControlStateRunning, OhciControl);
    }

    // Perform a reset via the UHCI Control register.
    m_pBase->write32(control & ~OhciControlStateFunctionalMask, OhciControl);
    Time::delay(200 * Time::Multiplier::Millisecond);

    // Grab the FM Interval register (5.1.1.4, OHCI spec).
    uint32_t interval = m_pBase->read32(OhciFmInterval);

    // Perform a full hardware reset.
    m_pBase->write32(OhciCommandHcReset, OhciCommandStatus);
    while (m_pBase->read32(OhciCommandStatus) & OhciCommandHcReset)
        Time::delay(5 * Time::Multiplier::Millisecond);

    // We now have 2 ms to complete all operations before we start the
    // controller. 5.1.1.4, OHCI spec.

    // Set up the HCCA block.
    m_pBase->write32(m_pHccaPhys, OhciHcca);

    // Set up the operational registers.
    m_pBase->write32(m_pControlEDListPhys, OhciControlHeadED);
    m_pBase->write32(m_pBulkEDListPhys, OhciBulkHeadED);

    // Reset may restore interrupt state, so keep the device silent until its
    // IRQ callback and preallocated port publications are ready.
    m_pBase->write32(OhciInterruptAll, OhciInterruptDisable);
    (void) m_pBase->read32(OhciInterruptEnable);
    m_pBase->write32(
        OhciInterruptOwnershipChange | 0x7F, OhciInterruptStatus);
    (void) m_pBase->read32(OhciInterruptStatus);

    // Prepare the control register
    control = m_pBase->read32(OhciControl);
    control &=
        ~(0x3 | 0x3C | OhciControlStateFunctionalMask |
          OhciControlInterruptRoute);  // Control bulk service, List enable, etc
    control |= OhciControlListsEnable | OhciControlStateRunning |
               0x3;  // 4:1 control/bulk ED ratio
    m_pBase->write32(control, OhciControl);

    // Controller is now running. Yay!

    // Restore the Frame Interval register (reset by a HC reset)
    m_pBase->write32(interval | (1U << 31U), OhciFmInterval);

    DEBUG_LOG(
        "USB: OHCI: maximum packet size is " << ((interval >> 16) & 0xEFFF));

    // Turn on all ports on the root hub.
    m_pBase->write32(OhciRhHubStsSetGlobalPower, OhciRhStatus);

    // Set up the RequestQueue
    initialise();

#if THREADS
    if (getLifecycleState() != RequestQueue::LifecycleState::Accepting)
    {
        ERROR("OHCI: request queue did not enter the accepting state");
        return;
    }
#endif

// Dequeue main thread
// new Thread(Processor::information().getCurrentThread()->getParent(),
// threadStub, reinterpret_cast<void*>(this));

// Install the IRQ handler
#if X86_COMMON
    m_IrqId =
        Machine::instance().getIrqManager()->registerHardPciIrqHandler(this, this);
    if (!m_IrqId)
    {
        ERROR("OHCI: could not register the PCI interrupt callback");
        return;
    }
    Machine::instance().getIrqManager()->control(
        getInterruptNumber(), IrqManager::MitigationThreshold,
        (1500000 / 64));  // 12KB/ms (12Mbps) in bytes, divided by 64 bytes
                          // maximum per transfer/IRQ-#else
#else
    InterruptManager::instance().registerInterruptHandler(
        pDev->getInterruptNumber(), this);
#endif

    // Get the number of ports and delay for power-up for this root hub.
    uint32_t rhDescA = m_pBase->read32(OhciRhDescriptorA);
    uint8_t powerWait = ((rhDescA >> 24) & 0xFF) * 2;
    m_nPorts = rhDescA & 0xFF;

    if (!UsbHcd::validOhciRootPortCount(m_nPorts))
    {
        ERROR("OHCI: unsupported root-port count " << Dec << m_nPorts << Hex);
        m_nPorts = 0;
        return;
    }

    for (size_t i = 0; i < m_nPorts; ++i)
    {
        if (!m_PortChanges[i].configure(*this, 0, i))
        {
            ERROR("OHCI: could not configure root-port publication " << i);
            return;
        }
    }

    DEBUG_LOG(
        "USB: OHCI: Reset complete, " << Dec << m_nPorts << Hex
                                      << " ports available");

    if (m_nPorts)
    {
        LockGuard<Spinlock> rootHubGuard(m_RootHubLock);

        // Establish a clean aggregate before the initial state scan. Changes
        // after this flush remain pending until RHSC is enabled below.
        m_pBase->write32(OhciInterruptRhStsChange, OhciInterruptStatus);
        (void) m_pBase->read32(OhciInterruptStatus);

        if (m_pBase->read32(OhciRhStatus) & OhciRhHubStsOverCurrentCh)
        {
            m_pBase->write32(OhciRhHubStsOverCurrentCh, OhciRhStatus);
            (void) m_pBase->read32(OhciRhStatus);
        }

        // The initial scan samples the current connection state directly, so
        // stale change indications can be retired without losing that state.
        for (size_t i = 0; i < m_nPorts; ++i)
        {
            const size_t portRegister = OhciRhPortStatus + (i * 4);
            const uint32_t portChanges =
                m_pBase->read32(portRegister) & OhciRhPortStsChangeMask;
            if (portChanges)
            {
                m_pBase->write32(portChanges, portRegister);
                (void) m_pBase->read32(portRegister);
            }
        }
    }

    // Transfer-completion sources become live only after IRQ registration,
    // queue startup, and root-port token configuration have all succeeded.
    m_pBase->write32(OhciInterruptOperational, OhciInterruptEnable);
    (void) m_pBase->read32(OhciInterruptEnable);

    for (size_t i = 0; i < m_nPorts; i++)
    {
        if (!(m_pBase->read32(OhciRhPortStatus + (i * 4)) & OhciRhPortStsPower))
        {
            DEBUG_LOG("USB: OHCI: applying power to port " << i);

            // Needs port power, do so
            m_pBase->write32(OhciRhPortStsPower, OhciRhPortStatus + (i * 4));

            // Wait as long as it needs
            Time::delay(powerWait * Time::Multiplier::Millisecond);
        }

        DEBUG_LOG("OHCI: Determining if there's a device on this port");

        // Check for a connected device
        if (m_pBase->read32(OhciRhPortStatus + (i * 4)) &
            OhciRhPortStsConnected)
            executeRequest(i);
    }

#if THREADS
    if (m_nPorts)
    {
        LockGuard<Spinlock> rootHubGuard(m_RootHubLock);
        m_RootHubStatusChangeDesired = true;
        setRootHubStatusChangeSource(true);
    }
#endif

    m_Initialised = true;
}

Ohci::~Ohci()
{
    // Quiesce only the root-port producer first. Transfer and SOF callbacks
    // must remain live while an active enumeration request drains.
    if (m_pBase)
    {
        LockGuard<Spinlock> irqGuard(m_IrqProcessingLock);
        LockGuard<Spinlock> rootHubGuard(m_RootHubLock);
        m_TeardownPhase = 1;
        m_RootHubStatusChangeDesired = false;
        setRootHubStatusChangeSource(false);
    }

    // The RHSC mask and IRQ serialization above close and drain observe().
    for (size_t i = 0; i < m_nPorts; ++i)
    {
        m_PortChanges[i].stopAfterQuiesce();
    }
    RequestQueue::destroy();

    {
        LockGuard<Spinlock> irqGuard(m_IrqProcessingLock);
        m_TeardownPhase = 2;
        if (m_pBase)
        {
            m_pBase->write32(OhciInterruptAll, OhciInterruptDisable);
            (void) m_pBase->read32(OhciInterruptEnable);
        }
        // Close admission while the IRQ lock prevents a level source from
        // repeatedly entering a just-closed callback path.
        m_CallbackOperations.close();
    }

#if X86_COMMON
    if (m_IrqId)
    {
        if (!Machine::instance().getIrqManager()->unregisterHandler(
                m_IrqId, this))
        {
            FATAL(
                "OHCI teardown could not synchronously unregister its IRQ "
                "callback");
        }
        m_IrqId = 0;
    }
#endif
    m_CallbackOperations.wait();

    if (m_pBase)
    {
        // USB suspend takes effect at a frame boundary. Once two frames have
        // elapsed, the controller no longer owns the HCCA/ED/TD memory that
        // the MemoryRegion destructor will release.
        uint32_t control = m_pBase->read32(OhciControl);
        control &= ~(OhciControlStateFunctionalMask | 0x3C);
        m_pBase->write32(
            control | OhciControlStateSuspended, OhciControl);
        (void) m_pBase->read32(OhciControl);
        Time::delay(2 * Time::Multiplier::Millisecond);

        m_pBase->write32(0, OhciHcca);
        m_pBase->write32(0, OhciControlHeadED);
        m_pBase->write32(0, OhciBulkHeadED);
        (void) m_pBase->read32(OhciHcca);

        // Leave the host controller in USBRESET, with every schedule disabled.
        m_pBase->write32(control, OhciControl);
        (void) m_pBase->read32(OhciControl);
        m_pHcca = nullptr;
    }
}

void Ohci::setRootHubStatusChangeSource(bool enabled)
{
    m_pBase->write32(
        OhciInterruptRhStsChange,
        enabled ? OhciInterruptEnable : OhciInterruptDisable);
    (void) m_pBase->read32(OhciInterruptEnable);
}

void Ohci::removeED(ED *pED)
{
    /// \note Refer to page 56 in the OHCI spec for this function.

    if (!pED || !pED->pMetaData)
        return;

#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG(
        "OHCI: removing ED #"
        << pED->pMetaData->id
        << " from the schedule to prepare for reclamation");
#endif

    // Make sure the ED is skipped by the host controller until it is properly
    // dequeued.
    pED->bSkip = true;
    pED->pMetaData->bIgnore = true;

    ED *pPrev = pED->pMetaData->pPrev;
    ED *pNext = pED->pMetaData->pNext;

    ED **pQueueHead = 0;
    ED **pQueueTail = 0;

    if (pED->pMetaData->edType == ControlList)
    {
        pQueueHead = &m_pControlQueueHead;
        pQueueTail = &m_pControlQueueTail;
    }
    else if (pED->pMetaData->edType == BulkList)
    {
        pQueueHead = &m_pBulkQueueHead;
        pQueueTail = &m_pBulkQueueTail;
    }
    else
    {
        ERROR("OHCI: ED #" << pED->pMetaData->id << " has an invalid type!");
        return;
    }

    bool bControl = pED->pMetaData->edType == ControlList;

    // Unlink from the hardware linked list.
    if (pED == *pQueueHead)
    {
#ifdef USB_VERBOSE_DEBUG
        DEBUG_LOG("OHCI: ED was a queue head, adjusting controller state "
                  "accordingly");
#endif

        *pQueueHead = pNext;

        if (bControl)
            m_pBase->write32(vtp_ed(pNext), OhciControlHeadED);
        else  /// \todo Isochronous and Periodic.
            m_pBase->write32(vtp_ed(pNext), OhciBulkHeadED);
    }
    else if (pPrev)
    {
        pPrev->pNext = pED->pNext;
    }

    // Simply for tracking purposes, make sure the tail is valid.
    if (pED == *pQueueTail)
    {
        *pQueueTail = pPrev;
    }

    // Unlink from the software linked list.
    if (pPrev)
        pPrev->pMetaData->pNext = pNext;
    if (pNext)
        pNext->pMetaData->pPrev = pPrev;

    // Disable list processing for this ED
    stop(pED->pMetaData->edType);

    // Prepare the ED for reclamation in the next USB frame.
    {
        LockGuard<Spinlock> guard(m_DequeueListLock);
        m_DequeueList.pushBack(pED);
    }

    // Clear any pending SOF interrupt and then enable the SOF IRQ.
    // The IRQ handler will pick up a SOF status and clean up the ED.
    m_pBase->write32(OhciInterruptStartOfFrame, OhciInterruptStatus);
    m_pBase->write32(OhciInterruptStartOfFrame, OhciInterruptEnable);
}

#if X86_COMMON
bool Ohci::irq(irq_id_t number, InterruptState &state)
#else
void Ohci::interrupt(size_t number, InterruptState &state)
#endif
{
    OperationBarrier::Lease callback;
    if (!m_CallbackOperations.tryAcquire(callback))
    {
        return
#if X86_COMMON
            false
#endif
            ;
    }
    LockGuard<Spinlock> transactionGuard(m_IrqProcessingLock);

    if (!m_pHcca)
    {
        // Assume not for us - no HCCA yet!
        return
#if X86_COMMON
            false
#endif
            ;
    }

    uint32_t nStatus = 0;

    // Find out if this came from either a done ED or a useful status.
    if (m_pHcca->pDoneHead)
    {
        // We must process this interrupt.
        nStatus = OhciInterruptWbDoneHead;
        if (m_pHcca->pDoneHead & 0x1)  // ... ???
            nStatus |= m_pBase->read32(OhciInterruptStatus) &
                       m_pBase->read32(OhciInterruptEnable);
    }
    else
    {
        nStatus = m_pBase->read32(OhciInterruptStatus) &
                  m_pBase->read32(OhciInterruptEnable);
    }

    // Not for us?
    if (!nStatus)
    {
        DEBUG_LOG("USB: OHCI: irq is not for us");
        return
#if X86_COMMON
            false
#endif
            ;
    }

    // However, make sure we do not get interrupted during handling.
    m_pBase->write32(OhciInterruptMIE, OhciInterruptDisable);

    // Clear the MIE bit from the interrupt status. We don't care for it.
    nStatus &= ~OhciInterruptMIE;

#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("OHCI: IRQ " << nStatus);
#endif

    if (nStatus & OhciInterruptUnrecoverableError)
    {
        /// \todo Handle.

        // Don't enable interrupts again, controller is not in a safe state.
        ERROR("OHCI: controller is hung!");
        return
#if X86_COMMON
            true
#endif
            ;
    }

    if (nStatus & OhciInterruptStartOfFrame)
    {
        LockGuard<Spinlock> guard(m_DequeueListLock);

#ifdef USB_VERBOSE_DEBUG
        DEBUG_LOG("OHCI: SOF, preparing to reclaim EDs...");
#endif

        // Firstly disable the SOF interrupt now that we've gotten it.
        m_pBase->write32(OhciInterruptStartOfFrame, OhciInterruptDisable);

        // Process the reclaim list.
        ED *pED = 0;
        while (1)
        {
            if (!m_DequeueList.count())
                break;

            pED = m_DequeueList.popFront();
            if (pED)
            {
                size_t id = pED->pMetaData->id & 0xFFF;
                Lists type = pED->pMetaData->edType;
                void (*completion)(uintptr_t, ssize_t) =
                    pED->pMetaData->pCallback;
                const uintptr_t completionParam = pED->pMetaData->pParam;
                const ssize_t completionResult =
                    pED->pMetaData->completionResult;

                for (List<TD *>::Iterator it =
                         pED->pMetaData->completedTdList.begin();
                     it != pED->pMetaData->completedTdList.end(); ++it)
                {
                    const size_t tdId = (*it)->id;
                    ByteSet(*it, 0, sizeof(TD));
                    m_TDBitmap.clear(tdId);
                }
                for (List<TD *>::Iterator it =
                         pED->pMetaData->tdList.begin();
                     it != pED->pMetaData->tdList.end(); ++it)
                {
                    const size_t tdId = (*it)->id;
                    ByteSet(*it, 0, sizeof(TD));
                    m_TDBitmap.clear(tdId);
                }

#ifdef USB_VERBOSE_DEBUG
                DEBUG_LOG("OHCI: freeing ED #" << pED->pMetaData->id << ".");
#endif

                // Destroy the ED and free it's memory space.
                delete pED->pMetaData;
                ByteSet(pED, 0, sizeof(ED));

                switch (type)
                {
                    case ControlList:
                        m_ControlEDBitmap.clear(id);
                        break;
                    case BulkList:
                        m_BulkEDBitmap.clear(id);
                        break;
                    case PeriodicList:
                        WARNING("periodic: not actually clearing bit");
                        // m_PeriodicEDBitmap.clear(edId);
                        break;
                    case IsochronousList:
                        DEBUG_LOG("USB: OHCI: dequeue on an isochronous ED, "
                                  "but we don't support them yet.");
                        break;
                }

                if (completion)
                    completion(completionParam, completionResult);

                // Safe to restore this list to the running state.
                /// \note List processing won't start until the NEXT SOF.
                start(type);
            }
        }
    }

    // Check for newly connected / disconnected devices. A threadless build
    // leaves RHSC masked because enumeration can block and allocate.
#if THREADS
    if (nStatus & OhciInterruptRhStsChange)
    {
        LockGuard<Spinlock> rootHubGuard(m_RootHubLock);

        // Clear and flush the aggregate before scanning. A change after its
        // port has been scanned will relatch RHSC and cannot be erased by a
        // trailing aggregate acknowledgement.
        m_pBase->write32(OhciInterruptRhStsChange, OhciInterruptStatus);
        (void) m_pBase->read32(OhciInterruptStatus);

        if (m_pBase->read32(OhciRhStatus) & OhciRhHubStsOverCurrentCh)
        {
            m_pBase->write32(OhciRhHubStsOverCurrentCh, OhciRhStatus);
            (void) m_pBase->read32(OhciRhStatus);
        }

        for (size_t i = 0; i < m_nPorts; i++)
        {
            const size_t portRegister = OhciRhPortStatus + (i * 4);
            const uint32_t portStatus = m_pBase->read32(portRegister);
            uint32_t acknowledgeMask =
                portStatus &
                (OhciRhPortStsEnableCh | OhciRhPortStsSuspendCh |
                 OhciRhPortStsOverCurrentCh);

            // A reset worker masks RHSC before issuing reset and owns PRSC
            // until it has sampled and cleared completion. A stale PRSC with
            // no owner can be retired here instead of causing an IRQ storm.
            if ((portStatus & OhciRhPortStsResCh) && !m_PortResetActive)
            {
                acknowledgeMask |= OhciRhPortStsResCh;
            }

            if (portStatus & OhciRhPortStsConnStsCh)
            {
                const bool ignored = m_IgnoredPorts.test(i);
                bool acknowledge = ignored;
                size_t generation = 0;
                if (!ignored)
                {
                    const auto observation = m_PortChanges[i].observe();
                    acknowledge =
                        UsbHcd::PortChangeRequest::canAcknowledge(
                            observation.result);
                    assert(acknowledge);
                    if (acknowledge)
                    {
                        generation = observation.generation;
                        m_DeferredPortChanges.defer(i, generation);
                    }
                }

                if (acknowledge)
                {
                    acknowledgeMask |= OhciRhPortStsConnStsCh;
                }
                else
                {
                    // A configured preallocated token has no fallible
                    // admission path while the queue is accepting. Preserve
                    // CSC for diagnosis, but mask RHSC to avoid a hard-IRQ
                    // livelock if that invariant is ever violated.
                    m_RootHubStatusChangeDesired = false;
                    setRootHubStatusChangeSource(false);
                }
            }

            if (acknowledgeMask)
            {
                // OHCI root-port command bits alias the readable status bits;
                // writing only upper change bits avoids replaying commands.
                m_pBase->write32(acknowledgeMask, portRegister);
                (void) m_pBase->read32(portRegister);
            }

            const size_t generation = m_DeferredPortChanges.release(i);
            if (generation)
            {
                m_PortChanges[i].acknowledge(generation);
            }
        }
    }
#endif

    // A list of EDs that persist in the schedule. Used to repopulate the
    // schedule list.
    List<ED *> persistList;

    if (nStatus & OhciInterruptWbDoneHead)
    {
        ED *pED = 0;
        do
        {
            {
                LockGuard<Spinlock> guard(m_ScheduleChangeLock);
                if (m_FullSchedule.count())
                    pED = m_FullSchedule.popFront();
                else
                    break;
            }

            // Assume not yet linked properly
            if (pED->pMetaData->bIgnore)
            {
                persistList.pushBack(pED);
                continue;
            }

            bool bPeriodic = pED->pMetaData->bPeriodic;

            // Iterate the TD list
            TD *pTD = 0;
            while (pED->pMetaData->tdList.count())
            {
                pTD = pED->pMetaData->tdList.popFront();

                // TD not yet handled - return to the list and go to the next
                // ED.
                if (pTD->nStatus == 0xF)
                {
                    pED->pMetaData->tdList.pushFront(pTD);
                    break;
                }

                ssize_t nResult;
                if (pTD->nStatus)
                {
#ifdef USB_VERBOSE_DEBUG
                    if (!bPeriodic)
                        ERROR_NOLOCK("TD Error " << Dec << pTD->nStatus << Hex);
#endif
                    nResult = -pTD->getError();
                }
                else
                {
                    if (pTD->pBufferStart)
                    {
                        // Only a part of the buffer has been transfered
                        size_t nBytesLeft =
                            pTD->pBufferEnd - pTD->pBufferStart + 1;
                        nResult = pTD->nBufferSize - nBytesLeft;
                    }
                    else
                        nResult = pTD->nBufferSize;
                    pED->pMetaData->nTotalBytes += nResult;
                }
#ifdef USB_VERBOSE_DEBUG
                DEBUG_LOG_NOLOCK(
                    "TD #" << Dec << pTD->id << Hex << " [from ED #" << Dec
                           << pED->pMetaData->id << Hex << "] DONE: " << Dec
                           << pED->nAddress << ":" << pED->nEndpoint << " "
                           << (pTD->nPid == 1 ?
                                   "OUT" :
                                   (pTD->nPid == 2 ?
                                        "IN" :
                                        (pTD->nPid == 0 ? "SETUP" : "")))
                           << " " << nResult << Hex);
#endif

                /// \note It might be nice to document this.
                bool bEndOfTransfer =
                    (!bPeriodic &&
                     ((nResult < 0) || (pTD == pED->pMetaData->pLastTD))) ||
                    (bPeriodic && (nResult >= 0));

                if (!bPeriodic)
                    pED->pMetaData->completedTdList.pushBack(pTD);

                // Last TD or error condition, if async, otherwise only when it
                // gives no error
                if (bEndOfTransfer)
                {
                    const ssize_t completionResult =
                        nResult < 0 ? nResult :
                                      pED->pMetaData->nTotalBytes;
                    const bool ownsCompletion =
                        bPeriodic ||
                        pED->pMetaData->completionState.compareAndSwap(1, 2);

                    if (!bPeriodic && ownsCompletion)
                    {
                        pED->pMetaData->completionResult = completionResult;
                        removeED(pED);
                        continue;
                    }
                    else if (bPeriodic)
                    {
                        // Invert data toggle
                        pTD->bDataToggle = !pTD->bDataToggle;

                        // Clear the total bytes field so it won't grow with
                        // each completed transfer
                        pED->pMetaData->nTotalBytes = 0;
                    }

                    if (bPeriodic && pED->pMetaData->pCallback)
                    {
                        pED->pMetaData->pCallback(
                            pED->pMetaData->pParam, completionResult);
                    }
                }

                // Interrupt TDs need to be always active
                if (bPeriodic)
                {
                    pTD->nStatus = 0xf;
                    pTD->pBufferStart = pTD->pBufferEnd - pTD->nBufferSize + 1;
                    pED->pHeadTD = PHYS_TD(pTD->id) >> 4;

                    pED->pMetaData->tdList.pushBack(pTD);
                    break;  // Only one TD in a periodic transfer.
                }
            }

            // If this ED is not queued for deletion, make sure we can use it
            // in the next IRQ.
            if (!pED->pMetaData->bIgnore)
                persistList.pushBack(pED);
        } while (pED);
    }

    // Restore EDs into the schedule if they were removed and need to persist.
    if (persistList.count())
    {
        LockGuard<Spinlock> guard(m_ScheduleChangeLock);
        for (List<ED *>::Iterator it = persistList.begin();
             it != persistList.end();)
        {
            m_FullSchedule.pushBack(*it);
            it = persistList.erase(it);
        }
    }

    // RHSC was acknowledged before its scan so a later port edge cannot be
    // erased here.
    const uint32_t acknowledgeStatus =
        nStatus & ~OhciInterruptRhStsChange;
    if (acknowledgeStatus)
    {
        m_pBase->write32(acknowledgeStatus, OhciInterruptStatus);
        (void) m_pBase->read32(OhciInterruptStatus);
    }

    if (m_TeardownPhase < 2)
    {
        m_pBase->write32(OhciInterruptMIE, OhciInterruptEnable);
    }

#if X86_COMMON
    return true;
#endif
}

void Ohci::addTransferToTransaction(
    uintptr_t pTransaction, bool bToggle, UsbPid pid, uintptr_t pBuffer,
    size_t nBytes)
{
    // Atomic operation: find clear bit, set it
    size_t nIndex = 0;
    {
        LockGuard<ControllerLock> guard(m_Mutex);
        nIndex = m_TDBitmap.getFirstClear();
        if (nIndex >= (0x1000 / sizeof(TD)))
        {
            ERROR("USB: OHCI: TD space full");
            return;
        }
        m_TDBitmap.set(nIndex);
    }

    // Grab the TD pointer we're going to set up now
    TD *pTD = &m_pTDList[nIndex];
    ByteSet(pTD, 0, sizeof(TD));
    pTD->id = nIndex;

    // Buffer rounding - allow packets smaller than the buffer we specify
    pTD->bBuffRounding = 1;

    // PID for the transfer
    switch (pid)
    {
        case UsbPidSetup:
            pTD->nPid = 0;
            break;
        case UsbPidOut:
            pTD->nPid = 1;
            break;
        case UsbPidIn:
            pTD->nPid = 2;
            break;
        default:
            pTD->nPid = 3;
    };

    // Active
    pTD->nStatus = 0xf;

    // Buffer for transfer
    if (nBytes)
    {
        VirtualAddressSpace &va =
            Processor::information().getVirtualAddressSpace();
        if (va.isMapped(reinterpret_cast<void *>(pBuffer)))
        {
            physical_uintptr_t phys = 0;
            size_t flags = 0;
            va.getMapping(reinterpret_cast<void *>(pBuffer), phys, flags);
            pTD->pBufferStart = phys + (pBuffer & 0xFFF);
            pTD->pBufferEnd = pTD->pBufferStart + nBytes - 1;
        }
        else
        {
            ERROR(
                "OHCI: addTransferToTransaction: Buffer (page "
                << Dec << pBuffer << Hex << ") isn't mapped!");
            m_TDBitmap.clear(nIndex);
            return;
        }

        pTD->nBufferSize = nBytes;
    }

    // This is the last TD so far
    pTD->bLast = true;

    // pTransaction will be 0x0xxx for CONTROL, 0x1xxx for BULK, 0x2xxx for
    // PERIODIC.
    size_t transactionType = (pTransaction & 0x3000) >> 12;
    uintptr_t edOffset = pTransaction & 0xFFF;

    ED *pED = 0;
    switch (transactionType)
    {
        case 0:
            pED = &m_pControlEDList[edOffset];
            break;
        case 1:
            pED = &m_pBulkEDList[edOffset];
            break;
        case 2:
            pED = &m_pPeriodicEDList[edOffset];
            break;
        default:
            break;
    }

    if (!pED)
    {
        /// \todo Clean up!
        ERROR("USB: OHCI: transaction " << pTransaction << " is invalid.");
        return;
    }

    // Add our TD to the ED's queue.
    if (pED->pMetaData->pLastTD)
    {
        pED->pMetaData->pLastTD->pNext = PHYS_TD(nIndex) >> 4;
        pED->pMetaData->pLastTD->nNextTDIndex = nIndex;
        pED->pMetaData->pLastTD->bLast = false;
    }
    else
    {
        pED->pMetaData->pFirstTD = pTD;
        pED->pHeadTD = PHYS_TD(nIndex) >> 4;
    }
    pED->pMetaData->pLastTD = pTD;

    pED->pMetaData->tdList.pushBack(pTD);
}

uintptr_t Ohci::createTransaction(UsbEndpoint endpointInfo)
{
    // Determine what kind of transaction this is.
    bool bIsBulk = endpointInfo.nEndpoint > 0;

    // Atomic operation: find clear bit, set it
    ED *pED = 0;
    size_t nIndex = 0;
    {
        LockGuard<ControllerLock> guard(m_Mutex);

        if (bIsBulk)
            nIndex = m_BulkEDBitmap.getFirstClear();
        else
            nIndex = m_ControlEDBitmap.getFirstClear();

        if (nIndex >= (0x1000 / sizeof(ED)))
        {
            ERROR("USB: OHCI: ED space full");
            return static_cast<uintptr_t>(-1);
        }

        if (bIsBulk)
        {
            m_BulkEDBitmap.set(nIndex);
            pED = &m_pBulkEDList[nIndex];
            nIndex += 0x1000;
        }
        else
        {
            m_ControlEDBitmap.set(nIndex);
            pED = &m_pControlEDList[nIndex];
        }
    }

    ByteSet(pED, 0, sizeof(ED));

    // Device address, endpoint and speed
    pED->nAddress = endpointInfo.nAddress;
    pED->nEndpoint = endpointInfo.nEndpoint;
    pED->bLoSpeed = endpointInfo.speed == LowSpeed;

    // Maximum packet size
    pED->nMaxPacketSize = endpointInfo.nMaxPacketSize;

    // Make sure this ED is ignored until it's properly queued.
    pED->bSkip = true;

    // Setup the metadata
    pED->pMetaData = new ED::MetaData;
    pED->pMetaData->endpointInfo = endpointInfo;
    pED->pMetaData->id = nIndex;
    pED->pMetaData->bIgnore = true;  // Don't handle this ED until we're ready.
    pED->pMetaData->edType = bIsBulk ? BulkList : ControlList;
    pED->pMetaData->bPeriodic = false;
    pED->pMetaData->pFirstTD = pED->pMetaData->pLastTD = 0;
    pED->pMetaData->nTotalBytes = 0;
    pED->pMetaData->pPrev = pED->pMetaData->pNext = 0;
    pED->pMetaData->bLinked = false;
    pED->pMetaData->pCallback = nullptr;
    pED->pMetaData->pParam = 0;
    pED->pMetaData->completionState = 0;
    pED->pMetaData->completionResult = -TransactionError;

    // Complete
    return nIndex;
}

bool Ohci::doAsync(
    uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t),
    uintptr_t pParam)
{
    // pTransaction will be 0x0xxx for CONTROL, 0x1xxx for BULK, 0x2xxx for
    // PERIODIC.
    size_t transactionType = (pTransaction & 0x3000) >> 12;
    uintptr_t edOffset = pTransaction & 0xFFF;

    Spinlock *pLock = 0;

    ED *pED = 0;
    {
        LockGuard<ControllerLock> guard(m_Mutex);

        bool bValid = false;
        if (transactionType == 0)
            bValid = m_ControlEDBitmap.test(edOffset);
        else if (transactionType == 1)
            bValid = m_BulkEDBitmap.test(edOffset);

        if ((pTransaction == static_cast<uintptr_t>(-1)) || !bValid)
        {
            ERROR(
                "OHCI: doAsync: didn't get a valid transaction id ["
                << pTransaction << ", " << edOffset << "].");
            return false;
        }

        if (transactionType == 0)
        {
            pED = &m_pControlEDList[edOffset];
            pLock = &m_ControlListChangeLock;
        }
        else if (transactionType == 1)
        {
            pED = &m_pBulkEDList[edOffset];
            pLock = &m_BulkListChangeLock;
        }
        else
        {
            ERROR(
                "OHCI: doAsync: only control and bulk transactions supported");
            return false;
        }

        if (!pED->pMetaData || !pED->pMetaData->pLastTD)
        {
            ERROR(
                "OHCI: doAsync: transaction has no transfers ["
                << pTransaction << "].");
            delete pED->pMetaData;
            ByteSet(pED, 0, sizeof(ED));
            if (transactionType == 0)
                m_ControlEDBitmap.clear(edOffset);
            else
                m_BulkEDBitmap.clear(edOffset);
            return false;
        }
    }

    // Set up all the metadata we can at the moment.
    pED->pMetaData->pCallback = pCallback;
    pED->pMetaData->pParam = pParam;
    pED->pMetaData->completionState = 1;
    pED->pMetaData->completionResult = -TransactionError;

    bool bControl = !pED->pMetaData->endpointInfo.nEndpoint;

    // Stop the controller as we are modifying the queue.
    // stop(pED->pMetaData->edType);

    // Lock while we modify the linked lists.
    pLock->acquire();

    // Always at the end of the ED queue. Zero means "no next ED" to OHCI.
    pED->pNext = 0;

    // Handle the case where there is not yet a queue head.
    if (bControl)
    {
        if (!m_pControlQueueHead)
        {
#ifdef USB_VERBOSE_DEBUG
            DEBUG_LOG("OHCI: ED is now the control queue head.");
#endif
            m_pControlQueueHead = pED;
        }
    }
    else
    {
        if (!m_pBulkQueueHead)
        {
#ifdef USB_VERBOSE_DEBUG
            DEBUG_LOG("OHCI: ED is now the control queue head.");
#endif
            m_pBulkQueueHead = pED;
        }
    }

    // Grab the queue head.
    ED *pQueueHead = 0;
    physical_uintptr_t queueHeadPhys = 0;
    if (bControl)
    {
        pQueueHead = m_pControlQueueHead;
        queueHeadPhys = vtp_ed(pQueueHead);
    }
    else
    {
        pQueueHead = m_pBulkQueueHead;
        queueHeadPhys = vtp_ed(pQueueHead);
    }

    // Update the head of the relevant list.
    if (queueHeadPhys == vtp_ed(pED))
    {
        if (bControl)
        {
#ifdef USB_VERBOSE_DEBUG
            DEBUG_LOG(
                "OHCI: new control queue head is "
                << queueHeadPhys << " compared to "
                << m_pBase->read32(OhciControlHeadED));
            DEBUG_LOG(
                "OHCI: current control queue ED is "
                << m_pBase->read32(OhciControlCurrentED));
#endif
            m_pBase->write32(queueHeadPhys, OhciControlHeadED);
        }
        else
        {
#ifdef USB_VERBOSE_DEBUG
            DEBUG_LOG("OHCI: new bulk queue head is " << queueHeadPhys);
#endif
            m_pBase->write32(queueHeadPhys, OhciBulkHeadED);
        }
    }

    // Grab the current tail of the list and update it to point to us.
    ED *pTail = 0;
    if (bControl)
    {
        pTail = m_pControlQueueTail;
        m_pControlQueueTail = pED;
    }
    else
    {
        pTail = m_pBulkQueueTail;
        m_pBulkQueueTail = pED;
    }

    // Point the old tail to this ED.
    if (pTail)
    {
        pTail->pNext = vtp_ed(pED) >> 4;
        pTail->pMetaData->pNext = pED;
    }

    // Fix up the software linked list.
    pED->pMetaData->pNext = 0;
    pED->pMetaData->pPrev = pTail;
    pQueueHead->pMetaData->pPrev = 0;

    // Enable handling of this ED now.
    pED->bSkip = pED->pMetaData->bIgnore = false;
    pED->pMetaData->bLinked = true;

    // Can now unlock.
    pLock->release();

    // Add to the housekeeping schedule before we link in proper.
    m_ScheduleChangeLock.acquire();
    m_FullSchedule.pushBack(pED);
    m_ScheduleChangeLock.release();

    // Restart the controller if it was stopped for some reason.
    start(pED->pMetaData->edType);

    // Tell the controller that the list has valid TD in it now.
    // The OHCI will automatically stop processing the ED list if it determines
    // no more transfers are pending.
    uint32_t status = m_pBase->read32(OhciCommandStatus);
    status |=
        bControl ? OhciCommandControlListFilled : OhciCommandBulkListFilled;
    m_pBase->write32(status, OhciCommandStatus);
    return true;
}

void Ohci::cancelAsyncAndDrain(
    uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t),
    uintptr_t pParam)
{
    bool deliverCompletion = false;
    ssize_t completionResult = -TransactionError;
    ED *pED = nullptr;

    {
        LockGuard<ControllerLock> guard(m_Mutex);

        // USBSUSPEND takes effect at a frame boundary. Waiting two frames
        // establishes the DMA ownership boundary before any TD is reclaimed.
        const uint32_t savedControl = m_pBase->read32(OhciControl);
        m_pBase->write32(OhciInterruptMIE, OhciInterruptDisable);
        m_pBase->write32(
            (savedControl & ~OhciControlStateFunctionalMask) |
                OhciControlStateSuspended,
            OhciControl);
        Time::delay(2 * Time::Multiplier::Millisecond);

        {
            LockGuard<Spinlock> irqGuard(m_IrqProcessingLock);

            const size_t transactionType = (pTransaction & 0x3000) >> 12;
            const uintptr_t edOffset = pTransaction & 0xFFF;
            bool valid = false;
            if (transactionType == 0)
            {
                valid = m_ControlEDBitmap.test(edOffset);
                pED = valid ? &m_pControlEDList[edOffset] : nullptr;
            }
            else if (transactionType == 1)
            {
                valid = m_BulkEDBitmap.test(edOffset);
                pED = valid ? &m_pBulkEDList[edOffset] : nullptr;
            }

            if (pED && pED->pMetaData &&
                pED->pMetaData->pCallback == pCallback &&
                pED->pMetaData->pParam == pParam)
            {
                const bool cancellationWon =
                    pED->pMetaData->completionState.compareAndSwap(1, 2);
                const bool naturalCompletionPending =
                    !cancellationWon &&
                    static_cast<size_t>(
                        pED->pMetaData->completionState) == 2;
                deliverCompletion =
                    cancellationWon || naturalCompletionPending;
                completionResult = cancellationWon ?
                                       -TransactionError :
                                       pED->pMetaData->completionResult;

                if (deliverCompletion)
                    pED->pMetaData->pCallback = nullptr;

                if (cancellationWon)
                {
                    pED->pMetaData->completionResult = completionResult;
                }
            }

            if (deliverCompletion && !pED->pMetaData->bIgnore)
            {
                {
                    LockGuard<Spinlock> scheduleGuard(m_ScheduleChangeLock);
                    for (List<ED *>::Iterator it = m_FullSchedule.begin();
                         it != m_FullSchedule.end();)
                    {
                        if (*it == pED)
                        {
                            m_FullSchedule.erase(it);
                            break;
                        }
                        ++it;
                    }
                }

                removeED(pED);
            }

            if (m_TeardownPhase < 2)
            {
                m_pBase->write32(savedControl, OhciControl);
                m_pBase->write32(OhciInterruptMIE, OhciInterruptEnable);
            }
            else
            {
                m_pBase->write32(OhciInterruptAll, OhciInterruptDisable);
            }
            (void) m_pBase->read32(OhciInterruptEnable);
        }
    }

    if (deliverCompletion)
        pCallback(pParam, completionResult);
}

void Ohci::addInterruptInHandler(
    UsbEndpoint endpointInfo, uintptr_t pBuffer, uint16_t nBytes,
    void (*pCallback)(uintptr_t, ssize_t), uintptr_t pParam)
{
    // Atomic operation: find clear bit, set it
    ED *pED = 0;
    size_t nIndex = 0;
    {
        LockGuard<ControllerLock> guard(m_Mutex);

        nIndex = m_PeriodicEDBitmap.getFirstClear();
        if (nIndex >= (0x1000 / sizeof(ED)))
        {
            ERROR("USB: OHCI: ED space full");
            return;
        }

        m_PeriodicEDBitmap.set(nIndex);
        pED = &m_pPeriodicEDList[nIndex];

        // Periodic identifier
        nIndex += 0x2000;
    }

    ByteSet(pED, 0, sizeof(ED));

    // Device address, endpoint and speed
    pED->nAddress = endpointInfo.nAddress;
    pED->nEndpoint = endpointInfo.nEndpoint;
    pED->bLoSpeed = endpointInfo.speed == LowSpeed;

    // Maximum packet size
    pED->nMaxPacketSize = endpointInfo.nMaxPacketSize;

    // Make sure this ED is ignored until it's properly queued.
    pED->bSkip = true;

    // Setup the metadata
    pED->pMetaData = new ED::MetaData;
    pED->pMetaData->endpointInfo = endpointInfo;
    pED->pMetaData->id = nIndex;
    pED->pMetaData->bIgnore = true;  // Don't handle this ED until we're ready.
    pED->pMetaData->edType = PeriodicList;
    pED->pMetaData->pFirstTD = pED->pMetaData->pLastTD = 0;
    pED->pMetaData->nTotalBytes = 0;
    pED->pMetaData->pPrev = pED->pMetaData->pNext = 0;
    pED->pMetaData->bLinked = false;

    pED->pMetaData->bPeriodic = true;

    // Set up the callback and pointer.
    pED->pMetaData->pCallback = pCallback;
    pED->pMetaData->pParam = pParam;
    pED->pMetaData->completionState = 1;
    pED->pMetaData->completionResult = -TransactionError;

    // Add to the housekeeping schedule before we link in proper.
    m_ScheduleChangeLock.acquire();
    m_FullSchedule.pushBack(pED);
    m_ScheduleChangeLock.release();

    // Lock before linking.
    LockGuard<Spinlock> guard(m_PeriodicListChangeLock);

    pED->pMetaData->pPrev = m_pPeriodicQueueTail;

    m_pPeriodicQueueTail->pNext = vtp_ed(pED) >> 4;
    m_pPeriodicQueueTail = pED;

    // All linked in and ready to go!
    pED->bSkip = pED->pMetaData->bIgnore = false;
    pED->pMetaData->bLinked = true;

    // Start processing of the list if it isn't already active.
    start(pED->pMetaData->edType);
}

bool Ohci::portReset(uint8_t nPort, bool bErrorResponse)
{
    /// \todo Error handling? Device fails to reset? Not present after reset?

    if (nPort >= m_nPorts)
    {
        return false;
    }

#if THREADS
    LockGuard<ControllerLock> resetGuard(m_PortResetMutex);
#endif
    const size_t portRegister = OhciRhPortStatus + (nPort * 4);

    {
        LockGuard<Spinlock> rootHubGuard(m_RootHubLock);

        // PRSC is level-signalled through RHSC. Mask the source while reset is
        // in flight so the worker that must clear PRSC cannot be starved by a
        // same-core interrupt loop.
        m_PortResetActive = true;
        setRootHubStatusChangeSource(false);

        // Root-port lower bits are write commands, not an RMW-safe control
        // image. Writing only SetPortReset cannot echo unrelated W1C bits.
        m_pBase->write32(OhciRhPortStsReset, portRegister);
        (void) m_pBase->read32(portRegister);
    }

    bool resetComplete = false;
    constexpr size_t ResetPolls = 200;
    for (size_t attempt = 0; attempt < ResetPolls; ++attempt)
    {
        if (m_pBase->read32(portRegister) & OhciRhPortStsResCh)
        {
            resetComplete = true;
            break;
        }
        if (m_TeardownPhase)
        {
            break;
        }
        Time::delay(5 * Time::Multiplier::Millisecond);
    }

    {
        LockGuard<Spinlock> rootHubGuard(m_RootHubLock);

        // The reset worker exclusively owns PRSC while RHSC is masked. A
        // completion that arrived at the timeout boundary is still retired.
        const uint32_t portStatus = m_pBase->read32(portRegister);
        resetComplete = resetComplete || (portStatus & OhciRhPortStsResCh);
        if (portStatus & OhciRhPortStsResCh)
        {
            m_pBase->write32(OhciRhPortStsResCh, portRegister);
            (void) m_pBase->read32(portRegister);
        }

        // SetPortEnable is also a command bit; do not echo the status image.
        if (
            resetComplete &&
            !(m_pBase->read32(portRegister) & OhciRhPortStsEnable))
        {
            m_pBase->write32(OhciRhPortStsEnable, portRegister);
            (void) m_pBase->read32(portRegister);
        }

        m_PortResetActive = false;
        if (m_RootHubStatusChangeDesired)
        {
            // Any CSC that arrived during reset remained set while the source
            // was masked and becomes deliverable again here.
            setRootHubStatusChangeSource(true);
        }
    }

    if (!resetComplete && !m_TeardownPhase)
    {
        ERROR("OHCI: timed out resetting root port " << nPort);
    }
    return resetComplete;
}

uint64_t Ohci::executeRequest(
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

    // Check for a connected device
    if (m_pBase->read32(OhciRhPortStatus + (p1 * 4)) & OhciRhPortStsConnected)
    {
        if (!portReset(p1))
            return 0;

        // Determine the speed of the attached device
        if (m_pBase->read32(OhciRhPortStatus + (p1 * 4)) & OhciRhPortStsLoSpeed)
        {
            DEBUG_LOG(
                "USB: OHCI: Port "
                << Dec << p1 << Hex
                << " has a low-speed device connected to it");
            deviceConnected(p1, LowSpeed);
        }
        else
        {
            DEBUG_LOG(
                "USB: OHCI: Port "
                << Dec << p1 << Hex
                << " has a full-speed device connected to it");
            deviceConnected(p1, FullSpeed);
        }
    }
    else
        deviceDisconnected(p1);
    return 0;
}

void Ohci::cancelRequest(const Request &request)
{
    if (request.p1 < m_nPorts)
    {
        m_PortChanges[request.p1].cancel(
            static_cast<size_t>(request.p8));
    }
}

void Ohci::stop(Lists list)
{
    if (!m_pHcca)
        return;

    uint32_t control = m_pBase->read32(OhciControl);
    control &= ~static_cast<int>(list);
    m_pBase->write32(control, OhciControl);
}

void Ohci::start(Lists list)
{
    if (!m_pHcca)
        return;

    uint32_t control = m_pBase->read32(OhciControl);
    control |= static_cast<int>(list);
    m_pBase->write32(control, OhciControl);
}
