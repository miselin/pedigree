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

#ifndef OHCI_H
#define OHCI_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/new"

#include "CallbackDelivery.h"
#include "PortChangeRequest.h"
#include "TransferCompletion.h"
#include "modules/system/usb/Usb.h"
#include "modules/system/usb/UsbHub.h"

class Device;
class IoBase;

/** Device driver for the Ohci class */
class Ohci : public UsbHub,
#if X86_COMMON
             public IrqHandler,
#endif
             public RequestQueue {
 private:
  static constexpr size_t EdRegionBytes = 4096;
  static constexpr size_t EdRegionOffsetMask = EdRegionBytes - 1;

#if THREADS
  using ControllerLock = Mutex;
#else
  // Non-threaded kernels still need IRQ-safe exclusion, but Mutex has no
  // implementation in that configuration.
  using ControllerLock = Spinlock;
#endif

#if X86_COMMON
  using IrqProcessingLock = Mutex;
#else
  using IrqProcessingLock = Spinlock;
#endif

  /// Enumeration of lists that can be stopped or started.
  enum Lists { PeriodicList = 0x4, IsochronousList = 0x8, ControlList = 0x10, BulkList = 0x20 };

 public:
  Ohci(Device* pDev);
  virtual ~Ohci();

  bool initialised() const {
    return m_Initialised;
  }

  struct TD {
    uint32_t res0 : 18;
    uint32_t bBuffRounding : 1;
    uint32_t nPid : 2;
    uint32_t nIntDelay : 3;
    uint32_t bDataToggleSrc : 1;
    uint32_t bDataToggle : 1;
    uint32_t nErrorCount : 2;
    uint32_t nStatus : 4;
    uint32_t pBufferStart;
    uint32_t res1 : 4;
    uint32_t pNext : 28;
    uint32_t pBufferEnd;

    // Custom TD fields
    uint16_t nBufferSize;
    uint16_t nNextTDIndex;
    bool bLast;

    size_t id;

    // Possible values for status
    enum StatusCodes { CrcError = 1, Stall = 4 };

    UsbError getError() {
      switch (nStatus) {
        case CrcError:
          return ::CrcError;
        case Stall:
          return ::Stall;
        default:
          return TransactionError;
      }
    }

  } PACKED ALIGN(16);

  struct ED {
    uint32_t nAddress : 7;
    uint32_t nEndpoint : 4;
    uint32_t bOut : 1;
    uint32_t bIn : 1;
    uint32_t bLoSpeed : 1;
    uint32_t bSkip : 1;
    uint32_t bIso : 1;
    uint32_t nMaxPacketSize : 11;
    uint32_t res0 : 9;
    uint32_t pTailTD : 28;
    uint32_t bHalted : 1;
    uint32_t bToggleCarry : 1;
    uint32_t res1 : 2;
    uint32_t pHeadTD : 28;
    uint32_t res2 : 4;
    uint32_t pNext : 28;

    struct MetaData {
      /** Used only by the recurring periodic path. */
      void (*pCallback)(uintptr_t, ssize_t);
      uintptr_t pParam;

      UsbEndpoint endpointInfo;

      bool bPeriodic;
      bool bBuildFailed;
      TD* pFirstTD;
      TD* pLastTD;
      size_t nTotalBytes;

      ED* pPrev;
      ED* pNext;

      List<TD*> tdList;
      List<TD*> completedTdList;

      bool bIgnore;

      bool bLinked;

      Lists edType;
      UsbHcd::TransferCompletion completion;
      bool acceptedOperation;

      size_t id;
    }* pMetaData;
  } PACKED ALIGN(16);

  struct Hcca {
    uint32_t pInterruptEDList[32];
    uint16_t nFrameNumber;
    uint16_t res0;
    volatile uint32_t pDoneHead;
  } PACKED;

  virtual void getName(String& str) {
    str.assign("OHCI", 5);
  }

  virtual void addTransferToTransaction(uintptr_t pTransaction, bool bToggle, UsbPid pid,
                                        uintptr_t pBuffer, size_t nBytes);
  virtual uintptr_t createTransaction(UsbEndpoint endpointInfo);

  MUST_USE_RESULT virtual bool doAsync(uintptr_t pTransaction,
                                       void (*pCallback)(uintptr_t, ssize_t) = 0,
                                       uintptr_t pParam = 0);
  MUST_USE_RESULT virtual bool addInterruptInHandler(UsbEndpoint endpointInfo, uintptr_t pBuffer,
                                                     uint16_t nBytes,
                                                     void (*pCallback)(uintptr_t, ssize_t),
                                                     UsbInterruptInHandle& handle,
                                                     uintptr_t pParam = 0);

/// IRQ handler
#if X86_COMMON
  IrqDisposition irq(irq_id_t number) override;
#endif

  virtual bool portReset(uint8_t nPort, bool bErrorResponse = false);

 protected:
  virtual void cancelAsyncAndDrain(uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t),
                                   uintptr_t pParam);
  MUST_USE_RESULT bool cancelInterruptInAndDrain(const UsbInterruptInToken& token,
                                                 void (*callback)(uintptr_t, ssize_t),
                                                 uintptr_t parameter,
                                                 bool producerAlreadyStopped) override;
  bool inInterruptCallbackContext() const override {
    return UsbHcd::CallbackDeliveryQueue::isInCallbackContext();
  }
  void replaySuppressedConnectionChange(size_t port) override;

  virtual uint64_t executeRequest(uint64_t p1 = 0, uint64_t p2 = 0, uint64_t p3 = 0,
                                  uint64_t p4 = 0, uint64_t p5 = 0, uint64_t p6 = 0,
                                  uint64_t p7 = 0, uint64_t p8 = 0);
  void cancelRequest(const Request& request) override;

 private:
  /// Stops the controller from processing the given list.
  void stop(Lists list);

  /// Starts processing of the given list.
  void start(Lists list);

  /**
   * Changes only the RootHubStatusChange interrupt source.
   *
   * Callers hold m_RootHubLock so reset and interrupt paths cannot race the
   * source mask.
   */
  void setRootHubStatusChangeSource(bool enabled);

  /** Writes HcControl and waits for the requested functional state. */
  void transitionControllerState(uint32_t control, const char* timeoutMessage);

  /// Detaches an ED and queues it for reclamation at the next frame.
  void removeED(ED* pED);

  /** Detaches a control or bulk ED without arming SOF or restarting a list. */
  void detachED(ED* pED);

  /** Removes an ED from the software completion schedule if present. */
  void removeFromFullSchedule(ED* pED);

  /** Removes an ED from the next-frame reclaim list if present. */
  void removeFromDequeueList(ED* pED);

  /** Releases every TD owned by an ED after the DMA ownership boundary. */
  void reclaimTransferDescriptors(ED* pED);

  /** Releases ED metadata and its accepted-operation lease. */
  void retireEDStorage(ED* pED);

  /** Builds the sole delivery record for a claimed one-shot transfer. */
  UsbHcd::CallbackDeliveryQueue::Record* prepareCompletion(
      ED* pED, const UsbHcd::TransferCompletion::Claim& claim);

  /** Claims and materializes one ED while the suspended controller is owned. */
  void terminalizeEDForTeardown(ED* pED, List<UsbHcd::CallbackDeliveryQueue::Record*>& completions);

  static void finishDeferredCompletion(void* context);

  /// Converts a software ED pointer to a physical address.
  inline physical_uintptr_t vtp_ed(ED* pED) {
    if (!pED || !pED->pMetaData)
      return 0;

    size_t id = pED->pMetaData->id & EdRegionOffsetMask;
    Lists type = pED->pMetaData->edType;
    switch (type) {
      case ControlList:
        return m_pControlEDListPhys + (id * sizeof(ED));
      case BulkList:
        return m_pBulkEDListPhys + (id * sizeof(ED));
      default:
        return 0;
    }
  }

  /// Converts a physical address to an ED pointer. Maybe.
  inline ED* ptv_ed(physical_uintptr_t phys) {
    if (!phys)
      return 0;

    // Figure out which list the ED was in.
    /// \todo defines for the list sizes so changing one doesn't involve
    /// rewriting heaps of code
    if ((m_pControlEDListPhys <= phys) && (phys < (m_pControlEDListPhys + EdRegionBytes))) {
      return &m_pControlEDList[(phys - m_pControlEDListPhys) / sizeof(ED)];
    } else if ((m_pBulkEDListPhys <= phys) && (phys < (m_pBulkEDListPhys + EdRegionBytes))) {
      return &m_pBulkEDList[(phys - m_pBulkEDListPhys) / sizeof(ED)];
    } else
      return 0;
  }

  enum OhciConstants {
    OhciVersion = 0x00,
    OhciControl = 0x04,           // HcControl register
    OhciCommandStatus = 0x08,     // HcCommandStatus register
    OhciInterruptStatus = 0x0c,   // HcInterruptStatus register
    OhciInterruptEnable = 0x10,   // HcIntrerruptEnable register
    OhciInterruptDisable = 0x14,  // HcIntrerruptDisable register
    OhciHcca = 0x18,              // HcHCCA register
    OhciControlHeadED = 0x20,     // HcControlHeadED register
    OhciControlCurrentED = 0x24,  // HcControlCurrentED register
    OhciBulkHeadED = 0x28,        // HcBulkHeadED register
    OhciBulkCurrentED = 0x2c,     // HcBulkCurrentED register
    OhciFmInterval = 0x34,
    OhciRhDescriptorA = 0x48,  // HcRhDescriptorA register
    OhciRhStatus = 0x50,
    OhciRhPortStatus = 0x54,  // HcRhPortStatus registers

    OhciControlStateFunctionalMask = 0xC0,

    OhciControlInterruptRoute = 0x100,
    OhciControlStateRunning = 0x80,    // HostControllerFunctionalState bits for USBOPERATIONAL
    OhciControlStateSuspended = 0xC0,  // HostControllerFunctionalState bits for USBSUSPEND
    OhciControlListsEnable = 0x30,     // 0x34     // PeriodicListEnable,
                                       // ControlListEnable and BulkListEnable
                                       // bits

    OhciCommandRequestOwnership = 0x08,   // Requests ownership change
    OhciCommandBulkListFilled = 0x04,     // BulkListFilled bit
    OhciCommandControlListFilled = 0x02,  // ControlListFilled bit
    OhciCommandHcReset = 0x01,            // HostControllerReset bit

    OhciInterruptMIE = 0x80000000,  // MasterInterruptEnable bit
    OhciInterruptOwnershipChange = 0x40000000,
    OhciInterruptRhStsChange = 0x40,  // RootHubStatusChange bit
    OhciInterruptFrameNumberOverflow = 0x20,
    OhciInterruptUnrecoverableError = 0x10,
    OhciInterruptResumeDetected = 0x08,
    OhciInterruptWbDoneHead = 0x02,  // WritebackDoneHead bit
    OhciInterruptSchedulingOverrun = 0x01,
    OhciInterruptStartOfFrame = 0x04,  // StartOfFrame interrupt
    OhciInterruptAll = OhciInterruptMIE | OhciInterruptOwnershipChange | 0x7F,
    OhciInterruptOperational = OhciInterruptMIE | OhciInterruptFrameNumberOverflow |
                               OhciInterruptUnrecoverableError | OhciInterruptWbDoneHead |
                               OhciInterruptSchedulingOverrun,

    // This bit reads as LPSC but writes as SetGlobalPower. It must never
    // be echoed as a generic change acknowledgement.
    OhciRhHubStsSetGlobalPower = 0x10000,
    OhciRhHubStsOverCurrentCh = 0x20000,

    OhciRhPortStsResCh = 0x100000,  // PortResetStatusChange bit
    OhciRhPortStsOverCurrentCh = 0x80000,
    OhciRhPortStsSuspendCh = 0x40000,
    OhciRhPortStsEnableCh = 0x20000,
    OhciRhPortStsConnStsCh = 0x10000,  // ConnectStatusChange bit
    OhciRhPortStsChangeMask = OhciRhPortStsResCh | OhciRhPortStsOverCurrentCh |
                              OhciRhPortStsSuspendCh | OhciRhPortStsEnableCh |
                              OhciRhPortStsConnStsCh,
    OhciRhPortStsLoSpeed = 0x200,   // LowSpeedDeviceAttached bit
    OhciRhPortStsPower = 0x100,     // PortPowerStatus / SetPortPower bit
    OhciRhPortStsReset = 0x10,      // SetPortReset bit
    OhciRhPortStsEnable = 0x02,     // SetPortEnable bit
    OhciRhPortStsConnected = 0x01,  // CurrentConnectStatus bit
  };

  IoBase* m_pBase;

  uint8_t m_nPorts;
  bool m_Initialised;
  UsbHcd::PortChangeRequest m_PortChanges[UsbHcd::OhciRootPortCount];
  UsbHcd::DeferredPortChanges m_DeferredPortChanges;

  /// Global lock.
  ControllerLock m_Mutex;

  /// Serializes threaded root-port resets without holding an IRQ-safe lock
  /// while waiting for hardware.
  ControllerLock m_PortResetMutex;

  Hcca* m_pHcca;
  uintptr_t m_pHccaPhys;

  /// Lock for modifying the schedule list itself (m_FullSchedule)
  IrqProcessingLock m_IrqProcessingLock;

  /** Per-generation callback publication and cancellation drain boundary. */
  UsbHcd::CallbackDeliveryQueue m_CompletionDeliveries;

  /// Serializes root-hub register access between reset and IRQ paths.
  Spinlock m_RootHubLock;
  bool m_RootHubStatusChangeDesired;
  bool m_PortResetActive;
  Atomic<size_t> m_TeardownPhase;

  Spinlock m_ScheduleChangeLock;

  /// Lock for changing the periodic list.
  Spinlock m_PeriodicListChangeLock;

  /// Lock for changing the control list.
  Spinlock m_ControlListChangeLock;

  /// Lock for changing the bulk list.
  Spinlock m_BulkListChangeLock;

  ED* m_pPeriodicEDList;
  uintptr_t m_pPeriodicEDListPhys;
  ExtensibleBitmap m_PeriodicEDBitmap;

  ED* m_pControlEDList;
  uintptr_t m_pControlEDListPhys;
  ExtensibleBitmap m_ControlEDBitmap;

  ED* m_pBulkEDList;
  uintptr_t m_pBulkEDListPhys;
  ExtensibleBitmap m_BulkEDBitmap;

  TD* m_pTDList;
  uintptr_t m_pTDListPhys;
  ExtensibleBitmap m_TDBitmap;

  // Pointers to the current bulk and control queue heads (can be null)
  ED* m_pBulkQueueHead;
  ED* m_pControlQueueHead;

  // Pointers to the current bulk and control queue tails
  ED* m_pBulkQueueTail;
  ED* m_pControlQueueTail;

  // Pointer to the current periodic queue tail
  ED* m_pPeriodicQueueTail;

  /// Dequeue list lock.
  Spinlock m_DequeueListLock;

  /// List of ED pointers in both the control and bulk queues. Used for
  /// IRQ handling.
  List<ED*> m_FullSchedule;

  /// List of EDs ready for dequeue (reclaiming)
  List<ED*> m_DequeueList;

  /// Semaphore for the dequeue list
  Semaphore m_DequeueCount;

  MemoryRegion m_OhciMR;

  /** Closes and drains interrupt callbacks before controller teardown. */
  OperationBarrier m_CallbackOperations;

  /** Short-lived create/build/submit operations closed before ED scanning. */
  OperationBarrier m_SubmissionOperations;

  /** Cancellation calls remain admitted until terminal callbacks drain. */
  OperationBarrier m_CancellationOperations;

  /** One lease per accepted one-shot or periodic subscription. */
  OperationBarrier m_AcceptedOperations;
  irq_id_t m_IrqId;

  Ohci(const Ohci&);
  void operator=(const Ohci&);
};

#endif
