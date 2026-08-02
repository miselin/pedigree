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

#ifndef RTL8139_H
#define RTL8139_H

#include "Rtl8139Constants.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/types.h"

#define RTL8139_VENDOR_ID 0x10ec
#define RTL8139_DEVICE_ID 0x8139

/** Device driver for the RTL8139 class of network device */
class Rtl8139 : public Network, public IrqHandler
{
  public:
    Rtl8139(Network *pDev);
    ~Rtl8139() override;

    void getName(String &str) override
    {
        str.assign("rtl8139", 8);
    }

    bool send(size_t nBytes, uintptr_t buffer) override;

    bool setStationInfo(const StationInfo &info) override;

    const StationInfo &getStationInfo() override;

    bool isConnected() override;

    // IRQ handler callback.
    IrqDisposition irq(irq_id_t number) override;

    bool isInitialised() const
    {
        return m_Initialised;
    }

  private:
    static constexpr size_t ReceivePacketBudget = 1024;

    struct Packet
    {
        uint8_t *buffer;
        size_t length;
    };

    /** Resets and configures the controller with its interrupt source masked. */
    void resetController();

    /** Stops DMA and verifies both engines relinquished their buffers. */
    void haltController();

    /** Drains complete receive-ring entries into the device-owned batch. */
    bool drainReceive(
        size_t &descriptorCount, size_t &packetCount, size_t &stagingBytes);

    /** Copies bytes from the wrapping 64K receive ring. */
    void copyFromReceiveRing(
        void *destination, size_t offset, size_t length) const;

    IoBase *m_pBase;

    uint32_t m_RxCurr;
    uint8_t m_TxCurr;

    Mutex m_DeviceLock;

    uint8_t *m_pRxBuffVirt;
    physical_uintptr_t m_pRxBuffPhys;
    uint8_t *m_pTxBuffers[RTL_TX_DESCRIPTOR_COUNT];
    physical_uintptr_t m_TxBufferPhysical[RTL_TX_DESCRIPTOR_COUNT];

    MemoryRegion m_RxBuffMR;
    MemoryRegion m_TxBuffMR;

    // The one-worker-per-line IRQ contract serializes use of this batch. It
    // lets the driver release its device lock before entering the network
    // stack without allocating while hardware state is locked.
    Packet m_ReceiveBatch[ReceivePacketBudget];
    uint8_t m_ReceiveStaging[RTL_RX_RING_SIZE];

    irq_id_t m_IrqId;
    bool m_Stopping;
    bool m_NetworkRegistered;
    bool m_Initialised;

    Rtl8139(const Rtl8139 &);
    void operator=(const Rtl8139 &);
};

#endif
