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

#include "Rtl8139.h"
#include "modules/system/network-stack/NetworkStack.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

namespace
{
constexpr uint16_t EnabledInterrupts =
    RTL_ISR_RXFOVW | RTL_ISR_RXOVW | RTL_ISR_TXERR | RTL_ISR_RXERR |
    RTL_ISR_RXOK;
constexpr uint16_t ReceiveInterrupts = RTL_ISR_RXERR | RTL_ISR_RXOK;
constexpr uint16_t ReceiveOverflowInterrupts =
    RTL_ISR_RXFOVW | RTL_ISR_RXOVW;
constexpr uint16_t InvalidReceiveStatus =
    RTL_RXSTS_ISE | RTL_RXSTS_RUNT | RTL_RXSTS_LONG | RTL_RXSTS_CRC |
    RTL_RXSTS_FAE;
constexpr size_t ResetPollLimit = 100;
constexpr size_t HaltPollLimit = 100;
constexpr size_t StartPollLimit = 100;
constexpr size_t InterruptPassLimit = 8;
constexpr size_t ReceivePacketBudget = 1024;

size_t pagesFor(size_t bytes)
{
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}
}  // namespace

Rtl8139::Rtl8139(Network *pDev)
    : Network(pDev), m_pBase(nullptr), m_RxCurr(0), m_TxCurr(0),
      m_DeviceLock(), m_pRxBuffVirt(nullptr), m_pRxBuffPhys(0),
      m_pTxBuffers(), m_TxBufferPhysical(),
      m_RxBuffMR("rtl8139-rxbuffer"), m_TxBuffMR("rtl8139-txbuffer"),
      m_IrqId(0), m_Stopping(false), m_NetworkRegistered(false),
      m_Initialised(false)
{
    setSpecificType(String("rtl8139-card"));

    if (!m_Addresses.count() || !m_Addresses[0] || !m_Addresses[0]->m_Io)
    {
        ERROR("RTL8139: device has no usable register mapping");
        return;
    }

    m_pBase = m_Addresses[0]->m_Io;

#if X86_COMMON
    // The card owns DMA buffers and may expose either an I/O or MMIO BAR.
    const uint32_t pciCommand =
        PciBus::instance().readConfigSpace(this, 1);
    PciBus::instance().writeConfigSpace(this, 1, pciCommand | 0x7);
#endif

    // The PCI source is level-triggered. Keep it physically silent until the
    // threaded handler and NetworkStack registration can own every callback.
    m_pBase->write16(0, RTL_IMR);
    (void) m_pBase->read16(RTL_IMR);
    m_pBase->write16(0xFFFF, RTL_ISR);

    const size_t dmaConstraints =
        PhysicalMemoryManager::continuous | PhysicalMemoryManager::below4GB;
    if (!PhysicalMemoryManager::instance().allocateRegion(
            m_RxBuffMR, pagesFor(RTL_RX_ALLOCATION_SIZE), dmaConstraints,
            VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write, -1))
    {
        ERROR("RTL8139: couldn't allocate the receive ring");
        return;
    }
    if (!PhysicalMemoryManager::instance().allocateRegion(
            m_TxBuffMR, pagesFor(RTL_TX_ALLOCATION_SIZE), dmaConstraints,
            VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write, -1))
    {
        ERROR("RTL8139: couldn't allocate transmit buffers");
        return;
    }

    m_pRxBuffVirt = static_cast<uint8_t *>(m_RxBuffMR.virtualAddress());
    m_pRxBuffPhys = m_RxBuffMR.physicalAddress();
    uint8_t *txBase = static_cast<uint8_t *>(m_TxBuffMR.virtualAddress());
    const physical_uintptr_t txPhysical = m_TxBuffMR.physicalAddress();
    for (size_t i = 0; i < RTL_TX_DESCRIPTOR_COUNT; ++i)
    {
        m_pTxBuffers[i] = txBase + (i * RTL_TX_BUFFER_SIZE);
        m_TxBufferPhysical[i] = txPhysical + (i * RTL_TX_BUFFER_SIZE);
    }

    resetController();

    for (size_t i = 0; i < 6; ++i)
        m_StationInfo.mac.setMac(m_pBase->read8(RTL_MAC + i), i);

    WARNING(
        "RTL8139: MAC is " << m_StationInfo.mac[0] << ":"
                           << m_StationInfo.mac[1] << ":"
                           << m_StationInfo.mac[2] << ":"
                           << m_StationInfo.mac[3] << ":"
                           << m_StationInfo.mac[4] << ":"
                           << m_StationInfo.mac[5] << ".");

    m_IrqId = Machine::instance().getIrqManager()->registerPciIrqHandler(
        static_cast<IrqHandler *>(this), this, IrqPolicy::pciIntxThreaded());
    if (!m_IrqId)
    {
        ERROR("RTL8139: could not register its PCI interrupt");
        m_Stopping = true;
        haltController();
        return;
    }

    NetworkStack::instance().registerDevice(this);
    m_NetworkRegistered = true;
    m_Initialised = true;

    // ISR records causes while masked. Preserve any packet which arrived
    // during registration and let enabling IMR assert the threaded line.
    m_pBase->write16(EnabledInterrupts, RTL_IMR);
    (void) m_pBase->read16(RTL_IMR);
}

Rtl8139::~Rtl8139()
{
    {
        LockGuard<Mutex> deviceGuard(m_DeviceLock);
        m_Stopping = true;
        m_Initialised = false;
        if (m_pBase)
            haltController();
    }

    if (m_IrqId &&
        !Machine::instance().getIrqManager()->unregisterHandler(m_IrqId, this))
    {
        panic("RTL8139 teardown could not synchronously unregister its IRQ");
    }
    m_IrqId = 0;

    if (m_NetworkRegistered)
    {
        NetworkStack::instance().deRegisterDevice(this);
        m_NetworkRegistered = false;
    }
}

void Rtl8139::resetController()
{
    m_pBase->write16(0, RTL_IMR);
    (void) m_pBase->read16(RTL_IMR);

    // A firmware or previous driver may have left the MAC asleep. Wake it
    // before asking the reset state machine to make forward progress.
    m_pBase->write8(RTL_CFG9346_UNLOCK, RTL_CFG9346);
    m_pBase->write8(0, RTL_CFG1);
    m_pBase->write8(RTL_CFG9346_LOCK, RTL_CFG9346);
    (void) m_pBase->read8(RTL_CFG1);

    m_pBase->write8(RTL_CMD_RES, RTL_CMD);
    size_t resetPolls = ResetPollLimit;
    while (resetPolls-- && (m_pBase->read8(RTL_CMD) & RTL_CMD_RES))
        Time::delay(1 * Time::Multiplier::Millisecond);
    if (m_pBase->read8(RTL_CMD) & RTL_CMD_RES)
        panic("RTL8139 controller reset timed out after 100 ms");

    // Clear reset-era status before receive DMA can record new work.
    m_pBase->write16(0xFFFF, RTL_ISR);

    // Reset completion is the DMA ownership boundary for both regions.
    m_RxCurr = 0;
    m_TxCurr = 0;
    ByteSet(m_pRxBuffVirt, 0, RTL_RX_ALLOCATION_SIZE);
    for (size_t i = 0; i < RTL_TX_DESCRIPTOR_COUNT; ++i)
        ByteSet(m_pTxBuffers[i], 0, RTL_TX_BUFFER_SIZE);

    m_pBase->write8(RTL_CFG9346_UNLOCK, RTL_CFG9346);
    m_pBase->write8(0, RTL_CFG1);

    FENCE();
    m_pBase->write32(static_cast<uint32_t>(m_pRxBuffPhys), RTL_RXBUFF);
    m_pBase->write16(
        static_cast<uint16_t>((m_RxCurr - RTL_CAPR_BIAS) & 0xFFFF), RTL_CAPR);
    for (size_t i = 0; i < RTL_TX_DESCRIPTOR_COUNT; ++i)
    {
        m_pBase->write32(
            static_cast<uint32_t>(m_TxBufferPhysical[i]),
            RTL_TXADDR0 + (i * 4));
    }

    m_pBase->write32(0, RTL_RXMIS);
    m_pBase->write16(
        RTL_BMCR_SPEED | RTL_BMCR_ANE | RTL_BMCR_DUPLEX, RTL_BMCR);
    m_pBase->write8(RTL_MSR_RXFCE, RTL_MSR);

    // The transfer engines must be enabled before their threshold registers
    // are programmed.
    m_pBase->write8(RTL_CMD_RXEN | RTL_CMD_TXEN, RTL_CMD);
    m_pBase->write32(
        RTL_RXCFG_FTH_NONE | RTL_RXCFG_RBLN_64K | RTL_RXCFG_MDMA_UNLM |
            RTL_RXCFG_AB | RTL_RXCFG_AM | RTL_RXCFG_APM,
        RTL_RXCFG);
    m_pBase->write32(
        RTL_TXCFG_IFG96 | RTL_TXCFG_MDMA_2K | RTL_TXCFG_RR_48, RTL_TXCFG);
    m_pBase->write32(0xFFFFFFFF, RTL_MAR);
    m_pBase->write32(0xFFFFFFFF, RTL_MAR + 4);
    m_pBase->write8(RTL_CFG9346_LOCK, RTL_CFG9346);

    size_t startPolls = StartPollLimit;
    while (
        startPolls-- &&
        ((m_pBase->read8(RTL_CMD) & (RTL_CMD_RXEN | RTL_CMD_TXEN)) !=
         (RTL_CMD_RXEN | RTL_CMD_TXEN)))
    {
        Time::delay(1 * Time::Multiplier::Millisecond);
    }
    if (
        (m_pBase->read8(RTL_CMD) & (RTL_CMD_RXEN | RTL_CMD_TXEN)) !=
        (RTL_CMD_RXEN | RTL_CMD_TXEN))
    {
        panic("RTL8139 DMA engines did not start within 100 ms");
    }

    if (m_IrqId && m_Initialised && !m_Stopping)
    {
        m_pBase->write16(EnabledInterrupts, RTL_IMR);
        (void) m_pBase->read16(RTL_IMR);
    }
}

void Rtl8139::haltController()
{
    m_pBase->write16(0, RTL_IMR);
    (void) m_pBase->read16(RTL_IMR);
    m_pBase->write8(0, RTL_CMD);

    size_t haltPolls = HaltPollLimit;
    while (
        haltPolls-- &&
        (m_pBase->read8(RTL_CMD) & (RTL_CMD_RXEN | RTL_CMD_TXEN)))
    {
        Time::delay(1 * Time::Multiplier::Millisecond);
    }
    if (m_pBase->read8(RTL_CMD) & (RTL_CMD_RXEN | RTL_CMD_TXEN))
        panic("RTL8139 DMA engines did not halt within 100 ms");

    m_pBase->write16(0xFFFF, RTL_ISR);
}

bool Rtl8139::send(size_t nBytes, uintptr_t buffer)
{
    if (
        !buffer || nBytes < RTL_ETHERNET_HEADER_SIZE ||
        nBytes > RTL_ETHERNET_FRAME_MAX)
    {
        ERROR(
            "RTL8139: invalid transmit packet length " << Dec << nBytes
                                                        << Hex);
        return false;
    }

    LockGuard<Mutex> deviceGuard(m_DeviceLock);
    if (m_Stopping || !m_Initialised || !m_IrqId)
        return false;

    const size_t descriptor = m_TxCurr;
    const uint32_t status =
        m_pBase->read32(RTL_TXSTS0 + (descriptor * 4));
    if (!(status & RTL_TXSTS_OWN))
        return false;
    FENCE();

    const size_t transmitLength =
        nBytes < RTL_ETHERNET_FRAME_MIN
            ? static_cast<size_t>(RTL_ETHERNET_FRAME_MIN)
            : nBytes;
    MemoryCopy(
        m_pTxBuffers[descriptor], reinterpret_cast<void *>(buffer), nBytes);
    if (transmitLength > nBytes)
    {
        ByteSet(
            m_pTxBuffers[descriptor] + nBytes, 0, transmitLength - nBytes);
    }

    FENCE();
    m_pBase->write32(
        static_cast<uint32_t>(m_TxBufferPhysical[descriptor]),
        RTL_TXADDR0 + (descriptor * 4));
    m_pBase->write32(
        RTL_TXSTS_EARLY_THRESHOLD | static_cast<uint32_t>(transmitLength),
        RTL_TXSTS0 + (descriptor * 4));

    m_TxCurr = (m_TxCurr + 1) % RTL_TX_DESCRIPTOR_COUNT;
    return true;
}

void Rtl8139::copyFromReceiveRing(
    void *destination, size_t offset, size_t length) const
{
    uint8_t *output = static_cast<uint8_t *>(destination);
    offset &= RTL_RX_RING_SIZE - 1;
    const size_t untilWrap = RTL_RX_RING_SIZE - offset;
    const size_t first = length < untilWrap ? length : untilWrap;

    if (first)
        MemoryCopy(output, m_pRxBuffVirt + offset, first);
    if (length > first)
        MemoryCopy(output + first, m_pRxBuffVirt, length - first);
}

bool Rtl8139::drainReceive(List<Packet *> &packets)
{
    for (size_t packetIndex = 0; packetIndex < ReceivePacketBudget;
         ++packetIndex)
    {
        if (m_pBase->read8(RTL_CMD) & RTL_CMD_BUFE)
            return true;

        FENCE();
        uint16_t header[2] = {0, 0};
        copyFromReceiveRing(header, m_RxCurr, sizeof(header));
        const uint16_t status = LITTLE_TO_HOST16(header[0]);
        const uint16_t rawLength = LITTLE_TO_HOST16(header[1]);
        const bool validLength =
            rawLength >= (RTL_ETHERNET_FRAME_MIN + RTL_ETHERNET_CRC_SIZE) &&
            rawLength <= (RTL_ETHERNET_FRAME_MAX + RTL_ETHERNET_CRC_SIZE);

        if (!validLength)
        {
            ERROR(
                "RTL8139: corrupt receive length " << Dec << rawLength << Hex);
            badPacket();
            resetController();
            return false;
        }

        const bool validStatus =
            (status & RTL_RXSTS_RXOK) && !(status & InvalidReceiveStatus);
        if (validStatus)
        {
            const size_t packetLength = rawLength - RTL_ETHERNET_CRC_SIZE;
            Packet *packet = new Packet;
            packet->buffer = new uint8_t[packetLength];
            packet->length = packetLength;
            copyFromReceiveRing(
                packet->buffer, m_RxCurr + sizeof(header), packetLength);
            packets.pushBack(packet);
            gotPacket();
        }
        else
        {
            WARNING(
                "RTL8139: dropping receive entry with status " << Hex
                                                               << status);
            badPacket();
        }

        m_RxCurr =
            (m_RxCurr + sizeof(header) + rawLength + 3) &
            ~static_cast<size_t>(3);
        m_RxCurr &= RTL_RX_RING_SIZE - 1;
        m_pBase->write16(
            static_cast<uint16_t>((m_RxCurr - RTL_CAPR_BIAS) & 0xFFFF),
            RTL_CAPR);
    }

    if (!(m_pBase->read8(RTL_CMD) & RTL_CMD_BUFE))
    {
        ERROR("RTL8139: receive-ring drain exceeded its packet budget");
        resetController();
        return false;
    }
    return true;
}

bool Rtl8139::setStationInfo(const StationInfo &info)
{
    LockGuard<Mutex> deviceGuard(m_DeviceLock);

    if (m_StationInfo.dnsServers != info.dnsServers)
        delete[] m_StationInfo.dnsServers;
    m_StationInfo.ipv4 = info.ipv4;
    m_StationInfo.ipv6 = info.ipv6;
    m_StationInfo.nIpv6Addresses = info.nIpv6Addresses;
    m_StationInfo.subnetMask = info.subnetMask;
    m_StationInfo.broadcast = info.broadcast;
    m_StationInfo.gateway = info.gateway;
    m_StationInfo.gatewayIpv6 = info.gatewayIpv6;
    m_StationInfo.dnsServers = info.dnsServers;
    m_StationInfo.nDnsServers = info.nDnsServers;
    return true;
}

const StationInfo &Rtl8139::getStationInfo()
{
    return m_StationInfo;
}

bool Rtl8139::isConnected()
{
    LockGuard<Mutex> deviceGuard(m_DeviceLock);
    return m_Initialised && !m_Stopping &&
           !(m_pBase->read8(RTL_MSR) & RTL_MSR_LINK);
}

IrqDisposition Rtl8139::irq(irq_id_t number)
{
    (void) number;
    bool handled = false;
    List<Packet *> packets;

    {
        LockGuard<Mutex> deviceGuard(m_DeviceLock);
        if (m_Stopping || !m_Initialised)
            return IrqDisposition::NotHandled;

        for (size_t pass = 0; pass < InterruptPassLimit; ++pass)
        {
            const uint16_t irqStatus =
                m_pBase->read16(RTL_ISR) & EnabledInterrupts;
            const bool receivePending =
                !(m_pBase->read8(RTL_CMD) & RTL_CMD_BUFE);
            if (!irqStatus && !receivePending)
                break;

            handled = true;
            if (irqStatus)
                m_pBase->write16(irqStatus, RTL_ISR);

            if (irqStatus & ReceiveOverflowInterrupts)
            {
                ERROR("RTL8139: receive FIFO or ring overflow");
                resetController();
                break;
            }

            if (irqStatus & RTL_ISR_TXERR)
                WARNING("RTL8139: packet transmission failed");
            if (irqStatus & RTL_ISR_RXERR)
                WARNING("RTL8139: receive error reported");

            if (receivePending || (irqStatus & ReceiveInterrupts))
            {
                if (!drainReceive(packets))
                    break;
            }
        }
        (void) m_pBase->read16(RTL_ISR);
    }

    while (packets.count())
    {
        Packet *packet = packets.popFront();
        NetworkStack::instance().receive(
            packet->length, reinterpret_cast<uintptr_t>(packet->buffer), this,
            0);
        delete[] packet->buffer;
        delete packet;
    }

    return handled ? IrqDisposition::Handled : IrqDisposition::NotHandled;
}
