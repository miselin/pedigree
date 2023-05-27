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
#include "AtaController.h"
#include "ata-common.h"
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

#if CRIPPLE_HDD
#pragma GCC diagnostic ignored "-Wunreachable-code"
#endif

// #define ATA_DEFAULT_BLOCK_SIZE 0x1000
#define ATA_DEFAULT_BLOCK_SIZE 0x10000 * 2

// Note the IrqReceived mutex is deliberately started in the locked state.
AtaDisk::AtaDisk(
    AtaController *pDev, bool isMaster, IoBase *commandRegs,
    IoBase *controlRegs, BusMasterIde *busMaster)
    : ScsiDisk(), m_IsMaster(isMaster), m_SupportsLBA28(true),
      m_SupportsLBA48(false), m_BlockSize(ATA_DEFAULT_BLOCK_SIZE),
      m_IrqReceived(0), m_AtaDiskType(NotPacket), m_PacketSize(0),
      m_Removable(false), m_CommandRegs(commandRegs),
      m_ControlRegs(controlRegs), m_BusMaster(busMaster), m_PrdTableLock(false),
      m_PrdTable(0), m_LastPrdTableOffset(0), m_PrdTablePhys(0),
      m_PrdTableMemRegion("ata-prdtable"), m_bDma(true)
{
    m_pParent = pDev;
}

AtaDisk::~AtaDisk()
{
}

bool AtaDisk::initialise(size_t nUnit)
{
    // Grab our parent.
    AtaController *pParent = static_cast<AtaController *>(m_pParent);

    // Grab our parent's IoPorts for command and control accesses.
    IoBase *commandRegs = m_CommandRegs;
    IoBase *controlRegs = m_ControlRegs;

    // Drive spin-up (go from standby to active, if necessary)
    setFeatures(0x07, 0, 0, 0, 0);

    // Check for device presence
    uint8_t devSelect = (m_IsMaster) ? 0xA0 : 0xB0;
    commandRegs->write8(devSelect, 6);
    commandRegs->write8(0xEC, 7);
    if (commandRegs->read8(7) == 0)
    {
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
    if (m3 == 0x14 && m4 == 0xeb)
    {
        // Run IDENTIFY PACKET DEVICE instead
        commandRegs->write8(devSelect, 6);
        commandRegs->write8(0xA1, 7);
        status = ataWait(commandRegs, controlRegs);
    }
    else
    {
        m_AtaDiskType = NotPacket;
    }

    // After checking signature and potentially retrying with IDENTIFY PACKET
    // DEVICE, we can check for an error proper now.
    if (status.reg.err)
    {
        WARNING("ATA drive errored on IDENTIFY!");
        return false;
    }

    // Read the data.
    for (int i = 0; i < 256; i++)
    {
        m_pIdent.__raw[i] = commandRegs->read16(0);
    }

    // Check for late error - final sanity check.
    if (commandRegs->read8(7) & 1)
    {
        WARNING(
            "ATA drive now has an error status after reading IDENTIFY data.");
        return false;
    }

    // Do we have integrity data?
    if (m_pIdent.data.signature == 0xA5)
    {
        // Yes. Run a checksum.
        uint8_t sum = 0;
        uint8_t *bytes = reinterpret_cast<uint8_t *>(m_pIdent.__raw);
        for (size_t i = 0; i < 512; ++i)
            sum += bytes[i];

        // The result should be zero if the checksum is in fact correct.
        if (sum)
        {
            WARNING("ATA IDENTIFY data failed checksum!");
            return false;
        }
    }

    // Interpret the data.

    // Good device?
    if ((m_AtaDiskType == NotPacket) && m_pIdent.data.general_config.not_ata)
    {
        ERROR("ATA: Device does not conform to the ATA specification.");
        return false;
    }
    else if (
        (m_AtaDiskType != NotPacket) && (!m_pIdent.data.general_config.not_ata))
    {
        ERROR("ATA: PACKET device does not conform to the ATA specification.");
        return false;
    }

    if (m_AtaDiskType != NotPacket)
    {
        m_AtaDiskType = static_cast<AtaDiskType>(
            m_pIdent.data.general_config.packet_cmdset);
    }

    // Get the device name.
    ataLoadSwapped(m_pName, m_pIdent.data.model_number, 20);

    // The device name is padded by spaces. Backtrack through converting spaces
    // into NULL bytes.
    for (int i = 39; i > 0; i--)
    {
        if (m_pName[i] != ' ')
            break;
        m_pName[i] = '\0';
    }
    m_pName[40] = '\0';

    // Get the serial number.
    ataLoadSwapped(m_pSerialNumber, m_pIdent.data.serial_number, 10);

    // The serial number is padded by spaces. Backtrack through converting
    // spaces into NULL bytes.
    for (int i = 19; i > 0; i--)
    {
        if (m_pSerialNumber[i] != ' ')
            break;
        m_pSerialNumber[i] = '\0';
    }
    m_pSerialNumber[20] = '\0';

    // Get the firmware revision.
    ataLoadSwapped(m_pFirmwareRevision, m_pIdent.data.firmware_revision, 4);

    // The device name is padded by spaces. Backtrack through converting spaces
    // into NULL bytes.
    for (int i = 7; i > 0; i--)
    {
        if (m_pFirmwareRevision[i] != ' ')
            break;
        m_pFirmwareRevision[i] = '\0';
    }
    m_pFirmwareRevision[8] = '\0';

    // Check that LBA48 is actually enabled.
    if (m_pIdent.data.command_sets_support.address48)
    {
        m_SupportsLBA48 = m_pIdent.data.command_sets_enabled.address48;
        if (!m_SupportsLBA48)
            WARNING("ATA: Device supports LBA48 but it isn't enabled.");
    }

    // And check for LBA28 support, just in case.
    if (!m_pIdent.data.caps.lba)
    {
        /// \todo should check that this doesn't break on ATAPI
        ERROR("ATA: Device does not support LBA.");
        return false;
    }

    // Do we have DMA?
    m_bDma = false;
    if (m_pIdent.data.caps.dma)
    {
        m_bDma = true;
        NOTICE("ATA: Device supports DMA.");

        if (m_pIdent.data.validity.multiword_dma_valid)
        {
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

            if (highest_mode != ~0U)
            {
                NOTICE(
                    "ATA: Device Multiword DMA: supports up to mode"
                    << Dec << highest_mode << Hex);
            }
            else
            {
                NOTICE("ATA: Device Multiword DMA: no support");
            }

            if (sel_mode != ~0U)
            {
                NOTICE(
                    "ATA: Device Multiword DMA: mode" << Dec << sel_mode << Hex
                                                      << " is selected");
            }
        }

        if (m_pIdent.data.validity.ultra_dma_valid)
        {
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

            if (highest_mode != ~0U)
            {
                NOTICE(
                    "ATA: Device Ultra DMA: supports up to mode"
                    << Dec << highest_mode << Hex);
            }
            else
            {
                NOTICE("ATA: Device Ultra DMA: no support");
            }

            if (sel_mode != ~0U)
            {
                NOTICE(
                    "ATA: Device Ultra DMA: mode" << Dec << sel_mode << Hex
                                                  << " is enabled");
            }
        }
    }

    // Do we have a bus master with which to work with?
    // ISA ATA does not.
    if (!m_BusMaster)
    {
        WARNING("ATA: Controller does not support DMA");
        m_bDma = false;
    }

    if (m_pIdent.data.sector_size.logical_larger_than_512b ||
        m_pIdent.data.sector_size.multiple_logical_per_physical)
    {
        // Large physical sectors.
        size_t logical_size = 512;
        if (m_pIdent.data.sector_size.logical_larger_than_512b)
            logical_size = m_pIdent.data.words_per_logical * sizeof(uint16_t);

        // Logical sectors per physical sector.
        size_t log_per_phys = 1
                              << m_pIdent.data.sector_size.logical_per_physical;
        size_t physical_size = log_per_phys * logical_size;

        NOTICE(
            "ATA: Physical sector size is " << Dec << physical_size << Hex
                                            << " bytes.");
        NOTICE(
            "ATA: Logical sector size is " << Dec << logical_size << Hex
                                           << " bytes.");

        if (physical_size > 512)
        {
            // Non-standard physical sectors; align block size to this.
            if (m_BlockSize % physical_size)
            {
                // Default block size doesn't map to physical sectors well.
                WARNING("ATA: Default block size doesn't map well to physical "
                        "sectors, performance may be degraded.");
            }

            // Always make sure our blocks are bigger than physical sectors.
            if (m_BlockSize < physical_size)
                m_BlockSize = physical_size;
        }
        else
        {
            // Standard physical sectors - default block size is okay.
        }
    }

    NOTICE("ATA: IRQ is #" << Dec << getInterruptNumber() << Hex << ".");

    // ATAPI pieces.
    if (m_AtaDiskType != NotPacket)
    {
        // Packet size?
        m_PacketSize = m_pIdent.data.general_config.packet_sz ? 16 : 12;
        NOTICE(
            "ATAPI: packet size is " << Dec << m_PacketSize << " bytes" << Hex);

        commandRegs->write8(devSelect, 6);
        commandRegs->write8(0xDA, 7);  // GET MEDIA STATUS
        status = ataWait(commandRegs, controlRegs);
        if (status.reg.err)
        {
            // We have information in the error register
            uint8_t err = commandRegs->read8(1);

            // ABORT?
            if (err & 0x4)
            {
                WARNING("ATAPI: device does not support GET MEDIA STATUS.");
            }
            else if (err & 2)
            {
                WARNING("ATAPI: No media present in the drive - aborting.");
                WARNING("       TODO: handle media changes/insertions/removal "
                        "properly");
                return false;
            }
            else
            {
                NOTICE("ATAPI: Media status: " << err << ".");
            }
        }

        // Initialise SCSI disk interface.
        if (!ScsiDisk::initialise(pParent, nUnit))
        {
            ERROR("ATAPI: ScsiDisk init failed.");
            return false;
        }

        // Grab Inquiry data to figure out what we're working with.
        const ScsiDisk::Inquiry *pInquiry = getInquiry();
        m_Removable = ((pInquiry->Removable & (1 << 7)) != 0);
        AtaDiskType inquiryType =
            static_cast<AtaDiskType>(pInquiry->Peripheral);
        if (inquiryType != m_AtaDiskType)
        {
            ERROR("ATAPI: IDENTIY PACKET DEVICE and SCSI INQUIRY disagree on "
                  "device type.");
            return false;
        }

        // Supported device?
        if (m_AtaDiskType != CdDvd && m_AtaDiskType != Block)
        {
            /// \todo Testing needs to be done on more than just CD/DVD and
            /// block devices...
            WARNING("Pedigree currently only supports CD/DVD and block ATAPI "
                    "devices.");
            return false;
        }
    }

    NOTICE(
        "Detected ATA device '" << m_pName << "', '" << m_pSerialNumber
                                << "', '" << m_pFirmwareRevision << "'");

    return true;
}

bool AtaDisk::sendCommand(
    size_t nUnit, uintptr_t pCommand, uint8_t nCommandSize,
    uintptr_t pRespBuffer, uint16_t nRespBytes, bool bWrite)
{
    if (m_AtaDiskType == NotPacket)
    {
        ERROR("AtaDisk::sendCommand called on a non-PACKET device");
        return false;
    }

    if (!m_PacketSize)
    {
        ERROR("sendCommand called but the packet size is not known!");
        return false;
    }

    AtaStatus status;

    IoBase *commandRegs = m_CommandRegs;
    IoBase *controlRegs = m_ControlRegs;

    uint16_t *tmpPacket = new uint16_t[m_PacketSize / 2];
    PointerGuard<uint16_t> tmpGuard(tmpPacket, true);
    MemoryCopy(tmpPacket, reinterpret_cast<void *>(pCommand), nCommandSize);
    ByteSet(tmpPacket + (nCommandSize / 2), 0, m_PacketSize - nCommandSize);

    // Set nIEN as we poll in sendCommand().
    controlRegs->write8(2, 2);

    // Wait for the device to finish any outstanding operations
    ataWait(commandRegs, controlRegs);

    // Select the device to transmit to
    uint8_t devSelect = m_IsMaster ? 0xA0 : 0xB0;
    commandRegs->write8(devSelect, 6);
    ataWait(commandRegs, controlRegs);

    // Verify that it's the correct device
    if ((commandRegs->read8(6) & devSelect) != devSelect)
    {
        WARNING("ATAPI: Device was not selected");
        return false;
    }

    bool bDmaSetup = false;
    if (m_bDma && nRespBytes)
    {
        bDmaSetup = m_BusMaster->add(pRespBuffer, nRespBytes);
    }

    // PACKET command
    if ((m_pIdent.__raw[62] & (1 << 15)) &&
        bDmaSetup)  // Device requires DMADIR for Packet DMA commands
        commandRegs->write8((bWrite ? 1 : 5), 1);  // Transfer to host, DMA
    else if (bDmaSetup)
        commandRegs->write8(1, 1);  // No overlap, DMA
    else
        commandRegs->write8(0, 1);              // No overlap, no DMA
    commandRegs->write8(0, 2);                  // Tag = 0
    commandRegs->write8(0, 3);                  // N/A for PACKET command
    commandRegs->write8(nRespBytes & 0xFF, 4);  // Byte count limit
    commandRegs->write8(((nRespBytes >> 8) & 0xFF), 5);

    // Transmit the PACKET command, wait for the device to be ready for the
    // command.
    commandRegs->write8(0xA0, 7);

    // Wait for sensible status before writing command packet.
    status = ataWait(commandRegs, controlRegs);

    // Error?
    if (status.reg.err)
    {
        ERROR(
            "ATAPI Packet command error [status=" << status.__reg_contents
                                                  << "]!");
        return false;
    }

    // If DMA is set up, begin that now, before sending the SCSI command.
    if (m_bDma && nRespBytes && bDmaSetup)
    {
        bDmaSetup = m_BusMaster->begin(bWrite);
    }

    // Transmit the command (padded as needed)
    for (size_t i = 0; i < (m_PacketSize / 2); i++)
    {
        commandRegs->write16(tmpPacket[i], 0);
    }

    // 400ns wait before reading status register.
    for (size_t i = 0; i < 4; ++i)
        controlRegs->read8(2);

    // Check for errors...
    // Note: not using ataWait as we don't want to block here.
    uint8_t statusreg = commandRegs->read8(7);
    if ((statusreg & 1) && !(statusreg & 0x80))
    {
        // CHK = 1, BSY = 0
        uint8_t error = commandRegs->read8(1);
        if (error & 0x4)
        {
            WARNING("ATAPI command failed (ABORT)");
        }
        else
        {
            WARNING(
                "ATAPI error with status " << statusreg << " [error=" << error
                                           << "]");
        }

        return false;
    }

    // If we aren't expecting anything from the device, we can just poll for
    // completion instead of waiting for an IRQ.
    if (!nRespBytes)
    {
        status = ataWait(commandRegs, controlRegs);
        return !status.reg.err;
    }

    while (true)
    {
        // Ensure we are not busy before continuing handling.
        status = ataWait(commandRegs, controlRegs);
        if (status.reg.err)
        {
            /// \todo What's the best way to handle this?
            if (m_bDma && bDmaSetup)
            {
                m_BusMaster->commandComplete();
                WARNING("ATAPI: read failed during DMA data transfer");
            }
            return false;
        }

        // Poll for completion.
        if (m_bDma && bDmaSetup)
        {
            if (m_BusMaster->hasInterrupt() || m_BusMaster->hasCompleted())
            {
                // commandComplete effectively resets the device state, so we
                // need to get the error register first.
                bool bError = m_BusMaster->hasError();
                m_BusMaster->commandComplete();
                if (bError)
                    return false;
                else
                    break;
            }
        }
        else
        {
            break;
        }
    }

    status = ataWait(commandRegs, controlRegs);
    if (status.reg.err)
    {
        WARNING("ATAPI sendCommand failed after sending command packet");
        logAtaStatus(status);
        return false;
    }

    // Check for DRQ, if not set, there's nothing to read
    if (!status.reg.drq)
        return true;

    // Read in the data, if we need to
    if (!m_bDma && !bDmaSetup)
    {
        size_t realSz = commandRegs->read8(4) | (commandRegs->read8(5) << 8);
        uint16_t *dest = reinterpret_cast<uint16_t *>(pRespBuffer);
        if (nRespBytes)
        {
            size_t sizeToRead =
                ((realSz > nRespBytes) ? nRespBytes : realSz) / 2;
            for (size_t i = 0; i < sizeToRead; i++)
            {
                if (bWrite)
                    commandRegs->write16(dest[i], 0);
                else
                    dest[i] = commandRegs->read16(0);
            }
        }

        // Discard unread data (or write pretend data)
        if (realSz > nRespBytes)
        {
            NOTICE(
                "sendCommand has to read beyond provided buffer ["
                << realSz << " is bigger than " << nRespBytes << "]");
            for (size_t i = nRespBytes; i < realSz; i += 2)
            {
                if (bWrite)
                    commandRegs->write16(0xFFFF, 0);
                else
                    commandRegs->read16(0);
            }
        }
    }

    // Complete
    uint8_t endStatus = commandRegs->read8(7);
    return (!(endStatus & 0x01));
}

uint64_t AtaDisk::doRead(uint64_t location)
{
    if (m_AtaDiskType != NotPacket)
        return ScsiDisk::doRead(location);

    // Memory for the "already-read" buffers to point at for DMA scatter/gather
    static char alreadyRead[4096] ALIGN(4096);

    // Create our set of buffers to read into.
    size_t nBytes = getBlockSize();
    location &= ~(nBytes - 1);  // Align location to block size.

    // Allocate list of buffers, allowing us to handle cache pages being widely
    // distributed around the virtual address space.
    size_t nBuffers = nBytes / 0x1000;  /// \todo getPageSize() here
    Buffer *buffers = new Buffer[nBuffers];
    PointerGuard<Buffer> guard2(buffers, true);

    bool bAlreadyAllRead = true;
    for (size_t i = 0; i < nBuffers; ++i)
    {
        buffers[i].offset = i * 0x1000;

        uintptr_t buffer = getCache().lookup(location + buffers[i].offset);
        if (buffer)
        {
            getCache().release(location + buffers[i].offset);
            buffer = reinterpret_cast<uintptr_t>(alreadyRead);
        }
        else
        {
            buffer = getCache().insert(location + buffers[i].offset);
            if (!buffer)
                FATAL("AtaDisk::doRead - couldn't get a buffer!");

            bAlreadyAllRead = false;
        }

        buffers[i].buffer = buffer;
    }

    if (bAlreadyAllRead)
    {
        // All pages were already in cache.
        return nBytes;
    }

    // Grab our parent's IoPorts for command and control accesses.
    IoBase *commandRegs = m_CommandRegs;
    IoBase *controlRegs = m_ControlRegs;

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
    while (nSectors > 0)
    {
        // Wait for status to be ready - spin until READY bit is set.
        while (!(commandRegs->read8(7) & 0x40))
            ;

        // Send out sector count.
        uint8_t nSectorsToRead =
            min(m_pIdent.data.max_sectors_per_irq, nSectors);
        nSectors -= nSectorsToRead;

        // Buffers are 4K each, so calculate the number of buffers used for
        // this particular read.
        size_t buffersThisRead = (nSectorsToRead * 512) / 0x1000;

        bool bDmaSetup = false;
        if (m_bDma)
        {
            for (size_t i = 0; i < buffersThisRead; ++i)
            {
                bDmaSetup = m_BusMaster->add(
                    buffers[buffersConsumed + i].buffer, 0x1000);
                if (!bDmaSetup)
                {
                    ERROR("DMA setup failed!");
                    break;
                }
            }

            buffersConsumed += buffersThisRead;
        }

        if (m_SupportsLBA48)
            setupLBA48(location, nSectorsToRead);
        else
        {
            if (location >= 0x2000000000ULL)
            {
                WARNING("Ata: Sector > 128GB requested but LBA48 addressing "
                        "not supported!");
            }
            setupLBA28(location, nSectorsToRead);
        }

        m_IrqReceived = new Mutex(true);
        PointerGuard<Mutex> irqGuard(&m_IrqReceived);

        if (getInterruptNumber() != 0xFF)
        {
            // Enable IRQs so we can avoid spinning if possible.
            controlRegs->write8(0, 2);

            bool oldInterrupts = Processor::getInterrupts();
            if (!oldInterrupts)
                Processor::setInterrupts(true);
        }

        if (m_bDma && bDmaSetup)
        {
            // Prepare DMA before we send the command.
            bDmaSetup = m_BusMaster->begin(false);

            if (!m_SupportsLBA48)
            {
                // Send command "read DMA"
                commandRegs->write8(0xC8, 7);
            }
            else
            {
                // Send command "read DMA EXT"
                commandRegs->write8(0x25, 7);
            }
        }
        else
        {
            if (m_SupportsLBA48)
            {
                // Send command "read sectors EXT"
                commandRegs->write8(0x24, 7);
            }
            else
            {
                // Send command "read sectors with retry"
                commandRegs->write8(0x20, 7);
            }
        }

        // Acquire the 'outstanding IRQ' mutex, or use other means if no IRQ.
        while (true)
        {
            if (getInterruptNumber() != 0xFF)
            {
                if (!m_IrqReceived->acquire(1, 10))
                {
                    // Timeout.
                    ERROR("ATA: timeout during data transfer");
                    return 0;
                }
            }

            // Ensure we are not busy before continuing handling.
            status = ataWait(commandRegs, controlRegs);
            if (status.reg.err)
            {
                /// \todo What's the best way to handle this?
                if (m_bDma && bDmaSetup)
                {
                    m_BusMaster->commandComplete();
                    WARNING("ATA: read failed during DMA data transfer");
                }
                return false;
            }

            if (m_bDma && bDmaSetup)
            {
                if (m_BusMaster->hasInterrupt() || m_BusMaster->hasCompleted())
                {
                    // commandComplete effectively resets the device state, so
                    // we need to get the error register first.
                    bool bError = m_BusMaster->hasError();
                    m_BusMaster->commandComplete();
                    if (bError)
                    {
                        return 0;
                    }
                    else
                    {
                        break;
                    }
                }
            }
            else
            {
                break;
            }
        }

        if (!m_bDma && !bDmaSetup)
        {
            size_t byteOffset = buffersConsumed * 0x1000;
            for (int i = 0; i < nSectorsToRead; i++)
            {
                // Wait until !BUSY
                status = ataWait(commandRegs, controlRegs);
                if (status.reg.err)
                {
                    // Ka-boom! Something went wrong :(
                    /// \todo What's the best way to handle this?
                    WARNING("ATA: read failed during data transfer");
                    return 0;
                }

                // Figure out which buffer we care about here.
                size_t nBuffer = byteOffset / 0x1000;
                size_t offset = byteOffset % 0x1000;

                // Read the sector.
                uint16_t *target = reinterpret_cast<uint16_t *>(
                    buffers[nBuffer].buffer + offset);
                for (int j = 0; j < 256; j++)
                {
                    *target++ = commandRegs->read16(0);
                }

                byteOffset += 512;
            }
        }

        location += nSectorsToRead * 512;
    }

    assert(buffersConsumed == nBuffers);

    // Update Cache - we're done reading.
    for (size_t i = 0; i < nBuffers; ++i)
    {
        if (buffers[i].buffer == reinterpret_cast<uintptr_t>(alreadyRead))
        {
            continue;
        }

        getCache().markNoLongerEditing(location + buffers[i].offset);
    }

    return nBytes;
}

uint64_t AtaDisk::doWrite(uint64_t location)
{
    if (location % 512)
        panic("AtaDisk: write request not on a sector boundary!");

// Safety check
#if CRIPPLE_HDD
    return 0;
#endif

    if (m_AtaDiskType != NotPacket)
    {
        /// \todo might still want to allow writes - assuming CDROM here...
        return 0;
    }

    // Write only the affected page. This deviates from the behaviour of reads,
    // which read a very large amount of data at once. Most writes (flush()
    // aside) are done asynchronously, while reads are synchronous.
    // This means we don't need to care about evicted pages within a disk block
    // because we're writing only a specific page that we already know exists.
    uintptr_t nBytes = 0x1000;
    uintptr_t buffer = getCache().lookup(location);
    if (!buffer)
    {
        FATAL("AtaDisk::doWrite - no buffer (completely misused method)");
    }

    // Undo the pin done by ScsiDisk::write that verified this location exists
    // in the first place. We have two active pins here: that from the above
    // lookup(), and the one from ScsiDisk::write. This just ensures we're
    // keeping the counts correct.
    getCache().release(location);

    // Make sure we don't leave the refcnt increased by writing.
    CachePageGuard guard(getCache(), location);

#if SUPERDEBUG
    NOTICE("doWrite(" << location << ")");
#endif

    // Grab our parent's IoPorts for command and control accesses.
    IoBase *commandRegs = m_CommandRegs;
    IoBase *controlRegs = m_ControlRegs;

    // How many sectors do we need to read?
    /// \todo logical sector size here
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

    uint16_t *tmp = reinterpret_cast<uint16_t *>(buffer);

    while (nSectors > 0)
    {
        // Wait for status to be ready - spin until READY bit is set.
        while (!(commandRegs->read8(7) & 0x40))
            ;

        // Send out sector count.
        uint8_t nSectorsToWrite =
            min(m_pIdent.data.max_sectors_per_irq, nSectors);
        nSectors -= nSectorsToWrite;

        bool bDmaSetup = false;
        if (m_bDma)
        {
            bDmaSetup = m_BusMaster->add(buffer, nSectorsToWrite * 512);
        }

        if (m_SupportsLBA48)
            setupLBA48(location, nSectorsToWrite);
        else
        {
            if (location >= 0x2000000000ULL)
            {
                WARNING("Ata: Sector > 128GB requested but LBA48 addressing "
                        "not supported!");
            }
            setupLBA28(location, nSectorsToWrite);
        }

        // Enable IRQs so we can avoid spinning if possible.
        controlRegs->write8(0, 2);

        if (m_IrqReceived)
            WARNING("ATA: IRQ mutex already existed");
        m_IrqReceived = new Mutex(true);
        PointerGuard<Mutex> guardReceivedMutex(&m_IrqReceived);

        bool oldInterrupts = Processor::getInterrupts();
        if (!oldInterrupts)
            Processor::setInterrupts(true);

        if (m_bDma && bDmaSetup)
        {
            // Start DMA before we send the command.
            bDmaSetup = m_BusMaster->begin(true);

            if (!m_SupportsLBA48)
            {
                // Send command "write DMA"
                commandRegs->write8(0xCA, 7);
            }
            else
            {
                // Send command "read write EXT"
                commandRegs->write8(0x35, 7);
            }
        }
        else
        {
            if (m_SupportsLBA48)
            {
                // Send command "write sectors EXT"
                commandRegs->write8(0x34, 7);
            }
            else
            {
                // Send command "write sectors with retry"
                commandRegs->write8(0x30, 7);
            }
        }

        // Wait for completion.
        while (true)
        {
            if (getInterruptNumber() != 0xFF)
            {
                // 10 second timeout.
                if (!m_IrqReceived->acquire(1, 10))
                {
                    WARNING("ATA: failed to get IRQ");
                }
            }

            // Ensure we are not busy before continuing handling.
            status = ataWait(commandRegs, controlRegs);
            if (status.reg.err)
            {
                /// \todo What's the best way to handle this?
                if (m_bDma && bDmaSetup)
                {
                    m_BusMaster->commandComplete();
                    WARNING("ATA: read failed during DMA data transfer");
                }
                return false;
            }

            if (m_bDma && bDmaSetup)
            {
                if (m_BusMaster->hasInterrupt() || m_BusMaster->hasCompleted())
                {
                    // commandComplete effectively resets the device state, so
                    // we need to get the error register first.
                    bool bError = m_BusMaster->hasError();
                    m_BusMaster->commandComplete();
                    if (bError)
                        return 0;
                    else
                        break;
                }
            }
            else
                break;
        }

        if (!m_bDma && !bDmaSetup)
        {
            for (int i = 0; i < nSectorsToWrite; i++)
            {
                // Wait until !BUSY
                status = ataWait(commandRegs, controlRegs);
                if (status.reg.err)
                {
                    // Ka-boom! Something went wrong :(
                    /// \todo What's the best way to handle this?
                    WARNING("ATA: write failed during data transfer");
                    return 0;
                }

                // Write the sector to disk.
                for (int j = 0; j < 256; j++)
                    commandRegs->write16(*tmp++, 0);
            }
        }
    }

#if SUPERDEBUG
    NOTICE("ATA: successfully wrote " << nBytes << " bytes to disk.");
#endif
    return nBytes;
}

void AtaDisk::irqReceived()
{
    if (m_IrqReceived)
        m_IrqReceived->release();
}

void AtaDisk::setupLBA28(uint64_t n, uint32_t nSectors)
{
    IoBase *commandRegs = m_CommandRegs;

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

void AtaDisk::setupLBA48(uint64_t n, uint32_t nSectors)
{
    IoBase *commandRegs = m_CommandRegs;

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

void AtaDisk::setFeatures(
    uint8_t command, uint8_t countreg, uint8_t lowreg, uint8_t midreg,
    uint8_t hireg)
{
    // Grab our parent's IoPorts for command and control accesses.
    IoBase *commandRegs = m_CommandRegs;

    uint8_t devSelect = (m_IsMaster) ? 0xA0 : 0xB0;
    commandRegs->write8(devSelect, 6);

    commandRegs->write8(command, 1);
    commandRegs->write8(countreg, 2);
    commandRegs->write8(lowreg, 3);
    commandRegs->write8(midreg, 4);
    commandRegs->write8(hireg, 5);
    commandRegs->write8(0xEF, 7);
}

size_t AtaDisk::getSize() const
{
    if (m_AtaDiskType != NotPacket)
    {
        return ScsiDisk::getSize();
    }

    // Determine sector count.
    size_t sector_count = 0;
    if (m_SupportsLBA48)
    {
        // Try for the LBA48 sector count.
        if (m_pIdent.data.max_user_lba48)
            sector_count = m_pIdent.data.max_user_lba48;
        else
            sector_count = m_pIdent.data.sector_count;
    }
    else
    {
        sector_count = m_pIdent.data.sector_count;
    }

    // Determine sector size.
    size_t sector_size = 512;
    if (m_pIdent.data.sector_size.logical_larger_than_512b)
    {
        // Calculate.
        sector_size = m_pIdent.data.words_per_logical * sizeof(uint16_t);
    }

    return sector_count * sector_size;
}

size_t AtaDisk::getBlockSize() const
{
    if (m_AtaDiskType != NotPacket)
    {
        return ScsiDisk::getBlockSize();
    }
    return m_BlockSize;
}

size_t AtaDisk::getNativeBlockSize() const
{
    if (m_AtaDiskType != NotPacket)
    {
        return ScsiDisk::getNativeBlockSize();
    }

    // Native blocks are just sectors.
    size_t sector_size = 512;
    if (m_pIdent.data.sector_size.logical_larger_than_512b)
    {
        // Calculate.
        sector_size = m_pIdent.data.words_per_logical * sizeof(uint16_t);
    }

    return sector_size;
}

size_t AtaDisk::getBlockCount() const
{
    if (m_AtaDiskType != NotPacket)
    {
        return ScsiDisk::getBlockCount();
    }

    // Determine sector count.
    size_t sector_count = 0;
    if (m_SupportsLBA48)
    {
        // Try for the LBA48 sector count.
        if (m_pIdent.data.max_user_lba48)
            sector_count = m_pIdent.data.max_user_lba48;
        else
            sector_count = m_pIdent.data.sector_count;
    }
    else
    {
        sector_count = m_pIdent.data.sector_count;
    }

    return sector_count;
}
