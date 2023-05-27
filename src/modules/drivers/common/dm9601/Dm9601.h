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

#ifndef DM9601_H
#define DM9601_H

#include "modules/system/usb/UsbDevice.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/new"

class Dm9601 : public UsbDevice, public Network
{
  public:
    Dm9601(UsbDevice *pDev);

    virtual ~Dm9601();

    virtual void initialiseDriver();

    virtual void getName(String &str)
    {
        str.assign("DM9601", 7);
    }

    virtual bool send(size_t nBytes, uintptr_t buffer);

    virtual bool setStationInfo(const StationInfo &info);

    virtual const StationInfo &getStationInfo();

  private:
    static int recvTrampoline(void *p) NORETURN;

    static int trampoline(void *p) NORETURN;

    void receiveThread() NORETURN;

    void receiveLoop() NORETURN;

    void doReceive();

    enum VendorRequests
    {
        ReadRegister = 0,
        WriteRegister = 1,
        ReadMemory = 2,
        WriteRegister1 = 3,
        WriteMemory = 5,
        WriteMemory1 = 7,
    };

    enum Registers
    {
        NetworkControl = 0,
        NetworkStatus = 1,
        TxControl = 2,
        TxStatus1 = 3,
        TxStatus2 = 4,
        RxControl = 5,
        RxStatus = 6,
        RxOverflowCount = 7,
        BackPressThreshold = 8,
        FlowControl = 9,
        RxFlowControl = 10,
        PhyControl = 11,
        PhyAddress = 12,
        PhyLowByte = 13,
        PhyHighByte = 14,
        WakeUpControl = 15,
        PhysicalAddress = 16,
        MulticastAddress = 22,
        GeneralPurposeCtl = 30,
        GeneralPurpose = 31,
        TxWriteAddressLo = 32,
        TxWriteAddressHi = 33,
        TxReadAddressLo = 34,
        TxReadAddressHi = 35,
        RxWriteAddressLo = 36,
        RxWriteAddressHi = 37,
        RxReadAddressLo = 38,
        RxReadAddressHi = 39,
        Vendor = 40,
        Product = 42,
        Chip = 44,

        UsbAddress = 0xF0,
        RxCounter = 0xF1,
        TxCount = 0xF2,
        UsbStatus = 0xF2,
        UsbControl = 0xF4
    };

    struct InterruptInPacket
    {
        uint8_t networkStatus;
        uint8_t txStatus1;
        uint8_t txStatus2;
        uint8_t rxStatus;
        uint8_t rxOverflowCounter;
        uint8_t rxCounter;
        uint8_t txCounter;
        uint8_t gpRegister;
    } PACKED;

    /// Reads data from a register into a buffer
    ssize_t readRegister(uint8_t reg, uintptr_t buffer, size_t nBytes);

    /// Writes data from a buffer to a register
    ssize_t writeRegister(uint8_t reg, uintptr_t buffer, size_t nBytes);

    /// Writes a single 8-bit value to a register
    ssize_t writeRegister(uint8_t reg, uint8_t data);

    /// Reads data from device memory into a buffer
    ssize_t readMemory(uint16_t offset, uintptr_t buffer, size_t nBytes);

    /// Writes data from a buffer into device memory
    ssize_t writeMemory(uint16_t offset, uintptr_t buffer, size_t nBytes);

    /// Writes a single 8-bit value into device memory
    ssize_t writeMemory(uint16_t offset, uint8_t data);

    /// Reads a 16-bit value from the device EEPROM
    uint16_t readEeprom(uint8_t offset);

    /// Writes a 16-bit value to the device EEPROM
    void writeEeprom(uint8_t offset, uint16_t data);

    /// Reads a 16-bit value from the external MII
    uint16_t readMii(uint8_t offset);

    /// Writes a 16-bit value to the external MII
    void writeMii(uint8_t offset, uint16_t data);

    /** Bulk IN endpoint */
    Endpoint *m_pInEndpoint;

    /** Bulk OUT endpoint */
    Endpoint *m_pOutEndpoint;

    /** Mutex to only allow one TX in progress at a time. */
    Mutex m_TxLock;

    /** Number of packets in the queue */
    Semaphore m_IncomingPackets;

    /** Packet queue */
    struct Packet
    {
        uintptr_t buffer;
        size_t len;
        uint32_t offset;
    };
    List<Packet *> m_RxPacketQueue;
    Spinlock m_RxPacketQueueLock;

    /** Internal state: which TX packet are we on at the moment */
    size_t m_TxPacket;

    Dm9601(const Dm9601 &);
    void operator=(const Dm9601 &);
};

#endif
