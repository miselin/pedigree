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

#ifndef NIC_3C90X_H
#define NIC_3C90X_H

#include "3Com90xConstants.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"

class IoBase;

/** Device driver for the Nic3C90x class of network device */
class Nic3C90x : public Network, public IrqHandler
{
  public:
    Nic3C90x(Network *pDev);
    ~Nic3C90x();

    virtual void getName(String &str)
    {
        str.assign("3C90x", 6);
    }

    virtual bool send(size_t nBytes, uintptr_t buffer);

    virtual bool setStationInfo(const StationInfo &info);

    virtual const StationInfo &getStationInfo();

    bool isInitialised() const
    {
        return m_Initialised;
    }

    // IRQ handler callback.
    virtual IrqDisposition irq(irq_id_t number);

    IoBase *m_pBase;

  private:
    static constexpr size_t ReceiveSlotSize = 1536;

    bool issueCommand(int cmd, int param);

    int setWindow(int window);

    bool waitForEepromReady();
    bool readEeprom(int address, uint16_t &value);

    int writeEepromWord(int address, uint16_t value);
    int writeEeprom(int address, uint16_t value);

    bool reset();
    bool quiesce();
    bool stopDeviceLocked();

    /** Local NIC information */
    uint8_t m_isBrev;
    uint8_t m_CurrentWindow;

    uint8_t *m_pRxBuffVirt;
    uint8_t *m_pTxBuffVirt;
    uintptr_t m_pRxBuffPhys;
    uintptr_t m_pTxBuffPhys;
    MemoryRegion m_RxBuffMR;
    MemoryRegion m_TxBuffMR;

    uintptr_t m_pDPD;
    MemoryRegion m_DPDMR;

    uintptr_t m_pUPD;
    MemoryRegion m_UPDMR;

    /** TX Descriptor */
    struct TXD
    {
        uint32_t DnNextPtr;
        uint32_t FrameStartHeader;
        // uint32_t HdrAddr;
        // uint32_t HdrLength;
        uint32_t DataAddr;
        uint32_t DataLength;
    } __attribute__((aligned(8)));

    /** RX Descriptor */
    struct RXD
    {
        uint32_t UpNextPtr;
        volatile uint32_t UpPktStatus;
        uint32_t DataAddr;
        uint32_t DataLength;
    } __attribute__((aligned(8)));

    TXD *m_TransmitDPD;
    RXD *m_ReceiveUPD;

    // The threaded IRQ worker is serialized for this line, so one device-owned
    // batch can carry packets across the device-lock boundary without invoking
    // the allocator while that lock is held.
    uint8_t m_ReceiveStaging[NUM_UPDS * ReceiveSlotSize];

    Nic3C90x(const Nic3C90x &);
    void operator=(const Nic3C90x &);

    Semaphore m_TxMutex;

    Mutex m_DeviceLock;
    Mutex m_SendLock;

    size_t m_RxConsumerIndex;
    irq_id_t m_IrqId;
    bool m_Active;
    bool m_Stopping;
    bool m_TxSuccessful;
    bool m_Initialised;
};

#endif
