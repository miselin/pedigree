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

/** Ported 3c90x driver from Etherboot.
 * I've changed it around to fit into our structure, and
 * to be a little more portable - Matt
 */

/*
 * 3c90x.c -- This file implements the 3c90x driver for etherboot.  Written
 * by Greg Beeley, Greg.Beeley@LightSys.org.  Modified by Steve Smith,
 * Steve.Smith@Juno.Com. Alignment bug fix Neil Newell (nn@icenoir.net).
 *
 * This program Copyright (C) 1999 LightSys Technology Services, Inc.
 * Portions Copyright (C) 1999 Steve Smith
 *
 * This program may be re-distributed in source or binary form, modified,
 * sold, or copied for any purpose, provided that the above copyright message
 * and this text are included with all source copies or derivative works, and
 * provided that the above copyright message and this text are included in the
 * documentation of any binary-only distributions.  This program is distributed
 * WITHOUT ANY WARRANTY, without even the warranty of FITNESS FOR A PARTICULAR
 * PURPOSE or MERCHANTABILITY.  Please read the associated documentation
 * "3c90x.txt" before compiling and using this driver.
 *
 * --------
 *
 * Program written with the assistance of the 3com documentation for
 * the 3c905B-TX card, as well as with some assistance from the 3c59x
 * driver Donald Becker wrote for the Linux kernel, and with some assistance
 * from the remainder of the Etherboot distribution.
 *
 * REVISION HISTORY:
 *
 * v0.10	1-26-1998	GRB	Initial implementation.
 * v0.90	1-27-1998	GRB	System works.
 * v1.00pre1	2-11-1998	GRB	Got prom boot issue fixed.
 * v2.0		9-24-1999	SCS	Modified for 3c905 (from 3c905b code)
 *					Re-wrote poll and transmit for
 *					better error recovery and heavy
 *					network traffic operation
 * v2.01    5-26-2993 NN Fixed driver alignment issue which
 *                  caused system lockups if driver structures
 *                  not 8-byte aligned.
 *
 */

#include "3Com90x.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/network/IpAddress.h"
#include "pedigree/kernel/network/MacAddress.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/utility.h"

#include "3Com90xConstants.h"
#include "modules/drivers/common/DmaBuffer.h"
#include "modules/system/network-stack/NetworkStack.h"

namespace {
constexpr size_t CommandPollLimit = 100;
constexpr size_t ResetCommandPollLimit = 5000;
constexpr size_t DownloadPollLimit = 100;
constexpr size_t EepromPollLimit = 100;
constexpr uint32_t UpPacketComplete = 1U << 15U;
constexpr uint32_t UpPacketError = 1U << 14U;
constexpr uint32_t UpPacketLengthMask = 0x1FFF;
constexpr size_t Dma32Constraints =
    PhysicalMemoryManager::continuous | PhysicalMemoryManager::below4GB;
}  // namespace

bool Nic3C90x::issueCommand(int cmd, int param) {
  const size_t pollLimit =
      (cmd == cmdGlobalReset || cmd == cmdRxReset) ? ResetCommandPollLimit : CommandPollLimit;
  const auto waitUntilIdle = [this, pollLimit]() {
    for (size_t poll = 0; poll < pollLimit; ++poll) {
      if (!(m_pBase->read16(regCommandIntStatus_w) & INT_CMDINPROGRESS))
        return true;

      if (!Time::delay(Time::Multiplier::Millisecond))
        break;
    }
    return false;
  };

  // A timed-out predecessor may still own the command register. Never
  // overwrite it while trying to recover or quiesce the controller.
  if (!waitUntilIdle()) {
    ERROR("3C90x: command register remained busy before command " << Hex << cmd);
    return false;
  }

  /** Build the cmd. **/
  uint32_t val = cmd;
  val <<= 11;
  val |= param;

  /** Send the cmd to the cmd register */
  m_pBase->write16(val, regCommandIntStatus_w);

  /** Wait for the cmd to complete, if necessary **/
  if (waitUntilIdle())
    return true;

  ERROR("3C90x: command " << Hex << cmd << " timed out");
  return false;
}

int Nic3C90x::setWindow(int window) {
  /** Window already as set? **/
  if (m_CurrentWindow == window)
    return 0;

  /** Issue the window command **/
  if (!issueCommand(cmdSelectRegisterWindow, window))
    return -1;
  m_CurrentWindow = window;

  return 0;
}

bool Nic3C90x::waitForEepromReady() {
  for (size_t poll = 0; poll < EepromPollLimit; ++poll) {
    if (!(m_pBase->read16(regEepromCommand_0_w) & (1U << 15U)))
      return true;

    if (!Time::delay(Time::Multiplier::Millisecond))
      break;
  }

  ERROR("3C90x: EEPROM command timed out");
  return false;
}

bool Nic3C90x::readEeprom(int address, uint16_t& value) {
  /** Select correct window **/
  if (setWindow(winEepromBios0) < 0)
    return false;

  /** Make sure the eeprom isn't busy **/
  if (!waitForEepromReady())
    return false;

  /** Read the value */
  m_pBase->write16(address + (0x02 << 6), regEepromCommand_0_w);
  if (!waitForEepromReady())
    return false;

  value = m_pBase->read16(regEepromData_0_w);
  return true;
}

int Nic3C90x::writeEepromWord(int address, uint16_t value) {
  /** Select register window **/
  if (setWindow(winEepromBios0) < 0)
    return -1;

  /** Verify Eeprom not busy **/
  if (!waitForEepromReady())
    return -1;

  /** Issue WriteEnable, and wait for completion **/
  m_pBase->write16(0x30, regEepromCommand_0_w);
  if (!waitForEepromReady())
    return -1;

  /** Issue EraseReigster, and wait for completion **/
  m_pBase->write16(address + (0x03 << 6), regEepromCommand_0_w);
  if (!waitForEepromReady())
    return -1;

  /** Send the new data to the eeprom, and wait for completion **/
  m_pBase->write16(value, regEepromData_0_w);
  m_pBase->write16(0x30, regEepromCommand_0_w);
  if (!waitForEepromReady())
    return -1;

  /** Burn the new data into the eeprom, and wait for completion **/
  m_pBase->write16(address + (0x01 << 6), regEepromCommand_0_w);
  if (!waitForEepromReady())
    return -1;

  return 0;
}

int Nic3C90x::writeEeprom(int address, uint16_t value) {
  int cksum = 0;
  uint16_t v = 0;
  int i;
  int maxAddress, cksumAddress;

  if (m_isBrev) {
    maxAddress = 0x1f;
    cksumAddress = 0x20;
  } else {
    maxAddress = 0x16;
    cksumAddress = 0x17;
  }

  /** Write the value. **/
  if (writeEepromWord(address, value) == -1)
    return -1;

  /** Recompute the checksum **/
  for (i = 0; i <= maxAddress; i++) {
    if (!readEeprom(i, v))
      return -1;
    cksum ^= (v & 0xff);
    cksum ^= ((v >> 8) & 0xff);
  }

  /** Write the checksum to the location in the eeprom **/
  if (writeEepromWord(cksumAddress, cksum) == -1)
    return -1;

  return 0;
}

bool Nic3C90x::reset() {
#ifdef CFG_3C90X_PRESERVE_XCVR
  int cfg;

  /** Read the current InternalConfig value **/
  if (setWindow(winTxRxOptions3) < 0)
    return false;
  cfg = m_pBase->read32(regInternalConfig_3_l);
#endif

  /** Send the reset command to the card **/
  NOTICE("3C90x: Issuing RESET");
  if (!issueCommand(cmdGlobalReset, 0))
    return false;

  // Global reset selects window zero independently of the software cache.
  m_CurrentWindow = 0;

  /** Global reset command resets station mask, non-B revision cards
   * require explicit reset of values
   */
  if (setWindow(winAddressing2) < 0)
    return false;
  m_pBase->write16(0, regStationAddress_2_3w + 0);
  m_pBase->write16(0, regStationAddress_2_3w + 2);
  m_pBase->write16(0, regStationAddress_2_3w + 4);

#ifdef CFG_3C90X_PRESERVE_XCVR
  /** Reset the original InternalConfig value from before reset **/
  if (setWindow(winTxRxOptions3) < 0)
    return false;
  m_pBase->write32(cfg, regInternalConfig_3_l);

  /** Enable DC converter for 10-Base-T **/
  if ((cfg & 0x0300) == 0x0300)
    if (!issueCommand(cmdEnableDcConverter, 0))
      return false;
#endif

  /** Issue transmit reset, wait for command completion **/
  if (!issueCommand(cmdTxReset, 0))
    return false;

  if (!m_isBrev)
    m_pBase->write8(0x01, regTxFreeThresh_b);
  if (!issueCommand(cmdTxEnable, 0))
    return false;

  /** Reset of the receiver on B-revision cards re-negotiates the link
   * Takes several seconds
   */
  if (m_isBrev) {
    if (!issueCommand(cmdRxReset, 0x04))
      return false;
  } else {
    if (!issueCommand(cmdRxReset, 0x00))
      return false;
  }

  if (!issueCommand(cmdRxEnable, 0))
    return false;

  /** Configure indications but leave the physical source gated. **/
  if (!issueCommand(cmdSetInterruptEnable, 0) ||
      !issueCommand(cmdSetIndicationEnable, ENABLED_INTS) ||
      !issueCommand(cmdAcknowledgeInterrupt, 0xff)) {
    return false;
  }

  return true;
}

bool Nic3C90x::quiesce() {
  if (!issueCommand(cmdSetInterruptEnable, 0) || !issueCommand(cmdSetIndicationEnable, 0)) {
    return false;
  }

  // Stop both bus-master engines before their descriptor regions can leave
  // this object's lifetime.
  if (!issueCommand(cmdRxDisable, 0) || !issueCommand(cmdTxDisable, 0) ||
      !issueCommand(cmdStallCtl, 0) || !issueCommand(cmdStallCtl, 2)) {
    return false;
  }

  m_pBase->write32(0, regUpListPtr_l);
  m_pBase->write32(0, regDnListPtr_l);
  return true;
}

bool Nic3C90x::stopDeviceLocked() {
  if (m_Stopping)
    return true;

  // Publish callback closure only after this device can no longer assert the
  // shared line or access its descriptor and packet storage.
  if (!quiesce())
    return false;

  m_Active = false;
  m_Stopping = true;
  m_TxSuccessful = false;
  m_TxMutex.release();
  return true;
}

bool Nic3C90x::send(size_t nBytes, uintptr_t buffer) {
  if (!buffer || !nBytes || nBytes > UpPacketLengthMask) {
    ERROR("3C90x: invalid transmit packet length " << Dec << nBytes << Hex);
    return false;
  }

  LockGuard<Mutex> sendGuard(m_SendLock);

  {
    LockGuard<Mutex> deviceGuard(m_DeviceLock);
    if (!m_Active || m_Stopping)
      return false;

    /** Stall the download engine **/
    if (!issueCommand(cmdStallCtl, 2)) {
      if (!stopDeviceLocked())
        FATAL("3C90x could not halt DMA after a command timeout.");
      return false;
    }

    // A completion belongs only to the descriptor published below.
    // Serialised senders should leave no credit, but drain defensively so a
    // late error status can never release a future caller's buffer.
    const size_t discardedCompletions = m_TxMutex.drainAvailable();
    (void)discardedCompletions;
    m_TxSuccessful = false;

    // The 3C90x descriptor has one 32-bit data pointer, not scatter/gather.
    // Keep arbitrary caller mappings out of that contract by using the
    // controller-owned contiguous bounce buffer for every transmission.
    MemoryCopy(m_pTxBuffVirt, reinterpret_cast<void*>(buffer), nBytes);
    const physical_uintptr_t destPtr = m_pTxBuffPhys;

    /** Setup the DPD (download descriptor) **/
    m_TransmitDPD->DnNextPtr = 0;

    /** Set notification for transmission complete (bit 15) **/
    m_TransmitDPD->FrameStartHeader = nBytes | 0x8000;
    // m_TransmitDPD->HdrAddr = m_pTxBuffPhys;
    // m_TransmitDPD->HdrLength = Ethernet::instance().ethHeaderSize();
    m_TransmitDPD->DataAddr =
        static_cast<uint32_t>(destPtr);  // m_pTxBuffPhys; // + m_TransmitDPD->HdrLength;
    m_TransmitDPD->DataLength = (nBytes /* - m_TransmitDPD->HdrLength */) + (1U << 31U);

    /** Send the packet **/
    FENCE();
    m_pBase->write32(m_pDPD, regDnListPtr_l);

    /** End Stall and Wait for upload to complete. **/
    if (!issueCommand(cmdStallCtl, 3)) {
      if (!stopDeviceLocked())
        FATAL("3C90x could not halt DMA after a command timeout.");
      return false;
    }

    bool downloadComplete = false;
    for (size_t poll = 0; poll < DownloadPollLimit; ++poll) {
      if (!m_pBase->read32(regDnListPtr_l)) {
        downloadComplete = true;
        break;
      }

      if (!Time::delay(Time::Multiplier::Millisecond))
        break;
    }
    if (!downloadComplete) {
      ERROR("3C90x: packet download timed out");
      if (!stopDeviceLocked())
        FATAL("3C90x could not halt DMA after a download timeout.");
      return false;
    }
  }

  // The card may still DMA from the caller's buffer after an interruptible
  // wake. Do not return until the IRQ has transferred completion ownership.
  if (!m_TxMutex.acquireForCompletion(1, 1, 0)) {
    LockGuard<Mutex> deviceGuard(m_DeviceLock);
    if (m_Active && !m_Stopping && !stopDeviceLocked())
      FATAL("3C90x could not halt DMA after a transmit timeout.");
    return false;
  }

  LockGuard<Mutex> deviceGuard(m_DeviceLock);
  return m_Active && !m_Stopping && m_TxSuccessful;
}

Nic3C90x::Nic3C90x(Network* pDev)
    : Network(pDev),
      m_pBase(0),
      m_isBrev(0),
      m_CurrentWindow(0),
      m_pRxBuffVirt(0),
      m_pTxBuffVirt(0),
      m_pRxBuffPhys(0),
      m_pTxBuffPhys(0),
      m_RxBuffMR("3c90x-rxbuffer"),
      m_TxBuffMR("3c90x-txbuffer"),
      m_pDPD(0),
      m_DPDMR("3c90x-dpd"),
      m_pUPD(0),
      m_UPDMR("3c90x-upd"),
      m_TransmitDPD(0),
      m_ReceiveUPD(0),
      m_TxMutex(0),
      m_DeviceLock(),
      m_SendLock(),
      m_RxConsumerIndex(0),
      m_IrqId(0),
      m_Active(false),
      m_Stopping(false),
      m_TxSuccessful(false),
      m_Initialised(false) {
  setSpecificType(String("3c90x-card"));

  int i, c;
  uint16_t eeprom[0x21];
  uint32_t cfg;
  uint32_t mopt;
  uint32_t mstat;
  uint16_t linktype;
#define HWADDR_OFFSET 10

  // allocate the rx and tx buffers
  const size_t packetBufferPages = DriverDma::pageCountForBytes(MAX_PACKET_SIZE);
  if (!PhysicalMemoryManager::instance().allocateRegion(
          m_RxBuffMR, packetBufferPages, Dma32Constraints, VirtualAddressSpace::Write, -1)) {
    ERROR("3C90x: Couldn't allocate Rx Buffer!");
    return;
  }
  if (!PhysicalMemoryManager::instance().allocateRegion(
          m_TxBuffMR, packetBufferPages, Dma32Constraints, VirtualAddressSpace::Write, -1)) {
    ERROR("3C90x: Couldn't allocate Tx Buffer!");
    return;
  }
  m_pRxBuffVirt = static_cast<uint8_t*>(m_RxBuffMR.virtualAddress());
  m_pTxBuffVirt = static_cast<uint8_t*>(m_TxBuffMR.virtualAddress());
  m_pRxBuffPhys = m_RxBuffMR.physicalAddress();
  m_pTxBuffPhys = m_TxBuffMR.physicalAddress();

  if (!PhysicalMemoryManager::instance().allocateRegion(m_DPDMR, 2, Dma32Constraints,
                                                        VirtualAddressSpace::Write, -1)) {
    ERROR("3C90x: Couldn't allocated buffer for DPD\n");
    return;
  }
  if (!PhysicalMemoryManager::instance().allocateRegion(m_UPDMR, 2, Dma32Constraints,
                                                        VirtualAddressSpace::Write, -1)) {
    ERROR("3C90x: Couldn't allocated buffer for UPD\n");
    return;
  }
  m_pDPD = m_DPDMR.physicalAddress();
  m_pUPD = m_UPDMR.physicalAddress();
  m_TransmitDPD = reinterpret_cast<TXD*>(m_DPDMR.virtualAddress());
  m_ReceiveUPD = reinterpret_cast<RXD*>(m_UPDMR.virtualAddress());

  // configure the UPD... turn it into a list with a well-defined beginning
  // and end
  for (size_t iUpd = 0; iUpd < NUM_UPDS; iUpd++) {
    if ((iUpd + 1) == NUM_UPDS)
      m_ReceiveUPD[iUpd].UpNextPtr = 0;
    else
      m_ReceiveUPD[iUpd].UpNextPtr = m_pUPD + ((iUpd + 1) * sizeof(RXD));
    m_ReceiveUPD[iUpd].UpPktStatus = 0;
    m_ReceiveUPD[iUpd].DataAddr = m_pRxBuffPhys + (iUpd * 1536);
    m_ReceiveUPD[iUpd].DataLength = 1536 + (1U << 31U);
  }

  // grab the IO ports
  m_pBase = m_Addresses[0]->m_Io;

  m_CurrentWindow = 255;

  if (!reset()) {
    ERROR("3C90x: controller reset timed out");
    return;
  }

  uint16_t productId = 0;
  if (!readEeprom(0x03, productId)) {
    ERROR("3C90x: failed to read product ID from EEPROM");
    return;
  }

  switch (productId) {
    case 0x9000: /** 10 Base TPO **/
    case 0x9001: /** 10/100 T4 **/
    case 0x9050: /** 10/100 TPO **/
    case 0x9051: /** 10 Base Combo **/
      m_isBrev = 0;
      break;

    case 0x9004: /** 10 Base TPO **/
    case 0x9005: /** 10 Base Combo **/
    case 0x9006: /** 10 Base TPO and Base2 **/
    case 0x900A: /** 10 Base FL **/
    case 0x9055: /** 10/100 TPO **/
    case 0x9056: /** 10/100 T4 **/
    case 0x905A: /** 10 Base FX **/
    default:
      m_isBrev = 1;
      break;
  }

  /** Load EEPROM contents **/
  if (m_isBrev) {
    for (i = 0; i < 0x20; i++) {
      if (!readEeprom(i, eeprom[i])) {
        ERROR("3C90x: failed to load EEPROM contents");
        return;
      }
    }

#ifdef CFG_3C90X_BOOTROM_FIX
    /** Set xcvrSelect in InternalConfig in eeprom. **/
    /* only necessary for 3c905b revision cards with boot PROM bug!!! */
    if (writeEeprom(0x13, 0x0160) < 0)
      return;
#endif

#ifdef CFG_3C90X_XCVR
    /** Clear the LanWorks register **/
    if (CFG_3C90X_XCVR == 255) {
      if (writeEeprom(0x16, 0) < 0)
        return;
    }

    /** Set the selected permanent-xcvrSelect in the
     ** LanWorks register
     **/
    else {
      if (writeEeprom(0x16, XCVR_MAGIC + ((CFG_3C90X_XCVR) & 0x000F)) < 0)
        return;
    }
#endif
  } else {
    for (i = 0; i < 0x17; i++) {
      if (!readEeprom(i, eeprom[i])) {
        ERROR("3C90x: failed to load EEPROM contents");
        return;
      }
    }
  }

  /** Get the hardware address */
  m_StationInfo.mac.setMac(eeprom, true);
  NOTICE("3C90x MAC: " << m_StationInfo.mac[0] << ":" << m_StationInfo.mac[1] << ":"
                       << m_StationInfo.mac[2] << ":" << m_StationInfo.mac[3] << ":"
                       << m_StationInfo.mac[4] << ":" << m_StationInfo.mac[5] << ".");

  /* Test if the link is good, if so continue */
  if (setWindow(winDiagnostics4) < 0)
    return;
  mstat = m_pBase->read16(regMediaStatus_4_w);
  if ((mstat & (1 << 11)) == 0) {
    ERROR("3C90x: Valid link not established");
    return;
  }

  /** Program the MAC address into the station address registers */
  if (setWindow(winAddressing2) < 0)
    return;
  m_pBase->write16(HOST_TO_BIG16(eeprom[HWADDR_OFFSET + 0]), regStationAddress_2_3w);
  m_pBase->write16(HOST_TO_BIG16(eeprom[HWADDR_OFFSET + 1]), regStationAddress_2_3w + 2);
  m_pBase->write16(HOST_TO_BIG16(eeprom[HWADDR_OFFSET + 2]), regStationAddress_2_3w + 4);
  m_pBase->write16(0, regStationMask_2_3w);
  m_pBase->write16(0, regStationMask_2_3w + 2);
  m_pBase->write16(0, regStationMask_2_3w + 4);

  /** Read the media options register, print a message and set default
   * xcvr.
   *
   * Uses Media Option command on B revision, Reset Option on non-B
   * revision cards -- same register address
   */
  if (setWindow(winTxRxOptions3) < 0)
    return;
  mopt = m_pBase->read16(regResetMediaOptions_3_w);

  /** mask out VCO bit that is defined as 10 base FL bit on B-rev cards **/
  if (!m_isBrev)
    mopt &= 0x7f;

  NOTICE("3C90x connectors present:");
  c = 0;
  linktype = 0x0008;
  if (mopt & 0x01) {
    NOTICE(((c++) ? ", " : "") << "100BASE-T4");
    linktype = 0x0006;
  }
  if (mopt & 0x04) {
    NOTICE(((c++) ? ", " : "") << "100BASE-FX");
    linktype = 0x0005;
  }
  if (mopt & 0x10) {
    NOTICE(((c++) ? ", " : "") << "10BASE2");
    linktype = 0x0003;
  }
  if (mopt & 0x20) {
    NOTICE(((c++) ? ", " : "") << "AUI");
    linktype = 0x0001;
  }
  if (mopt & 0x40) {
    NOTICE(((c++) ? ", " : "") << "MII");
    linktype = 0x0006;
  }
  if ((mopt & 0xA) == 0xA) {
    NOTICE(((c++) ? ", " : "") << "10BASE-T / 100BASE-TX");
    linktype = 0x0008;
  } else if ((mopt & 0xa) == 0x2) {
    NOTICE(((c++) ? ", " : "") << "100BASE-TX");
    linktype = 0x0008;
  } else if ((mopt & 0xa) == 0x8) {
    NOTICE(((c++) ? ", " : "") << "10BASE-T");
    linktype = 0x0008;
  }

  /** Determine transceiver type to use, depending on value stored in
   * eeprom 0x16
   */
  if (m_isBrev) {
    if ((eeprom[0x16] & 0xff00) == XCVR_MAGIC)
      linktype = eeprom[0x16] & 0x000f;
  } else {
#ifdef CFG_3C90X_XCVR
    if (CFG_3C90X_XCVR != 255)
      linktype = CFG_3C90X_XCVR;
#endif

    if (linktype == 0x0009) {
      if (m_isBrev)
        WARNING(
            "3C90x: MII External MAC mode only supported on "
            "B-revision cards! Falling back to MII mode.");
      linktype = 0x0006;
    }
  }

  /** Enable DC converter for 10-BASE-T **/
  if (linktype == 0x0003 && !issueCommand(cmdEnableDcConverter, 0))
    return;

  /** Set the link to the type we just determined **/
  if (setWindow(winTxRxOptions3) < 0)
    return;
  cfg = m_pBase->read32(regInternalConfig_3_l);
  cfg &= ~(0xF << 20);
  cfg |= (linktype << 20);
  m_pBase->write32(cfg, regInternalConfig_3_l);

  /** Now that we've set the xcvr type, reset TX and RX, re-enable **/
  if (!issueCommand(cmdTxReset, 0))
    return;

  if (!m_isBrev)
    m_pBase->write8(0x01, regTxFreeThresh_b);
  if (!issueCommand(cmdTxEnable, 0))
    return;

  /** reset of the receiver on B-revision cards re-negotiates the link
   * takes several seconds
   */
  if (m_isBrev) {
    if (!issueCommand(cmdRxReset, 0x04))
      return;
  } else {
    if (!issueCommand(cmdRxReset, 0x00))
      return;
  }

  /** Set the RX filter = receive only individual packets & multicast &
   * broadcast **/
  if (!issueCommand(cmdSetRxFilter, 0x01 + 0x02 + 0x04) || !issueCommand(cmdRxEnable, 0)) {
    return;
  }

  /** Configure indications but leave the physical source gated. **/
  if (!issueCommand(cmdSetInterruptEnable, 0) ||
      !issueCommand(cmdSetIndicationEnable, ENABLED_INTS) ||
      !issueCommand(cmdAcknowledgeInterrupt, 0xff)) {
    return;
  }

  // Set the location for the UPD
  FENCE();
  m_pBase->write32(m_pUPD, regUpListPtr_l);

  // install the IRQ
  m_IrqId = Machine::instance().getIrqManager()->registerPciIrqHandler(
      static_cast<IrqHandler*>(this), this, IrqPolicy::pciIntxThreaded());
  if (!m_IrqId) {
    ERROR("3C90x: could not register its PCI interrupt");
    if (!quiesce())
      FATAL("3C90x could not halt DMA after IRQ registration failed.");
    m_Stopping = true;
    return;
  }

  NetworkStack::instance().registerDevice(this);
  bool enabled = false;
  {
    LockGuard<Mutex> deviceGuard(m_DeviceLock);
    m_Active = true;
    enabled = issueCommand(cmdSetInterruptEnable, ENABLED_INTS);
    if (enabled)
      m_Initialised = true;
    else if (!stopDeviceLocked())
      FATAL("3C90x could not halt DMA after interrupt enable failed.");
  }

  if (!enabled) {
    if (!Machine::instance().getIrqManager()->unregisterHandler(m_IrqId, this)) {
      FATAL("3C90x could not unregister a failed IRQ callback.");
    }
    m_IrqId = 0;
    NetworkStack::instance().deRegisterDevice(this);
  }
}

Nic3C90x::~Nic3C90x() {
  if (!m_Initialised) {
    return;
  }

  {
    LockGuard<Mutex> deviceGuard(m_DeviceLock);
    if (!stopDeviceLocked())
      FATAL("3C90x teardown could not establish a DMA halt boundary.");
  }
  if (m_IrqId && !Machine::instance().getIrqManager()->unregisterHandler(m_IrqId, this)) {
    FATAL("3C90x teardown could not unregister its IRQ callback.");
  }
  m_IrqId = 0;

  NetworkStack::instance().deRegisterDevice(this);
  {
    LockGuard<Mutex> sendGuard(m_SendLock);
  }
  m_Initialised = false;
}

IrqDisposition Nic3C90x::irq(irq_id_t number) {
  struct ReceivedPacket {
    uint8_t* data;
    size_t length;
  };

  constexpr size_t PassLimit = 16;
  ReceivedPacket receivedPackets[NUM_UPDS] = {};
  size_t receivedPacketCount = 0;
  bool handled = false;
  (void)number;

  {
    LockGuard<Mutex> deviceGuard(m_DeviceLock);
    if (m_Stopping)
      return IrqDisposition::Quiesced;
    if (!m_Active)
      return IrqDisposition::NotHandled;

    uint16_t status = m_pBase->read16(regCommandIntStatus_w);
    if ((status & ENABLED_INTS) == 0)
      return IrqDisposition::NotHandled;

    // Indications continue to latch while the source is gated. A cause
    // which arrives after its acknowledgement is therefore owned by a
    // later pass.
    if (!issueCommand(cmdSetInterruptEnable, 0)) {
      if (!stopDeviceLocked())
        FATAL("3C90x could not quiesce after IRQ gating failed.");
      return IrqDisposition::Handled;
    }

    for (size_t pass = 0; pass < PassLimit; ++pass) {
      status = m_pBase->read16(regCommandIntStatus_w);

      // check that one of the enabled IRQs is triggered
      if ((status & ENABLED_INTS) == 0)
        break;
      handled = true;

      // Acknowledge the captured cause before inspecting descriptors so
      // a completion which arrives during the scan remains latched for
      // the next bounded pass.
      if (!issueCommand(cmdAcknowledgeInterrupt, (status & ENABLED_INTS))) {
        if (!stopDeviceLocked())
          FATAL("3C90x could not quiesce after IRQ ACK failed.");
        break;
      }

      bool receiveBatch = false;
      if (status & INT_UPCOMPLETE) {
        receiveBatch = true;
        bool wrapped = false;
        for (size_t scanned = 0; scanned < NUM_UPDS; ++scanned) {
          RXD& usedUpd = m_ReceiveUPD[m_RxConsumerIndex];
          const uint32_t packetStatus = usedUpd.UpPktStatus;
          if (!(packetStatus & UpPacketComplete))
            break;
          FENCE();

          const size_t packetLength = packetStatus & UpPacketLengthMask;
          if (packetStatus & UpPacketError) {
            ERROR("3C90x: receive error, UpPktStatus = " << packetStatus << ".");
          } else if (!packetLength || packetLength > ReceiveSlotSize) {
            ERROR("3C90x: invalid received packet length " << packetLength);
          } else {
            uint8_t* packet = m_ReceiveStaging + (receivedPacketCount * ReceiveSlotSize);
            MemoryCopy(packet, m_pRxBuffVirt + (m_RxConsumerIndex * ReceiveSlotSize), packetLength);
            receivedPackets[receivedPacketCount++] = {packet, packetLength};
          }

          usedUpd.UpPktStatus = 0;
          ++m_RxConsumerIndex;
          if (m_RxConsumerIndex == NUM_UPDS) {
            m_RxConsumerIndex = 0;
            wrapped = true;
            break;
          }
        }

        if (wrapped) {
          const bool uploadStalled = issueCommand(cmdStallCtl, 0);
          if (uploadStalled) {
            FENCE();
            m_pBase->write32(m_pUPD, regUpListPtr_l);
          }
          if (!uploadStalled || !issueCommand(cmdStallCtl, 1)) {
            if (!stopDeviceLocked()) {
              FATAL(
                  "3C90x could not halt DMA after receive-list "
                  "restart failed.");
            }
          }
        }
      }

      if (status & INT_HOSTERROR) {
        ERROR("3C90x: host error IRQ");
        if (!stopDeviceLocked())
          FATAL("3C90x could not halt DMA after a host error.");
      } else if (status & INT_TXCOMPLETE) {
        const uint8_t txStatus = m_pBase->read8(regTxStatus_b);

        // TxComplete advances through the status FIFO rather than the
        // command-register acknowledgement path.
        m_pBase->write8(0, regTxStatus_b);
        m_TxSuccessful = (txStatus & 0xbf) == 0x80;

        if (!m_TxSuccessful) {
          if (txStatus & 0x02)
            ERROR("3C90x: TX Reclaim Error");
          else if (txStatus & 0x04)
            ERROR("3C90x: TX Status Overflow");
          else if (txStatus & 0x08)
            ERROR("3C90x: TX Max Collisions");
          else if (txStatus & 0x10)
            ERROR("3C90x: TX Underrun");
          else if (txStatus & 0x20)
            ERROR("3C90x: TX Jabber");
          else
            ERROR(
                "3C90x: Internal Error - Incomplete "
                "Transmission");

          if (!stopDeviceLocked()) {
            FATAL(
                "3C90x could not halt DMA after a transmit "
                "error.");
          }
        } else {
          m_TxMutex.release();
        }
      }

      if (m_Stopping || receiveBatch)
        break;
    }

    // Re-enable this device source only after every captured cause has
    // either transferred ownership or forced the device offline.
    if (m_Active && !m_Stopping && !issueCommand(cmdSetInterruptEnable, ENABLED_INTS)) {
      if (!stopDeviceLocked())
        FATAL("3C90x could not quiesce after IRQ rearm failed.");
    }
  }

  for (size_t i = 0; i < receivedPacketCount; ++i) {
    NetworkStack::instance().receive(receivedPackets[i].length,
                                     reinterpret_cast<uintptr_t>(receivedPackets[i].data), this, 0);
  }

  /*
  XL_SEL_WIN(7);

    if (ifp->if_snd.ifq_head != NULL)
      xl_start(ifp);
  */

  return handled ? IrqDisposition::Handled : IrqDisposition::NotHandled;
}

bool Nic3C90x::setStationInfo(const StationInfo& info) {
  // free the old DNS servers list, if there is one
  if (m_StationInfo.dnsServers)
    delete[] m_StationInfo.dnsServers;

  // MAC isn't changeable, so set it all manually
  m_StationInfo.ipv4 = info.ipv4;
  NOTICE("3C90x: Setting ipv4, " << info.ipv4.toString() << ", " << m_StationInfo.ipv4.toString()
                                 << "...");
  m_StationInfo.ipv6 = info.ipv6;

  m_StationInfo.subnetMask = info.subnetMask;
  NOTICE("3C90x: Setting subnet mask, " << info.subnetMask.toString() << ", "
                                        << m_StationInfo.subnetMask.toString() << "...");
  m_StationInfo.gateway = info.gateway;
  NOTICE("3C90x: Setting gateway, " << info.gateway.toString() << ", "
                                    << m_StationInfo.gateway.toString() << "...");

  // Callers do not free their dnsServers memory
  m_StationInfo.dnsServers = info.dnsServers;
  m_StationInfo.nDnsServers = info.nDnsServers;
  NOTICE("3C90x: Setting DNS servers [" << Dec << m_StationInfo.nDnsServers << Hex
                                        << " servers being set]...");

  return true;
}

const StationInfo& Nic3C90x::getStationInfo() {
  return m_StationInfo;
}
