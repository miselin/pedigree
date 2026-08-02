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

#ifndef UHCI_H
#define UHCI_H

#include "CallbackDelivery.h"
#include "PortChangeRequest.h"
#include "TransferCompletion.h"
#include "modules/system/usb/Usb.h"
#include "modules/system/usb/UsbHub.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/TimerHandler.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/new"

class Device;
class IoBase;
class Thread;

/** Device driver for the Uhci class */
class Uhci : public UsbHub,
             public IrqHandler,
             public RequestQueue,
             public TimerHandler
{
  public:
    Uhci(Device *pDev);
    virtual ~Uhci();

    struct TD
    {
        uint32_t bNextInvalid : 1;
        uint32_t bNextQH : 1;
        uint32_t bNextDepth : 1;
        uint32_t res0 : 1;
        uint32_t pNext : 28;
        uint32_t nActLen : 11;
        uint32_t res1 : 5;
        uint32_t nStatus : 8;
        uint32_t bIoc : 1;
        uint32_t bIsochronus : 1;
        uint32_t bLoSpeed : 1;
        uint32_t nErr : 2;
        uint32_t bSpd : 1;
        uint32_t res2 : 2;
        uint32_t nPid : 8;
        uint32_t nAddress : 7;
        uint32_t nEndpoint : 4;
        uint32_t bDataToggle : 1;
        uint32_t res3 : 1;
        uint32_t nMaxLen : 11;
        uint32_t pBuff;

        // Custom TD fields
        uint16_t nBufferSize;
        bool bShortTransferTD;

        size_t id;

        // Possible values for status
        enum StatusCodes
        {
            Timeout = 0x4,
            Nak = 0x8,
            Babble = 0x10,
            Stall = 0x40,
        };

        inline UsbError getError()
        {
            if (nStatus & Stall)
                return ::Stall;
            else if (nStatus & Nak)
                return NakNyet;
            else if (nStatus & Babble)
                return ::Babble;
            else if (nStatus & Timeout)
                return ::Timeout;
            else
                return TransactionError;
        }
    } PACKED ALIGN(16);

    struct QH
    {
        uint32_t bNextInvalid : 1;
        uint32_t bNextQH : 1;
        uint32_t res0 : 2;
        uint32_t pNext : 28;
        uint32_t bElemInvalid : 1;
        uint32_t bElemQH : 1;
        uint32_t res1 : 2;
        uint32_t pElem : 28;

        struct MetaData
        {
            Uhci *pOwner;
            void (*pPeriodicCallback)(uintptr_t, ssize_t);
            uintptr_t pPeriodicParam;

            UsbEndpoint endpointInfo;

            bool bPeriodic;
            TD *pFirstTD;
            TD *pLastTD;
            size_t nTotalBytes;

            QH *pPrev;
            QH *pNext;

            List<TD *> tdList;
            List<TD *> completedTdList;

            bool bIgnore;  /// Ignore this QH when iterating over the list -
                           /// don't look at any of its TDs
            UsbHcd::TransferCompletion completion;

            size_t id;
        } * pMetaData;
    } PACKED ALIGN(16);

    virtual void getName(String &str)
    {
        str.assign("UHCI", 5);
    }

    virtual void addTransferToTransaction(
        uintptr_t pTransaction, bool bToggle, UsbPid pid, uintptr_t pBuffer,
        size_t nBytes);
    virtual uintptr_t createTransaction(UsbEndpoint endpointInfo);

    MUST_USE_RESULT virtual bool doAsync(
        uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t) = 0,
        uintptr_t pParam = 0);
    virtual void addInterruptInHandler(
        UsbEndpoint endpointInfo, uintptr_t pBuffer, uint16_t nBytes,
        void (*pCallback)(uintptr_t, ssize_t), uintptr_t pParam = 0);

    /// IRQ handler
    IrqDisposition irq(irq_id_t number) override;

    void doDequeue();

    /// Timer callback to handle port status changes
    void timer(uint64_t delta);

    virtual bool portReset(uint8_t nPort, bool bErrorResponse = false);

  protected:
    virtual void cancelAsyncAndDrain(
        uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t),
        uintptr_t pParam);
    void replaySuppressedConnectionChange(size_t port) override;

    virtual uint64_t executeRequest(
        uint64_t p1 = 0, uint64_t p2 = 0, uint64_t p3 = 0, uint64_t p4 = 0,
        uint64_t p5 = 0, uint64_t p6 = 0, uint64_t p7 = 0, uint64_t p8 = 0);
    void cancelRequest(const Request &request) override;

  private:
    /** Runs after callback delivery and hands the QH to the reclaim worker. */
    static void enqueueCompletedTransfer(void *context);

    /** Requires IRQ then queue-list ownership and an established DMA halt. */
    void detachQueueHeadLocked(QH *pQH);

    /** Reclaims a detached or never-published QH while m_Mutex is held. */
    void reclaimQueueHeadLocked(QH *pQH);

    /// Stops the controller or panics before DMA ownership can be transferred.
    void stop();

    /// Starts the controller or panics instead of leaving work silently stuck.
    void start();

    /// Updates the lower USBLEGSUP control word without clobbering its upper
    /// word.
    void setLegacySupportControl(uint16_t control);

    /// Serialises control RMWs with scanning without echoing W1C status bits.
    void modifyPortControl(
        size_t portRegister, uint16_t clearMask, uint16_t setMask);

    enum UhciConstants
    {
        UHCI_CMD = 0x00,     // Command register
        UHCI_STS = 0x02,     // Status register
        UHCI_INTR = 0x04,    // Intrerrupt Enable register
        UHCI_FRMN = 0x06,    // Frame Number register
        UHCI_FRLP = 0x08,    // Frame List Pointer register
        UHCI_PORTSC = 0x10,  // Port Status/Control registers

        UHCI_CMD_GRES = 0x04,   // Global Reset bit
        UHCI_CMD_HCRES = 0x02,  // Host Controller Reset bit
        UHCI_CMD_RUN = 0x01,    // Run bit

        UHCI_STS_HALT = 0x20,  // Controller is halted
        UHCI_STS_ERR = 0x02,   // UHCI Error
        UHCI_STS_INT = 0x01,   // On Completition Interrupt bit

        UHCI_PORTSC_PRES = 0x200,     // Port Reset bit
        UHCI_PORTSC_LOSPEED = 0x100,  // Port has Low Speed Device attached bit
        UHCI_PORTSC_EDCH = 0x8,       // Port Enable/Disable Change bit
        UHCI_PORTSC_ENABLE = 0x4,     // Port Enable bit
        UHCI_PORTSC_CSCH = 0x2,       // Port Connect Status Change bit
        UHCI_PORTSC_CONN = 0x1,       // Port Connected bit
    };

    IoBase *m_pBase;
    /** Rejects submissions which race transfer teardown. */
    OperationBarrier m_SubmissionOperations;
    /** Drains cancellation callers before their callback state is reclaimed. */
    OperationBarrier m_CancelOperations;
    /** Retained from accepted publication through worker reclamation. */
    OperationBarrier m_TransferOperations;
    OperationBarrier m_CallbackOperations;
    irq_id_t m_IrqId;
    bool m_TimerRegistered;
    Atomic<bool> m_InterruptsClosing;

    uint8_t m_nPorts;
    UsbHcd::PortChangeRequest m_PortChanges[UsbHcd::UhciRootPortCount];
    Atomic<bool> m_PortChangesClosing;
    Spinlock m_PortChangeLock;

    Mutex m_Mutex;

    /** Serialises transaction completion with synchronous cancellation. */
    Mutex m_IrqProcessingLock;
    /** Per-generation callback publication and cancellation drain boundary. */
    UsbHcd::CallbackDeliveryQueue m_CompletionDeliveries;
    Mutex m_AsyncQueueListChangeLock;

    uint32_t *m_pFrameList;
    uintptr_t m_pFrameListPhys;

    TD *m_pTDList;
    uintptr_t m_pTDListPhys;
    ExtensibleBitmap m_TDBitmap;

    QH *m_pAsyncQH;
    QH *m_pPeriodicQH;

    QH *m_pQHList;
    uintptr_t m_pQHListPhys;
    ExtensibleBitmap m_QHBitmap;

    MemoryRegion m_UhciMR;

    /// Pointer to the current queue tail, which allows insertion of new queue
    /// heads to the asynchronous schedule.
    QH *m_pCurrentAsyncQueueTail;

    /// Pointer to the current queue head. Used to fill pNext automatically
    /// for new queue heads inserted to the asynchronous schedule.
    QH *m_pCurrentAsyncQueueHead;

    /// List of QHs in the active asynchronous schedule
    List<QH *> m_AsyncSchedule;

    /// List of QHs ready for dequeue
    List<QH *> m_DequeueList;

    /// Semaphore for the dequeue list
    Semaphore m_DequeueCount;

    /**
     * Protects queue-head admission from racing controller teardown.
     * The persistent worker is joined separately after admitted work drains.
     */
    OperationBarrier m_DequeueOperations;
    Thread *m_pDequeueThread;

    /// The time passed since last port check
    uint64_t m_nPortCheckTicks;

    Uhci(const Uhci &);
    void operator=(const Uhci &);
};

#endif

#endif
