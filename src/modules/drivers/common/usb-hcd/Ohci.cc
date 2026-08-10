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
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/usb/Usb.h"
#include "modules/system/usb/UsbHub.h"

#define INDEX_FROM_TD(ptr) (((reinterpret_cast<uintptr_t>((ptr)) & 0xFFF) / sizeof(TD)))
#define PHYS_TD(idx) (m_pTDListPhys + ((idx) * sizeof(TD)))

namespace {
struct OhciCompletionCleanup {
  Ohci* controller;
  Ohci::ED* ed;
  size_t generation;
};
}  // namespace

void Ohci::finishDeferredCompletion(void* context) {
  auto* cleanup = reinterpret_cast<OhciCompletionCleanup*>(context);
  Ohci* controller = cleanup->controller;
  {
    LockGuard<ControllerLock> guard(controller->m_Mutex);
    ED* ed = cleanup->ed;
    if (ed->pMetaData && ed->pMetaData->completion.generation() == cleanup->generation) {
      controller->retireEDStorage(ed);
    } else
      panic("OHCI completion cleanup lost its transaction generation");
  }
  delete cleanup;
}

Ohci::Ohci(Device* pDev)
    : UsbHub(pDev),
      RequestQueue(MakeConstantString("OHCI")),
      m_pBase(0),
      m_nPorts(0),
      m_Initialised(false),
      m_Mutex(),
      m_PortResetMutex(),
      m_pHcca(nullptr),
      m_pHccaPhys(0),
      m_IrqProcessingLock(),
      m_CompletionDeliveries(),
      m_RootHubLock(),
      m_RootHubStatusChangeDesired(false),
      m_PortResetActive(false),
      m_TeardownPhase(0),
      m_ScheduleChangeLock(),
      m_PeriodicListChangeLock(),
      m_ControlListChangeLock(),
      m_BulkListChangeLock(),
      m_pPeriodicEDList(nullptr),
      m_pPeriodicEDListPhys(0),
      m_PeriodicEDBitmap(),
      m_pControlEDList(nullptr),
      m_pControlEDListPhys(0),
      m_ControlEDBitmap(),
      m_pBulkEDList(nullptr),
      m_pBulkEDListPhys(0),
      m_BulkEDBitmap(),
      m_pTDList(nullptr),
      m_pTDListPhys(0),
      m_TDBitmap(),
      m_pBulkQueueHead(nullptr),
      m_pControlQueueHead(nullptr),
      m_pBulkQueueTail(nullptr),
      m_pControlQueueTail(nullptr),
      m_pPeriodicQueueTail(nullptr),
      m_DequeueListLock(),
      m_DequeueList(),
      m_DequeueCount(0),
      m_OhciMR("Ohci-MR"),
      m_CallbackOperations(),
      m_SubmissionOperations(),
      m_CancellationOperations(),
      m_AcceptedOperations(),
      m_IrqId(0) {
  setSpecificType(String("OHCI"));

#if !X86_COMMON
  // Completion and root-port processing require ordinary thread context.
  // No maintained non-x86 machine provides a supported OHCI IRQ path.
  ERROR("OHCI requires threaded PCI IRQ delivery");
  return;
#endif

  // Allocate the memory region
  if (!PhysicalMemoryManager::instance().allocateRegion(
          m_OhciMR, 5, PhysicalMemoryManager::continuous,
          VirtualAddressSpace::Write | VirtualAddressSpace::KernelMode)) {
    ERROR("USB: OHCI: Couldn't allocate memory region!");
    return;
  }

  uintptr_t virtualBase = reinterpret_cast<uintptr_t>(m_OhciMR.virtualAddress());
  uintptr_t physicalBase = m_OhciMR.physicalAddress();

  m_pHcca = reinterpret_cast<Hcca*>(virtualBase);
  m_pBulkEDList = reinterpret_cast<ED*>(virtualBase + 0x1000);
  m_pControlEDList = reinterpret_cast<ED*>(virtualBase + 0x2000);
  m_pPeriodicEDList = reinterpret_cast<ED*>(virtualBase + 0x3000);
  m_pTDList = reinterpret_cast<TD*>(virtualBase + 0x4000);

  m_pHccaPhys = physicalBase;
  m_pBulkEDListPhys = physicalBase + 0x1000;
  m_pControlEDListPhys = physicalBase + 0x2000;
  m_pPeriodicEDListPhys = physicalBase + 0x3000;
  m_pTDListPhys = physicalBase + 0x4000;

  // Clear out the HCCA block.
  ByteSet(m_pHcca, 0, 0x800);

  // Get an ED for the periodic list
  m_PeriodicEDBitmap.set(0);
  ED* pPeriodicED = m_pPeriodicEDList;
  ByteSet(pPeriodicED, 0, sizeof(ED));
  pPeriodicED->bSkip = true;
  pPeriodicED->pMetaData = new ED::MetaData;
  pPeriodicED->pMetaData->pCallback = nullptr;
  pPeriodicED->pMetaData->pParam = 0;
  pPeriodicED->pMetaData->bPeriodic = true;
  pPeriodicED->pMetaData->pFirstTD = nullptr;
  pPeriodicED->pMetaData->pLastTD = nullptr;
  pPeriodicED->pMetaData->nTotalBytes = 0;
  pPeriodicED->pMetaData->bIgnore = true;
  pPeriodicED->pMetaData->bLinked = false;
  pPeriodicED->pMetaData->edType = PeriodicList;
  pPeriodicED->pMetaData->acceptedOperation = false;
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
  DEBUG_LOG("USB: OHCI: starting up - controller is version "
            << Dec << ((version & 0xF0) >> 4) << "." << (version & 0xF) << Hex << ".");

  // Do not let a firmware-programmed source reach the PCI line while the
  // controller is being taken over and reset.
  m_pBase->write32(OhciInterruptAll, OhciInterruptDisable);
  (void)m_pBase->read32(OhciInterruptEnable);

  // Determine first of all if the HC is controlled by the BIOS.
  uint32_t control = m_pBase->read32(OhciControl);
  if (control & OhciControlInterruptRoute) {
    // SMM.
    DEBUG_LOG("USB: OHCI: currently in SMM!");
    uint32_t status = m_pBase->read32(OhciCommandStatus);
    m_pBase->write32(status | OhciCommandRequestOwnership, OhciCommandStatus);
    constexpr size_t OwnershipPollLimit = 1000;
    size_t ownershipPolls = OwnershipPollLimit;
    while ((control = m_pBase->read32(OhciControl)) & OhciControlInterruptRoute) {
      if (!ownershipPolls--)
        panic("OHCI firmware ownership handoff timed out after 1 s");
      Time::delay(1 * Time::Multiplier::Millisecond);
    }
  } else {
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
  constexpr size_t ResetPollLimit = 20;
  size_t resetPolls = ResetPollLimit;
  while (m_pBase->read32(OhciCommandStatus) & OhciCommandHcReset) {
    if (!resetPolls--)
      panic("OHCI controller reset timed out after 100 ms");
    Time::delay(5 * Time::Multiplier::Millisecond);
  }

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
  (void)m_pBase->read32(OhciInterruptEnable);
  m_pBase->write32(OhciInterruptOwnershipChange | 0x7F, OhciInterruptStatus);
  (void)m_pBase->read32(OhciInterruptStatus);

  // Prepare the control register
  control = m_pBase->read32(OhciControl);
  control &= ~(0x3 | 0x3C | OhciControlStateFunctionalMask |
               OhciControlInterruptRoute);  // Control bulk service, List enable, etc
  control |= OhciControlListsEnable | OhciControlStateRunning | 0x3;  // 4:1 control/bulk ED ratio
  m_pBase->write32(control, OhciControl);

  // Controller is now running. Yay!

  // Restore the Frame Interval register (reset by a HC reset)
  m_pBase->write32(interval | (1U << 31U), OhciFmInterval);

  DEBUG_LOG("USB: OHCI: maximum packet size is " << ((interval >> 16) & 0xEFFF));

  // Turn on all ports on the root hub.
  m_pBase->write32(OhciRhHubStsSetGlobalPower, OhciRhStatus);

  // Set up the RequestQueue
  initialise();

#if THREADS
  if (getLifecycleState() != RequestQueue::LifecycleState::Accepting) {
    ERROR("OHCI: request queue did not enter the accepting state");
    return;
  }
#endif

// Dequeue main thread
// new Thread(Processor::information().getCurrentThread()->getParent(),
// threadStub, reinterpret_cast<void*>(this));

// Install the IRQ handler
#if X86_COMMON
  m_IrqId = Machine::instance().getIrqManager()->registerPciIrqHandler(
      static_cast<IrqHandler*>(this), this, IrqPolicy::pciIntxThreaded());
  if (!m_IrqId) {
    ERROR("OHCI: could not register the PCI interrupt callback");
    return;
  }
  Machine::instance().getIrqManager()->control(
      getInterruptNumber(), IrqManager::MitigationThreshold,
      (1500000 / 64));  // 12KB/ms (12Mbps) in bytes, divided by 64 bytes
                        // maximum per transfer/IRQ
#endif

  // Get the number of ports and delay for power-up for this root hub.
  uint32_t rhDescA = m_pBase->read32(OhciRhDescriptorA);
  uint8_t powerWait = ((rhDescA >> 24) & 0xFF) * 2;
  m_nPorts = rhDescA & 0xFF;

  if (!UsbHcd::validOhciRootPortCount(m_nPorts)) {
    ERROR("OHCI: unsupported root-port count " << Dec << m_nPorts << Hex);
    m_nPorts = 0;
    return;
  }

  for (size_t i = 0; i < m_nPorts; ++i) {
    if (!m_PortChanges[i].configure(*this, 0, i)) {
      ERROR("OHCI: could not configure root-port publication " << i);
      return;
    }
  }

  DEBUG_LOG("USB: OHCI: Reset complete, " << Dec << m_nPorts << Hex << " ports available");

  if (m_nPorts) {
    LockGuard<Spinlock> rootHubGuard(m_RootHubLock);

    // Establish a clean aggregate before the initial state scan. Changes
    // after this flush remain pending until RHSC is enabled below.
    m_pBase->write32(OhciInterruptRhStsChange, OhciInterruptStatus);
    (void)m_pBase->read32(OhciInterruptStatus);

    if (m_pBase->read32(OhciRhStatus) & OhciRhHubStsOverCurrentCh) {
      m_pBase->write32(OhciRhHubStsOverCurrentCh, OhciRhStatus);
      (void)m_pBase->read32(OhciRhStatus);
    }

    // The initial scan samples the current connection state directly, so
    // stale change indications can be retired without losing that state.
    for (size_t i = 0; i < m_nPorts; ++i) {
      const size_t portRegister = OhciRhPortStatus + (i * 4);
      const uint32_t portChanges = m_pBase->read32(portRegister) & OhciRhPortStsChangeMask;
      if (portChanges) {
        m_pBase->write32(portChanges, portRegister);
        (void)m_pBase->read32(portRegister);
      }
    }
  }

  // Transfer-completion sources become live only after IRQ registration,
  // queue startup, and root-port token configuration have all succeeded.
  m_pBase->write32(OhciInterruptOperational, OhciInterruptEnable);
  (void)m_pBase->read32(OhciInterruptEnable);

  for (size_t i = 0; i < m_nPorts; i++) {
    if (!(m_pBase->read32(OhciRhPortStatus + (i * 4)) & OhciRhPortStsPower)) {
      DEBUG_LOG("USB: OHCI: applying power to port " << i);

      // Needs port power, do so
      m_pBase->write32(OhciRhPortStsPower, OhciRhPortStatus + (i * 4));

      // Wait as long as it needs
      Time::delay(powerWait * Time::Multiplier::Millisecond);
    }

    DEBUG_LOG("OHCI: Determining if there's a device on this port");

    // Check for a connected device
    if (m_pBase->read32(OhciRhPortStatus + (i * 4)) & OhciRhPortStsConnected) {
#if THREADS
      // Device discovery constructs the controller under the global tree
      // lock. The worker's topology mutation waits until the factory returns.
      const auto observation = m_PortChanges[i].observe();
      if (!UsbHcd::PortChangeRequest::canAcknowledge(observation.result)) {
        panic("OHCI could not publish its initial root-port state");
      }
      m_PortChanges[i].acknowledge(observation.generation);
#else
      executeRequest(i);
#endif
    }
  }

#if THREADS
  if (m_nPorts) {
    LockGuard<Spinlock> rootHubGuard(m_RootHubLock);
    m_RootHubStatusChangeDesired = true;
    setRootHubStatusChangeSource(true);
  }
#endif

  m_Initialised = true;
}

Ohci::~Ohci() {
  // Quiesce only the root-port producer first. Transfer and SOF callbacks
  // must remain live while an active enumeration request drains.
  if (m_pBase) {
    LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);
    LockGuard<Spinlock> rootHubGuard(m_RootHubLock);
    m_TeardownPhase = 1;
    m_RootHubStatusChangeDesired = false;
    setRootHubStatusChangeSource(false);
  }

  // The RHSC mask and IRQ serialization above close and drain observe().
  for (size_t i = 0; i < m_nPorts; ++i) {
    m_PortChanges[i].stopAfterQuiesce();
  }
  RequestQueue::destroy();

  // Enumeration is quiesced, while transfer cancellation and DMA are still
  // live for class-driver destructors.
  disconnectAllDevices();

  // No new descriptor builder can race the terminal scan. Calls which were
  // already admitted finish while the controller and transfer IRQ are live.
  m_SubmissionOperations.closeAndWait();
  m_AcceptedOperations.close();

  List<UsbHcd::CallbackDeliveryQueue::Record*> completions;
  uint32_t resetControl = 0;

  if (m_pBase && m_pHcca) {
    LockGuard<ControllerLock> controllerGuard(m_Mutex);

    // MIE closes new hardware publications. Confirming USBSUSPEND
    // establishes ownership of every ED and TD before reclamation.
    const uint32_t control = m_pBase->read32(OhciControl);
    resetControl = control & ~(OhciControlStateFunctionalMask | 0x3C);
    m_pBase->write32(OhciInterruptMIE, OhciInterruptDisable);
    transitionControllerState(resetControl | OhciControlStateSuspended,
                              "OHCI teardown suspend timed out after 100 ms");

    {
      LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);
      m_TeardownPhase = 2;
      m_CallbackOperations.close();

      // A captured completion normally waits for SOF. The controller is
      // now suspended, so teardown itself owns that reclamation boundary.
      while (true) {
        ED* pED = nullptr;
        {
          LockGuard<Spinlock> dequeueGuard(m_DequeueListLock);
          if (m_DequeueList.count())
            pED = m_DequeueList.popFront();
        }
        if (!pED)
          break;
        terminalizeEDForTeardown(pED, completions);
      }

      constexpr size_t EdCount = 0x1000 / sizeof(ED);
      for (size_t i = 0; i < EdCount; ++i) {
        if (m_ControlEDBitmap.test(i))
          terminalizeEDForTeardown(&m_pControlEDList[i], completions);
        if (m_BulkEDBitmap.test(i))
          terminalizeEDForTeardown(&m_pBulkEDList[i], completions);
      }

      // Periodic callbacks are recurring events, not a one-shot terminal
      // contract. Stop the subscription and reclaim it without inventing
      // a final callback. Records already captured by an IRQ own snapshots
      // and are drained by m_CallbackOperations below.
      ED* pPeriodicDummy = m_pPeriodicEDList;
      {
        LockGuard<Spinlock> periodicGuard(m_PeriodicListChangeLock);
        if (pPeriodicDummy) {
          pPeriodicDummy->pNext = 0;
          if (pPeriodicDummy->pMetaData) {
            pPeriodicDummy->pMetaData->pPrev = pPeriodicDummy;
            pPeriodicDummy->pMetaData->pNext = pPeriodicDummy;
          }
          m_pPeriodicQueueTail = pPeriodicDummy;
        }
      }
      for (size_t i = 1; i < EdCount; ++i) {
        if (!m_PeriodicEDBitmap.test(i))
          continue;

        ED* pED = &m_pPeriodicEDList[i];
        removeFromFullSchedule(pED);
        pED->bSkip = true;
        if (pED->pMetaData) {
          pED->pMetaData->bIgnore = true;
          pED->pMetaData->bLinked = false;
          pED->pMetaData->pPrev = nullptr;
          pED->pMetaData->pNext = nullptr;
        }
        pED->pNext = 0;
        retireEDStorage(pED);
      }

      if (completions.count())
        m_CompletionDeliveries.publish(completions);

      m_pBase->write32(OhciInterruptAll, OhciInterruptDisable);
      (void)m_pBase->read32(OhciInterruptEnable);
      m_pBase->write32(OhciInterruptOwnershipChange | 0x7F, OhciInterruptStatus);
      (void)m_pBase->read32(OhciInterruptStatus);

      m_pBase->write32(0, OhciHcca);
      m_pBase->write32(0, OhciControlHeadED);
      m_pBase->write32(0, OhciBulkHeadED);
      (void)m_pBase->read32(OhciHcca);
    }
  } else {
    LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);
    m_TeardownPhase = 2;
    m_CallbackOperations.close();
  }

#if X86_COMMON
  if (m_IrqId) {
    if (!Machine::instance().getIrqManager()->unregisterHandler(m_IrqId,
                                                                static_cast<IrqHandler*>(this))) {
      panic(
          "OHCI teardown could not synchronously unregister its IRQ "
          "callback");
    }
    m_IrqId = 0;
  }
#endif

  while (completions.count()) {
    auto* completion = completions.popFront();
    m_CompletionDeliveries.deliver(completion);
  }

  m_CallbackOperations.wait();
  m_AcceptedOperations.wait();
  m_CancellationOperations.closeAndWait();

  if (m_pPeriodicEDList && m_PeriodicEDBitmap.test(0)) {
    LockGuard<ControllerLock> controllerGuard(m_Mutex);
    retireEDStorage(m_pPeriodicEDList);
    m_pPeriodicQueueTail = nullptr;
  }

  assert(m_CompletionDeliveries.empty());
  assert(!m_DequeueList.count());
  assert(!m_FullSchedule.count());
  assert(!m_pControlQueueHead && !m_pControlQueueTail);
  assert(!m_pBulkQueueHead && !m_pBulkQueueTail);
  assert(!m_pPeriodicQueueTail);
  constexpr size_t FinalEdCount = 0x1000 / sizeof(ED);
  for (size_t i = 0; i < FinalEdCount; ++i) {
    assert(!m_ControlEDBitmap.test(i));
    assert(!m_BulkEDBitmap.test(i));
    assert(!m_PeriodicEDBitmap.test(i));
  }
  constexpr size_t FinalTdCount = 0x1000 / sizeof(TD);
  for (size_t i = 0; i < FinalTdCount; ++i)
    assert(!m_TDBitmap.test(i));
  assert(m_SubmissionOperations.isClosedAndDrained());
  assert(m_CallbackOperations.isClosedAndDrained());
  assert(m_AcceptedOperations.isClosedAndDrained());
  assert(m_CancellationOperations.isClosedAndDrained());

  if (m_pBase && m_pHcca) {
    // Leave the host controller in USBRESET, with every schedule disabled.
    m_pBase->write32(resetControl, OhciControl);
    (void)m_pBase->read32(OhciControl);
    m_pHcca = nullptr;
  }
}

void Ohci::setRootHubStatusChangeSource(bool enabled) {
  m_pBase->write32(OhciInterruptRhStsChange, enabled ? OhciInterruptEnable : OhciInterruptDisable);
  (void)m_pBase->read32(OhciInterruptEnable);
}

void Ohci::transitionControllerState(uint32_t control, const char* timeoutMessage) {
  const uint32_t expected = control & OhciControlStateFunctionalMask;
  m_pBase->write32(control, OhciControl);
  (void)m_pBase->read32(OhciControl);

  constexpr size_t TransitionPollLimit = 100;
  size_t polls = TransitionPollLimit;
  while (polls-- && ((m_pBase->read32(OhciControl) & OhciControlStateFunctionalMask) != expected)) {
    Time::delay(1 * Time::Multiplier::Millisecond);
  }

  if ((m_pBase->read32(OhciControl) & OhciControlStateFunctionalMask) != expected)
    panic(timeoutMessage);
}

void Ohci::removeED(ED* pED) {
  /// \note Refer to page 56 in the OHCI spec for this function.

  if (!pED || !pED->pMetaData)
    return;

#ifdef USB_VERBOSE_DEBUG
  DEBUG_LOG("OHCI: removing ED #" << pED->pMetaData->id
                                  << " from the schedule to prepare for reclamation");
#endif

  const Lists type = pED->pMetaData->edType;
  detachED(pED);

  // This list remains stopped until SOF establishes the reclamation boundary.
  stop(type);

  {
    LockGuard<Spinlock> guard(m_DequeueListLock);
    m_DequeueList.pushBack(pED);
  }

  // Clear any pending SOF interrupt and then enable the SOF IRQ.
  m_pBase->write32(OhciInterruptStartOfFrame, OhciInterruptStatus);
  m_pBase->write32(OhciInterruptStartOfFrame, OhciInterruptEnable);
}

void Ohci::detachED(ED* pED) {
  if (!pED || !pED->pMetaData)
    return;

  pED->bSkip = true;
  pED->pMetaData->bIgnore = true;

  if (!pED->pMetaData->bLinked)
    return;

  ED* pPrev = pED->pMetaData->pPrev;
  ED* pNext = pED->pMetaData->pNext;

  ED** pQueueHead = 0;
  ED** pQueueTail = 0;
  Spinlock* pListLock = nullptr;

  if (pED->pMetaData->edType == ControlList) {
    pQueueHead = &m_pControlQueueHead;
    pQueueTail = &m_pControlQueueTail;
    pListLock = &m_ControlListChangeLock;
  } else if (pED->pMetaData->edType == BulkList) {
    pQueueHead = &m_pBulkQueueHead;
    pQueueTail = &m_pBulkQueueTail;
    pListLock = &m_BulkListChangeLock;
  } else {
    ERROR("OHCI: ED #" << pED->pMetaData->id << " has an invalid type!");
    return;
  }

  LockGuard<Spinlock> listGuard(*pListLock);
  bool bControl = pED->pMetaData->edType == ControlList;

  // Unlink from the hardware linked list.
  if (pED == *pQueueHead) {
#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG(
        "OHCI: ED was a queue head, adjusting controller state "
        "accordingly");
#endif

    *pQueueHead = pNext;

    if (bControl)
      m_pBase->write32(vtp_ed(pNext), OhciControlHeadED);
    else  /// \todo Isochronous and Periodic.
      m_pBase->write32(vtp_ed(pNext), OhciBulkHeadED);
  } else if (pPrev) {
    pPrev->pNext = pED->pNext;
  }

  // Simply for tracking purposes, make sure the tail is valid.
  if (pED == *pQueueTail) {
    *pQueueTail = pPrev;
  }

  // Unlink from the software linked list.
  if (pPrev)
    pPrev->pMetaData->pNext = pNext;
  if (pNext)
    pNext->pMetaData->pPrev = pPrev;

  pED->pMetaData->pPrev = nullptr;
  pED->pMetaData->pNext = nullptr;
  pED->pMetaData->bLinked = false;
  pED->pNext = 0;
}

void Ohci::removeFromFullSchedule(ED* pED) {
  LockGuard<Spinlock> scheduleGuard(m_ScheduleChangeLock);
  for (List<ED*>::Iterator it = m_FullSchedule.begin(); it != m_FullSchedule.end();) {
    if (*it == pED) {
      m_FullSchedule.erase(it);
      return;
    }
    ++it;
  }
}

void Ohci::removeFromDequeueList(ED* pED) {
  LockGuard<Spinlock> dequeueGuard(m_DequeueListLock);
  for (List<ED*>::Iterator it = m_DequeueList.begin(); it != m_DequeueList.end();) {
    if (*it == pED) {
      m_DequeueList.erase(it);
      return;
    }
    ++it;
  }
}

void Ohci::reclaimTransferDescriptors(ED* pED) {
  if (!pED || !pED->pMetaData)
    return;

  for (List<TD*>::Iterator it = pED->pMetaData->completedTdList.begin();
       it != pED->pMetaData->completedTdList.end(); ++it) {
    const size_t tdId = (*it)->id;
    ByteSet(*it, 0, sizeof(TD));
    m_TDBitmap.clear(tdId);
  }
  pED->pMetaData->completedTdList.clear();

  for (List<TD*>::Iterator it = pED->pMetaData->tdList.begin(); it != pED->pMetaData->tdList.end();
       ++it) {
    const size_t tdId = (*it)->id;
    ByteSet(*it, 0, sizeof(TD));
    m_TDBitmap.clear(tdId);
  }
  pED->pMetaData->tdList.clear();
  pED->pMetaData->pFirstTD = nullptr;
  pED->pMetaData->pLastTD = nullptr;
}

void Ohci::retireEDStorage(ED* pED) {
  if (!pED || !pED->pMetaData)
    return;

  reclaimTransferDescriptors(pED);
  ED::MetaData* metadata = pED->pMetaData;
  const size_t id = metadata->id & 0xFFF;
  const Lists type = metadata->edType;
  const bool acceptedOperation = metadata->acceptedOperation;
  delete metadata;
  ByteSet(pED, 0, sizeof(ED));

  if (type == ControlList)
    m_ControlEDBitmap.clear(id);
  else if (type == BulkList)
    m_BulkEDBitmap.clear(id);
  else if (type == PeriodicList)
    m_PeriodicEDBitmap.clear(id);

  if (acceptedOperation)
    m_AcceptedOperations.leave();
}

UsbHcd::CallbackDeliveryQueue::Record* Ohci::prepareCompletion(
    ED* pED, const UsbHcd::TransferCompletion::Claim& claim) {
  assert(pED && pED->pMetaData);
  assert(!pED->pMetaData->bPeriodic);
  assert(claim.generation == pED->pMetaData->completion.generation());

  reclaimTransferDescriptors(pED);
  auto* cleanup = new OhciCompletionCleanup{this, pED, claim.generation};
  return m_CompletionDeliveries.create({pED->pMetaData->id, claim.generation}, claim.callback,
                                       claim.parameter, claim.result, finishDeferredCompletion,
                                       cleanup);
}

void Ohci::terminalizeEDForTeardown(ED* pED,
                                    List<UsbHcd::CallbackDeliveryQueue::Record*>& completions) {
  if (!pED || !pED->pMetaData || pED->pMetaData->bPeriodic)
    return;

  removeFromFullSchedule(pED);
  detachED(pED);
  removeFromDequeueList(pED);

  if (pED->pMetaData->completion.state() == UsbHcd::TransferCompletion::State::Idle) {
    retireEDStorage(pED);
    return;
  }

  UsbHcd::TransferCompletion::Claim claim;
  if (pED->pMetaData->completion.claimForTeardown(-TransactionError, claim)) {
    completions.pushBack(prepareCompletion(pED, claim));
  }
}

#if X86_COMMON
IrqDisposition Ohci::irq(irq_id_t number) {
  (void)number;

  OperationBarrier::Lease callback;
  if (!m_CallbackOperations.tryAcquire(callback)) {
    return IrqDisposition::Quiesced;
  }

  List<UsbHcd::CallbackDeliveryQueue::Record*> completions;
  {
    LockGuard<IrqProcessingLock> transactionGuard(m_IrqProcessingLock);

    if (m_TeardownPhase == 2) {
      return IrqDisposition::Quiesced;
    }

    if (!m_pHcca) {
      // Assume not for us - no HCCA yet!
      return IrqDisposition::NotHandled;
    }

    uint32_t nStatus = m_pBase->read32(OhciInterruptStatus) & m_pBase->read32(OhciInterruptEnable);
    const uint32_t observedDoneHead = m_pHcca->pDoneHead;
    if (observedDoneHead) {
      nStatus |= OhciInterruptWbDoneHead;
    }

    // Not for us?
    if (!nStatus) {
      DEBUG_LOG("USB: OHCI: irq is not for us");
      return IrqDisposition::NotHandled;
    }

    // However, make sure we do not get interrupted during handling.
    m_pBase->write32(OhciInterruptMIE, OhciInterruptDisable);
    (void)m_pBase->read32(OhciInterruptEnable);

    // HCCA DoneHead belongs to software until WDH is acknowledged. Re-read
    // it after closing MIE, then save and clear it before processing so the
    // controller has an empty slot when WDH is retired below.
    const uint32_t doneHead = m_pHcca->pDoneHead;
    if (doneHead) {
      m_pHcca->pDoneHead = 0;
      FENCE();
      nStatus |= OhciInterruptWbDoneHead;
      if (doneHead & 0x1) {
        nStatus |= m_pBase->read32(OhciInterruptStatus) & m_pBase->read32(OhciInterruptEnable);
      }
    }

    // Clear the MIE bit from the interrupt status. We don't care for it.
    nStatus &= ~OhciInterruptMIE;
    bool sofDrained = true;
    bool doneHeadDrained = true;

#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("OHCI: IRQ " << nStatus);
#endif

    if (nStatus & OhciInterruptUnrecoverableError) {
      /// \todo Handle.

      // Don't enable interrupts again, controller is not in a safe state.
      ERROR("OHCI: controller is hung!");
      return IrqDisposition::Handled;
    }

    if (nStatus & OhciInterruptStartOfFrame) {
#ifdef USB_VERBOSE_DEBUG
      DEBUG_LOG("OHCI: SOF, preparing to reclaim EDs...");
#endif

      // Firstly disable the SOF interrupt now that we've gotten it.
      m_pBase->write32(OhciInterruptStartOfFrame, OhciInterruptDisable);

      // Process the reclaim list.
      constexpr size_t EdListCount = 3 * (0x1000 / sizeof(ED));
      size_t reclaimBudget = EdListCount;
      while (reclaimBudget) {
        ED* pED = nullptr;
        {
          LockGuard<Spinlock> guard(m_DequeueListLock);
          if (m_DequeueList.count())
            pED = m_DequeueList.popFront();
          else
            break;
        }

        --reclaimBudget;
        if (pED) {
          const Lists type = pED->pMetaData->edType;
          UsbHcd::TransferCompletion::Claim claim;
          const bool ownsPublication = pED->pMetaData->completion.claimCaptured(claim);

#ifdef USB_VERBOSE_DEBUG
          DEBUG_LOG("OHCI: freeing ED #" << pED->pMetaData->id << ".");
#endif

          if (ownsPublication)
            completions.pushBack(prepareCompletion(pED, claim));

          // Safe to restore this list to the running state.
          /// \note List processing won't start until the NEXT SOF.
          start(type);
        }
      }

      {
        LockGuard<Spinlock> guard(m_DequeueListLock);
        if (m_DequeueList.count()) {
          sofDrained = false;
          ERROR_NOLOCK("OHCI: exceeded the SOF reclaim scan budget");
        }
      }
    }

    // Check for newly connected / disconnected devices. A threadless build
    // leaves RHSC masked because enumeration can block and allocate.
#if THREADS
    if (nStatus & OhciInterruptRhStsChange) {
      LockGuard<Spinlock> rootHubGuard(m_RootHubLock);

      // Clear and flush the aggregate before scanning. A change after its
      // port has been scanned will relatch RHSC and cannot be erased by a
      // trailing aggregate acknowledgement.
      m_pBase->write32(OhciInterruptRhStsChange, OhciInterruptStatus);
      (void)m_pBase->read32(OhciInterruptStatus);

      if (m_pBase->read32(OhciRhStatus) & OhciRhHubStsOverCurrentCh) {
        m_pBase->write32(OhciRhHubStsOverCurrentCh, OhciRhStatus);
        (void)m_pBase->read32(OhciRhStatus);
      }

      for (size_t i = 0; i < m_nPorts; i++) {
        const size_t portRegister = OhciRhPortStatus + (i * 4);
        const uint32_t portStatus = m_pBase->read32(portRegister);
        uint32_t acknowledgeMask = portStatus & (OhciRhPortStsEnableCh | OhciRhPortStsSuspendCh |
                                                 OhciRhPortStsOverCurrentCh);

        // A reset worker masks RHSC before issuing reset and owns PRSC
        // until it has sampled and cleared completion. A stale PRSC
        // with no owner can be retired here instead of causing an IRQ
        // storm.
        if ((portStatus & OhciRhPortStsResCh) && !m_PortResetActive) {
          acknowledgeMask |= OhciRhPortStsResCh;
        }

        if (portStatus & OhciRhPortStsConnStsCh) {
          const bool deferred = deferConnectionChangeIfSuppressed(i);
          bool acknowledge = deferred;
          size_t generation = 0;
          if (!deferred) {
            const auto observation = m_PortChanges[i].observe();
            acknowledge = UsbHcd::PortChangeRequest::canAcknowledge(observation.result);
            assert(acknowledge);
            if (acknowledge) {
              generation = observation.generation;
              m_DeferredPortChanges.defer(i, generation);
            }
          }

          if (acknowledge) {
            acknowledgeMask |= OhciRhPortStsConnStsCh;
          } else {
            // A configured preallocated token has no fallible
            // admission path while the queue is accepting. Preserve
            // CSC for diagnosis, but mask RHSC to avoid a
            // level-triggered IRQ livelock if that invariant is
            // ever violated.
            m_RootHubStatusChangeDesired = false;
            setRootHubStatusChangeSource(false);
          }
        }

        if (acknowledgeMask) {
          // OHCI root-port command bits alias the readable status
          // bits; writing only upper change bits avoids replaying
          // commands.
          m_pBase->write32(acknowledgeMask, portRegister);
          (void)m_pBase->read32(portRegister);
        }

        const size_t generation = m_DeferredPortChanges.release(i);
        if (generation) {
          m_PortChanges[i].acknowledge(generation);
        }
      }
    }
#endif

    // A list of EDs that persist in the schedule. Used to repopulate the
    // schedule list.
    List<ED*> persistList;

    if (nStatus & OhciInterruptWbDoneHead) {
      constexpr size_t EdListCount = 3 * (0x1000 / sizeof(ED));
      constexpr size_t TdListCount = 0x1000 / sizeof(TD);
      size_t scheduleBudget = EdListCount;
      ED* pED = 0;
      while (scheduleBudget) {
        --scheduleBudget;
        {
          LockGuard<Spinlock> guard(m_ScheduleChangeLock);
          if (m_FullSchedule.count())
            pED = m_FullSchedule.popFront();
          else
            break;
        }

        // Assume not yet linked properly
        if (pED->pMetaData->bIgnore) {
          persistList.pushBack(pED);
          continue;
        }

        bool bPeriodic = pED->pMetaData->bPeriodic;

        // Iterate the TD list
        TD* pTD = 0;
        size_t tdBudget = TdListCount;
        while (pED->pMetaData->tdList.count() && tdBudget) {
          --tdBudget;
          pTD = pED->pMetaData->tdList.popFront();

          // TD not yet handled - return to the list and go to the
          // next ED.
          if (pTD->nStatus == 0xF) {
            pED->pMetaData->tdList.pushFront(pTD);
            break;
          }

          ssize_t nResult;
          if (pTD->nStatus) {
#ifdef USB_VERBOSE_DEBUG
            if (!bPeriodic)
              ERROR_NOLOCK("TD Error " << Dec << pTD->nStatus << Hex);
#endif
            nResult = -pTD->getError();
          } else {
            if (pTD->pBufferStart) {
              // Only a part of the buffer has been transfered
              size_t nBytesLeft = pTD->pBufferEnd - pTD->pBufferStart + 1;
              nResult = pTD->nBufferSize - nBytesLeft;
            } else
              nResult = pTD->nBufferSize;
            pED->pMetaData->nTotalBytes += nResult;
          }
#ifdef USB_VERBOSE_DEBUG
          DEBUG_LOG_NOLOCK(
              "TD #" << Dec << pTD->id << Hex << " [from ED #" << Dec << pED->pMetaData->id << Hex
                     << "] DONE: " << Dec << pED->nAddress << ":" << pED->nEndpoint << " "
                     << (pTD->nPid == 1 ? "OUT"
                                        : (pTD->nPid == 2 ? "IN" : (pTD->nPid == 0 ? "SETUP" : "")))
                     << " " << nResult << Hex);
#endif

          /// \note It might be nice to document this.
          bool bEndOfTransfer =
              (!bPeriodic && ((nResult < 0) || (pTD == pED->pMetaData->pLastTD))) ||
              (bPeriodic && (nResult >= 0));

          if (!bPeriodic)
            pED->pMetaData->completedTdList.pushBack(pTD);

          // Last TD or error condition, if async, otherwise only when
          // it gives no error
          if (bEndOfTransfer) {
            const ssize_t completionResult = nResult < 0 ? nResult : pED->pMetaData->nTotalBytes;
            const bool ownsCompletion =
                bPeriodic || pED->pMetaData->completion.captureNatural(completionResult);

            if (!bPeriodic && ownsCompletion) {
              removeED(pED);
              continue;
            } else if (bPeriodic) {
              // Invert data toggle
              pTD->bDataToggle = !pTD->bDataToggle;

              // Clear the total bytes field so it won't grow with
              // each completed transfer
              pED->pMetaData->nTotalBytes = 0;
            }

            if (bPeriodic && pED->pMetaData->pCallback) {
              completions.pushBack(m_CompletionDeliveries.create(
                  {pED->pMetaData->id, m_CompletionDeliveries.nextGeneration()},
                  pED->pMetaData->pCallback, pED->pMetaData->pParam, completionResult));
            }
          }

          // Interrupt TDs need to be always active
          if (bPeriodic) {
            pTD->nStatus = 0xf;
            pTD->pBufferStart = pTD->pBufferEnd - pTD->nBufferSize + 1;
            pED->pHeadTD = PHYS_TD(pTD->id) >> 4;

            pED->pMetaData->tdList.pushBack(pTD);
            break;  // Only one TD in a periodic transfer.
          }
        }

        if (!tdBudget && pED->pMetaData->tdList.count()) {
          doneHeadDrained = false;
          ERROR_NOLOCK("OHCI: ED #" << Dec << pED->pMetaData->id << Hex
                                    << " exceeded the TD scan budget");
        }

        // If this ED is not queued for deletion, make sure we can use
        // it in the next IRQ.
        if (!pED->pMetaData->bIgnore)
          persistList.pushBack(pED);
      }

      {
        LockGuard<Spinlock> guard(m_ScheduleChangeLock);
        if (m_FullSchedule.count()) {
          doneHeadDrained = false;
          ERROR_NOLOCK("OHCI: exceeded the done-head ED scan budget");
        }
      }
    }

    // Restore EDs into the schedule if they were removed and need to
    // persist.
    if (persistList.count()) {
      LockGuard<Spinlock> guard(m_ScheduleChangeLock);
      for (List<ED*>::Iterator it = persistList.begin(); it != persistList.end();) {
        m_FullSchedule.pushBack(*it);
        it = persistList.erase(it);
      }
    }

    // RHSC was acknowledged before its scan so a later port edge cannot be
    // erased here.
    uint32_t acknowledgeStatus = nStatus & ~OhciInterruptRhStsChange;
    if (!sofDrained) {
      acknowledgeStatus &= ~OhciInterruptStartOfFrame;
      m_pBase->write32(OhciInterruptStartOfFrame, OhciInterruptEnable);
    }
    if (!doneHeadDrained) {
      acknowledgeStatus &= ~OhciInterruptWbDoneHead;
    }
    if (acknowledgeStatus) {
      m_pBase->write32(acknowledgeStatus, OhciInterruptStatus);
      (void)m_pBase->read32(OhciInterruptStatus);
    }

    if (m_TeardownPhase < 2) {
      m_pBase->write32(OhciInterruptMIE, OhciInterruptEnable);
    }

    if (completions.count())
      m_CompletionDeliveries.publish(completions);
  }

  while (completions.count()) {
    auto* completion = completions.popFront();
    m_CompletionDeliveries.deliver(completion);
  }

  return IrqDisposition::Handled;
}
#endif

void Ohci::addTransferToTransaction(uintptr_t pTransaction, bool bToggle, UsbPid pid,
                                    uintptr_t pBuffer, size_t nBytes) {
  OperationBarrier::Lease submission;
  if (!m_SubmissionOperations.tryAcquire(submission))
    return;

  LockGuard<ControllerLock> controllerGuard(m_Mutex);
  LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);

  constexpr size_t EdCount = 0x1000 / sizeof(ED);
  const size_t transactionType = (pTransaction & 0x3000) >> 12;
  const uintptr_t edOffset = pTransaction & 0xFFF;
  ED* pED = nullptr;
  bool valid = edOffset < EdCount;
  if (valid && transactionType == 0) {
    valid = m_ControlEDBitmap.test(edOffset);
    pED = valid ? &m_pControlEDList[edOffset] : nullptr;
  } else if (valid && transactionType == 1) {
    valid = m_BulkEDBitmap.test(edOffset);
    pED = valid ? &m_pBulkEDList[edOffset] : nullptr;
  } else if (valid && transactionType == 2) {
    valid = m_PeriodicEDBitmap.test(edOffset);
    pED = valid ? &m_pPeriodicEDList[edOffset] : nullptr;
  } else
    valid = false;

  if (pTransaction == static_cast<uintptr_t>(-1) || !valid || !pED || !pED->pMetaData ||
      pED->pMetaData->acceptedOperation ||
      pED->pMetaData->completion.state() != UsbHcd::TransferCompletion::State::Idle) {
    ERROR("USB: OHCI: transaction " << pTransaction << " is invalid.");
    return;
  }

  size_t nIndex = m_TDBitmap.getFirstClear();
  if (nIndex >= (0x1000 / sizeof(TD))) {
    ERROR("USB: OHCI: TD space full");
    return;
  }
  m_TDBitmap.set(nIndex);

  // Grab the TD pointer we're going to set up now
  TD* pTD = &m_pTDList[nIndex];
  ByteSet(pTD, 0, sizeof(TD));
  pTD->id = nIndex;

  // Buffer rounding - allow packets smaller than the buffer we specify
  pTD->bBuffRounding = 1;

  // PID for the transfer
  switch (pid) {
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
  (void)bToggle;

  // Buffer for transfer
  if (nBytes) {
    VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
    if (va.isMapped(reinterpret_cast<void*>(pBuffer))) {
      physical_uintptr_t phys = 0;
      size_t flags = 0;
      va.getMapping(reinterpret_cast<void*>(pBuffer), phys, flags);
      pTD->pBufferStart = phys + (pBuffer & 0xFFF);
      pTD->pBufferEnd = pTD->pBufferStart + nBytes - 1;
    } else {
      ERROR("OHCI: addTransferToTransaction: Buffer (page " << Dec << pBuffer << Hex
                                                            << ") isn't mapped!");
      ByteSet(pTD, 0, sizeof(TD));
      m_TDBitmap.clear(nIndex);
      return;
    }

    pTD->nBufferSize = nBytes;
  }

  // This is the last TD so far
  pTD->bLast = true;

  // Add our TD to the ED's queue.
  if (pED->pMetaData->pLastTD) {
    pED->pMetaData->pLastTD->pNext = PHYS_TD(nIndex) >> 4;
    pED->pMetaData->pLastTD->nNextTDIndex = nIndex;
    pED->pMetaData->pLastTD->bLast = false;
  } else {
    pED->pMetaData->pFirstTD = pTD;
    pED->pHeadTD = PHYS_TD(nIndex) >> 4;
  }
  pED->pMetaData->pLastTD = pTD;

  pED->pMetaData->tdList.pushBack(pTD);
}

uintptr_t Ohci::createTransaction(UsbEndpoint endpointInfo) {
  OperationBarrier::Lease submission;
  if (!m_SubmissionOperations.tryAcquire(submission))
    return static_cast<uintptr_t>(-1);

  // Determine what kind of transaction this is.
  bool bIsBulk = endpointInfo.nEndpoint > 0;

  LockGuard<ControllerLock> controllerGuard(m_Mutex);
  LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);

  ED* pED = nullptr;
  size_t nIndex = bIsBulk ? m_BulkEDBitmap.getFirstClear() : m_ControlEDBitmap.getFirstClear();

  if (nIndex >= (0x1000 / sizeof(ED))) {
    ERROR("USB: OHCI: ED space full");
    return static_cast<uintptr_t>(-1);
  }

  if (bIsBulk) {
    m_BulkEDBitmap.set(nIndex);
    pED = &m_pBulkEDList[nIndex];
    nIndex += 0x1000;
  } else {
    m_ControlEDBitmap.set(nIndex);
    pED = &m_pControlEDList[nIndex];
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
  pED->pMetaData->acceptedOperation = false;

  // Complete
  return nIndex;
}

bool Ohci::doAsync(uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t),
                   uintptr_t pParam) {
  OperationBarrier::Lease submission;
  if (!m_SubmissionOperations.tryAcquire(submission))
    return false;

  LockGuard<ControllerLock> controllerGuard(m_Mutex);
  LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);

  // pTransaction will be 0x0xxx for CONTROL, 0x1xxx for BULK, 0x2xxx for
  // PERIODIC.
  const size_t transactionType = (pTransaction & 0x3000) >> 12;
  const uintptr_t edOffset = pTransaction & 0xFFF;
  constexpr size_t EdCount = 0x1000 / sizeof(ED);

  Spinlock* pLock = nullptr;
  ED* pED = nullptr;
  bool valid = edOffset < EdCount;
  if (valid && transactionType == 0) {
    valid = m_ControlEDBitmap.test(edOffset);
    pED = valid ? &m_pControlEDList[edOffset] : nullptr;
    pLock = &m_ControlListChangeLock;
  } else if (valid && transactionType == 1) {
    valid = m_BulkEDBitmap.test(edOffset);
    pED = valid ? &m_pBulkEDList[edOffset] : nullptr;
    pLock = &m_BulkListChangeLock;
  } else
    valid = false;

  if (pTransaction == static_cast<uintptr_t>(-1) || !valid) {
    ERROR("OHCI: doAsync: didn't get a valid transaction id [" << pTransaction << ", " << edOffset
                                                               << "].");
    return false;
  }

  if (!pED->pMetaData) {
    ERROR("OHCI: doAsync: transaction metadata is missing");
    return false;
  }

  if (pED->pMetaData->completion.state() != UsbHcd::TransferCompletion::State::Idle) {
    ERROR("OHCI: doAsync: transaction is already accepted");
    return false;
  }

  if (!pED->pMetaData->pLastTD) {
    ERROR("OHCI: doAsync: transaction has no transfers [" << pTransaction << "].");
    retireEDStorage(pED);
    return false;
  }

  if (!m_AcceptedOperations.tryEnter()) {
    retireEDStorage(pED);
    return false;
  }
  pED->pMetaData->acceptedOperation = true;

  const bool bControl = transactionType == 0;

  // Link the ED while it is still skipped. IRQ completion, cancellation and
  // teardown are all excluded by m_IrqProcessingLock until the final unskip.
  pLock->acquire();

  // Always at the end of the ED queue. Zero means "no next ED" to OHCI.
  pED->pNext = 0;

  // Handle the case where there is not yet a queue head.
  if (bControl) {
    if (!m_pControlQueueHead) {
#ifdef USB_VERBOSE_DEBUG
      DEBUG_LOG("OHCI: ED is now the control queue head.");
#endif
      m_pControlQueueHead = pED;
    }
  } else {
    if (!m_pBulkQueueHead) {
#ifdef USB_VERBOSE_DEBUG
      DEBUG_LOG("OHCI: ED is now the control queue head.");
#endif
      m_pBulkQueueHead = pED;
    }
  }

  // Grab the queue head.
  ED* pQueueHead = nullptr;
  physical_uintptr_t queueHeadPhys = 0;
  if (bControl) {
    pQueueHead = m_pControlQueueHead;
    queueHeadPhys = vtp_ed(pQueueHead);
  } else {
    pQueueHead = m_pBulkQueueHead;
    queueHeadPhys = vtp_ed(pQueueHead);
  }

  // Update the head of the relevant list.
  if (queueHeadPhys == vtp_ed(pED)) {
    if (bControl) {
#ifdef USB_VERBOSE_DEBUG
      DEBUG_LOG("OHCI: new control queue head is " << queueHeadPhys << " compared to "
                                                   << m_pBase->read32(OhciControlHeadED));
      DEBUG_LOG("OHCI: current control queue ED is " << m_pBase->read32(OhciControlCurrentED));
#endif
      m_pBase->write32(queueHeadPhys, OhciControlHeadED);
    } else {
#ifdef USB_VERBOSE_DEBUG
      DEBUG_LOG("OHCI: new bulk queue head is " << queueHeadPhys);
#endif
      m_pBase->write32(queueHeadPhys, OhciBulkHeadED);
    }
  }

  // Grab the current tail of the list and update it to point to us.
  ED* pTail = nullptr;
  if (bControl) {
    pTail = m_pControlQueueTail;
    m_pControlQueueTail = pED;
  } else {
    pTail = m_pBulkQueueTail;
    m_pBulkQueueTail = pED;
  }

  // Point the old tail to this ED.
  if (pTail) {
    pTail->pNext = vtp_ed(pED) >> 4;
    pTail->pMetaData->pNext = pED;
  }

  // Fix up the software linked list.
  pED->pMetaData->pNext = nullptr;
  pED->pMetaData->pPrev = pTail;
  pQueueHead->pMetaData->pPrev = nullptr;
  pED->pMetaData->bLinked = true;

  pLock->release();

  {
    LockGuard<Spinlock> scheduleGuard(m_ScheduleChangeLock);
    m_FullSchedule.pushBack(pED);
  }

  // Arming immediately before unskip makes the callback obligation and the
  // hardware publication one indivisible commit to every competing path.
  pED->pMetaData->completion.arm(pCallback, pParam, m_CompletionDeliveries.nextGeneration());
  FENCE();
  pED->bSkip = pED->pMetaData->bIgnore = false;

  // Restart the controller if it was stopped for some reason.
  start(pED->pMetaData->edType);

  // Tell the controller that the list has valid TD in it now.
  // The OHCI will automatically stop processing the ED list if it determines
  // no more transfers are pending.
  uint32_t status = m_pBase->read32(OhciCommandStatus);
  status |= bControl ? OhciCommandControlListFilled : OhciCommandBulkListFilled;
  m_pBase->write32(status, OhciCommandStatus);
  return true;
}

void Ohci::cancelAsyncAndDrain(uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t),
                               uintptr_t pParam) {
  OperationBarrier::Lease cancellation;
  if (!m_CancellationOperations.tryAcquire(cancellation) || !m_pBase)
    return;

  bool drainDelivery = false;
  UsbHcd::CallbackDeliveryQueue::Key deliveryKey = {0, 0};
  List<UsbHcd::CallbackDeliveryQueue::Record*> completions;
  ED* pED = nullptr;

  {
    LockGuard<ControllerLock> controllerGuard(m_Mutex);

    // Confirm USBSUSPEND before treating the controller's EDs and TDs as
    // software-owned.
    uint32_t savedControl = m_pBase->read32(OhciControl);
    m_pBase->write32(OhciInterruptMIE, OhciInterruptDisable);
    transitionControllerState(
        (savedControl & ~OhciControlStateFunctionalMask) | OhciControlStateSuspended,
        "OHCI cancellation suspend timed out after 100 ms");

    bool restoreController = false;

    {
      LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);

      const size_t transactionType = (pTransaction & 0x3000) >> 12;
      const uintptr_t edOffset = pTransaction & 0xFFF;
      constexpr size_t EdCount = 0x1000 / sizeof(ED);
      bool valid = edOffset < EdCount;
      if (valid && transactionType == 0) {
        valid = m_ControlEDBitmap.test(edOffset);
        pED = valid ? &m_pControlEDList[edOffset] : nullptr;
      } else if (valid && transactionType == 1) {
        valid = m_BulkEDBitmap.test(edOffset);
        pED = valid ? &m_pBulkEDList[edOffset] : nullptr;
      } else
        valid = false;

      Lists completedList = ControlList;
      if (valid && pED && pED->pMetaData) {
        UsbHcd::TransferCompletion::Claim claim;
        const auto disposition = pED->pMetaData->completion.claimCancellation(
            pCallback, pParam, -TransactionError, claim);
        if (disposition == UsbHcd::TransferCompletion::CancellationDisposition::Claimed) {
          completedList = pED->pMetaData->edType;
          removeFromFullSchedule(pED);
          detachED(pED);
          removeFromDequeueList(pED);
          completions.pushBack(prepareCompletion(pED, claim));
          m_CompletionDeliveries.publish(completions);
        } else if (disposition ==
                   UsbHcd::TransferCompletion::CancellationDisposition::DrainPublished) {
          deliveryKey = {pTransaction, claim.generation};
          drainDelivery = true;
        }

        // A natural completion may have stopped its list while waiting
        // for SOF. Cancellation supplied that DMA boundary itself.
        if (completions.count())
          savedControl |= static_cast<uint32_t>(completedList);
      }

      restoreController = m_TeardownPhase < 2;
      if (!restoreController) {
        m_pBase->write32(OhciInterruptAll, OhciInterruptDisable);
        (void)m_pBase->read32(OhciInterruptEnable);
      }
    }

    if (restoreController) {
      // This may restore USBOPERATIONAL or USBRESUME. The state poll is
      // outside IRQ serialization so a slow controller cannot hold up
      // an interrupt path.
      transitionControllerState(savedControl, "OHCI cancellation restore timed out after 100 ms");

      LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);
      m_pBase->write32(OhciInterruptMIE, OhciInterruptEnable);
      (void)m_pBase->read32(OhciInterruptEnable);
    }
  }

  while (completions.count()) {
    auto* completion = completions.popFront();
    m_CompletionDeliveries.deliver(completion);
  }
  if (drainDelivery)
    (void)m_CompletionDeliveries.drain(deliveryKey);
}

bool Ohci::cancelInterruptInAndDrain(const UsbInterruptInToken& token,
                                     void (*callback)(uintptr_t, ssize_t), uintptr_t parameter,
                                     bool producerAlreadyStopped) {
  (void)token;
  (void)callback;
  (void)parameter;
  (void)producerAlreadyStopped;
  panic("OHCI returned an interrupt-IN handle for an unsupported transfer");
  return false;
}

bool Ohci::addInterruptInHandler(UsbEndpoint endpointInfo, uintptr_t pBuffer, uint16_t nBytes,
                                 void (*pCallback)(uintptr_t, ssize_t),
                                 UsbInterruptInHandle& handle, uintptr_t pParam) {
  (void)endpointInfo;
  (void)pBuffer;
  (void)nBytes;
  (void)pCallback;
  (void)handle;
  (void)pParam;
  // The legacy OHCI path never built the periodic TD it claimed to publish.
  // Failing here retains neither a DMA buffer nor a callback target.
  WARNING("USB: OHCI: recurring interrupt-IN transfers are not implemented");
  return false;
}

void Ohci::replaySuppressedConnectionChange(size_t port) {
#if THREADS
  if (port >= m_nPorts) {
    ERROR("OHCI: invalid suppressed root-port replay " << Dec << port);
    return;
  }

  LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);
  // Teardown stops publication before its active enumeration worker returns.
  if (m_TeardownPhase) {
    return;
  }
  const auto observation = m_PortChanges[port].observe();
  const bool accepted = UsbHcd::PortChangeRequest::canAcknowledge(observation.result);
  if (accepted) {
    m_PortChanges[port].acknowledge(observation.generation);
    return;
  }

  m_TeardownPhase = 1;
  {
    LockGuard<Spinlock> rootHubGuard(m_RootHubLock);
    m_RootHubStatusChangeDesired = false;
    setRootHubStatusChangeSource(false);
  }
  ERROR("OHCI: live suppressed root-port replay could not be published");
  assert(false);
#else
  (void)port;
#endif
}

bool Ohci::portReset(uint8_t nPort, bool bErrorResponse) {
  /// \todo Error handling? Device fails to reset? Not present after reset?

  if (nPort >= m_nPorts) {
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
    (void)m_pBase->read32(portRegister);
  }

  bool resetComplete = false;
  constexpr size_t ResetPolls = 200;
  for (size_t attempt = 0; attempt < ResetPolls; ++attempt) {
    if (m_pBase->read32(portRegister) & OhciRhPortStsResCh) {
      resetComplete = true;
      break;
    }
    if (m_TeardownPhase) {
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
    if (portStatus & OhciRhPortStsResCh) {
      m_pBase->write32(OhciRhPortStsResCh, portRegister);
      (void)m_pBase->read32(portRegister);
    }

    // SetPortEnable is also a command bit; do not echo the status image.
    if (resetComplete && !(m_pBase->read32(portRegister) & OhciRhPortStsEnable)) {
      m_pBase->write32(OhciRhPortStsEnable, portRegister);
      (void)m_pBase->read32(portRegister);
    }

    m_PortResetActive = false;
    if (m_RootHubStatusChangeDesired) {
      // Any CSC that arrived during reset remained set while the source
      // was masked and becomes deliverable again here.
      setRootHubStatusChangeSource(true);
    }
  }

  if (!resetComplete && !m_TeardownPhase) {
    ERROR("OHCI: timed out resetting root port " << nPort);
  }
  return resetComplete;
}

uint64_t Ohci::executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
                              uint64_t p6, uint64_t p7, uint64_t p8) {
  if (p1 >= m_nPorts) {
    return 0;
  }
  UsbHcd::PortChangeRequest::Completion completion(m_PortChanges[p1], static_cast<size_t>(p8));
  if (!completion) {
    return 0;
  }

  // Check for a connected device
  if (m_pBase->read32(OhciRhPortStatus + (p1 * 4)) & OhciRhPortStsConnected) {
    if (!portReset(p1))
      return 0;

    // Determine the speed of the attached device
    if (m_pBase->read32(OhciRhPortStatus + (p1 * 4)) & OhciRhPortStsLoSpeed) {
      DEBUG_LOG("USB: OHCI: Port " << Dec << p1 << Hex
                                   << " has a low-speed device connected to it");
      deviceConnected(p1, LowSpeed);
    } else {
      DEBUG_LOG("USB: OHCI: Port " << Dec << p1 << Hex
                                   << " has a full-speed device connected to it");
      deviceConnected(p1, FullSpeed);
    }
  } else
    deviceDisconnected(p1);
  return 0;
}

void Ohci::cancelRequest(const Request& request) {
  if (request.p1 < m_nPorts) {
    m_PortChanges[request.p1].cancel(static_cast<size_t>(request.p8));
  }
}

void Ohci::stop(Lists list) {
  if (!m_pHcca)
    return;

  uint32_t control = m_pBase->read32(OhciControl);
  control &= ~static_cast<int>(list);
  m_pBase->write32(control, OhciControl);
}

void Ohci::start(Lists list) {
  if (!m_pHcca)
    return;

  uint32_t control = m_pBase->read32(OhciControl);
  control |= static_cast<int>(list);
  m_pBase->write32(control, OhciControl);
}
