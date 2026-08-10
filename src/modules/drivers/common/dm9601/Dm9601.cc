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

#include "Dm9601.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/network/IpAddress.h"
#include "pedigree/kernel/network/MacAddress.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/MemoryPool.h"
#include "pedigree/kernel/utilities/PointerGuard.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/network-stack/NetworkStack.h"
#include "modules/system/usb/UsbConstants.h"

namespace {
constexpr size_t MaxPacketBytes = 1518;
constexpr size_t RxUsbOverhead = 8;
constexpr uint32_t ControlTransferTimeoutMs = 250;
constexpr uint32_t BulkTransferTimeoutMs = 1000;
constexpr Time::Timestamp ResetTimeout = 20 * Time::Multiplier::Millisecond;
constexpr Time::Timestamp SharedOperationTimeout = 20 * Time::Multiplier::Millisecond;
constexpr Time::Timestamp LinkWaitTimeout = Time::Multiplier::Second;
constexpr Time::Timestamp LinkPollInterval = 50 * Time::Multiplier::Millisecond;
constexpr Time::Timestamp TxReadyTimeout = 100 * Time::Multiplier::Millisecond;
constexpr Time::Timestamp RegisterPollInterval = Time::Multiplier::Millisecond;
constexpr Time::Timestamp SharedPollInterval = Time::Multiplier::Millisecond;
constexpr size_t ResetPollLimit = 16;
constexpr size_t SharedPollLimit = 16;
constexpr size_t LinkPollLimit = 16;
constexpr size_t TxReadyPollLimit = 16;
constexpr size_t TxPaddingLimit = 3;

constexpr uint8_t NetworkStatusLinkUp = 1 << 6;
constexpr uint8_t NetworkStatusTxBusy = 1 << 4;
constexpr uint8_t PhyControlBusy = 1;
constexpr uint8_t RxStatusErrorMask = 0xBF;
}  // namespace

Dm9601::Dm9601(UsbDevice* pDev)
    : UsbDevice(pDev),
      ::Network(),
      m_pInEndpoint(0),
      m_pOutEndpoint(0),
      m_TxLock(),
      m_SharedRegisterLock(),
      m_IncomingPackets(false),
      m_RxQueueHead(nullptr),
      m_RxQueueTail(nullptr),
      m_RxPacketQueueLock(),
      m_TxPacket(0),
      m_Running(false),
      m_Registered(false),
      m_Operations(),
      m_PacketWorker(),
      m_ReceiveWorker() {}

Dm9601::~Dm9601() {
  // Closing admission first makes deregistration a publication change rather
  // than the only protection against a late network-stack caller.
  m_Operations.close();
  m_Running = false;

  if (m_Registered) {
    NetworkStack::instance().deRegisterDevice(this);
    m_Registered = false;
  }

  // doSync cancellation drains its USB callback before the RX thread joins.
  // The packet worker is then unable to race queue retirement below.
  m_ReceiveWorker.stop();
  m_IncomingPackets.release();
  m_PacketWorker.stop();
  m_Operations.wait();

  Packet* packets = nullptr;
  {
    LockGuard<Spinlock> guard(m_RxPacketQueueLock);
    packets = m_RxQueueHead;
    m_RxQueueHead = nullptr;
    m_RxQueueTail = nullptr;
  }
  while (packets) {
    Packet* packet = packets;
    packets = packet->next;
    if (packet->buffer) {
      NetworkStack::instance().getMemPool().free(packet->buffer);
    }
    delete packet;
  }
}

void Dm9601::initialiseDriver() {
  // Grab USB endpoints for the driver to use later
  for (size_t i = 0; i < m_pInterface->endpointList.count(); i++) {
    Endpoint* pEndpoint = m_pInterface->endpointList[i];
    if (!m_pInEndpoint && (pEndpoint->nTransferType == Endpoint::Bulk) && pEndpoint->bIn)
      m_pInEndpoint = pEndpoint;
    if (!m_pOutEndpoint && (pEndpoint->nTransferType == Endpoint::Bulk) && pEndpoint->bOut)
      m_pOutEndpoint = pEndpoint;
    if (m_pInEndpoint && m_pOutEndpoint)
      break;
  }

  if (!m_pInEndpoint) {
    ERROR("dm9601: no bulk IN endpoint");
    return;
  }

  if (!m_pOutEndpoint) {
    ERROR("dm9601: no bulk OUT endpoint");
    return;
  }

  uint8_t mac[6] ALIGN(16) = {};
  for (size_t i = 0; i < 3; ++i) {
    uint16_t word = 0;
    if (!readEeprom(i, word)) {
      ERROR("dm9601: could not read its MAC address from EEPROM");
      return;
    }
    mac[i * 2] = static_cast<uint8_t>(word);
    mac[i * 2 + 1] = static_cast<uint8_t>(word >> 8);
    m_StationInfo.mac.setMac(mac[i * 2], i * 2);
    m_StationInfo.mac.setMac(mac[i * 2 + 1], i * 2 + 1);
  }

  NOTICE("DM9601: MAC " << m_StationInfo.mac[0] << ":" << m_StationInfo.mac[1] << ":"
                        << m_StationInfo.mac[2] << ":" << m_StationInfo.mac[3] << ":"
                        << m_StationInfo.mac[4] << ":" << m_StationInfo.mac[5]);

  if (!resetDevice()) {
    ERROR("dm9601: device reset did not complete");
    return;
  }

  // Publish no software-visible device until every setup transfer succeeded.
  if (!writeRegister(NetworkControl, 0) || !writeRegister(GeneralPurposeCtl, 0x1) ||
      !writeRegister(GeneralPurpose, 0) || !writeRegister(BackPressThreshold, 0x37) ||
      !writeRegister(FlowControl, 0x38) || !writeRegister(RxFlowControl, 1 | (1 << 3) | (1 << 5)) ||
      !writeRegister(UsbControl, 0) ||
      !writeRegister(PhysicalAddress, reinterpret_cast<uintptr_t>(mac), sizeof(mac)) ||
      !writeRegister(RxControl, 5 | (1 << 3))) {
    ERROR("dm9601: device setup transfer failed");
    return;
  }

  if (!waitForLink()) {
    ERROR("dm9601: could not read initial link status");
    return;
  }

  m_Running = true;
  Thread* pThread =
      new Thread(Processor::information().getCurrentThread()->getParent(), trampoline, this);
  pThread->setName("DM9601 RX worker");
  m_PacketWorker.adopt(pThread);
  pThread =
      new Thread(Processor::information().getCurrentThread()->getParent(), recvTrampoline, this);
  pThread->setName("DM9601 RX loop");
  m_ReceiveWorker.adopt(pThread);

  NetworkStack::instance().registerDevice(this);
  m_Registered = true;

  m_UsbState = HasDriver;
}

int Dm9601::recvTrampoline(void* p) {
  Dm9601* pDm9601 = reinterpret_cast<Dm9601*>(p);
  pDm9601->receiveLoop();
  return 0;
}

int Dm9601::trampoline(void* p) {
  Dm9601* pDm9601 = reinterpret_cast<Dm9601*>(p);
  pDm9601->receiveThread();
  return 0;
}

void Dm9601::receiveThread() {
  while (true) {
    if (!m_IncomingPackets.acquire()) {
      if (!m_Running)
        return;
      continue;
    }

    TerminationDeferral packetLifetime;
    Packet* pPacket = nullptr;
    {
      LockGuard<Spinlock> guard(m_RxPacketQueueLock);
      pPacket = m_RxQueueHead;
      if (pPacket) {
        m_RxQueueHead = pPacket->next;
        if (!m_RxQueueHead)
          m_RxQueueTail = nullptr;
      }
    }
    if (!pPacket) {
      if (!m_Running)
        return;
      continue;
    }

    if (m_Running) {
      NetworkStack::instance().receive(pPacket->len, pPacket->buffer + pPacket->offset, this, 0);
    }

    uintptr_t buffer = pPacket->buffer;
    delete pPacket;

    NetworkStack::instance().getMemPool().free(buffer);
    if (!m_Running)
      return;
  }
}

void Dm9601::receiveLoop() {
  while (m_Running) {
    doReceive();
  }
}

bool Dm9601::send(size_t nBytes, uintptr_t buffer) {
  TerminationDeferral operationLifetime;
  OperationBarrier::Lease operation;
  if (!m_Operations.tryAcquire(operation) || !m_Running || !buffer || !nBytes ||
      nBytes > MaxPacketBytes || !m_pOutEndpoint || !m_pOutEndpoint->nMaxPacketSize) {
    return false;
  }

  size_t payloadBytes = nBytes < 64 ? 64 : nBytes;
  size_t txSize = payloadBytes + 2;
  for (size_t padding = 0;
       padding < TxPaddingLimit && ((txSize & 1) || !(txSize % m_pOutEndpoint->nMaxPacketSize));
       ++padding) {
    ++payloadBytes;
    ++txSize;
  }
  if ((txSize & 1) || !(txSize % m_pOutEndpoint->nMaxPacketSize))
    return false;

  const size_t txWords = (txSize + sizeof(uint16_t) - 1) / sizeof(uint16_t);
  uint16_t* pBuffer = new uint16_t[txWords];
  PointerGuard<uint16_t> bufferGuard(pBuffer, true);
  ByteSet(pBuffer, 0, txWords * sizeof(uint16_t));
  *pBuffer = HOST_TO_LITTLE16(static_cast<uint16_t>(payloadBytes));
  MemoryCopy(&pBuffer[1], reinterpret_cast<void*>(buffer), nBytes);

  // The mutex begins after all caller-independent allocation and formatting.
  LockGuard<Mutex> guard(m_TxLock);
  if (!m_Running || !waitForTxReady())
    return false;

  const ssize_t ret =
      syncOut(m_pOutEndpoint, reinterpret_cast<uintptr_t>(pBuffer), txSize, BulkTransferTimeoutMs);
  if (ret != static_cast<ssize_t>(txSize)) {
    WARNING("dm9601: transmit transfer failed or was short (" << ret << "/" << txSize << ")");
    return false;
  }

  // Grab the TX status register so we can find errors
  uint8_t txStatus ALIGN(16) = 0;
  uint8_t networkStatus ALIGN(16) = 0;
  const uint8_t txStatusRegister = static_cast<uint8_t>(TxStatus1 + m_TxPacket);
  m_TxPacket = (m_TxPacket + 1) % 2;
  if (!readRegister(txStatusRegister, reinterpret_cast<uintptr_t>(&txStatus), 1)) {
    WARNING("dm9601: could not read transmit status");
    return false;
  }

  // Read and clear the network status (which will contain the "packet
  // complete" indicator)
  if (!readRegister(NetworkStatus, reinterpret_cast<uintptr_t>(&networkStatus), 1)) {
    WARNING("dm9601: could not clear network status after transmit");
    return false;
  }

  return true;
}

void Dm9601::doReceive() {
  TerminationDeferral receiveLifetime;
  uintptr_t buff = NetworkStack::instance().getMemPool().allocate();
  if (!buff)
    return;

  const ssize_t ret =
      syncIn(m_pInEndpoint, buff, MaxPacketBytes + RxUsbOverhead, BulkTransferTimeoutMs);

  if (ret < 0) {
    if (m_Running) {
      DEBUG_LOG("dm9601: receive transfer did not complete: " << ret);
    }
    NetworkStack::instance().getMemPool().free(buff);
    return;
  }

  if (!m_Running) {
    NetworkStack::instance().getMemPool().free(buff);
    return;
  }

  if (ret < 7) {
    WARNING("dm9601: receive transfer was too short: " << ret);
    NetworkStack::instance().getMemPool().free(buff);
    badPacket();
    return;
  }

  uint8_t* pBuffer = reinterpret_cast<uint8_t*>(buff);
  uint8_t rxstatus = pBuffer[0];
  const uint16_t wireLength =
      static_cast<uint16_t>(pBuffer[1]) | static_cast<uint16_t>(pBuffer[2] << 8);
  if (wireLength < 4 || wireLength > static_cast<size_t>(ret - 3)) {
    WARNING("dm9601: invalid receive length " << wireLength << " in " << ret << " byte transfer");
    NetworkStack::instance().getMemPool().free(buff);
    badPacket();
    return;
  }
  const size_t len = wireLength - 4;

  if ((rxstatus & RxStatusErrorMask) || !len || len > MaxPacketBytes) {
    WARNING("dm9601: rx failure: " << rxstatus << ", length was " << len);
    NetworkStack::instance().getMemPool().free(buff);
    badPacket();
    return;
  }

  Packet* pPacket = new Packet;
  pPacket->buffer = buff;
  pPacket->len = len;
  pPacket->offset = 3;
  pPacket->next = nullptr;

  bool queued = false;
  {
    LockGuard<Spinlock> guard(m_RxPacketQueueLock);
    if (m_Running) {
      if (m_RxQueueTail)
        m_RxQueueTail->next = pPacket;
      else
        m_RxQueueHead = pPacket;
      m_RxQueueTail = pPacket;
      queued = true;
    }
  }

  if (queued) {
    m_IncomingPackets.release();
  } else {
    delete pPacket;
    NetworkStack::instance().getMemPool().free(buff);
  }
}

bool Dm9601::setStationInfo(const StationInfo& info) {
  // Free the old DNS server list, if there is one
  if (m_StationInfo.dnsServers)
    delete[] m_StationInfo.dnsServers;

  // MAC isn't changeable, so set it all manually
  m_StationInfo.ipv4 = info.ipv4;
  NOTICE("DM9601: Setting ipv4, " << info.ipv4.toString() << ", " << m_StationInfo.ipv4.toString()
                                  << "...");
  m_StationInfo.ipv6 = info.ipv6;

  m_StationInfo.subnetMask = info.subnetMask;
  NOTICE("DM9601: Setting subnet mask, " << info.subnetMask.toString() << ", "
                                         << m_StationInfo.subnetMask.toString() << "...");
  m_StationInfo.gateway = info.gateway;
  NOTICE("DM9601: Setting gateway, " << info.gateway.toString() << ", "
                                     << m_StationInfo.gateway.toString() << "...");

  // Callers do not free their dnsServers memory
  m_StationInfo.dnsServers = info.dnsServers;
  m_StationInfo.nDnsServers = info.nDnsServers;
  NOTICE("DM9601: Setting DNS servers [" << Dec << m_StationInfo.nDnsServers << Hex
                                         << " servers being set]...");

  return true;
}

const StationInfo& Dm9601::getStationInfo() {
  return m_StationInfo;
}

bool Dm9601::readRegister(uint8_t reg, uintptr_t buffer, size_t nBytes) {
  if (!buffer || (nBytes > 0xFF))
    return false;
  return controlRequest(UsbRequestType::Vendor | UsbRequestDirection::In, ReadRegister, 0, reg,
                        nBytes, buffer, ControlTransferTimeoutMs);
}

bool Dm9601::writeRegister(uint8_t reg, uintptr_t buffer, size_t nBytes) {
  if (!buffer || (nBytes > 0xFF))
    return false;
  return controlRequest(UsbRequestType::Vendor | UsbRequestDirection::Out, WriteRegister, 0, reg,
                        nBytes, buffer, ControlTransferTimeoutMs);
}

bool Dm9601::writeRegister(uint8_t reg, uint8_t data) {
  return controlRequest(UsbRequestType::Vendor | UsbRequestDirection::Out, WriteRegister1, data,
                        reg, 0, 0, ControlTransferTimeoutMs);
}

bool Dm9601::readMemory(uint16_t offset, uintptr_t buffer, size_t nBytes) {
  if (!buffer || (nBytes > 0xFF))
    return false;
  return controlRequest(UsbRequestType::Vendor | UsbRequestDirection::In, ReadMemory, 0, offset,
                        nBytes, buffer, ControlTransferTimeoutMs);
}

bool Dm9601::writeMemory(uint16_t offset, uintptr_t buffer, size_t nBytes) {
  if (!buffer || (nBytes > 0xFF))
    return false;
  return controlRequest(UsbRequestType::Vendor | UsbRequestDirection::Out, WriteMemory, 0, offset,
                        nBytes, buffer, ControlTransferTimeoutMs);
}

bool Dm9601::writeMemory(uint16_t offset, uint8_t data) {
  return controlRequest(UsbRequestType::Vendor | UsbRequestDirection::Out, WriteMemory1, data,
                        offset, 0, 0, ControlTransferTimeoutMs);
}

bool Dm9601::readEeprom(uint8_t offset, uint16_t& value) {
  return readSharedWord(false, offset, value);
}

bool Dm9601::writeEeprom(uint8_t offset, uint16_t data) {
  return writeSharedWord(false, offset, data);
}

bool Dm9601::readMii(uint8_t offset, uint16_t& value) {
  return readSharedWord(true, offset, value);
}

bool Dm9601::writeMii(uint8_t offset, uint16_t data) {
  return writeSharedWord(true, offset, data);
}

bool Dm9601::readSharedWord(bool phy, uint8_t offset, uint16_t& value) {
  LockGuard<Mutex> guard(m_SharedRegisterLock);
  uint16_t rawValue ALIGN(16) = 0;
  const uint8_t address = phy ? static_cast<uint8_t>(offset | 0x40) : offset;
  const uint8_t command = phy ? 0x0C : 0x04;

  if (!writeRegister(PhyAddress, address) || !writeRegister(PhyControl, command) ||
      !waitForSharedOperation()) {
    (void)writeRegister(PhyControl, 0);
    return false;
  }

  if (!writeRegister(PhyControl, 0) ||
      !readRegister(PhyLowByte, reinterpret_cast<uintptr_t>(&rawValue), sizeof(rawValue))) {
    return false;
  }

  value = LITTLE_TO_HOST16(rawValue);
  return true;
}

bool Dm9601::writeSharedWord(bool phy, uint8_t offset, uint16_t value) {
  LockGuard<Mutex> guard(m_SharedRegisterLock);
  uint16_t rawValue ALIGN(16) = HOST_TO_LITTLE16(value);
  const uint8_t address = phy ? static_cast<uint8_t>(offset | 0x40) : offset;
  const uint8_t command = phy ? 0x1A : 0x12;

  if (!writeRegister(PhyLowByte, reinterpret_cast<uintptr_t>(&rawValue), sizeof(rawValue)) ||
      !writeRegister(PhyAddress, address) || !writeRegister(PhyControl, command) ||
      !waitForSharedOperation()) {
    (void)writeRegister(PhyControl, 0);
    return false;
  }

  return writeRegister(PhyControl, 0);
}

bool Dm9601::waitForSharedOperation() {
  const Time::Timestamp deadline = Time::getTicks() + SharedOperationTimeout;
  for (size_t poll = 0; poll < SharedPollLimit; ++poll) {
    uint8_t control ALIGN(16) = 0;
    if (!readRegister(PhyControl, reinterpret_cast<uintptr_t>(&control), sizeof(control))) {
      return false;
    }
    if (!(control & PhyControlBusy))
      return true;
    if (Time::getTicks() >= deadline || poll + 1 == SharedPollLimit ||
        !Time::delay(SharedPollInterval))
      break;
  }

  WARNING("dm9601: EEPROM/PHY operation timed out");
  return false;
}

bool Dm9601::resetDevice() {
  if (!writeRegister(NetworkControl, 1) || !Time::delay(20 * Time::Multiplier::Microsecond)) {
    return false;
  }

  const Time::Timestamp deadline = Time::getTicks() + ResetTimeout;
  for (size_t poll = 0; poll < ResetPollLimit; ++poll) {
    uint8_t control ALIGN(16) = 0;
    if (!readRegister(NetworkControl, reinterpret_cast<uintptr_t>(&control), sizeof(control))) {
      return false;
    }
    if (!(control & 1))
      return true;
    if (Time::getTicks() >= deadline || poll + 1 == ResetPollLimit ||
        !Time::delay(RegisterPollInterval))
      break;
  }

  return false;
}

bool Dm9601::waitForLink() {
  const Time::Timestamp deadline = Time::getTicks() + LinkWaitTimeout;
  for (size_t poll = 0; poll < LinkPollLimit; ++poll) {
    uint8_t status ALIGN(16) = 0;
    if (!readRegister(NetworkStatus, reinterpret_cast<uintptr_t>(&status), sizeof(status))) {
      return false;
    }
    if (status & NetworkStatusLinkUp)
      return true;
    if (Time::getTicks() >= deadline || poll + 1 == LinkPollLimit)
      break;
    if (!Time::delay(LinkPollInterval))
      return false;
  }

  NOTICE("dm9601: link is down; continuing without blocking USB discovery");
  return true;
}

bool Dm9601::waitForTxReady() {
  const Time::Timestamp deadline = Time::getTicks() + TxReadyTimeout;
  for (size_t poll = 0; poll < TxReadyPollLimit; ++poll) {
    if (!m_Running)
      return false;

    uint8_t status ALIGN(16) = 0;
    if (!readRegister(NetworkStatus, reinterpret_cast<uintptr_t>(&status), sizeof(status))) {
      return false;
    }
    if (!(status & NetworkStatusTxBusy))
      return true;
    if (Time::getTicks() >= deadline || poll + 1 == TxReadyPollLimit ||
        !Time::delay(RegisterPollInterval))
      break;
  }

  WARNING("dm9601: transmitter remained busy");
  return false;
}
