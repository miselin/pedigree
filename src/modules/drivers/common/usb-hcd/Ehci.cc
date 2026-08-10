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

#include "Ehci.h"
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
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
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
#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/usb/Usb.h"
#include "modules/system/usb/UsbHub.h"

#define INDEX_FROM_QTD(ptr) (((reinterpret_cast<uintptr_t>((ptr)) & 0xFFF) / sizeof(qTD)))
#define PHYS_QTD(idx) (m_pqTDListPhys + ((idx) * sizeof(qTD)))

static int threadStub(void* p);

static constexpr size_t EhciPollLimit = 100;
static constexpr uint32_t EhciAsyncScheduleStatus = 0x8000;
static constexpr uint32_t EhciScheduleStatusMask = 0xc000;

static bool waitForMmioState(IoBase* base, size_t registerOffset, uint32_t mask, uint32_t expected,
                             size_t pollLimit = EhciPollLimit) {
  expected &= mask;
  while (pollLimit) {
    if ((base->read32(registerOffset) & mask) == expected)
      return true;
    --pollLimit;
    Time::delay(1 * Time::Multiplier::Millisecond);
  }
  return (base->read32(registerOffset) & mask) == expected;
}

#define GET_PAGE(param, page, qtdIndex)                                                          \
  do {                                                                                           \
    if ((nBufferPageOffset + nBytes) > ((page) * 0x1000)) {                                      \
      if (va.isMapped(reinterpret_cast<void*>(pBufferPageStart + (page) * 0x1000))) {            \
        physical_uintptr_t phys = 0;                                                             \
        size_t flags = 0;                                                                        \
        va.getMapping(reinterpret_cast<void*>(pBufferPageStart + (page) * 0x1000), phys, flags); \
        (param) = phys >> 12;                                                                    \
      } else {                                                                                   \
        ERROR("EHCI: addTransferToTransaction: Buffer (page " << Dec << (page) << Hex            \
                                                              << ") isn't mapped!");             \
        pQH->pMetaData->bBuildFailed = true;                                                     \
        m_qTDBitmap.clear((qtdIndex));                                                           \
        ByteSet(pqTD, 0, sizeof(qTD));                                                           \
        return;                                                                                  \
      }                                                                                          \
    }                                                                                            \
  } while (0)

Ehci::Ehci(Device* pDev)
    : UsbHub(pDev),
      RequestQueue(MakeConstantString("EHCI")),
      m_pBase(nullptr),
      m_nOpRegsOffset(0),
      m_nPorts(0),
      m_IrqProcessingLock(),
      m_CompletionDeliveries(),
      m_pQHList(nullptr),
      m_pQHListPhys(0),
      m_pFrameList(nullptr),
      m_pFrameListPhys(0),
      m_pqTDList(nullptr),
      m_pqTDListPhys(0),
      m_pCurrentQueueTail(0),
      m_pCurrentQueueHead(0),
      m_EhciMR("Ehci-MR"),
      m_DequeueCount(0),
      m_DequeueStopping(false),
      m_DequeueThread(),
      m_SubmissionOperations(),
      m_CancelOperations(),
      m_CallbackOperations(),
      m_IrqId(0),
      m_InterruptHandlerRegistered(false),
      m_InterruptClosure(0) {
  setSpecificType(String("EHCI"));
}

bool Ehci::initialiseController() {
  // Allocate the pages we need
  if (!PhysicalMemoryManager::instance().allocateRegion(
          m_EhciMR, 4, PhysicalMemoryManager::continuous,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write)) {
    ERROR("USB: EHCI: Couldn't allocate Memory Region!");
    return false;
  }

  uintptr_t virtualBase = reinterpret_cast<uintptr_t>(m_EhciMR.virtualAddress());
  uintptr_t physicalBase = m_EhciMR.physicalAddress();
  m_pQHList = reinterpret_cast<QH*>(virtualBase);
  m_pFrameList = reinterpret_cast<uint32_t*>(virtualBase + 0x2000);
  m_pqTDList = reinterpret_cast<qTD*>(virtualBase + 0x3000);
  m_pQHListPhys = physicalBase;
  m_pFrameListPhys = physicalBase + 0x2000;
  m_pqTDListPhys = physicalBase + 0x3000;

  DoubleWordSet(m_pFrameList, 1, 0x400);

#if X86_COMMON
  uint32_t nPciCmdSts = PciBus::instance().readConfigSpace(this, 1);
#ifdef USB_VERBOSE_DEBUG
  DEBUG_LOG("USB: EHCI: PCI command register: " << (nPciCmdSts & 0xffff));
  DEBUG_LOG("USB: EHCI: PCI status register: " << ((nPciCmdSts & 0xffff0000) >> 16));
#endif
  PciBus::instance().writeConfigSpace(this, 1, nPciCmdSts | 0x4);
#endif

  // Grab the ports
  m_pBase = m_Addresses[0]->m_Io;
  NOTICE("EHCI: Working off: " << (m_Addresses[0]->m_IsIoSpace ? "P" : "MM") << "IO");
  m_Addresses[0]->map();

  uint32_t hccapbase = m_pBase->read32(EHCI_CAPLENGTH);
  uint16_t version = hccapbase >> 16;

  m_nOpRegsOffset = hccapbase & 0xF;
#ifdef USB_VERBOSE_DEBUG
  NOTICE("EHCI operation registers are at offset " << m_nOpRegsOffset);
#endif
  if (m_nOpRegsOffset == 0) {
    // No offset for operational base: this is almost certainly not really
    // a controller.
    return false;
  }

  NOTICE("EHCI controller version " << ((version & 0xFF) >> 8) << "." << (version & 0xFF));

  // Get structural capabilities to determine the number of physical ports
  // we have available to us.
  uint32_t hcsparams = m_pBase->read32(EHCI_HCSPARAMS);
  m_nPorts = hcsparams & 0xF;
  if (!UsbHcd::validEhciRootPortCount(m_nPorts)) {
    ERROR("EHCI: unsupported root-port count " << Dec << m_nPorts << Hex);
    return false;
  }
#ifdef USB_VERBOSE_DEBUG
  NOTICE("EHCI controller has " << Dec << m_nPorts << Hex << " physical ports.");
#endif

  uint32_t hccparams = m_pBase->read32(EHCI_HCCPARAMS);
  uint8_t eecp = (hccparams >> 8) & 0xFF;
  constexpr uint8_t EecpMinimum = 0x40;
  constexpr uint8_t EecpMaximum = 0xfc;
  if (eecp && (eecp < EecpMinimum || eecp > EecpMaximum || (eecp & 0x3))) {
    ERROR("EHCI: EECP pointer is invalid");
    return false;
  }

#ifdef USB_VERBOSE_DEBUG
  DEBUG_LOG("EHCI: Host controller " << (hccparams & 1 ? "does" : "does not")
                                     << " require 64-bit data structures.");
  DEBUG_LOG("      Host controller " << (hccparams & 2 ? "does" : "does not")
                                     << " allow us to use frame lists with "
                                        "anything other than 1024 items in them.");
  DEBUG_LOG("      Host controller " << (hccparams & 4 ? "does" : "does not")
                                     << " support the asynchronous schedule park capability.");
  DEBUG_LOG("      HCCAPBASE is " << hccapbase);
  DEBUG_LOG("      HCCPARAMS is " << hccparams);
  DEBUG_LOG("      HCSPARAMS is " << hcsparams);
  DEBUG_LOG("      EECP is " << eecp);
#endif

#if X86_COMMON
  // Pre-OS to OS handoff
  constexpr size_t EecpSlotCount = (0x100 - EecpMinimum) / 4;
  size_t capabilityBudget = EecpSlotCount;
  uint64_t visitedCapabilities = 0;
  while (eecp) {
    if (!capabilityBudget) {
      ERROR("EHCI: EECP capability chain exceeded PCI config space");
      return false;
    }
    --capabilityBudget;
    const size_t capabilitySlot = (eecp - EecpMinimum) / 4;
    const uint64_t capabilityBit = uint64_t(1) << capabilitySlot;
    if (visitedCapabilities & capabilityBit) {
      ERROR("EHCI: cyclic EECP capability chain");
      return false;
    }
    visitedCapabilities |= capabilityBit;

#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("EHCI: Reading LEGSUP register and checking for BIOS ownership.");
#endif
    const uint32_t dwordOffset = eecp / sizeof(uint32_t);
    uint32_t legsup = PciBus::instance().readConfigSpace(this, dwordOffset);

    if ((legsup & 0xff) == 1) {
      // Perform handoff if necessary
      constexpr uint32_t BiosOwned = 1U << 16;
      constexpr uint32_t OsOwned = 1U << 24;
      if (legsup & BiosOwned) {
#ifdef USB_VERBOSE_DEBUG
        DEBUG_LOG("EHCI: Performing handoff from BIOS to the OS...");
#endif

        // Take ownership of the controller
        legsup |= OsOwned;
        PciBus::instance().writeConfigSpace(this, dwordOffset, legsup);

        // Wait for the BIOS to relinquish control
        constexpr size_t OwnershipPollLimit = 1000;
        size_t ownershipPolls = OwnershipPollLimit;
        while (ownershipPolls && (legsup & BiosOwned)) {
          --ownershipPolls;
          Time::delay(1 * Time::Multiplier::Millisecond);
          legsup = PciBus::instance().readConfigSpace(this, dwordOffset);
        }
        if ((legsup & BiosOwned) || !(legsup & OsOwned)) {
          ERROR(
              "EHCI: BIOS ownership handoff did not complete within "
              "1 second");
          return false;
        }
      }
    }

    eecp = (legsup >> 8) & 0xFF;  // Zero = "end of list"
    if (eecp && (eecp < EecpMinimum || eecp > EecpMaximum || (eecp & 0x3))) {
      ERROR("EHCI: EECP capability chain contains an invalid pointer");
      return false;
    }
  }
#endif

#ifdef USB_VERBOSE_DEBUG
  DEBUG_LOG("USB: EHCI: disabling running schedules");
#endif
  // Disable any running schedules gracefully before halting the controller
  m_pBase->write32(
      m_pBase->read32(m_nOpRegsOffset + EHCI_CMD) & ~(EHCI_CMD_ASYNCLE | EHCI_CMD_PERIODICLE),
      m_nOpRegsOffset + EHCI_CMD);
  if (!waitForMmioState(m_pBase, m_nOpRegsOffset + EHCI_STS, EhciScheduleStatusMask, 0)) {
    ERROR("EHCI: schedules did not stop within 100 ms");
    return false;
  }

  uint32_t status = m_pBase->read32(m_nOpRegsOffset + EHCI_STS);
  if (!(status & EHCI_STS_HALTED)) {
#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("USB: EHCI: pausing controller");
#endif
    // Must halt the controller, it's not yet halted.
    m_pBase->write32(m_pBase->read32(m_nOpRegsOffset + EHCI_CMD) & ~EHCI_CMD_RUN,
                     m_nOpRegsOffset + EHCI_CMD);
    if (!waitForMmioState(m_pBase, m_nOpRegsOffset + EHCI_STS, EHCI_STS_HALTED, EHCI_STS_HALTED)) {
      ERROR("EHCI: controller did not halt within 100 ms");
      return false;
    }
  }

#ifdef USB_VERBOSE_DEBUG
  DEBUG_LOG("USB: EHCI: resetting controller");
#endif
  // Write host controller reset command and wait for it to complete
  m_pBase->write32(EHCI_CMD_HCRES, m_nOpRegsOffset + EHCI_CMD);
  if (!waitForMmioState(m_pBase, m_nOpRegsOffset + EHCI_CMD, EHCI_CMD_HCRES, 0)) {
    ERROR("EHCI: controller reset did not complete within 100 ms");
    return false;
  }
#ifdef USB_VERBOSE_DEBUG
  DEBUG_LOG("USB: EHCI: Reset complete, status: " << m_pBase->read32(m_nOpRegsOffset + EHCI_STS)
                                                  << ".");
#endif

  // The queue and every port token must be live before the interrupt source
  // can publish a port change.
#if THREADS
  RequestQueue::initialise();
  if (getLifecycleState() != RequestQueue::LifecycleState::Accepting) {
    ERROR("EHCI: request queue did not enter the accepting state");
    RequestQueue::destroy();
    return false;
  }
  for (size_t i = 0; i < m_nPorts; ++i) {
    if (!m_PortChanges[i].configure(*this, 0, i)) {
      ERROR("EHCI: could not configure root-port publication " << i);
      RequestQueue::destroy();
      return false;
    }
  }
#endif

  // Do not rely on reset defaults while installing the handler.
  m_pBase->write32(0, m_nOpRegsOffset + EHCI_INTR);
  (void)m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);

// Install the IRQ handler
#if X86_COMMON
  m_IrqId = Machine::instance().getIrqManager()->registerPciIrqHandler(
      static_cast<IrqHandler*>(this), this, IrqPolicy::pciIntxThreaded());
  m_InterruptHandlerRegistered = m_IrqId != 0;
#else
  // InterruptManager pointer removal cannot drain a dispatch that already
  // loaded this handler. Do not advertise teardown safety on that dormant
  // platform path until it has a synchronous registration API.
  ERROR("EHCI requires synchronous IRQ handler lifetime management");
#if THREADS
  RequestQueue::destroy();
#endif
  return false;
#endif
  if (!m_InterruptHandlerRegistered) {
    ERROR("EHCI: could not register interrupt handler");
#if THREADS
    RequestQueue::destroy();
#endif
    return false;
  }
  Machine::instance().getIrqManager()->control(
      getInterruptNumber(), IrqManager::MitigationThreshold,
      7500000 / 64);  // 58 MB/s (480Mbps) in bytes/s, divided by 64 bytes
                      // maximum per control transfer/IRQ

  // Zero the top 64 bits for addresses of EHCI data structures
  m_pBase->write32(0, m_nOpRegsOffset + EHCI_CTRLDSEG);

  // Write the base address of the periodic frame list - all T-bits are set to
  // one
  m_pBase->write32(m_pFrameListPhys, m_nOpRegsOffset + EHCI_PERIODICLP);

  Time::delay(5 * Time::Multiplier::Millisecond);

  // Create a dummy QH and qTD
  m_QHBitmap.set(0);
  m_qTDBitmap.set(0);
  QH* pDummyQH = &m_pQHList[0];
  qTD* pDummyTD = &m_pqTDList[0];
  ByteSet(pDummyQH, 0, sizeof(QH));
  ByteSet(pDummyTD, 0, sizeof(qTD));

  // Configure the dummy TD
  pDummyTD->bNextInvalid = pDummyTD->bAltNextInvalid = 1;

  // Configure the dummy QH
  pDummyQH->pNext = m_pQHListPhys >> 5;
  pDummyQH->nNextType = 1;

  pDummyQH->pQTD = m_pqTDListPhys >> 5;
  pDummyQH->mult = 1;
  pDummyQH->hrcl = 1;

  pDummyQH->pMetaData = new QH::MetaData;
  pDummyQH->pMetaData->pFirstQTD = 0;
  pDummyQH->pMetaData->pLastQTD = pDummyTD;
  pDummyQH->pMetaData->pNext = pDummyQH;
  pDummyQH->pMetaData->pPrev = pDummyQH;

  MemoryCopy(&pDummyQH->overlay, pDummyTD, sizeof(qTD));

  m_pCurrentQueueHead = m_pCurrentQueueTail = pDummyQH;

  // Disable the asynchronous schedule, and wait for it to become disabled
  m_pBase->write32(m_pBase->read32(m_nOpRegsOffset + EHCI_CMD) & ~EHCI_CMD_ASYNCLE,
                   m_nOpRegsOffset + EHCI_CMD);
  if (!waitForMmioState(m_pBase, m_nOpRegsOffset + EHCI_STS, EhciAsyncScheduleStatus, 0)) {
    panic("EHCI asynchronous schedule did not stop within 100 ms");
  }

  // Write the async list head pointer
  m_pBase->write32(m_pQHListPhys, m_nOpRegsOffset + EHCI_ASYNCLP);

  // Set the desired interrupt threshold (frame list size = 4096 bytes)
  m_pBase->write32((m_pBase->read32(m_nOpRegsOffset + EHCI_CMD) & ~0xFF0000) | 0x80000,
                   m_nOpRegsOffset + EHCI_CMD);

  // Turn on the controller
  m_pBase->write32(m_pBase->read32(m_nOpRegsOffset + EHCI_CMD) | EHCI_CMD_RUN,
                   m_nOpRegsOffset + EHCI_CMD);
  if (!waitForMmioState(m_pBase, m_nOpRegsOffset + EHCI_STS, EHCI_STS_HALTED, 0)) {
    panic("EHCI controller did not start within 100 ms");
  }

  m_DequeueThread.adopt(new Thread(Processor::information().getCurrentThread()->getParent(),
                                   threadStub, reinterpret_cast<void*>(this)));
  m_DequeueThread->setName("EHCI dequeue");

  // The schedules, queue metadata, and completion worker are now live.
  // PORTCH remains masked until the initial root-port scan below.
  m_pBase->write32(0x3b, m_nOpRegsOffset + EHCI_INTR);
  (void)m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);

  // Take over the ports
  m_pBase->write32(1, m_nOpRegsOffset + EHCI_CFGFLAG);

  // If it's supported, enable the asynchronous park mode with only one
  // transaction before advancing through the queue.
  if (hccparams & 4) {
    m_pBase->write32(m_pBase->read32(m_nOpRegsOffset + EHCI_CMD) | 0x900,
                     m_nOpRegsOffset + EHCI_CMD);
  }

  // Enable the asynchronous schedule, and wait for it to become enabled
  m_pBase->write32(m_pBase->read32(m_nOpRegsOffset + EHCI_CMD) | EHCI_CMD_ASYNCLE,
                   m_nOpRegsOffset + EHCI_CMD);
  if (!waitForMmioState(m_pBase, m_nOpRegsOffset + EHCI_STS, EhciAsyncScheduleStatus,
                        EhciAsyncScheduleStatus)) {
    panic("EHCI asynchronous schedule did not start within 100 ms");
  }

  // Clear the aggregate before scanning. Any edge after this flush remains
  // pending for the live publication path when PORTCH is enabled below.
  m_pBase->write32(EHCI_STS_PORTCH, m_nOpRegsOffset + EHCI_STS);
  (void)m_pBase->read32(m_nOpRegsOffset + EHCI_STS);

  // Search for ports with devices and initialise them.
  for (size_t i = 0; i < m_nPorts; i++) {
#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("USB: EHCI: Port " << Dec << i << Hex << " - status initially: "
                                 << m_pBase->read32(m_nOpRegsOffset + EHCI_PORTSC + i * 4));
#endif
    // Check for port power
    if (!(m_pBase->read32(m_nOpRegsOffset + EHCI_PORTSC + i * 4) & EHCI_PORTSC_PPOW)) {
      modifyPortControl(m_nOpRegsOffset + EHCI_PORTSC + i * 4, 0, EHCI_PORTSC_PPOW);
      Time::delay(20 * Time::Multiplier::Millisecond);
#ifdef USB_VERBOSE_DEBUG
      DEBUG_LOG("USB: EHCI: Port " << Dec << i << Hex << " - status after power-up: "
                                   << m_pBase->read32(m_nOpRegsOffset + EHCI_PORTSC + i * 4));
#endif
    }

    // Check for an existing reset on the port and request termination
    const size_t portRegister = m_nOpRegsOffset + EHCI_PORTSC + i * 4;
    modifyPortControl(portRegister, EHCI_PORTSC_PRES, 0);
    if (!waitForMmioState(m_pBase, portRegister, EHCI_PORTSC_PRES, 0)) {
      ERROR("EHCI: initial reset on port " << Dec << i << Hex
                                           << " did not clear within "
                                              "100 ms");
      continue;
    }

    constexpr uint32_t ChangeMask = EHCI_PORTSC_CSCH | EHCI_PORTSC_ENCH | EHCI_PORTSC_OCCH;
    const uint32_t portStatus = m_pBase->read32(portRegister);
    const uint32_t acknowledgeMask = portStatus & ChangeMask;
    if (acknowledgeMask) {
      m_pBase->write32(UsbHcd::selectiveW1cValue(portStatus, ChangeMask, acknowledgeMask),
                       portRegister);
      (void)m_pBase->read32(portRegister);
    }

    executeRequest(i);
  }

#if THREADS
  m_pBase->write32(0x3f, m_nOpRegsOffset + EHCI_INTR);
#else
  // Enumerating a device can block and must never run in interrupt context.
  m_pBase->write32(0x3b, m_nOpRegsOffset + EHCI_INTR);
#endif

  return true;
}

Ehci::~Ehci() {
  // Quiesce only the port producer first. Transfer completion IRQs and the
  // dequeue worker must remain live while an active port request drains.
  {
    LockGuard<IrqProcessingLock> transactionGuard(m_IrqProcessingLock);
    m_InterruptClosure = 1;
    if (m_pBase && m_nOpRegsOffset) {
      const uint32_t interrupts = m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);
      m_pBase->write32(interrupts & ~EHCI_STS_PORTCH, m_nOpRegsOffset + EHCI_INTR);
      (void)m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);
    }
  }

  // The PORTCH mask and IRQ serialization above close and drain observe().
  for (size_t i = 0; i < m_nPorts; ++i) {
    m_PortChanges[i].stopAfterQuiesce();
  }
  RequestQueue::destroy();

  // Port enumeration has drained, so no internal producer needs to construct
  // another transfer while the controller is being halted.
  m_SubmissionOperations.closeAndWait();

  {
    LockGuard<Mutex> controllerGuard(m_Mutex);
    {
      LockGuard<IrqProcessingLock> transactionGuard(m_IrqProcessingLock);
      m_InterruptClosure = 2;
      if (m_pBase && m_nOpRegsOffset) {
        m_pBase->write32(0, m_nOpRegsOffset + EHCI_INTR);
        (void)m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);
        const uint32_t pending = m_pBase->read32(m_nOpRegsOffset + EHCI_STS) & 0x3f;
        if (pending) {
          m_pBase->write32(pending, m_nOpRegsOffset + EHCI_STS);
          (void)m_pBase->read32(m_nOpRegsOffset + EHCI_STS);
        }
      }
      m_CallbackOperations.close();
    }

    if (m_pBase && m_nOpRegsOffset) {
      const uint32_t command = m_pBase->read32(m_nOpRegsOffset + EHCI_CMD);
      m_pBase->write32(command & ~EHCI_CMD_RUN, m_nOpRegsOffset + EHCI_CMD);
      (void)m_pBase->read32(m_nOpRegsOffset + EHCI_CMD);

      if (!waitForMmioState(m_pBase, m_nOpRegsOffset + EHCI_STS, EHCI_STS_HALTED,
                            EHCI_STS_HALTED)) {
        panic("EHCI teardown could not establish the DMA halt boundary");
      }

      const uint32_t pending = m_pBase->read32(m_nOpRegsOffset + EHCI_STS) & 0x3f;
      if (pending) {
        m_pBase->write32(pending, m_nOpRegsOffset + EHCI_STS);
        (void)m_pBase->read32(m_nOpRegsOffset + EHCI_STS);
      }
      m_pBase->write32(0, m_nOpRegsOffset + EHCI_ASYNCLP);
      m_pBase->write32(0, m_nOpRegsOffset + EHCI_PERIODICLP);
      (void)m_pBase->read32(m_nOpRegsOffset + EHCI_PERIODICLP);
    }
  }
#if X86_COMMON
  if (m_InterruptHandlerRegistered) {
    if (!Machine::instance().getIrqManager()->unregisterHandler(m_IrqId,
                                                                static_cast<IrqHandler*>(this))) {
      panic(
          "EHCI teardown could not synchronously unregister its IRQ "
          "callback");
    }
    m_IrqId = 0;
    m_InterruptHandlerRegistered = false;
  }
#endif
  m_CallbackOperations.wait();
  m_DequeueStopping = true;
  m_DequeueCount.release();
  m_DequeueThread.join();

  // The IRQ and dequeue worker are the only ordinary callback publishers.
  // Cancellation stays open while their callbacks drain and while teardown
  // callbacks are captured below.
  (void)m_CompletionDeliveries.drainAll();

  List<UsbHcd::CallbackDeliveryQueue::Record*> completions;
  {
    LockGuard<Mutex> controllerGuard(m_Mutex);
    LockGuard<IrqProcessingLock> transactionGuard(m_IrqProcessingLock);

    if (m_pFrameList) {
      DoubleWordSet(m_pFrameList, 1, 0x400);
      for (size_t i = 0; i < 1024; ++i) {
        if (m_FrameBitmap.test(i))
          m_FrameBitmap.clear(i);
      }
    }

    if (m_pQHList) {
      constexpr size_t QhCount = 0x2000 / sizeof(QH);
      for (size_t i = 1; i < QhCount; ++i) {
        if (!m_QHBitmap.test(i))
          continue;

        QH* qh = &m_pQHList[i];
        if (!qh->pMetaData) {
          ByteSet(qh, 0, sizeof(QH));
          m_QHBitmap.clear(i);
          continue;
        }

        if (qh->pMetaData->bPeriodic ||
            qh->pMetaData->completion.state() == UsbHcd::TransferCompletion::State::Idle) {
          reclaimQhLocked(i);
          continue;
        }

        UsbHcd::TransferCompletion::Claim claim;
        if (qh->pMetaData->completion.claimForTeardown(-TransactionError, claim)) {
          captureCompletionLocked(i, qh, claim, completions);
        }
      }

      if (m_QHBitmap.test(0)) {
        QH* dummy = &m_pQHList[0];
        LockGuard<Spinlock> queueGuard(m_QueueListChangeLock);
        dummy->pNext = m_pQHListPhys >> 5;
        dummy->nNextType = 1;
        if (dummy->pMetaData) {
          dummy->pMetaData->pNext = dummy;
          dummy->pMetaData->pPrev = dummy;
        }
        m_pCurrentQueueHead = m_pCurrentQueueTail = dummy;
      }
    }

    if (completions.count())
      m_CompletionDeliveries.publish(completions);
  }

  while (completions.count()) {
    auto* completion = completions.popFront();
    m_CompletionDeliveries.deliver(completion);
  }

  // A cancellation may have stolen a teardown record. Keep cancellation
  // admission open until every such callback, including nested drains, exits.
  (void)m_CompletionDeliveries.drainAll();
  m_CancelOperations.closeAndWait();
  (void)m_CompletionDeliveries.drainAll();
  assert(m_CompletionDeliveries.empty());

  {
    LockGuard<Mutex> controllerGuard(m_Mutex);
    if (m_pQHList) {
      constexpr size_t QhCount = 0x2000 / sizeof(QH);
      for (size_t i = 1; i < QhCount; ++i)
        assert(!m_QHBitmap.test(i));

      if (m_QHBitmap.test(0)) {
        delete m_pQHList[0].pMetaData;
        ByteSet(&m_pQHList[0], 0, sizeof(QH));
        m_QHBitmap.clear(0);
      }
    }

    if (m_pqTDList) {
      constexpr size_t QtdCount = 0x1000 / sizeof(qTD);
      for (size_t i = 1; i < QtdCount; ++i)
        assert(!m_qTDBitmap.test(i));
      if (m_qTDBitmap.test(0)) {
        ByteSet(&m_pqTDList[0], 0, sizeof(qTD));
        m_qTDBitmap.clear(0);
      }
    }

    for (size_t i = 0; i < 1024; ++i)
      assert(!m_FrameBitmap.test(i));
    m_pCurrentQueueHead = nullptr;
    m_pCurrentQueueTail = nullptr;
  }
}

static int threadStub(void* p) {
  Ehci* pEhci = reinterpret_cast<Ehci*>(p);
  pEhci->doDequeue();
  return 0;
}

namespace {
struct EhciCompletionCleanup {
  Ehci* controller;
  size_t transaction;
  size_t generation;
};
}  // namespace

void Ehci::releaseQtdChainLocked(QH* qh) {
  if (!qh || !qh->pMetaData || !qh->pMetaData->pFirstQTD)
    return;

  constexpr size_t QtdCount = 0x1000 / sizeof(qTD);
  size_t qtdIndex = INDEX_FROM_QTD(qh->pMetaData->pFirstQTD);
  size_t budget = QtdCount;
  bool foundLast = false;
  while (budget) {
    --budget;
    if (qtdIndex >= QtdCount)
      panic("EHCI: invalid qTD index during reclamation");

    qTD* qtd = &m_pqTDList[qtdIndex];
    const bool last = qtd->bNextInvalid;
    size_t nextIndex = 0;
    if (!last)
      nextIndex = ((qtd->pNext << 5) & 0xFFF) / sizeof(qTD);

    if (m_qTDBitmap.test(qtdIndex))
      m_qTDBitmap.clear(qtdIndex);
    ByteSet(qtd, 0, sizeof(qTD));

    if (last) {
      foundLast = true;
      break;
    }
    if (nextIndex == qtdIndex)
      panic("EHCI: circular qTD chain during reclamation");
    qtdIndex = nextIndex;
  }
  if (!foundLast)
    panic("EHCI: qTD reclamation exceeded its descriptor budget");

  qh->pMetaData->pFirstQTD = nullptr;
  qh->pMetaData->pLastQTD = nullptr;
  qh->pQTD = 0;
  ByteSet(&qh->overlay, 0, sizeof(qTD));
}

void Ehci::reclaimQhLocked(size_t transaction) {
  if (!m_pQHList || !m_QHBitmap.test(transaction))
    return;

  QH* qh = &m_pQHList[transaction];
  if (qh->pMetaData) {
    releaseQtdChainLocked(qh);
    delete qh->pMetaData;
  }
  ByteSet(qh, 0, sizeof(QH));
  m_QHBitmap.clear(transaction);
}

void Ehci::captureCompletionLocked(size_t transaction, QH* qh,
                                   const UsbHcd::TransferCompletion::Claim& claim,
                                   List<UsbHcd::CallbackDeliveryQueue::Record*>& completions) {
  assert(qh && qh->pMetaData && !qh->pMetaData->bPeriodic);
  assert(claim.generation == qh->pMetaData->completion.generation());

  releaseQtdChainLocked(qh);
  qh->pMetaData->bIgnore = true;
  auto* cleanup = new EhciCompletionCleanup{this, transaction, claim.generation};
  completions.pushBack(m_CompletionDeliveries.create({transaction, claim.generation},
                                                     claim.callback, claim.parameter, claim.result,
                                                     finishDeferredCompletion, cleanup));
}

void Ehci::finishDeferredCompletion(void* context) {
  auto* cleanup = reinterpret_cast<EhciCompletionCleanup*>(context);
  Ehci* controller = cleanup->controller;
  {
    LockGuard<Mutex> guard(controller->m_Mutex);
    const size_t transaction = cleanup->transaction;
    if (controller->m_QHBitmap.test(transaction)) {
      QH* qh = &controller->m_pQHList[transaction];
      if (qh->pMetaData && qh->pMetaData->completion.generation() == cleanup->generation) {
        controller->reclaimQhLocked(transaction);
      }
    }
  }
  delete cleanup;
}

void Ehci::doDequeue() {
  TerminationDeferral workerLifetime;
  while (true) {
    const bool woken = m_DequeueCount.acquireForCompletion();
    (void)woken;
    if (m_DequeueStopping)
      return;

    List<UsbHcd::CallbackDeliveryQueue::Record*> completions;
    {
      // Absolutely cannot have queue insertions during a dequeue.
      LockGuard<Mutex> guard(m_Mutex);
      LockGuard<IrqProcessingLock> transactionGuard(m_IrqProcessingLock);

      for (size_t i = 1; i < 0x2000 / sizeof(QH); i++) {
        if (!m_QHBitmap.test(i))
          continue;

        QH* pQH = &m_pQHList[i];

        // Is this QH valid?
        if (!pQH->pMetaData) {
#ifdef USB_VERBOSE_DEBUG
          DEBUG_LOG("Not performing dequeue on QH #" << Dec << i << Hex
                                                     << " as it's not even initialised.");
#endif
          continue;
        }

        // Is this QH even linked!?
        if (!pQH->pMetaData->bIgnore) {
#ifdef USB_VERBOSE_DEBUG
          DEBUG_LOG("Not performing dequeue on QH #" << Dec << i << Hex
                                                     << " as it's still active.");
#endif
          continue;
        }

        if (pQH->pMetaData->bPeriodic)
          continue;

        UsbHcd::TransferCompletion::Claim claim;
        if (!pQH->pMetaData->completion.claimCaptured(claim))
          continue;
        captureCompletionLocked(i, pQH, claim, completions);

#ifdef USB_VERBOSE_DEBUG
        DEBUG_LOG("Dequeue for QH #" << Dec << i << Hex << ".");
#endif
      }

      if (completions.count())
        m_CompletionDeliveries.publish(completions);
    }

    while (completions.count()) {
      auto* completion = completions.popFront();
      m_CompletionDeliveries.deliver(completion);
    }
  }
}

#if X86_COMMON
IrqDisposition Ehci::irq(irq_id_t number)
#else
void Ehci::interrupt(size_t number, InterruptState& state)
#endif
{
  (void)number;
#if !X86_COMMON
  (void)state;
#endif

  OperationBarrier::Lease callback;
  if (!m_CallbackOperations.tryAcquire(callback)) {
    return
#if X86_COMMON
        IrqDisposition::Quiesced
#endif
        ;
  }
#if X86_COMMON
  List<UsbHcd::CallbackDeliveryQueue::Record*> completions;
  {
#endif
    LockGuard<IrqProcessingLock> transactionGuard(m_IrqProcessingLock);

    if (m_InterruptClosure == 2) {
      return
#if X86_COMMON
          IrqDisposition::Quiesced
#endif
          ;
    }

    /*
    uint32_t pciStatus = PciBus::instance().readConfigSpace(this, 1) >> 16;
    if(!(pciStatus & 8))
    {
            NOTICE_NOLOCK("EHCI: IRQ fired, but not for us [" << pciStatus <<
    "]"); return #if X86_COMMON false #endif
        ;
    }
    */

    uint32_t nStatus =
        m_pBase->read32(m_nOpRegsOffset + EHCI_STS) & m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);

    if (!nStatus) {
      WARNING_NOLOCK("EHCI: unwanted IRQ?");
      return
#if X86_COMMON
          IrqDisposition::NotHandled  // Shared IRQ: another device
#endif
          ;
    }

    // Clear and flush the aggregate before scanning. A later edge will
    // relatch PORTCH instead of being erased after its port was already
    // scanned.
#if THREADS
    if (nStatus & EHCI_STS_PORTCH) {
      m_pBase->write32(EHCI_STS_PORTCH, m_nOpRegsOffset + EHCI_STS);
      (void)m_pBase->read32(m_nOpRegsOffset + EHCI_STS);
    }
#endif

    // ACK non-port causes early.
    const uint32_t immediateStatus = nStatus & ~EHCI_STS_PORTCH;
    if (immediateStatus) {
      m_pBase->write32(immediateStatus, m_nOpRegsOffset + EHCI_STS);
      (void)m_pBase->read32(m_nOpRegsOffset + EHCI_STS);
    }

    if (nStatus & 0x16) {
      NOTICE_NOLOCK("EHCI: Unusual IRQ, status is " << nStatus);
    }

#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG_NOLOCK("EHCI IRQ " << nStatus);
#endif
#if THREADS
    if (nStatus & EHCI_STS_PORTCH) {
      constexpr uint32_t ChangeMask = EHCI_PORTSC_CSCH | EHCI_PORTSC_ENCH | EHCI_PORTSC_OCCH;
      for (size_t i = 0; i < m_nPorts; i++) {
        const size_t portRegister = m_nOpRegsOffset + EHCI_PORTSC + i * 4;
        const uint32_t portStatus = m_pBase->read32(portRegister);
        uint32_t acknowledgeMask = portStatus & (EHCI_PORTSC_ENCH | EHCI_PORTSC_OCCH);

        if (portStatus & EHCI_PORTSC_CSCH) {
          if (deferConnectionChangeIfSuppressed(i)) {
            acknowledgeMask |= EHCI_PORTSC_CSCH;
          } else {
            const auto observation = m_PortChanges[i].observe();
            const bool accepted = UsbHcd::PortChangeRequest::canAcknowledge(observation.result);
            assert(accepted);
            if (!accepted) {
              // Queue acceptance is established before PORTCH is
              // enabled and is retained until after PORTCH
              // drains. Preserve CSC and fail closed if that
              // invariant ever regresses in a non-asserting
              // build.
              m_InterruptClosure = 1;
              const uint32_t interrupts = m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);
              m_pBase->write32(interrupts & ~EHCI_STS_PORTCH, m_nOpRegsOffset + EHCI_INTR);
              (void)m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);
              continue;
            }

            acknowledgeMask |= EHCI_PORTSC_CSCH;
            m_DeferredPortChanges.defer(i, observation.generation);
          }
        }

        if (acknowledgeMask) {
          m_pBase->write32(UsbHcd::selectiveW1cValue(portStatus, ChangeMask, acknowledgeMask),
                           portRegister);
          // Flush posted MMIO before allowing the worker to sample.
          (void)m_pBase->read32(portRegister);
        }
      }

      for (size_t i = 0; i < m_nPorts; ++i) {
        const size_t generation = m_DeferredPortChanges.release(i);
        if (generation) {
          m_PortChanges[i].acknowledge(generation);
        }
      }
    }
#endif

    // Because there's no IOC for *every* transfer, we need to handle errors
    // that occur before the last transfer. These will create an error
    // status only.
    if (nStatus & (EHCI_STS_INT | EHCI_STS_ERR)) {
      for (size_t i = 1; i < 128; i++) {
        if (!m_QHBitmap.test(i))
          continue;

        QH* pQH = &m_pQHList[i];
        if (!pQH->pMetaData)  // This QH isn't actually ready to be
                              // handled yet.
          continue;
        if (!(pQH->pMetaData->pPrev && pQH->pMetaData->pNext))  // This QH isn't actually linked yet
          continue;
        if (pQH->pMetaData->bIgnore)
          continue;
        if (!(pQH->pMetaData->pFirstQTD && pQH->pMetaData->pLastQTD))
          continue;

        bool bPeriodic = pQH->pMetaData->bPeriodic;

        size_t nQTDIndex = INDEX_FROM_QTD(pQH->pMetaData->pFirstQTD);
        constexpr size_t QtdCount = 0x1000 / sizeof(qTD);
        size_t qtdBudget = QtdCount;
        while (qtdBudget) {
          --qtdBudget;
          if (nQTDIndex >= QtdCount) {
            ERROR_NOLOCK("EHCI: QH #" << Dec << i << Hex << " has an out-of-range qTD pointer");
            break;
          }

          qTD* pqTD = &m_pqTDList[nQTDIndex];

          if (pqTD->nStatus != 0x80) {
            ssize_t nResult;
            if ((pqTD->nStatus & 0x7c) || (nStatus & EHCI_STS_ERR)) {
#ifdef USB_VERBOSE_DEBUG
              ERROR_NOLOCK(((nStatus & EHCI_STS_ERR) ? "USB" : "qTD") << " ERROR!");
              ERROR_NOLOCK("qTD Status: " << pqTD->nStatus
                                          << " [overlay status=" << pQH->overlay.nStatus << "]");
              ERROR_NOLOCK("qTD Error Counter: " << pqTD->nErr << " [overlay counter="
                                                 << pQH->overlay.nErr << "]");
              ERROR_NOLOCK("QH NAK counter: " << pqTD->res1
                                              << " [overlay count=" << pQH->overlay.res1 << "]");
              ERROR_NOLOCK("qTD PID: " << pqTD->nPid << ".");
#endif
              nResult = -pqTD->getError();
            } else {
              nResult = pqTD->nBufferSize - pqTD->nBytes;
              pQH->pMetaData->nTotalBytes += nResult;
            }
#ifdef USB_VERBOSE_DEBUG
            DEBUG_LOG_NOLOCK("qTD #"
                             << Dec << nQTDIndex << Hex << " [from QH #" << Dec << i << Hex
                             << "] DONE: " << Dec << pQH->nAddress << ":" << pQH->nEndpoint << " "
                             << (pqTD->nPid == 0
                                     ? "OUT"
                                     : (pqTD->nPid == 1 ? "IN" : (pqTD->nPid == 2 ? "SETUP" : "")))
                             << " " << nResult << Hex);
#endif

            // Last qTD or error condition?
            if ((nResult < 0) || (pqTD == pQH->pMetaData->pLastQTD)) {
              const ssize_t completionResult = nResult < 0 ? nResult : pQH->pMetaData->nTotalBytes;
              const bool ownsCompletion =
                  bPeriodic || pQH->pMetaData->completion.captureNatural(completionResult);

              if (!bPeriodic && ownsCompletion) {
                // Ensure the list doesn't change as we modify
                // it
                m_QueueListChangeLock.acquire();  // Atomic operation

                // Was the reclaim head bit set?
                if (pQH->hrcl)
                  pQH->pMetaData->pNext->hrcl = 1;  // Make sure there's always a
                                                    // reclaim head

                // This queue head is done, dequeue.
                QH* pPrev = pQH->pMetaData->pPrev;
                QH* pNext = pQH->pMetaData->pNext;

                // Main non-hardware linked list update
                pPrev->pMetaData->pNext = pNext;
                pNext->pMetaData->pPrev = pPrev;

                // Hardware linked list update
                pPrev->pNext = pQH->pNext;

                // Update the tail pointer if we need to
                if (pQH == m_pCurrentQueueTail) {
                  m_pCurrentQueueTail = pPrev;
                }

                // Interrupt on Async Advance Doorbell - will
                // run the dequeue thread to clear bits in the
                // QH and qTD bitmaps
                size_t cmdReg = m_pBase->read32(m_nOpRegsOffset + EHCI_CMD);
                m_pBase->write32(cmdReg | (1 << 6), m_nOpRegsOffset + EHCI_CMD);

                // Now ready for dequeue.
                pQH->pMetaData->bIgnore = true;

                m_QueueListChangeLock.release();
              }

              if (bPeriodic && pQH->pMetaData->pCallback) {
#if X86_COMMON
                completions.pushBack(m_CompletionDeliveries.create(
                    {i, pQH->pMetaData->periodicGeneration}, pQH->pMetaData->pCallback,
                    pQH->pMetaData->pParam, completionResult));
#else
              pQH->pMetaData->pCallback(pQH->pMetaData->pParam, completionResult);
#endif
              }
            }
            // Interrupt qTDs need constant refresh
            if (bPeriodic) {
              pqTD->nStatus = 0x80;
              pqTD->nBytes = pqTD->nBufferSize;
              pqTD->nPage = 0;
              // pqTD->nOffset =
              // pQH->pMetaData->nBufferOffset%0x1000;
              pqTD->nErr = 0;
              // pqTD->pPage0 = m_pTransferPagesPhys>>12;
              // pqTD->pPage1 = (m_pTransferPagesPhys +
              // 0x1000)>>12; pqTD->pPage2 = (m_pTransferPagesPhys
              // + 0x2000)>>12; pqTD->pPage3 =
              // (m_pTransferPagesPhys + 0x3000)>>12; pqTD->pPage4
              // = (m_pTransferPagesPhys + 0x4000)>>12;
              MemoryCopy(&pQH->overlay, pqTD, sizeof(qTD));
            }
          }

          size_t oldIndex = nQTDIndex;

          if (pqTD->bNextInvalid)
            break;
          else
            nQTDIndex = ((pqTD->pNext << 5) & 0xFFF) / sizeof(qTD);

          if (nQTDIndex == oldIndex) {
            ERROR_NOLOCK("EHCI: QH #" << Dec << i << Hex
                                      << "'s qTD list is invalid - circular reference!");
            break;
          } else if (pqTD->pNext == 0) {
            ERROR_NOLOCK("EHCI: QH #" << Dec << i << Hex
                                      << "'s qTD list is invalid - null pNext "
                                         "pointer (and T bit not set)!");
            break;
          }

          if (!qtdBudget) {
            ERROR_NOLOCK("EHCI: QH #" << Dec << i << Hex << " exceeded the qTD scan budget");
            break;
          }
        }
      }
    }

    if (nStatus & EHCI_STS_ASYNCADVANCE) {
      m_DequeueCount.release();
    }

#if X86_COMMON
    if (completions.count())
      m_CompletionDeliveries.publish(completions);
  }

  while (completions.count()) {
    auto* completion = completions.popFront();
    m_CompletionDeliveries.deliver(completion);
  }
  return IrqDisposition::Handled;
#endif
}

void Ehci::addTransferToTransaction(uintptr_t nTransaction, bool bToggle, UsbPid pid,
                                    uintptr_t pBuffer, size_t nBytes) {
  OperationBarrier::Lease submission;
  if (!m_SubmissionOperations.tryAcquire(submission))
    return;
  addTransferToTransactionAdmitted(nTransaction, bToggle, pid, pBuffer, nBytes);
}

void Ehci::addTransferToTransactionAdmitted(uintptr_t nTransaction, bool bToggle, UsbPid pid,
                                            uintptr_t pBuffer, size_t nBytes) {
  LockGuard<Mutex> guard(m_Mutex);
  constexpr size_t QhCount = 0x2000 / sizeof(QH);
  if (nTransaction >= QhCount || !m_QHBitmap.test(nTransaction)) {
    ERROR("EHCI: addTransferToTransaction: invalid transaction");
    return;
  }

  QH* pQH = &m_pQHList[nTransaction];
  if (!pQH->pMetaData || pQH->pMetaData->bBuildFailed ||
      pQH->pMetaData->completion.state() != UsbHcd::TransferCompletion::State::Idle) {
    ERROR("EHCI: addTransferToTransaction: transaction is not building");
    return;
  }

  const size_t nIndex = m_qTDBitmap.getFirstClear();
  if (nIndex >= (0x1000 / sizeof(qTD))) {
    ERROR("USB: EHCI: qTD space full");
    pQH->pMetaData->bBuildFailed = true;
    return;
  }
  m_qTDBitmap.set(nIndex);

  // Grab the qTD pointer we're going to set up now
  qTD* pqTD = &m_pqTDList[nIndex];
  ByteSet(pqTD, 0, sizeof(qTD));

  // There's nothing after us for now
  pqTD->bNextInvalid = 1;
  pqTD->bAltNextInvalid = 1;

  // PID for the transfer
  switch (pid) {
    case UsbPidOut:
      pqTD->nPid = 0;
      break;
    case UsbPidIn:
      pqTD->nPid = 1;
      break;
    case UsbPidSetup:
      pqTD->nPid = 2;
      break;
    default:
      pqTD->nPid = 3;
  };

  // Active, we want an interrupt on completion, and reset the error counter
  pqTD->nStatus = 0x80;
  pqTD->bIoc = 0;  // Interrupt only on last TD
  pqTD->nErr = 3;  // Up to 3 retries of this transaction

  // Set up the transfer
  pqTD->nBytes = nBytes;
  pqTD->nBufferSize = nBytes;
  pqTD->bDataToggle = bToggle;

  if (nBytes) {
    // Configure transfer pages
    uintptr_t nBufferPageOffset = pBuffer & 0xFFF, pBufferPageStart = pBuffer & ~0xFFF;
    pqTD->nOffset = nBufferPageOffset;

    if (nBufferPageOffset + nBytes >= 0x5000) {
      ERROR(
          "EHCI: addTransferToTransaction: Too many bytes for a single "
          "transaction!");
      pQH->pMetaData->bBuildFailed = true;
      m_qTDBitmap.clear(nIndex);
      ByteSet(pqTD, 0, sizeof(qTD));
      return;
    }

    VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
    GET_PAGE(pqTD->pPage0, 0, nIndex);
    GET_PAGE(pqTD->pPage1, 1, nIndex);
    GET_PAGE(pqTD->pPage2, 2, nIndex);
    GET_PAGE(pqTD->pPage3, 3, nIndex);
    GET_PAGE(pqTD->pPage4, 4, nIndex);
  }

  // Add our qTD to the transaction.
  if (pQH->pMetaData->pLastQTD) {
    pQH->pMetaData->pLastQTD->pNext = PHYS_QTD(nIndex) >> 5;
    pQH->pMetaData->pLastQTD->bNextInvalid = 0;

    if (pQH->pMetaData->pLastQTD == pQH->pMetaData->pFirstQTD) {
      pQH->overlay.pNext = pQH->pMetaData->pLastQTD->pNext;
      pQH->overlay.bNextInvalid = pQH->pMetaData->pLastQTD->bNextInvalid;
    }
  } else {
    pQH->pMetaData->pFirstQTD = pqTD;
    pQH->pQTD = PHYS_QTD(nIndex) >> 5;
    MemoryCopy(&pQH->overlay, pqTD, sizeof(qTD));
  }
  pQH->pMetaData->pLastQTD = pqTD;
}

uintptr_t Ehci::createTransaction(UsbEndpoint endpointInfo) {
  OperationBarrier::Lease submission;
  if (!m_SubmissionOperations.tryAcquire(submission))
    return static_cast<uintptr_t>(-1);
  return createTransactionAdmitted(endpointInfo);
}

uintptr_t Ehci::createTransactionAdmitted(UsbEndpoint endpointInfo) {
  LockGuard<Mutex> guard(m_Mutex);
  const size_t nIndex = m_QHBitmap.getFirstClear();
  if (nIndex >= (0x2000 / sizeof(QH))) {
    ERROR("USB: EHCI: QH space full");
    return static_cast<uintptr_t>(-1);
  }
  m_QHBitmap.set(nIndex);

  QH* pQH = &m_pQHList[nIndex];
  ByteSet(pQH, 0, sizeof(QH));

  // The pointer to the next QH gets set in doAsync
  pQH->nNextType = 1;

  // NAK counter reload = 15
  pQH->nNakReload = 15;

  // Head of the reclaim list
  pQH->hrcl = true;

  // LS/FS handling
  pQH->nHubAddress = endpointInfo.speed != HighSpeed ? endpointInfo.nHubAddress : 0;
  pQH->nHubPort = endpointInfo.speed != HighSpeed ? endpointInfo.nHubPort : 0;
  pQH->bControlEndpoint = (endpointInfo.speed != HighSpeed) && !endpointInfo.nEndpoint;

  // Data toggle controlled by qTD
  pQH->bDataToggleSrc = 1;

  // Device address and speed
  pQH->nAddress = endpointInfo.nAddress;
  pQH->nSpeed = endpointInfo.speed;

  // Endpoint number and maximum packet size
  pQH->nEndpoint = endpointInfo.nEndpoint;
  pQH->nMaxPacketSize = endpointInfo.nMaxPacketSize;

  // Bandwidth multiplier - number of transactions that can be performed in a
  // microframe
  pQH->mult = 1;

  pQH->pMetaData = new QH::MetaData();

  // Complete
  return nIndex;
}

bool Ehci::doAsync(uintptr_t nTransaction, void (*pCallback)(uintptr_t, ssize_t),
                   uintptr_t pParam) {
  OperationBarrier::Lease submission;
  if (!m_SubmissionOperations.tryAcquire(submission))
    return false;

  LockGuard<Mutex> guard(m_Mutex);
  constexpr size_t QhCount = 0x2000 / sizeof(QH);
  if ((nTransaction == static_cast<uintptr_t>(-1)) || nTransaction >= QhCount ||
      !m_QHBitmap.test(nTransaction)) {
    ERROR("EHCI: doAsync: didn't get a valid transaction id [" << nTransaction << "].");
    return false;
  }

  QH* pQH = &m_pQHList[nTransaction];
  if (pQH->pMetaData &&
      pQH->pMetaData->completion.state() != UsbHcd::TransferCompletion::State::Idle) {
    ERROR("EHCI: doAsync: transaction is already submitted");
    return false;
  }
  if (!pQH->pMetaData || pQH->pMetaData->bBuildFailed || !pQH->pMetaData->pLastQTD ||
      pQH->pMetaData->bPeriodic) {
    ERROR("EHCI: doAsync: transaction could not be submitted [" << nTransaction << "].");
    reclaimQhLocked(nTransaction);
    return false;
  }

  if (!m_pCurrentQueueTail || !m_pCurrentQueueHead || m_InterruptClosure >= 2) {
    ERROR("EHCI: asynchronous schedule is closed");
    reclaimQhLocked(nTransaction);
    return false;
  }

  LockGuard<IrqProcessingLock> transactionGuard(m_IrqProcessingLock);
  pQH->pMetaData->pLastQTD->bIoc = 1;

  // Only one transaction on this QH?
  if (pQH->pMetaData->pFirstQTD == pQH->pMetaData->pLastQTD) {
    // Update IOC bit in Transfer Overlay as well (as we have just changed)
    pQH->overlay.bIoc = 1;
  }

#ifdef USB_VERBOSE_DEBUG
  DEBUG_LOG("START #" << Dec << nTransaction << Hex << " " << Dec << pQH->nAddress << ":"
                      << pQH->nEndpoint << Hex);
#endif

  // This QH is NOT the queue head. If we leave this set to one, and the
  // reclaim bit is set, the controller will think it's executed a full
  // circle, when in fact it's only partway there.
  pQH->hrcl = 0;

  const size_t queueHeadIndex =
      (reinterpret_cast<uintptr_t>(m_pCurrentQueueHead) & 0xFFF) / sizeof(QH);
  pQH->pNext = (m_pQHListPhys + (queueHeadIndex * sizeof(QH))) >> 5;
  pQH->pMetaData->pNext = m_pCurrentQueueHead;
  pQH->pMetaData->pPrev = m_pCurrentQueueTail;
  QH* pOldTail = m_pCurrentQueueTail;

  {
    LockGuard<Spinlock> queueGuard(m_QueueListChangeLock);

    // Arming is the final software transition before the old tail makes the
    // QH visible to the controller. There are no fallible paths after it.
    pQH->pMetaData->completion.arm(pCallback, pParam, m_CompletionDeliveries.nextGeneration());
    m_pCurrentQueueTail = pQH;
    pOldTail->pNext = (m_pQHListPhys + (nTransaction * sizeof(QH))) >> 5;
    pOldTail->nNextType = 1;
    pOldTail->pMetaData->pNext = pQH;
  }

  m_pCurrentQueueHead->hrcl = 1;
  return true;
}

void Ehci::cancelAsyncAndDrain(uintptr_t nTransaction, void (*pCallback)(uintptr_t, ssize_t),
                               uintptr_t pParam) {
  OperationBarrier::Lease cancellation;
  if (!m_CancelOperations.tryAcquire(cancellation))
    return;

  List<UsbHcd::CallbackDeliveryQueue::Record*> completions;
  bool drainDelivery = false;
  UsbHcd::CallbackDeliveryQueue::Key deliveryKey = {0, 0};

  {
    LockGuard<Mutex> guard(m_Mutex);

    const bool teardownHalted = m_InterruptClosure >= 2;
    uint32_t savedInterrupts = 0;
    uint32_t savedCommand = 0;
    if (!teardownHalted && m_pBase && m_nOpRegsOffset) {
      savedInterrupts = m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);
      savedCommand = m_pBase->read32(m_nOpRegsOffset + EHCI_CMD);
      m_pBase->write32(0, m_nOpRegsOffset + EHCI_INTR);
      m_pBase->write32(savedCommand & ~EHCI_CMD_RUN, m_nOpRegsOffset + EHCI_CMD);
      if (!waitForMmioState(m_pBase, m_nOpRegsOffset + EHCI_STS, EHCI_STS_HALTED,
                            EHCI_STS_HALTED)) {
        panic(
            "EHCI cancellation could not establish the DMA halt "
            "boundary");
      }
    }

    {
      LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);

      constexpr size_t QhCount = 0x2000 / sizeof(QH);
      if ((nTransaction != static_cast<uintptr_t>(-1)) && nTransaction < QhCount &&
          m_QHBitmap.test(nTransaction)) {
        QH* pQH = &m_pQHList[nTransaction];
        QH::MetaData* metadata = pQH->pMetaData;
        if (metadata && !metadata->bPeriodic) {
          UsbHcd::TransferCompletion::Claim claim;
          const auto disposition =
              metadata->completion.claimCancellation(pCallback, pParam, -TransactionError, claim);
          if (disposition == UsbHcd::TransferCompletion::CancellationDisposition::Claimed) {
            if (!metadata->bIgnore) {
              LockGuard<Spinlock> queueGuard(m_QueueListChangeLock);
              QH* pPrev = metadata->pPrev;
              QH* pNext = metadata->pNext;
              if (pPrev && pNext) {
                if (pQH->hrcl)
                  pNext->hrcl = 1;
                pPrev->pMetaData->pNext = pNext;
                pNext->pMetaData->pPrev = pPrev;
                pPrev->pNext = pQH->pNext;
                if (pQH == m_pCurrentQueueTail)
                  m_pCurrentQueueTail = pPrev;
              }
              metadata->bIgnore = true;
            }
            deliveryKey = {nTransaction, claim.generation};
            captureCompletionLocked(nTransaction, pQH, claim, completions);
          } else if (disposition ==
                     UsbHcd::TransferCompletion::CancellationDisposition::DrainPublished) {
            deliveryKey = {nTransaction, claim.generation};
            drainDelivery = true;
            assert(m_CompletionDeliveries.contains(deliveryKey));
          }
        }
      }

      if (completions.count())
        m_CompletionDeliveries.publish(completions);
    }

    if (!teardownHalted && m_pBase && m_nOpRegsOffset) {
      m_pBase->write32(savedCommand, m_nOpRegsOffset + EHCI_CMD);
      if (savedCommand & EHCI_CMD_RUN) {
        if (!waitForMmioState(m_pBase, m_nOpRegsOffset + EHCI_STS, EHCI_STS_HALTED, 0)) {
          panic(
              "EHCI cancellation could not establish the DMA "
              "restart boundary");
        }
      }
      {
        LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);
        uint32_t restoredInterrupts = savedInterrupts;
        if (m_InterruptClosure == 1)
          restoredInterrupts &= ~EHCI_STS_PORTCH;
        m_pBase->write32(restoredInterrupts, m_nOpRegsOffset + EHCI_INTR);
        (void)m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);
      }
    }
  }

  if (completions.count()) {
    auto* completion = completions.popFront();
    (void)m_CompletionDeliveries.drain(deliveryKey);
    m_CompletionDeliveries.deliver(completion);
  } else if (drainDelivery)
    (void)m_CompletionDeliveries.drain(deliveryKey);
}

void Ehci::addInterruptInHandler(UsbEndpoint endpointInfo, uintptr_t pBuffer, uint16_t nBytes,
                                 void (*pCallback)(uintptr_t, ssize_t), uintptr_t pParam) {
  OperationBarrier::Lease submission;
  if (!m_SubmissionOperations.tryAcquire(submission))
    return;

  // Find an empty frame entry
  size_t nFrameIndex = 0;
  {
    LockGuard<Mutex> guard(m_Mutex);
    nFrameIndex = m_FrameBitmap.getFirstClear();
    if (nFrameIndex >= 1024) {
      ERROR("USB: EHCI: Frame list full");
      return;
    }
    m_FrameBitmap.set(nFrameIndex);
  }

  // Create a new transaction
  uintptr_t nTransaction = createTransactionAdmitted(endpointInfo);
  if (nTransaction == static_cast<uintptr_t>(-1)) {
    LockGuard<Mutex> guard(m_Mutex);
    m_FrameBitmap.clear(nFrameIndex);
    return;
  }

  // Get the QH and set the periodic flag
  {
    LockGuard<Mutex> guard(m_Mutex);
    m_pQHList[nTransaction].pMetaData->bPeriodic = true;
  }

  // Add a single transfer to the transaction
  addTransferToTransactionAdmitted(nTransaction, false, UsbPidIn, pBuffer, nBytes);

  // Add the QH to the frame list
  {
    LockGuard<Mutex> guard(m_Mutex);
    QH* pQH = &m_pQHList[nTransaction];
    if (!pQH->pMetaData || pQH->pMetaData->bBuildFailed || !pQH->pMetaData->pLastQTD ||
        m_InterruptClosure >= 2) {
      ERROR("USB: EHCI: Couldn't add interrupt transfer!");
      reclaimQhLocked(nTransaction);
      m_FrameBitmap.clear(nFrameIndex);
      return;
    }

    LockGuard<IrqProcessingLock> transactionGuard(m_IrqProcessingLock);
    pQH->pMetaData->pLastQTD->nErr = 0;
    pQH->pMetaData->pCallback = pCallback;
    pQH->pMetaData->pParam = pParam;
    pQH->pMetaData->periodicGeneration = m_CompletionDeliveries.nextGeneration();
    m_pFrameList[nFrameIndex] = (m_pQHListPhys + nTransaction * sizeof(QH)) | 2;
  }
}

void Ehci::replaySuppressedConnectionChange(size_t port) {
#if THREADS
  if (port >= m_nPorts) {
    ERROR("EHCI: invalid suppressed root-port replay " << Dec << port);
    return;
  }

  LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);
  // Teardown stops publication before its active enumeration worker returns.
  if (m_InterruptClosure) {
    return;
  }
  const auto observation = m_PortChanges[port].observe();
  const bool accepted = UsbHcd::PortChangeRequest::canAcknowledge(observation.result);
  if (accepted) {
    m_PortChanges[port].acknowledge(observation.generation);
    return;
  }

  m_InterruptClosure = 1;
  const uint32_t interrupts = m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);
  m_pBase->write32(interrupts & ~EHCI_STS_PORTCH, m_nOpRegsOffset + EHCI_INTR);
  (void)m_pBase->read32(m_nOpRegsOffset + EHCI_INTR);
  ERROR("EHCI: live suppressed root-port replay could not be published");
  assert(false);
#else
  (void)port;
#endif
}

void Ehci::modifyPortControl(size_t portRegister, uint32_t clearMask, uint32_t setMask) {
  LockGuard<IrqProcessingLock> irqGuard(m_IrqProcessingLock);
  uint32_t control = portControlValue(m_pBase->read32(portRegister));
  control &= ~clearMask;
  control |= portControlValue(setMask);
  m_pBase->write32(control, portRegister);
  (void)m_pBase->read32(portRegister);
}

bool Ehci::portReset(uint8_t nPort, bool bErrorResponse) {
  if (nPort >= m_nPorts)
    return false;

  const size_t portRegister = m_nOpRegsOffset + EHCI_PORTSC + (nPort * 4);
  if (bErrorResponse) {
    modifyPortControl(portRegister, EHCI_PORTSC_EN, 0);
    if (!waitForMmioState(m_pBase, portRegister, EHCI_PORTSC_EN, 0)) {
      ERROR("EHCI: port " << Dec << nPort << Hex << " did not disable within 100 ms");
      return false;
    }
  }

  int retry;
  for (retry = 0; retry < 3; retry++) {
#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("USB: EHCI: Port " << Dec << nPort << Hex << " - status before reset: "
                                 << m_pBase->read32(m_nOpRegsOffset + EHCI_PORTSC + (nPort * 4)));
#endif

    // Set the reset bit
    modifyPortControl(portRegister, 0, EHCI_PORTSC_PRES);

    Time::delay(50 * Time::Multiplier::Millisecond);

    // Unset the reset bit
    modifyPortControl(portRegister, EHCI_PORTSC_PRES, 0);

    // Wait for the reset to complete
    if (!waitForMmioState(m_pBase, portRegister, EHCI_PORTSC_PRES, 0)) {
      ERROR("EHCI: reset on port " << Dec << nPort << Hex << " did not clear within 100 ms");
      return false;
    }

#ifdef USB_VERBOSE_DEBUG
    DEBUG_LOG("USB: EHCI: Port " << Dec << nPort << Hex << " - status after reset: "
                                 << m_pBase->read32(m_nOpRegsOffset + EHCI_PORTSC + (nPort * 4)));
#endif

    if ((m_pBase->read32(m_nOpRegsOffset + EHCI_PORTSC + (nPort * 4)) & EHCI_PORTSC_EN) &&
        (m_pBase->read32(m_nOpRegsOffset + EHCI_PORTSC + (nPort * 4)) & EHCI_PORTSC_CONN)) {
      DEBUG_LOG("USB: EHCI: Port " << Dec << nPort << Hex << " is connected");
      return true;
    } else {
      DEBUG_LOG("USB: EHCI: Port " << Dec << nPort << Hex
                                   << " seems to be not HighSpeed. Returning "
                                      "to companion controllers.");
      modifyPortControl(portRegister, 0, 0x2000);
    }
  }

  WARNING("EHCI: Port " << Dec << nPort << Hex << " could not be connected");

  return false;
}

uint64_t Ehci::executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
                              uint64_t p6, uint64_t p7, uint64_t p8) {
  if (p1 >= m_nPorts) {
    return 0;
  }
  UsbHcd::PortChangeRequest::Completion completion(m_PortChanges[p1], static_cast<size_t>(p8));
  if (!completion) {
    return 0;
  }

  // See if there's any device attached on the port
  if (m_pBase->read32(m_nOpRegsOffset + EHCI_PORTSC + p1 * 4) & EHCI_PORTSC_CONN) {
    if (portReset(p1))
      if (!deviceConnected(p1, HighSpeed))
        WARNING("EHCI: Port " << Dec << p1 << Hex
                              << " appeared to be connected but could not be set up");
  } else {
    DEBUG_LOG("USB: EHCI: Port " << Dec << p1 << Hex << " is disconnected");

    deviceDisconnected(p1);
  }
  return 0;
}

void Ehci::cancelRequest(const Request& request) {
  if (request.p1 < m_nPorts) {
    m_PortChanges[request.p1].cancel(static_cast<size_t>(request.p8));
  }
}
