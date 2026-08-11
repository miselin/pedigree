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

#include "AtaDisk.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/PointerGuard.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "AtaController.h"
#include "AtaWriteCache.h"
#include "ata-common.h"

#if CRIPPLE_HDD
#pragma GCC diagnostic ignored "-Wunreachable-code"
#endif

// #define ATA_DEFAULT_BLOCK_SIZE 0x1000
#define ATA_DEFAULT_BLOCK_SIZE 0x10000 * 2

namespace {
constexpr Time::Timestamp AtapiCommandTimeout = 30 * Time::Multiplier::Second;
constexpr size_t AtapiMaximumStatusPolls = 30000000;
constexpr Time::Timestamp AtaDmaCompletionTimeout = 30 * Time::Multiplier::Second;
constexpr size_t AtaDmaCompletionPollLimit = 30000000;
constexpr Time::Timestamp AtaPioCompletionTimeout = 30 * Time::Multiplier::Second;
constexpr size_t AtaPioCompletionPollLimit = 30000000;

bool waitForAtapiStatus(IoBase* commandRegs, IoBase* controlRegs, Time::Timestamp commandStarted,
                        size_t& commandPolls, AtaStatus& status) {
  // One PACKET command gets one budget. Restarting ataWait's timeout at each
  // data phase lets a device keep the caller here forever while still making
  // occasional progress.
  if (commandPolls >= AtapiMaximumStatusPolls ||
      (Time::getTicks() - commandStarted) >= AtapiCommandTimeout) {
    WARNING("ATAPI: PACKET command exceeded its completion deadline");
    return false;
  }

  for (size_t i = 0; i < 4; ++i) {
    controlRegs->read8(2);
  }

  status.__reg_contents = commandRegs->read8(7);
  while (status.reg.bsy || (!status.reg.drq && !status.reg.drdy && !status.reg.err)) {
    if (!status.__reg_contents) {
      return true;
    }
    if (++commandPolls >= AtapiMaximumStatusPolls ||
        (Time::getTicks() - commandStarted) >= AtapiCommandTimeout) {
      WARNING(
          "ATAPI: PACKET command timed out waiting for device status, "
          "status="
          << status.__reg_contents);
      return false;
    }

    Processor::pause();
    status.__reg_contents = commandRegs->read8(7);
  }

  return true;
}

class ScopedBusMasterTransaction {
 public:
  explicit ScopedBusMasterTransaction(BusMasterIde* busMaster)
      : m_BusMaster(busMaster), m_OwnsTransaction(false) {}

  ~ScopedBusMasterTransaction() {
    complete();
  }

  bool add(uintptr_t buffer, size_t bytes) {
    if (!m_BusMaster) {
      return false;
    }
    if (!m_OwnsTransaction && !(m_OwnsTransaction = m_BusMaster->beginTransaction())) {
      return false;
    }
    return m_BusMaster->add(buffer, bytes);
  }

  bool begin(bool write) {
    return m_OwnsTransaction && m_BusMaster->begin(write);
  }

  void complete() {
    if (m_OwnsTransaction) {
      m_BusMaster->commandComplete();
      m_OwnsTransaction = false;
    }
  }

 private:
  ScopedBusMasterTransaction(const ScopedBusMasterTransaction&) = delete;
  ScopedBusMasterTransaction& operator=(const ScopedBusMasterTransaction&) = delete;

  BusMasterIde* m_BusMaster;
  bool m_OwnsTransaction;
};

class CachePageFillGuard {
 public:
  CachePageFillGuard(Cache& cache, uint64_t location, bool* insertedPages, size_t pageCount)
      : m_Cache(cache),
        m_Location(location),
        m_InsertedPages(insertedPages),
        m_PageCount(pageCount),
        m_Published(false) {}

  ~CachePageFillGuard() {
    if (m_Published) {
      return;
    }

    bool discarded = true;
    for (size_t i = 0; i < m_PageCount; ++i) {
      if (m_InsertedPages[i] && !m_Cache.discardEditing(m_Location + (i * 4096))) {
        discarded = false;
      }
    }
    if (!discarded) {
      WARNING(
          "AtaDisk could not discard every page from a failed cache "
          "fill at "
          << m_Location);
    }
  }

  void publish() {
    for (size_t i = 0; i < m_PageCount; ++i) {
      if (m_InsertedPages[i]) {
        m_Cache.markNoLongerEditing(m_Location + (i * 4096));
      }
    }
    m_Published = true;
  }

 private:
  CachePageFillGuard(const CachePageFillGuard&) = delete;
  CachePageFillGuard& operator=(const CachePageFillGuard&) = delete;

  Cache& m_Cache;
  uint64_t m_Location;
  bool* m_InsertedPages;
  size_t m_PageCount;
  bool m_Published;
};
}  // namespace

AtaDisk::IrqCompletion::IrqCompletion(AtaDisk& disk) : m_Disk(disk), m_Completion(0) {
  LockGuard<Spinlock> guard(m_Disk.m_IrqLock);
  if (m_Disk.m_IrqReceived) {
    FATAL("ATA command attempted to replace a live IRQ completion");
  }
  m_Disk.m_IrqReceived = &m_Completion;
}

AtaDisk::IrqCompletion::~IrqCompletion() {
  LockGuard<Spinlock> guard(m_Disk.m_IrqLock);
  if (m_Disk.m_IrqReceived != &m_Completion) {
    FATAL("ATA IRQ completion publication was corrupted");
  }
  m_Disk.m_IrqReceived = nullptr;
}

AtaDisk::AtaDisk(AtaController* pDev, bool isMaster, IoBase* commandRegs, IoBase* controlRegs,
                 BusMasterIde* busMaster)
    : ScsiDisk(),
      m_IsMaster(isMaster),
      m_SupportsLBA28(true),
      m_SupportsLBA48(false),
      m_BlockSize(ATA_DEFAULT_BLOCK_SIZE),
      m_IrqReceived(0),
      m_AtaDiskType(NotPacket),
      m_PacketSize(0),
      m_Removable(false),
      m_CommandRegs(commandRegs),
      m_ControlRegs(controlRegs),
      m_BusMaster(busMaster),
      m_PrdTableLock(),
      m_PrdTable(0),
      m_LastPrdTableOffset(0),
      m_PrdTablePhys(0),
      m_PrdTableMemRegion("ata-prdtable"),
      m_bDma(true) {
  m_pParent = pDev;
}

AtaDisk::~AtaDisk() {}

void AtaDisk::maskInterrupts() {
  if (m_ControlRegs) {
    // nIEN prevents fresh device IRQs after the controller queue drains.
    m_ControlRegs->write8(0x02, 2);
  }
}

void AtaDisk::stopDma() {
  if (m_BusMaster) {
    m_BusMaster->commandComplete();
  }
}

bool AtaDisk::initialise(size_t nUnit) {
  // Grab our parent.
  AtaController* pParent = static_cast<AtaController*>(m_pParent);

  // Grab our parent's IoPorts for command and control accesses.
  IoBase* commandRegs = m_CommandRegs;
  IoBase* controlRegs = m_ControlRegs;

  // Drive spin-up (go from standby to active, if necessary)
  setFeatures(0x07, 0, 0, 0, 0);

  // Check for device presence
  uint8_t devSelect = (m_IsMaster) ? 0xA0 : 0xB0;
  commandRegs->write8(devSelect, 6);
  commandRegs->write8(0xEC, 7);
  if (commandRegs->read8(7) == 0) {
    NOTICE("ATA: No device present here");
    return false;
  }

  // Select the device to transmit to
  devSelect = (m_IsMaster) ? 0xA0 : 0xB0;
  commandRegs->write8(devSelect, 6);

  // Wait for it to be selected
  ataWait(commandRegs, controlRegs);

  // DEVICE RESET
  commandRegs->write8(8, 7);

  // Wait for the drive to reset before requesting a device change
  ataWait(commandRegs, controlRegs);

  //
  // Start IDENTIFY command.
  //

  AtaStatus status;

  // Disable IRQs on this device for now.
  controlRegs->write8(0x2, 2);

  // Send IDENTIFY.
  commandRegs->read8(7);
  commandRegs->write8(0xEC, 7);

  // Read status register.
  status = ataWait(commandRegs, controlRegs);

  // Check that the device actually exists
  if (status.__reg_contents == 0)
    return false;

  // Check for an ATAPI device
  uint8_t m1 = commandRegs->read8(2);
  uint8_t m2 = commandRegs->read8(3);
  uint8_t m3 = commandRegs->read8(4);
  uint8_t m4 = commandRegs->read8(5);
  // #ifdef DEBUG
  NOTICE("ATA signature: " << m1 << ", " << m2 << ", " << m3 << ", " << m4);
  // #endif
  m_AtaDiskType = None;
  if (m3 == 0x14 && m4 == 0xeb) {
    // Run IDENTIFY PACKET DEVICE instead
    commandRegs->write8(devSelect, 6);
    commandRegs->write8(0xA1, 7);
    status = ataWait(commandRegs, controlRegs);
  } else {
    m_AtaDiskType = NotPacket;
  }

  // After checking signature and potentially retrying with IDENTIFY PACKET
  // DEVICE, we can check for an error proper now.
  if (status.reg.err) {
    WARNING("ATA drive errored on IDENTIFY!");
    return false;
  }

  // Read the data.
  for (int i = 0; i < 256; i++) {
    m_pIdent.__raw[i] = commandRegs->read16(0);
  }

  // Check for late error - final sanity check.
  if (commandRegs->read8(7) & 1) {
    WARNING("ATA drive now has an error status after reading IDENTIFY data.");
    return false;
  }

  // Do we have integrity data?
  if (m_pIdent.data.signature == 0xA5) {
    // Yes. Run a checksum.
    uint8_t sum = 0;
    uint8_t* bytes = reinterpret_cast<uint8_t*>(m_pIdent.__raw);
    for (size_t i = 0; i < 512; ++i)
      sum += bytes[i];

    // The result should be zero if the checksum is in fact correct.
    if (sum) {
      WARNING("ATA IDENTIFY data failed checksum!");
      return false;
    }
  }

  // Interpret the data.

  // Good device?
  if ((m_AtaDiskType == NotPacket) && m_pIdent.data.general_config.not_ata) {
    ERROR("ATA: Device does not conform to the ATA specification.");
    return false;
  } else if ((m_AtaDiskType != NotPacket) && (!m_pIdent.data.general_config.not_ata)) {
    ERROR("ATA: PACKET device does not conform to the ATA specification.");
    return false;
  }

  if (m_AtaDiskType != NotPacket) {
    m_AtaDiskType = static_cast<AtaDiskType>(m_pIdent.data.general_config.packet_cmdset);
  }

  // Get the device name.
  ataLoadSwapped(m_pName, m_pIdent.data.model_number, 20);

  // The device name is padded by spaces. Backtrack through converting spaces
  // into NULL bytes.
  for (int i = 39; i > 0; i--) {
    if (m_pName[i] != ' ')
      break;
    m_pName[i] = '\0';
  }
  m_pName[40] = '\0';

  // Get the serial number.
  ataLoadSwapped(m_pSerialNumber, m_pIdent.data.serial_number, 10);

  // The serial number is padded by spaces. Backtrack through converting
  // spaces into NULL bytes.
  for (int i = 19; i > 0; i--) {
    if (m_pSerialNumber[i] != ' ')
      break;
    m_pSerialNumber[i] = '\0';
  }
  m_pSerialNumber[20] = '\0';

  // Get the firmware revision.
  ataLoadSwapped(m_pFirmwareRevision, m_pIdent.data.firmware_revision, 4);

  // The device name is padded by spaces. Backtrack through converting spaces
  // into NULL bytes.
  for (int i = 7; i > 0; i--) {
    if (m_pFirmwareRevision[i] != ' ')
      break;
    m_pFirmwareRevision[i] = '\0';
  }
  m_pFirmwareRevision[8] = '\0';

  // Check that LBA48 is actually enabled.
  if (m_pIdent.data.command_sets_support.address48) {
    m_SupportsLBA48 = m_pIdent.data.command_sets_enabled.address48;
    if (!m_SupportsLBA48)
      WARNING("ATA: Device supports LBA48 but it isn't enabled.");
  }

  // And check for LBA28 support, just in case.
  if (!m_pIdent.data.caps.lba) {
    /// \todo should check that this doesn't break on ATAPI
    ERROR("ATA: Device does not support LBA.");
    return false;
  }

  // Do we have DMA?
  m_bDma = false;
  if (m_pIdent.data.caps.dma) {
    m_bDma = true;
    NOTICE("ATA: Device supports DMA.");

    if (m_pIdent.data.validity.multiword_dma_valid) {
      size_t highest_mode = ~0U;
      if (m_pIdent.data.multiword_dma.mode0)
        highest_mode = 0;
      if (m_pIdent.data.multiword_dma.mode1)
        highest_mode = 1;
      if (m_pIdent.data.multiword_dma.mode2)
        highest_mode = 2;

      size_t sel_mode = ~0U;
      if (m_pIdent.data.multiword_dma.sel_mode0)
        sel_mode = 0;
      if (m_pIdent.data.multiword_dma.sel_mode1)
        sel_mode = 1;
      if (m_pIdent.data.multiword_dma.sel_mode2)
        sel_mode = 2;

      if (highest_mode != ~0U) {
        NOTICE("ATA: Device Multiword DMA: supports up to mode" << Dec << highest_mode << Hex);
      } else {
        NOTICE("ATA: Device Multiword DMA: no support");
      }

      if (sel_mode != ~0U) {
        NOTICE("ATA: Device Multiword DMA: mode" << Dec << sel_mode << Hex << " is selected");
      }
    }

    if (m_pIdent.data.validity.ultra_dma_valid) {
      size_t highest_mode = ~0U;
      if (m_pIdent.data.ultra_dma.supp_mode0)
        highest_mode = 0;
      if (m_pIdent.data.ultra_dma.supp_mode1)
        highest_mode = 1;
      if (m_pIdent.data.ultra_dma.supp_mode2)
        highest_mode = 2;
      if (m_pIdent.data.ultra_dma.supp_mode3)
        highest_mode = 3;
      if (m_pIdent.data.ultra_dma.supp_mode4)
        highest_mode = 4;
      if (m_pIdent.data.ultra_dma.supp_mode5)
        highest_mode = 5;
      if (m_pIdent.data.ultra_dma.supp_mode6)
        highest_mode = 6;

      size_t sel_mode = ~0U;
      if (m_pIdent.data.ultra_dma.sel_mode0)
        sel_mode = 0;
      if (m_pIdent.data.ultra_dma.sel_mode1)
        sel_mode = 1;
      if (m_pIdent.data.ultra_dma.sel_mode2)
        sel_mode = 2;
      if (m_pIdent.data.ultra_dma.sel_mode3)
        sel_mode = 3;
      if (m_pIdent.data.ultra_dma.sel_mode4)
        sel_mode = 4;
      if (m_pIdent.data.ultra_dma.sel_mode5)
        sel_mode = 5;
      if (m_pIdent.data.ultra_dma.sel_mode6)
        sel_mode = 6;

      if (highest_mode != ~0U) {
        NOTICE("ATA: Device Ultra DMA: supports up to mode" << Dec << highest_mode << Hex);
      } else {
        NOTICE("ATA: Device Ultra DMA: no support");
      }

      if (sel_mode != ~0U) {
        NOTICE("ATA: Device Ultra DMA: mode" << Dec << sel_mode << Hex << " is enabled");
      }
    }
  }

  // Do we have a bus master with which to work with?
  // ISA ATA does not.
  if (!m_BusMaster) {
    WARNING("ATA: Controller does not support DMA");
    m_bDma = false;
  }

  if (m_pIdent.data.sector_size.logical_larger_than_512b ||
      m_pIdent.data.sector_size.multiple_logical_per_physical) {
    // Large physical sectors.
    size_t logical_size = 512;
    if (m_pIdent.data.sector_size.logical_larger_than_512b)
      logical_size = m_pIdent.data.words_per_logical * sizeof(uint16_t);

    // Logical sectors per physical sector.
    size_t log_per_phys = 1 << m_pIdent.data.sector_size.logical_per_physical;
    size_t physical_size = log_per_phys * logical_size;

    NOTICE("ATA: Physical sector size is " << Dec << physical_size << Hex << " bytes.");
    NOTICE("ATA: Logical sector size is " << Dec << logical_size << Hex << " bytes.");

    if (physical_size > 512) {
      // Non-standard physical sectors; align block size to this.
      if (m_BlockSize % physical_size) {
        // Default block size doesn't map to physical sectors well.
        WARNING(
            "ATA: Default block size doesn't map well to physical "
            "sectors, performance may be degraded.");
      }

      // Always make sure our blocks are bigger than physical sectors.
      if (m_BlockSize < physical_size)
        m_BlockSize = physical_size;
    } else {
      // Standard physical sectors - default block size is okay.
    }
  }

  NOTICE("ATA: IRQ is #" << Dec << getInterruptNumber() << Hex << ".");

  // ATAPI pieces.
  if (m_AtaDiskType != NotPacket) {
    // Packet size?
    m_PacketSize = m_pIdent.data.general_config.packet_sz ? 16 : 12;
    NOTICE("ATAPI: packet size is " << Dec << m_PacketSize << " bytes" << Hex);

    commandRegs->write8(devSelect, 6);
    commandRegs->write8(0xDA, 7);  // GET MEDIA STATUS
    status = ataWait(commandRegs, controlRegs);
    if (status.reg.err) {
      // We have information in the error register
      uint8_t err = commandRegs->read8(1);

      // ABORT?
      if (err & 0x4) {
        WARNING("ATAPI: device does not support GET MEDIA STATUS.");
      } else if (err & 2) {
        WARNING("ATAPI: No media present in the drive - aborting.");
        WARNING(
            "       TODO: handle media changes/insertions/removal "
            "properly");
        return false;
      } else {
        NOTICE("ATAPI: Media status: " << err << ".");
      }
    }

    // Initialise SCSI disk interface.
    if (!ScsiDisk::initialise(pParent, nUnit)) {
      ERROR("ATAPI: ScsiDisk init failed.");
      return false;
    }

    // Grab Inquiry data to figure out what we're working with.
    const ScsiDisk::Inquiry* pInquiry = getInquiry();
    m_Removable = ((pInquiry->Removable & (1 << 7)) != 0);
    AtaDiskType inquiryType = static_cast<AtaDiskType>(pInquiry->Peripheral & 0x1F);
    if (inquiryType != m_AtaDiskType) {
      ERROR(
          "ATAPI: IDENTIY PACKET DEVICE and SCSI INQUIRY disagree on "
          "device type.");
      return false;
    }

    // Supported device?
    if (m_AtaDiskType != CdDvd && m_AtaDiskType != Block) {
      /// \todo Testing needs to be done on more than just CD/DVD and
      /// block devices...
      WARNING(
          "Pedigree currently only supports CD/DVD and block ATAPI "
          "devices.");
      return false;
    }
  }

  NOTICE("Detected ATA device '" << m_pName << "', '" << m_pSerialNumber << "', '"
                                 << m_pFirmwareRevision << "'");

  return true;
}

bool AtaDisk::sendCommand(size_t nUnit, uintptr_t pCommand, uint8_t nCommandSize,
                          uintptr_t pRespBuffer, uint16_t nRespBytes, bool bWrite) {
  if (m_AtaDiskType == NotPacket) {
    ERROR("AtaDisk::sendCommand called on a non-PACKET device");
    return false;
  }

  if (!m_PacketSize) {
    ERROR("sendCommand called but the packet size is not known!");
    return false;
  }

  AtaStatus status;

  IoBase* commandRegs = m_CommandRegs;
  IoBase* controlRegs = m_ControlRegs;

  uint16_t* tmpPacket = new uint16_t[m_PacketSize / 2];
  PointerGuard<uint16_t> tmpGuard(tmpPacket, true);
  if (nCommandSize > m_PacketSize) {
    ERROR("ATAPI command is " << Dec << nCommandSize << " bytes, but the device accepts only "
                              << m_PacketSize << Hex);
    return false;
  }
  MemoryCopy(tmpPacket, reinterpret_cast<void*>(pCommand), nCommandSize);
  ByteSet(reinterpret_cast<uint8_t*>(tmpPacket) + nCommandSize, 0, m_PacketSize - nCommandSize);

  // Set nIEN as we poll in sendCommand().
  controlRegs->write8(2, 2);

  // Status belongs to the currently selected device. Select our target
  // before waiting so a failed probe of the other device cannot strand us
  // polling an absent slave forever.
  uint8_t devSelect = m_IsMaster ? 0xA0 : 0xB0;
  commandRegs->write8(devSelect, 6);
  ataWait(commandRegs, controlRegs);

  // Verify that it's the correct device
  if ((commandRegs->read8(6) & devSelect) != devSelect) {
    WARNING("ATAPI: Device was not selected");
    return false;
  }

  ScopedBusMasterTransaction dmaTransaction(m_BusMaster);
  bool bDmaSetup = false;
  if (m_bDma && nRespBytes) {
    bDmaSetup = dmaTransaction.add(pRespBuffer, nRespBytes);
    if (bDmaSetup) {
      // Start the bus master before selecting PACKET DMA. If this fails,
      // the command can still be issued cleanly in PIO mode.
      bDmaSetup = dmaTransaction.begin(bWrite);
    }
  }

  // PACKET command
  if ((m_pIdent.__raw[62] & (1 << 15)) &&
      bDmaSetup)                               // Device requires DMADIR for Packet DMA commands
    commandRegs->write8((bWrite ? 1 : 5), 1);  // Transfer to host, DMA
  else if (bDmaSetup)
    commandRegs->write8(1, 1);  // No overlap, DMA
  else
    commandRegs->write8(0, 1);                // No overlap, no DMA
  commandRegs->write8(0, 2);                  // Tag = 0
  commandRegs->write8(0, 3);                  // N/A for PACKET command
  commandRegs->write8(nRespBytes & 0xFF, 4);  // Byte count limit
  commandRegs->write8(((nRespBytes >> 8) & 0xFF), 5);

  // Packet commands such as optical-media reads may legitimately need time
  // for the medium to become ready, but every status transition and data
  // phase below shares this 30-second command budget.
  const Time::Timestamp commandStarted = Time::getTicks();
  size_t commandPolls = 0;

  // Transmit the PACKET command, wait for the device to be ready for the
  // command.
  commandRegs->write8(0xA0, 7);

  // Wait for sensible status before writing command packet.
  if (!waitForAtapiStatus(commandRegs, controlRegs, commandStarted, commandPolls, status)) {
    return false;
  }

  // Error?
  if (status.reg.err) {
    ERROR("ATAPI Packet command error [status=" << status.__reg_contents << "]!");
    return false;
  }

  // Transmit the command (padded as needed)
  for (size_t i = 0; i < (m_PacketSize / 2); i++) {
    commandRegs->write16(tmpPacket[i], 0);
  }

  // 400ns wait before reading status register.
  for (size_t i = 0; i < 4; ++i)
    controlRegs->read8(2);

  // Check for errors...
  // Note: not using ataWait as we don't want to block here.
  uint8_t statusreg = commandRegs->read8(7);
  if ((statusreg & 1) && !(statusreg & 0x80)) {
    // CHK = 1, BSY = 0
    uint8_t error = commandRegs->read8(1);
    if (error & 0x4) {
      WARNING("ATAPI command failed (ABORT)");
    } else {
      WARNING("ATAPI error with status " << statusreg << " [error=" << error << "]");
    }

    return false;
  }

  // If we aren't expecting anything from the device, we can just poll for
  // completion instead of waiting for an IRQ.
  if (!nRespBytes) {
    if (!waitForAtapiStatus(commandRegs, controlRegs, commandStarted, commandPolls, status)) {
      return false;
    }
    return !status.reg.err;
  }

  if (bDmaSetup) {
    while (true) {
      if (commandPolls >= AtapiMaximumStatusPolls ||
          (Time::getTicks() - commandStarted) >= AtapiCommandTimeout) {
        WARNING("ATAPI: DMA command timed out");
        return false;
      }

      status.__reg_contents = commandRegs->read8(7);
      if (!status.reg.bsy && status.reg.err) {
        WARNING("ATAPI: read failed during DMA data transfer");
        return false;
      }

      if (m_BusMaster->hasInterrupt() || m_BusMaster->hasCompleted()) {
        // commandComplete effectively resets the device state, so we
        // need to get the error register first.
        bool bError = m_BusMaster->hasError();
        dmaTransaction.complete();
        if (bError)
          return false;
        else
          break;
      }

      ++commandPolls;
      Processor::pause();
    }

    if (!waitForAtapiStatus(commandRegs, controlRegs, commandStarted, commandPolls, status)) {
      return false;
    }
    if (status.reg.err) {
      WARNING("ATAPI sendCommand failed after sending command packet");
      logAtaStatus(status);
      return false;
    }

    return true;
  }

  // ATAPI PIO may split a response into multiple DRQ phases. This commonly
  // happens for CD reads, where each phase is limited to one native sector.
  size_t transferred = 0;
  size_t pioPhases = 0;
  const size_t maximumPioPhases = nRespBytes / sizeof(uint16_t);
  while (true) {
    if ((Time::getTicks() - commandStarted) >= AtapiCommandTimeout) {
      WARNING("ATAPI: PIO command timed out");
      return false;
    }
    if (!waitForAtapiStatus(commandRegs, controlRegs, commandStarted, commandPolls, status)) {
      return false;
    }
    if (status.reg.err) {
      WARNING("ATAPI PIO command failed during a data phase");
      logAtaStatus(status);
      return false;
    }
    if (!status.reg.drq) {
      return true;
    }
    if (++pioPhases > maximumPioPhases) {
      ERROR("ATAPI PIO command exceeded its possible data-phase count");
      return false;
    }

    size_t realSz = commandRegs->read8(4) | (commandRegs->read8(5) << 8);
    if (!realSz) {
      ERROR("ATAPI PIO data phase reported a zero byte count");
      return false;
    }
    if (realSz & 1) {
      ERROR("ATAPI PIO data phase reported an odd byte count");
      return false;
    }

    const size_t remaining = nRespBytes - transferred;
    const size_t transferSz = realSz < remaining ? realSz : remaining;
    uint16_t* dest = reinterpret_cast<uint16_t*>(pRespBuffer + transferred);
    for (size_t i = 0; i < (transferSz / 2); ++i) {
      if (bWrite)
        commandRegs->write16(dest[i], 0);
      else
        dest[i] = commandRegs->read16(0);
    }

    // A malformed device must not make us touch past the caller's buffer,
    // but abandoning a partially consumed phase leaves DRQ asserted and
    // poisons the next command on the channel. Drain this one bounded
    // 16-bit phase, then report the protocol violation.
    for (size_t i = transferSz; i < realSz; i += sizeof(uint16_t)) {
      if (bWrite)
        commandRegs->write16(0xFFFF, 0);
      else
        commandRegs->read16(0);
    }
    if (realSz > remaining) {
      ERROR("ATAPI PIO data phase exceeds the response buffer: phase="
            << Dec << realSz << ", remaining=" << remaining << Hex);
      return false;
    }
    transferred += transferSz;
  }
}

uint64_t AtaDisk::doRead(uint64_t location) {
  if (m_AtaDiskType != NotPacket)
    return ScsiDisk::doRead(location);

  // Memory for the "already-read" buffers to point at for DMA scatter/gather
  static char alreadyRead[4096] ALIGN(4096);

  // Create our set of buffers to read into.
  size_t nBytes = getCacheFillLength(location);
  if (!nBytes) {
    return 0;
  }
  const uint64_t cacheLocation = location;
  uint64_t ioLocation = location;

  // Allocate list of buffers, allowing us to handle cache pages being widely
  // distributed around the virtual address space.
  size_t nBuffers = nBytes / 0x1000;  /// \todo getPageSize() here
  Buffer* buffers = new Buffer[nBuffers];
  PointerGuard<Buffer> guard2(buffers, true);
  bool* insertedPages = new bool[nBuffers];
  PointerGuard<bool> insertedPagesGuard(insertedPages, true);
  ByteSet(insertedPages, 0, nBuffers * sizeof(bool));
  CachePageFillGuard fillGuard(getCache(), cacheLocation, insertedPages, nBuffers);

  bool bAlreadyAllRead = true;
  for (size_t i = 0; i < nBuffers; ++i) {
    buffers[i].offset = i * 0x1000;

    uintptr_t buffer = getCache().lookup(location + buffers[i].offset);
    if (buffer) {
      getCache().release(location + buffers[i].offset);
      buffer = reinterpret_cast<uintptr_t>(alreadyRead);
    } else {
      bool didExist = false;
      buffer = getCache().insert(location + buffers[i].offset, &didExist);
      if (!buffer) {
        FATAL("AtaDisk::doRead - couldn't get a buffer!");
        return 0;
      }

      if (didExist) {
        buffer = reinterpret_cast<uintptr_t>(alreadyRead);
      } else {
        insertedPages[i] = true;
        bAlreadyAllRead = false;
      }
    }

    buffers[i].buffer = buffer;
  }

  if (bAlreadyAllRead) {
    // All pages were already in cache.
    return nBytes;
  }

  // Grab our parent's IoPorts for command and control accesses.
  IoBase* commandRegs = m_CommandRegs;
  IoBase* controlRegs = m_ControlRegs;

  // How many sectors do we need to read?
  /// \todo logical sector size here
  uint32_t nSectors = nBytes / 512;

  // Wait for BSY and DRQ to be zero before selecting the device
  AtaStatus status;
  ataWait(commandRegs, controlRegs);

  // Select the device to transmit to
  uint8_t devSelect;
  if (m_SupportsLBA48)
    devSelect = (m_IsMaster) ? 0xE0 : 0xF0;
  else
    devSelect = (m_IsMaster) ? 0xA0 : 0xB0;
  commandRegs->write8(devSelect, 6);

  // Wait for it to be selected
  ataWait(commandRegs, controlRegs);

  size_t buffersConsumed = 0;
  while (nSectors > 0) {
    ScopedBusMasterTransaction dmaTransaction(m_BusMaster);

    // ataWait applies both a wall-clock and poll-count deadline. A device
    // which never becomes ready must fail this request rather than strand
    // the RequestQueue worker forever.
    status = ataWait(commandRegs, controlRegs);
    if (status.reg.err || !status.reg.drdy) {
      ERROR("ATA: drive did not become ready for read");
      return 0;
    }

    // Send out sector count.
    uint8_t nSectorsToRead = min(m_pIdent.data.max_sectors_per_irq, nSectors);
    nSectors -= nSectorsToRead;

    // Buffers are 4K each, so calculate the number of buffers used for
    // this particular read.
    size_t buffersThisRead = (nSectorsToRead * 512) / 0x1000;
    const size_t firstBuffer = buffersConsumed;

    bool bDmaSetup = false;
    if (m_bDma) {
      for (size_t i = 0; i < buffersThisRead; ++i) {
        bDmaSetup = dmaTransaction.add(buffers[firstBuffer + i].buffer, 0x1000);
        if (!bDmaSetup) {
          ERROR("DMA setup failed!");
          break;
        }
      }
    }

    if (m_SupportsLBA48)
      setupLBA48(ioLocation, nSectorsToRead);
    else {
      if (ioLocation >= 0x2000000000ULL) {
        WARNING(
            "Ata: Sector > 128GB requested but LBA48 addressing "
            "not supported!");
      }
      setupLBA28(ioLocation, nSectorsToRead);
    }

    IrqCompletion irqCompletion(*this);

    if (getInterruptNumber() != 0xFF) {
      // Enable IRQs so we can avoid spinning if possible.
      controlRegs->write8(0, 2);

      bool oldInterrupts = Processor::getInterrupts();
      if (!oldInterrupts)
        Processor::setInterrupts(true);
    }

    if (m_bDma && bDmaSetup) {
      // Prepare DMA before we send the command.
      bDmaSetup = dmaTransaction.begin(false);

      if (!m_SupportsLBA48) {
        // Send command "read DMA"
        commandRegs->write8(0xC8, 7);
      } else {
        // Send command "read DMA EXT"
        commandRegs->write8(0x25, 7);
      }
    } else {
      if (m_SupportsLBA48) {
        // Send command "read sectors EXT"
        commandRegs->write8(0x24, 7);
      } else {
        // Send command "read sectors with retry"
        commandRegs->write8(0x20, 7);
      }
    }

    const Time::Timestamp completionStarted = Time::getTicks();
    size_t completionPolls = 0;

    // Acquire the 'outstanding IRQ' mutex, or use other means if no IRQ.
    while (true) {
      if (++completionPolls >= AtaDmaCompletionPollLimit ||
          (Time::getTicks() - completionStarted) >= AtaDmaCompletionTimeout) {
        ERROR("ATA: DMA read exceeded its completion deadline");
        return 0;
      }

      if (getInterruptNumber() != 0xFF) {
        if (!irqCompletion->acquireForCompletion(1, 10)) {
          // Timeout.
          ERROR("ATA: timeout during data transfer");
          return 0;
        }
      }

      // Ensure we are not busy before continuing handling.
      status = ataWait(commandRegs, controlRegs);
      if (status.reg.err) {
        /// \todo What's the best way to handle this?
        if (m_bDma && bDmaSetup) {
          WARNING("ATA: read failed during DMA data transfer");
        }
        return false;
      }

      if (m_bDma && bDmaSetup) {
        if (m_BusMaster->hasInterrupt() || m_BusMaster->hasCompleted()) {
          // commandComplete effectively resets the device state, so
          // we need to get the error register first.
          bool bError = m_BusMaster->hasError();
          dmaTransaction.complete();
          if (bError) {
            return 0;
          } else {
            break;
          }
        }
      } else {
        break;
      }
      Processor::pause();
    }

    if (!bDmaSetup) {
      size_t byteOffset = firstBuffer * 0x1000;
      for (int i = 0; i < nSectorsToRead; i++) {
        // Wait until !BUSY
        status = ataWait(commandRegs, controlRegs);
        if (status.reg.err) {
          // Ka-boom! Something went wrong :(
          /// \todo What's the best way to handle this?
          WARNING("ATA: read failed during data transfer");
          return 0;
        }

        // Figure out which buffer we care about here.
        size_t nBuffer = byteOffset / 0x1000;
        size_t offset = byteOffset % 0x1000;

        // Read the sector.
        uint16_t* target = reinterpret_cast<uint16_t*>(buffers[nBuffer].buffer + offset);
        for (int j = 0; j < 256; j++) {
          *target++ = commandRegs->read16(0);
        }

        byteOffset += 512;
      }
    }

    buffersConsumed += buffersThisRead;
    ioLocation += nSectorsToRead * 512;
  }

  assert(buffersConsumed == nBuffers);

  fillGuard.publish();
  return nBytes;
}

uint64_t AtaDisk::doWrite(uint64_t location) {
  if (location % 512)
    panic("AtaDisk: write request not on a sector boundary!");

// Safety check
#if CRIPPLE_HDD
  return 0;
#endif

  if (m_AtaDiskType != NotPacket) {
    /// \todo might still want to allow writes - assuming CDROM here...
    // ATA controllers bypass ScsiController's post-write unpin, so unsupported
    // packet writes must retire the cache pin transferred by ScsiDisk::write.
    getCache().release(location);
    return 0;
  }

  // Write only the affected page. This deviates from the behaviour of reads,
  // which read a very large amount of data at once. Most writes (flush()
  // aside) are done asynchronously, while reads are synchronous.
  // This means we don't need to care about evicted pages within a disk block
  // because we're writing only a specific page that we already know exists.
  const uintptr_t buffer = ataTakeQueuedWritePage(getCache(), location);
  if (!buffer) {
    ERROR("AtaDisk::doWrite - queued buffer was not in cache");
    return 0;
  }

  // Make sure we don't leave the refcnt increased by writing.
  CachePageGuard guard(getCache(), location);

  return writePageBuffer(location, buffer);
}

uint64_t AtaDisk::doWriteDirect(uint64_t location, uintptr_t page) {
  if ((location % 512) || !page)
    return 0;

// Safety check
#if CRIPPLE_HDD
  return 0;
#endif

  if (m_AtaDiskType != NotPacket)
    return 0;

  return writePageBuffer(location, page);
}

uint64_t AtaDisk::writePageBuffer(uint64_t location, uintptr_t buffer) {
  const uintptr_t nBytes = 0x1000;

  if (getNativeBlockSize() != 512) {
    ERROR("ATA: writes require 512-byte logical sectors");
    return 0;
  }

#if SUPERDEBUG
  NOTICE("doWrite(" << location << ")");
#endif

  // Grab our parent's IoPorts for command and control accesses.
  IoBase* commandRegs = m_CommandRegs;
  IoBase* controlRegs = m_ControlRegs;

  // Geometry has been validated above, so one cache page is eight sectors.
  uint32_t nSectors = nBytes / 512;
  if (nBytes % 512)
    nSectors++;

  // Wait for BSY and DRQ to be zero before selecting the device
  AtaStatus status;
  ataWait(commandRegs, controlRegs);

  // Select the device to transmit to
  uint8_t devSelect;
  if (m_SupportsLBA48)
    devSelect = (m_IsMaster) ? 0xE0 : 0xF0;
  else
    devSelect = (m_IsMaster) ? 0xA0 : 0xB0;
  commandRegs->write8(devSelect, 6);

  // Wait for it to be selected
  ataWait(commandRegs, controlRegs);

  uint16_t* tmp = reinterpret_cast<uint16_t*>(buffer);

  while (nSectors > 0) {
    ScopedBusMasterTransaction dmaTransaction(m_BusMaster);

    // Keep write admission on the same bounded readiness contract as
    // reads. This used to be an unbounded boot/storage stall.
    status = ataWait(commandRegs, controlRegs);
    if (status.reg.err || !status.reg.drdy) {
      ERROR("ATA: drive did not become ready for write");
      return 0;
    }

    // PIO WRITE SECTORS is one page-sized command. The IDENTIFY multiple-mode
    // limit does not apply to it.
    uint8_t nSectorsToWrite =
        m_bDma ? min(m_pIdent.data.max_sectors_per_irq, nSectors) : static_cast<uint8_t>(nSectors);

    bool bDmaSetup = false;
    if (m_bDma && nSectorsToWrite) {
      bDmaSetup = dmaTransaction.add(buffer, nSectorsToWrite * 512);
    }
    if (!bDmaSetup) {
      nSectorsToWrite = static_cast<uint8_t>(nSectors);
    }
    nSectors -= nSectorsToWrite;

    if (m_SupportsLBA48)
      setupLBA48(location, nSectorsToWrite);
    else {
      if (location >= 0x2000000000ULL) {
        WARNING(
            "Ata: Sector > 128GB requested but LBA48 addressing "
            "not supported!");
      }
      setupLBA28(location, nSectorsToWrite);
    }

    // Enable IRQs so we can avoid spinning if possible.
    controlRegs->write8(0, 2);

    IrqCompletion irqCompletion(*this);

    bool oldInterrupts = Processor::getInterrupts();
    if (!oldInterrupts)
      Processor::setInterrupts(true);

    if (m_bDma && bDmaSetup) {
      // Start DMA before we send the command.
      if (!dmaTransaction.begin(true)) {
        ERROR("ATA: failed to start DMA write");
        return 0;
      }

      if (!m_SupportsLBA48) {
        // Send command "write DMA"
        commandRegs->write8(0xCA, 7);
      } else {
        // Send command "read write EXT"
        commandRegs->write8(0x35, 7);
      }
      const Time::Timestamp completionStarted = Time::getTicks();
      size_t completionPolls = 0;

      // Wait for completion.
      while (true) {
        if (++completionPolls >= AtaDmaCompletionPollLimit ||
            (Time::getTicks() - completionStarted) >= AtaDmaCompletionTimeout) {
          ERROR("ATA: DMA write exceeded its completion deadline");
          return 0;
        }

        if (getInterruptNumber() != 0xFF) {
          // 10 second timeout.
          if (!irqCompletion->acquireForCompletion(1, 10)) {
            WARNING("ATA: failed to get IRQ");
            return 0;
          }
        }

        // Ensure we are not busy before continuing handling.
        status = ataWait(commandRegs, controlRegs);
        if (status.reg.err) {
          /// \todo What's the best way to handle this?
          WARNING("ATA: write failed during DMA data transfer");
          return false;
        }

        if (m_BusMaster->hasInterrupt() || m_BusMaster->hasCompleted()) {
          // commandComplete effectively resets the device state, so
          // we need to get the error register first.
          bool bError = m_BusMaster->hasError();
          dmaTransaction.complete();
          if (bError)
            return 0;
          else
            break;
        }
        Processor::pause();
      }
    } else {
      AtaPioPollBudget budget = {Time::getTicks(), AtaPioCompletionTimeout, 0,
                                 AtaPioCompletionPollLimit};

      if (m_SupportsLBA48) {
        // Send command "write sectors EXT"
        commandRegs->write8(0x34, 7);
      } else {
        // Send command "write sectors with retry"
        commandRegs->write8(0x30, 7);
      }

      // A write-completion IRQ cannot arrive until the host services the
      // initial DRQ, so the data-phase poll must begin immediately.
      if (!ataPioWrite512ByteSectors(commandRegs, controlRegs, tmp, nSectorsToWrite, budget,
                                     status)) {
        WARNING("ATA: PIO write failed, status=" << status.__reg_contents);
        return 0;
      }
    }
  }

#if SUPERDEBUG
  NOTICE("ATA: successfully wrote " << nBytes << " bytes to disk.");
#endif
  return nBytes;
}

void AtaDisk::irqReceived() {
  LockGuard<Spinlock> guard(m_IrqLock);
  if (m_IrqReceived) {
    m_IrqReceived->release();
  }
}

void AtaDisk::setupLBA28(uint64_t n, uint32_t nSectors) {
  IoBase* commandRegs = m_CommandRegs;

  commandRegs->write8(static_cast<uint8_t>(nSectors & 0xFF), 2);

  // Get the sector number of the address.
  n /= 512;

  uint8_t sector = static_cast<uint8_t>(n & 0xFF);
  uint8_t cLow = static_cast<uint8_t>((n >> 8) & 0xFF);
  uint8_t cHigh = static_cast<uint8_t>((n >> 16) & 0xFF);
  uint8_t head = static_cast<uint8_t>((n >> 24) & 0x0F);
  if (m_IsMaster)
    head |= 0xE0;
  else
    head |= 0xF0;

  commandRegs->write8(head, 6);
  commandRegs->write8(sector, 3);
  commandRegs->write8(cLow, 4);
  commandRegs->write8(cHigh, 5);
}

void AtaDisk::setupLBA48(uint64_t n, uint32_t nSectors) {
  IoBase* commandRegs = m_CommandRegs;

  // Get the sector number of the address.
  n /= 512;

  uint8_t lba1 = static_cast<uint8_t>(n & 0xFF);
  uint8_t lba2 = static_cast<uint8_t>((n >> 8) & 0xFF);
  uint8_t lba3 = static_cast<uint8_t>((n >> 16) & 0xFF);
  uint8_t lba4 = static_cast<uint8_t>((n >> 24) & 0xFF);
  uint8_t lba5 = static_cast<uint8_t>((n >> 32) & 0xFF);
  uint8_t lba6 = static_cast<uint8_t>((n >> 40) & 0xFF);

  commandRegs->write8((nSectors & 0xFFFF) >> 8, 2);
  commandRegs->write8(lba4, 3);
  commandRegs->write8(lba5, 4);
  commandRegs->write8(lba6, 5);
  commandRegs->write8((nSectors & 0xFF), 2);
  commandRegs->write8(lba1, 3);
  commandRegs->write8(lba2, 4);
  commandRegs->write8(lba3, 5);
}

void AtaDisk::setFeatures(uint8_t command, uint8_t countreg, uint8_t lowreg, uint8_t midreg,
                          uint8_t hireg) {
  // Grab our parent's IoPorts for command and control accesses.
  IoBase* commandRegs = m_CommandRegs;

  uint8_t devSelect = (m_IsMaster) ? 0xA0 : 0xB0;
  commandRegs->write8(devSelect, 6);

  commandRegs->write8(command, 1);
  commandRegs->write8(countreg, 2);
  commandRegs->write8(lowreg, 3);
  commandRegs->write8(midreg, 4);
  commandRegs->write8(hireg, 5);
  commandRegs->write8(0xEF, 7);
}

size_t AtaDisk::getSize() const {
  if (m_AtaDiskType != NotPacket) {
    return ScsiDisk::getSize();
  }

  // Determine sector count.
  size_t sector_count = 0;
  if (m_SupportsLBA48) {
    // Try for the LBA48 sector count.
    if (m_pIdent.data.max_user_lba48)
      sector_count = m_pIdent.data.max_user_lba48;
    else
      sector_count = m_pIdent.data.sector_count;
  } else {
    sector_count = m_pIdent.data.sector_count;
  }

  // Determine sector size.
  size_t sector_size = 512;
  if (m_pIdent.data.sector_size.logical_larger_than_512b) {
    // Calculate.
    sector_size = m_pIdent.data.words_per_logical * sizeof(uint16_t);
  }

  return sector_count * sector_size;
}

size_t AtaDisk::getBlockSize() const {
  if (m_AtaDiskType != NotPacket) {
    return ScsiDisk::getBlockSize();
  }
  return m_BlockSize;
}

size_t AtaDisk::getCacheFillSize() const {
  if (m_AtaDiskType != NotPacket) {
    return ScsiDisk::getCacheFillSize();
  }
  return m_BlockSize;
}

size_t AtaDisk::getNativeBlockSize() const {
  if (m_AtaDiskType != NotPacket) {
    return ScsiDisk::getNativeBlockSize();
  }

  // Native blocks are just sectors.
  size_t sector_size = 512;
  if (m_pIdent.data.sector_size.logical_larger_than_512b) {
    // Calculate.
    sector_size = m_pIdent.data.words_per_logical * sizeof(uint16_t);
  }

  return sector_size;
}

size_t AtaDisk::getBlockCount() const {
  if (m_AtaDiskType != NotPacket) {
    return ScsiDisk::getBlockCount();
  }

  // Determine sector count.
  size_t sector_count = 0;
  if (m_SupportsLBA48) {
    // Try for the LBA48 sector count.
    if (m_pIdent.data.max_user_lba48)
      sector_count = m_pIdent.data.max_user_lba48;
    else
      sector_count = m_pIdent.data.sector_count;
  } else {
    sector_count = m_pIdent.data.sector_count;
  }

  return sector_count;
}
