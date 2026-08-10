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

#include "Ne2k.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/network/IpAddress.h"
#include "pedigree/kernel/network/MacAddress.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/MemoryPool.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Ne2kConstants.h"
#include "modules/system/network-stack/NetworkStack.h"

// #define NE2K_NO_THREADS

namespace {
constexpr uint8_t CommandPage0Stop = E8390_NODMA | E8390_PAGE0 | E8390_STOP;
constexpr uint8_t CommandPage0Start = E8390_NODMA | E8390_PAGE0 | E8390_START;
constexpr uint8_t CommandPage1Stop = E8390_NODMA | E8390_PAGE1 | E8390_STOP;
constexpr uint8_t CommandPage1Start = E8390_NODMA | E8390_PAGE1 | E8390_START;
constexpr uint8_t CommandRemoteRead = E8390_RREAD | E8390_PAGE0 | E8390_START;
constexpr uint8_t CommandRemoteWrite = E8390_RWRITE | E8390_PAGE0 | E8390_START;
constexpr uint8_t CommandTransmit = E8390_NODMA | E8390_TRANS | E8390_PAGE0 | E8390_START;

constexpr uint8_t InterruptReceive = 0x01;
constexpr uint8_t InterruptTransmit = 0x02;
constexpr uint8_t InterruptReceiveError = 0x04;
constexpr uint8_t InterruptTransmitError = 0x08;
constexpr uint8_t InterruptOverflow = 0x10;
constexpr uint8_t InterruptCounters = 0x20;
constexpr uint8_t InterruptRemoteDmaComplete = 0x40;
constexpr uint8_t InterruptReset = 0x80;
constexpr uint8_t EnabledInterrupts = InterruptReceive | InterruptReceiveError |
                                      InterruptTransmitError | InterruptOverflow |
                                      InterruptCounters;

constexpr size_t RemoteDmaPollLimit = 1000000;
constexpr size_t ResetPollLimit = 100000;
constexpr size_t MinimumPacketLength = 14;
constexpr size_t MaximumPacketLength = 1600;
}  // namespace

Ne2k::Ne2k(Network* pDev)
    : Network(pDev),
      m_pBase(0),
      m_NextPacket(0),
      m_PacketQueueSize(0),
      m_PacketQueue(),
      m_PacketQueueLock(),
      m_DmaLock(),
      m_IrqId(0),
      m_Stopping(false),
      m_NetworkRegistered(false),
      m_ReceiveThread() {
  setSpecificType(String("ne2k-card"));

  // grab the ports
  m_pBase = m_Addresses[0]->m_Io;

  // Reset the card, and clear interrupts
  // m_pBase->write8(m_pBase->read8(NE_RESET), NE_RESET);
  // while((m_pBase->read8(NE_ISR) & 0x80) == 0);
  // m_pBase->write8(0xff, NE_ISR);

  // reset command
  m_pBase->write8(0x21, NE_CMD);

  // 16-bit transfer, monitor to avoid recv, loopback just in case
  // because we don't want to receive packets yet
  m_pBase->write8(0x09, NE_DCR);
  m_pBase->write8(0x20, NE_RCR);
  m_pBase->write8(0x02, NE_TCR);

  // turn off interrupts
  m_pBase->write8(0xff, NE_ISR);
  m_pBase->write8(0x00, NE_IMR);

  // get the MAC from PROM
  m_pBase->write8(0x00, NE_RSAR0);
  m_pBase->write8(0x00, NE_RSAR1);

  m_pBase->write8(32, NE_RBCR0);  // 32 bytes of data
  m_pBase->write8(0, NE_RBCR1);

  m_pBase->write8(0x0a, NE_CMD);  // remote read, STOP

  uint16_t prom[16];
  int i;
  for (i = 0; i < 16; i++)
    prom[i] = m_pBase->read16(NE_DATA);

  // set the MAC address in the card itself
  m_pBase->write8(0x61, NE_CMD);
  for (i = 0; i < 6; i++) {
    m_StationInfo.mac.setMac(prom[i] & 0xff, i);
    m_pBase->write8(prom[i] & 0xff, NE_PAR + i);
  }

  WARNING("NE2K: MAC is " << m_StationInfo.mac[0] << ":" << m_StationInfo.mac[1] << ":"
                          << m_StationInfo.mac[2] << ":" << m_StationInfo.mac[3] << ":"
                          << m_StationInfo.mac[4] << ":" << m_StationInfo.mac[5] << ".");

  // reset current page, put the card into normal mode, and set
  // packet buffer information
  m_pBase->write8(0x61, NE_CMD);
  m_pBase->write8(PAGE_RX + 1, NE_CURR);

  m_NextPacket = PAGE_RX + 1;

  m_pBase->write8(0x21, NE_CMD);

  m_pBase->write8(PAGE_RX, NE_PSTART);
  m_pBase->write8(PAGE_RX, NE_BNDRY);
  m_pBase->write8(PAGE_STOP, NE_PSTOP);

  // accept multicast, broadcast, and runt packets (<64 bytes)
  /// \todo Proper multicast subscription via the Network card abstraction
  m_pBase->write8(0x14, NE_RCR);
  m_pBase->write8(0x00, NE_TCR);

  // Accept all multicast packets. Once we have an API for multicast
  // subscription this will be different, as we may not want to receive every
  // single multicast packet that arrives.
  uint8_t tmp = m_pBase->read8(NE_CMD);
  m_pBase->write8(tmp | 0x40, NE_CMD);
  for (i = 0; i < MAR_SIZE; i++)
    m_pBase->write8(0xFF, NE_MAR + i);
  m_pBase->write8(tmp, NE_CMD);

// register the packet queue handler before we install the IRQ
#if THREADS
  Thread* pThread = new Thread(Processor::information().getCurrentThread()->getParent(),
                               &trampoline, reinterpret_cast<void*>(this));
  pThread->setName("NE2K receive worker");
  m_ReceiveThread.adopt(pThread);
#endif

  // install the IRQ
  NOTICE("NE2K: IRQ is " << getInterruptNumber());
  m_IrqId = Machine::instance().getIrqManager()->registerPciIrqHandler(
      static_cast<IrqHandler*>(this), this, IrqPolicy::pciIntxThreaded());
  if (!m_IrqId) {
    ERROR("NE2K: could not register its PCI interrupt");
    m_pBase->write8(CommandPage0Stop, NE_CMD);
    m_pBase->write8(0x00, NE_IMR);
    m_Stopping = true;
    m_ReceiveThread.stop();
    return;
  }

  // clear interrupts and enable the ones we want
  m_pBase->write8(0xff, NE_ISR);
  m_pBase->write8(EnabledInterrupts, NE_IMR);  // No IRQ for successful transmission.

  // start the card working properly
  m_pBase->write8(CommandPage0Start, NE_CMD);

  NetworkStack::instance().registerDevice(this);
  m_NetworkRegistered = true;
}

Ne2k::~Ne2k() {
  {
    LockGuard<Mutex> dmaGuard(m_DmaLock);
    m_Stopping = true;
    // NE_IMR aliases a multicast register outside page zero. Normalise the
    // page while callback and transmit register access is excluded.
    m_pBase->write8(CommandPage0Stop, NE_CMD);
    m_pBase->write8(0x00, NE_IMR);
    m_pBase->write8(0x00, NE_RBCR0);
    m_pBase->write8(0x00, NE_RBCR1);
  }
  if (m_IrqId && !Machine::instance().getIrqManager()->unregisterHandler(m_IrqId, this)) {
    FATAL("NE2K teardown could not unregister its IRQ callback.");
  }
  m_IrqId = 0;

  m_ReceiveThread.stop();
  if (m_NetworkRegistered) {
    NetworkStack::instance().deRegisterDevice(this);
    m_NetworkRegistered = false;
  }

  LockGuard<Spinlock> guard(m_PacketQueueLock);
  while (m_PacketQueue.count()) {
    packet* pending = m_PacketQueue.popFront();
    if (pending) {
      if (pending->ptr) {
        NetworkStack::instance().getMemPool().free(pending->ptr);
      }
      delete pending;
    }
  }
}

bool Ne2k::send(size_t nBytes, uintptr_t buffer) {
  if (!nBytes || nBytes > MaximumPacketLength) {
    ERROR("NE2K: invalid transmit packet length " << Dec << nBytes << Hex);
    return false;
  }

  LockGuard<Mutex> dmaGuard(m_DmaLock);
  if (m_Stopping || !m_IrqId) {
    return false;
  }

  const size_t transmitLength = nBytes < 64 ? 64 : nBytes;
  const size_t dmaLength = (transmitLength + 1) & ~static_cast<size_t>(1);

  // A second upload must not overwrite the single transmit page while the
  // 8390 is still sending from it.
  bool transmitterIdle = false;
  for (size_t transmitPoll = 0; transmitPoll < RemoteDmaPollLimit; ++transmitPoll) {
    if (!(m_pBase->read8(NE_CMD) & E8390_TRANS)) {
      transmitterIdle = true;
      break;
    }
    Processor::pause();
  }
  if (!transmitterIdle) {
    ERROR("NE2K: transmitter did not become idle");
    resetController();
    return false;
  }

  // length & address for the write
  m_pBase->write8(CommandPage0Start, NE_CMD);
  m_pBase->write8(InterruptRemoteDmaComplete, NE_ISR);
  m_pBase->write8(0, NE_RSAR0);
  m_pBase->write8(PAGE_TX, NE_RSAR1);
  m_pBase->write8(dmaLength & 0xff, NE_RBCR0);
  m_pBase->write8(dmaLength >> 8, NE_RBCR1);
  m_pBase->write8(CommandRemoteWrite, NE_CMD);

  uint16_t* data = reinterpret_cast<uint16_t*>(buffer);
  size_t i;
  for (i = 0; (i + 1) < nBytes; i += 2)
    m_pBase->write16(data[i / 2], NE_DATA);

  // handle odd byte
  if (nBytes & 1) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(buffer);
    m_pBase->write16(bytes[i], NE_DATA);
    i += 2;
  }

  // Pad short and odd-sized transfers without reading beyond the caller's
  // packet. The transmitter byte count still excludes the DMA alignment.
  for (; i < dmaLength; i += 2)
    m_pBase->write16(0, NE_DATA);

  // let it complete
  if (!waitForRemoteDma()) {
    ERROR("NE2K: remote DMA write timed out");
    resetController();
    return false;
  }
  m_pBase->write8(InterruptRemoteDmaComplete, NE_ISR);

  // execute the transmission
  m_pBase->write8(transmitLength & 0xff, NE_TBCR0);
  m_pBase->write8(transmitLength >> 8, NE_TBCR1);

  m_pBase->write8(PAGE_TX, NE_TPSR);

  // PTX is not enabled in IMR, so retire a previous completion before TXP
  // starts a new lifetime. Overflow recovery can then distinguish whether
  // this transmission completed while the receiver was being stopped.
  m_pBase->write8(InterruptTransmit | InterruptTransmitError, NE_ISR);
  m_pBase->write8(CommandTransmit, NE_CMD);

  // success!
  return true;
}

bool Ne2k::waitForRemoteDma() {
  for (size_t i = 0; i < RemoteDmaPollLimit; ++i) {
    if (m_pBase->read8(NE_ISR) & InterruptRemoteDmaComplete) {
      return true;
    }
    Processor::pause();
  }
  return false;
}

bool Ne2k::resetController() {
  m_pBase->write8(CommandPage0Stop, NE_CMD);
  m_pBase->write8(0x00, NE_IMR);

  const uint8_t resetLatch = m_pBase->read8(NE_RESET);
  m_pBase->write8(resetLatch, NE_RESET);

  bool resetComplete = false;
  for (size_t i = 0; i < ResetPollLimit; ++i) {
    if (m_pBase->read8(NE_ISR) & InterruptReset) {
      resetComplete = true;
      break;
    }
    Processor::pause();
  }
  if (!resetComplete) {
    ERROR("NE2K: controller reset timed out");
    m_pBase->write8(CommandPage0Stop, NE_CMD);
    m_pBase->write8(0x00, NE_IMR);
    return false;
  }

  // Re-establish all page-window and ring state. A DMA timeout leaves the
  // remote count/address registers untrustworthy, so merely clearing RDC is
  // not sufficient before the next transfer.
  m_pBase->write8(CommandPage0Stop, NE_CMD);
  m_pBase->write8(0x09, NE_DCR);
  m_pBase->write8(0x00, NE_RBCR0);
  m_pBase->write8(0x00, NE_RBCR1);
  m_pBase->write8(0x20, NE_RCR);
  m_pBase->write8(E8390_TXOFF, NE_TCR);
  m_pBase->write8(PAGE_TX, NE_TPSR);
  m_pBase->write8(PAGE_RX, NE_PSTART);
  m_pBase->write8(PAGE_RX, NE_BNDRY);
  m_pBase->write8(PAGE_STOP, NE_PSTOP);
  m_pBase->write8(0xff, NE_ISR);
  m_pBase->write8(0x00, NE_IMR);

  m_pBase->write8(CommandPage1Stop, NE_CMD);
  for (size_t i = 0; i < 6; ++i) {
    m_pBase->write8(m_StationInfo.mac[i], NE_PAR + i);
  }
  m_pBase->write8(PAGE_RX + 1, NE_CURR);
  for (size_t i = 0; i < MAR_SIZE; ++i) {
    m_pBase->write8(0xff, NE_MAR + i);
  }

  m_NextPacket = PAGE_RX + 1;
  m_pBase->write8(CommandPage0Stop, NE_CMD);
  m_pBase->write8(0x14, NE_RCR);
  m_pBase->write8(E8390_TXCONFIG, NE_TCR);
  m_pBase->write8(0xff, NE_ISR);
  m_pBase->write8(EnabledInterrupts, NE_IMR);
  m_pBase->write8(CommandPage0Start, NE_CMD);
  return true;
}

bool Ne2k::recoverReceiveOverflow(uint8_t irqStatus) {
  const bool wasTransmitting = (m_pBase->read8(NE_CMD) & E8390_TRANS) != 0;

  // National Semiconductor's overrun sequence requires the receiver to be
  // stopped for at least one full frame time before its remote count is
  // cleared. The reset-complete bit is explicitly not reliable here.
  m_pBase->write8(CommandPage0Stop, NE_CMD);
  if (!Time::delay(10 * Time::Multiplier::Millisecond)) {
    return resetController();
  }
  m_pBase->write8(0x00, NE_RBCR0);
  m_pBase->write8(0x00, NE_RBCR1);

  const uint8_t completion =
      (irqStatus | m_pBase->read8(NE_ISR)) & (InterruptTransmit | InterruptTransmitError);
  const bool resend = wasTransmitting && !completion;

  // Clear captured non-overrun causes before draining. A new receive during
  // the drain then leaves RX set for the next bounded pass.
  m_pBase->write8(irqStatus & ~InterruptOverflow, NE_ISR);
  m_pBase->write8(completion, NE_ISR);
  m_pBase->write8(E8390_TXOFF, NE_TCR);
  m_pBase->write8(CommandPage0Start, NE_CMD);

  if (!recv()) {
    return resetController();
  }

  m_pBase->write8(InterruptOverflow, NE_ISR);
  m_pBase->write8(E8390_TXCONFIG, NE_TCR);
  if (resend) {
    m_pBase->write8(CommandTransmit, NE_CMD);
  }
  return true;
}

void Ne2k::advanceReceiveBoundary(uint8_t nextPacket) {
  m_NextPacket = nextPacket;
  m_pBase->write8((m_NextPacket == PAGE_RX) ? (PAGE_STOP - 1) : (m_NextPacket - 1), NE_BNDRY);
}

bool Ne2k::recv() {
  constexpr size_t RingPageCount = PAGE_STOP - PAGE_RX;

  for (size_t packets = 0; packets < RingPageCount; ++packets) {
    // Refresh CURR for every packet. A packet arriving after the IRQ was
    // acknowledged must either be drained here or raise the source again.
    m_pBase->write8(CommandPage1Start, NE_CMD);
    const uint8_t current = m_pBase->read8(NE_CURR);
    m_pBase->write8(CommandPage0Start, NE_CMD);
    if (current < PAGE_RX || current >= PAGE_STOP) {
      ERROR("NE2K: receive ring reported an invalid current page");
      return false;
    }
    if (m_NextPacket == current) {
      return true;
    }

    // Want status and length
    m_pBase->write8(InterruptRemoteDmaComplete, NE_ISR);
    m_pBase->write8(0, NE_RSAR0);
    m_pBase->write8(m_NextPacket, NE_RSAR1);
    m_pBase->write8(4, NE_RBCR0);
    m_pBase->write8(0, NE_RBCR1);
    m_pBase->write8(CommandRemoteRead, NE_CMD);

    // Grab the information we want
    const uint16_t status = m_pBase->read16(NE_DATA);
    const uint16_t rawLength = m_pBase->read16(NE_DATA);
    if (!waitForRemoteDma()) {
      ERROR("NE2K: receive header DMA timed out");
      return false;
    }
    m_pBase->write8(InterruptRemoteDmaComplete, NE_ISR);

    const uint8_t nextPacket = status >> 8;
    const bool nextInRing = nextPacket >= PAGE_RX && nextPacket < PAGE_STOP;
    const bool validLength = rawLength >= 4 + MinimumPacketLength &&
                             (static_cast<size_t>(rawLength) - 4) <= MaximumPacketLength;
    bool validNext = false;
    if (nextInRing && validLength) {
      // The 8390 header count excludes the four-byte ring header. Some
      // clones round the next page one page higher, which is the only
      // tolerated discrepancy.
      size_t expected = m_NextPacket + 1 + (static_cast<size_t>(rawLength) >> 8);
      while (expected >= PAGE_STOP) {
        expected -= RingPageCount;
      }
      size_t roundedExpected = expected + 1;
      if (roundedExpected >= PAGE_STOP) {
        roundedExpected = PAGE_RX;
      }
      validNext = nextPacket == expected || nextPacket == roundedExpected;
    }
    const bool receiveOk = (status & InterruptReceive) != 0;
    if (!validNext || !validLength || !receiveOk) {
      WARNING("NE2K: dropping corrupt receive-ring entry (status="
              << Hex << status << ", length=" << Dec << rawLength << Hex << ")");
      // A valid next pointer preserves later packets. A corrupt pointer
      // cannot be retried safely, so drop the visible ring contents.
      advanceReceiveBoundary((validNext && validLength) ? nextPacket : current);
      continue;
    }

    // The 8390 byte count includes the four-byte Ethernet CRC.
    const uint16_t length = rawLength - 4;
    // Remote byte count decrements by two in word mode. Round the hardware
    // transfer up while keeping the packet handed to the stack exact.
    const uint16_t dmaLength = (length + 1) & ~static_cast<uint16_t>(1);

    // packet buffer
    uint8_t* tmp = reinterpret_cast<uint8_t*>(NetworkStack::instance().getMemPool().allocateNow());
    if (!tmp) {
      advanceReceiveBoundary(nextPacket);
      continue;
    }
    uint16_t* packBuffer = reinterpret_cast<uint16_t*>(tmp);
    ByteSet(tmp, 0, length);

    m_pBase->write8(InterruptRemoteDmaComplete, NE_ISR);
    m_pBase->write8(4, NE_RSAR0);
    m_pBase->write8(m_NextPacket, NE_RSAR1);
    m_pBase->write8(dmaLength & 0xff, NE_RBCR0);
    m_pBase->write8(dmaLength >> 8, NE_RBCR1);
    m_pBase->write8(CommandRemoteRead, NE_CMD);

    // read the packet
    int i, words = length / 2, oddbytes = length % 2;
    for (i = 0; i < words; ++i)
      packBuffer[i] = m_pBase->read16(NE_DATA);
    if (oddbytes) {
      for (i = 0; i < oddbytes; ++i)
        tmp[(length - oddbytes) + i] =
            m_pBase->read16(NE_DATA) & 0xFF;  // odd packet length handler
    }

    if (!waitForRemoteDma()) {
      ERROR("NE2K: receive payload DMA timed out");
      NetworkStack::instance().getMemPool().free(reinterpret_cast<uintptr_t>(tmp));
      advanceReceiveBoundary(nextPacket);
      return false;
    }
    m_pBase->write8(InterruptRemoteDmaComplete, NE_ISR);

    advanceReceiveBoundary(nextPacket);

    // push onto the queue
    packet* p = new packet;
    p->ptr = reinterpret_cast<uintptr_t>(packBuffer);
    p->len = length;

#ifdef NE2K_NO_THREADS

    NetworkStack::instance().receive(p->len, p->ptr, this, 0);

    NetworkStack::instance().getMemPool().free(p->ptr);
    delete p;

#else

    {
      LockGuard<Spinlock> guard(m_PacketQueueLock);
      m_PacketQueue.pushBack(p);
      m_PacketQueueSize.release();
    }

#endif
  }

  WARNING("NE2K: receive ring drain exceeded its page budget");
  return false;
}

int Ne2k::trampoline(void* p) {
  Ne2k* pNe = reinterpret_cast<Ne2k*>(p);
  pNe->receiveThread();
}

void Ne2k::receiveThread() {
  while (true) {
    // handle the incoming packet
    if (!m_PacketQueueSize.acquire()) {
      continue;
    }

    // grab from the front
    packet* p = 0;
    {
      LockGuard<Spinlock> guard(m_PacketQueueLock);
      p = m_PacketQueue.popFront();
    }

    if (!p)
      continue;
    if (!p->ptr || !p->len) {
      if (p->ptr) {
        NetworkStack::instance().getMemPool().free(p->ptr);
      }
      delete p;
      continue;
    }

    // pass to the network stack
    NetworkStack::instance().receive(p->len, p->ptr, this, 0);

    // destroy the buffer now that it's handled
    NetworkStack::instance().getMemPool().free(p->ptr);
    delete p;
  }
}

bool Ne2k::setStationInfo(const StationInfo& info) {
  // free the old DNS servers list, if there is one
  if (m_StationInfo.dnsServers)
    delete[] m_StationInfo.dnsServers;

  // MAC isn't changeable, so set it all manually
  m_StationInfo.ipv4 = info.ipv4;
  NOTICE("NE2K: Setting ipv4, " << info.ipv4.toString() << ", " << m_StationInfo.ipv4.toString()
                                << "...");

  m_StationInfo.ipv6 = info.ipv6;
  m_StationInfo.nIpv6Addresses = info.nIpv6Addresses;
  NOTICE("NE2K: Copied " << info.nIpv6Addresses << " IPv6 addresses.");

  m_StationInfo.subnetMask = info.subnetMask;
  NOTICE("NE2K: Setting subnet mask, " << info.subnetMask.toString() << ", "
                                       << m_StationInfo.subnetMask.toString() << "...");
  m_StationInfo.gateway = info.gateway;
  NOTICE("NE2K: Setting gateway, " << info.gateway.toString() << ", "
                                   << m_StationInfo.gateway.toString() << "...");

  // Callers do not free their dnsServers memory
  m_StationInfo.dnsServers = info.dnsServers;
  m_StationInfo.nDnsServers = info.nDnsServers;
  NOTICE("NE2K: Setting DNS servers [" << Dec << m_StationInfo.nDnsServers << Hex
                                       << " servers being set]...");

  return true;
}

const StationInfo& Ne2k::getStationInfo() {
  return m_StationInfo;
}

IrqDisposition Ne2k::irq(irq_id_t number) {
  constexpr size_t PassLimit = 8;
  bool handled = false;
  (void)number;

  LockGuard<Mutex> dmaGuard(m_DmaLock);
  if (m_Stopping) {
    return IrqDisposition::NotHandled;
  }
  for (size_t pass = 0; pass < PassLimit; ++pass) {
    const uint8_t irqStatus = m_pBase->read8(NE_ISR) & EnabledInterrupts;
    if (!irqStatus) {
      break;
    }
    handled = true;

    if (irqStatus & InterruptOverflow) {
      WARNING("NE2K: Receive buffer overflow");
      if (!recoverReceiveOverflow(irqStatus)) {
        ERROR("NE2K: receive overflow recovery failed");
      }
      continue;
    }

    // Acknowledge the captured bits before draining. If another packet
    // arrives during the drain it sets RX again and the next pass owns it.
    m_pBase->write8(irqStatus, NE_ISR);

    if (irqStatus & (InterruptReceive | InterruptReceiveError)) {
      if (!recv()) {
        resetController();
      }
    }
    if (irqStatus & InterruptTransmitError) {
      WARNING("NE2K: Packet transmit failed!");
    }
    if (irqStatus & InterruptCounters) {
      WARNING("NE2K: Counter overflow");
    }
  }

  return handled ? IrqDisposition::Handled : IrqDisposition::NotHandled;
}

bool Ne2k::isConnected() {
  // The NE2K chip doesn't support detecting the link state, so we have to
  // just assume the link is active.
  return true;
}
