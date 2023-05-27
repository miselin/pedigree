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

#include "PciAtaController.h"
#include "AtaDisk.h"
#include "BusMasterIde.h"
#include "ata-common.h"
#include "modules/drivers/common/scsi/ScsiController.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Controller.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

class IrqHandler;

PciAtaController::PciAtaController(Controller *pDev, int nController)
    : AtaController(pDev, nController), m_PciControllerType(UnknownController)
{
    setSpecificType(String("ata-controller"));

    // Determine controller type
    NormalStaticString str;
    switch (getPciDeviceId())
    {
        case 0x1230:
            m_PciControllerType = PIIX;
            str = "PIIX";
            break;
        case 0x7010:
            m_PciControllerType = PIIX3;
            str = "PIIX3";
            break;
        case 0x7111:
            m_PciControllerType = PIIX4;
            str = "PIIX4";
            break;
        case 0x2411:
            m_PciControllerType = ICH;
            str = "ICH";
            break;
        case 0x2421:
            m_PciControllerType = ICH0;
            str = "ICH0";
            break;
        case 0x244A:
        case 0x244B:
            m_PciControllerType = ICH2;
            str = "ICH2";
            break;
        case 0x248A:
        case 0x248B:
            m_PciControllerType = ICH3;
            str = "ICH3";
            break;
        case 0x24CA:
        case 0x24CB:
            m_PciControllerType = ICH4;
            str = "ICH4";
            break;
        case 0x24DB:
            m_PciControllerType = ICH5;
            str = "ICH5";
            break;
        default:
            m_PciControllerType = UnknownController;
            str = "<unknown>";
            break;
    }
    str += " PCI IDE controller found";
    NOTICE(static_cast<const char *>(str));

    if (m_PciControllerType == UnknownController)
        return;

    // Find BAR4 (BusMaster registers)
    Device::Address *bar4 = 0;
    for (size_t i = 0; i < addresses().count(); i++)
    {
        if (!StringCompare(
                static_cast<const char *>(addresses()[i]->m_Name), "bar4"))
            bar4 = addresses()[i];
    }

    m_Children.clear();

    // Read the BusMaster interface base address register and tell it where we
    // would like to talk to it (BAR4).
    if (bar4)
    {
        uint32_t busMasterIfaceAddr =
            PciBus::instance().readConfigSpace(pDev, 8);
        busMasterIfaceAddr &= 0xFFFF000F;
        busMasterIfaceAddr |= bar4->m_Address & 0xFFF0;
        NOTICE(
            "    - Bus master interface base register at " << bar4->m_Address);
        PciBus::instance().writeConfigSpace(pDev, 8, busMasterIfaceAddr);

        // Read the command register and then enable I/O space. We do this so
        // that we can still access drives using PIO. We also enable the
        // BusMaster function on the controller.
        uint32_t commandReg = PciBus::instance().readConfigSpace(pDev, 1);
        commandReg = (commandReg & ~(0x7)) | 0x7;
        PciBus::instance().writeConfigSpace(pDev, 1, commandReg);

        // Fiddle with the IDE timing registers
        // TIME0, TIME1, IE0, IE1, PPE0, PPE1, DTE0, DTE1, minimum recovery
        // time, minimum IORDY sample point, IDE decode enable.
        uint32_t ideTiming = 0xB3FF;
        // Apply to both channels.
        ideTiming |= (ideTiming << 16);
        PciBus::instance().writeConfigSpace(pDev, 0x10, ideTiming);

        // Write the interrupt line into the PCI space if needed.
        // This is only meaningful for < PIIX3...
        if (m_PciControllerType == PIIX)
        {
            uint32_t miscFields = PciBus::instance().readConfigSpace(pDev, 0xF);
            if ((miscFields & 0xF) != getInterruptNumber())
            {
                if (getInterruptNumber())
                {
                    miscFields &= ~0xF;
                    miscFields |= getInterruptNumber() & 0xF;
                }
            }
            PciBus::instance().writeConfigSpace(pDev, 0xF, miscFields);
        }

        // PIIX4+ has Ultra DMA configuration.
        /// \todo for ICH and the like, there's more Ultra DMA configuration.
        if (m_PciControllerType == PIIX4)
        {
            // UDMACTL register - enable UDMA mode for all drives.
            uint32_t udmactl = PciBus::instance().readConfigSpace(pDev, 0x12);
            udmactl |= 0xF;
            PciBus::instance().writeConfigSpace(pDev, 0x12, udmactl);

            // Set timings for UDMA2 (Ultra DMA 33, max supported by PIIX4).
            uint32_t timings = PciBus::instance().readConfigSpace(pDev, 0x13);
            uint16_t target_timings = 0x2 * 0x3333;
            timings = (timings & 0xFFFF) | target_timings;
            PciBus::instance().writeConfigSpace(pDev, 0x13, timings);
        }
    }

    // The controller must be able to perform BusMaster IDE DMA transfers, or
    // else we have to fall back to PIO transfers.
    bool bDma = false;
    if (bar4 && (pDev->getPciProgInterface() & 0x80))
    {
        NOTICE("    - This is a DMA capable controller");
        bDma = true;
    }

#if !KERNEL_PROCESSOR_NO_PORT_IO
    IoPort *masterCommand = new IoPort("pci-ide-master-cmd");
    IoPort *slaveCommand = new IoPort("pci-ide-slave-cmd");
    IoPort *masterControl = new IoPort("pci-ide-master-ctl");
    IoPort *slaveControl = new IoPort("pci-ide-slave-ctl");

    /// \todo Bus master registerss may be memory mapped...
    IoPort *bar4_a = 0;
    IoPort *bar4_b = 0;
    BusMasterIde *primaryBusMaster = 0;
    BusMasterIde *secondaryBusMaster = 0;
    if (bDma)
    {
        uintptr_t addr = bar4->m_Address;

        // Okay, now delete the BAR's IoBase. We're not going to use it again,
        // and we need the ports.
        delete bar4->m_Io;
        bar4->m_Io = 0;

        bar4_a = new IoPort("pci-ide-busmaster-primary");
        if (!bar4_a->allocate(addr, 8))
        {
            ERROR("Couldn't allocate primary BusMaster ports");
            delete bar4_a;
            bar4_a = 0;
        }

        bar4_b = new IoPort("pci-ide-busmaster-secondary");
        if (!bar4_b->allocate(addr + 8, 8))
        {
            ERROR("Couldn't allocate secondary BusMaster ports");
            delete bar4_b;
            bar4_b = 0;
        }

        // Create the BusMasterIde objects
        if (bar4_a)
        {
            primaryBusMaster = new BusMasterIde;
            if (!primaryBusMaster->initialise(bar4_a))
            {
                ERROR("Couldn't initialise primary BusMaster IDE interface");
                delete primaryBusMaster;
                delete bar4_a;

                primaryBusMaster = 0;
            }
        }
        if (bar4_b)
        {
            secondaryBusMaster = new BusMasterIde;
            if (!secondaryBusMaster->initialise(bar4_b))
            {
                ERROR("Couldn't initialise secondary BusMaster IDE interface");
                delete secondaryBusMaster;
                delete bar4_b;

                secondaryBusMaster = 0;
            }
        }
    }

    // By default, this is the port layout we can expect for the system
    /// \todo ICH will have "native mode" to worry about
    if (!masterCommand->allocate(0x1F0, 8))
        ERROR("Couldn't allocate master command ports");
    if (!masterControl->allocate(0x3F4, 4))
        ERROR("Couldn't allocate master control ports");
    if (!slaveCommand->allocate(0x170, 8))
        ERROR("Couldn't allocate slave command ports");
    if (!slaveControl->allocate(0x374, 4))
        ERROR("Couldn't allocate slave control ports");

    // Check for non-existent controllers.
    AtaStatus masterStatus = ataWait(masterCommand, masterControl);
    AtaStatus slaveStatus = ataWait(slaveCommand, slaveControl);
    if (masterStatus.__reg_contents == 0xff)
    {
        delete masterCommand;
        delete masterControl;
        masterCommand = 0;
        masterControl = 0;
    }
    if (slaveStatus.__reg_contents == 0xff)
    {
        delete slaveCommand;
        delete slaveControl;
        slaveCommand = 0;
        slaveControl = 0;
    }

    // Kick off an SRST on each control port.
    if (masterControl)
    {
        masterControl->write8(0x6, 2);
        Processor::pause();  // Hold SRST for 5 nanoseconds. /// \todo Better
                             // way of doing this?
        masterControl->write8(0x2, 2);
    }
    if (slaveControl)
    {
        slaveControl->write8(0x6, 2);
        Processor::pause();
        slaveControl->write8(0x2, 2);
    }

    Time::delay(
        2 * Time::Multiplier::Millisecond);  // Wait 2 ms after clearing.

    if (masterCommand)
        ataWait(masterCommand, masterControl);
    if (slaveCommand)
        ataWait(slaveCommand, slaveControl);

    // Install our IRQ handler
    if (getInterruptNumber() != 0xFF)
    {
        Machine::instance().getIrqManager()->registerIsaIrqHandler(
            getInterruptNumber(), static_cast<IrqHandler *>(this));
    }

    /// \todo Detect PCI IRQ, don't use ISA IRQs in native mode (etc...)
    size_t primaryIrq = 14, secondaryIrq = 15;
    if (primaryIrq != getInterruptNumber())
        Machine::instance().getIrqManager()->registerIsaIrqHandler(
            primaryIrq, static_cast<IrqHandler *>(this));
    if (secondaryIrq != getInterruptNumber())
        Machine::instance().getIrqManager()->registerIsaIrqHandler(
            secondaryIrq, static_cast<IrqHandler *>(this));

    // And finally, create disks
    if (masterControl)
    {
        diskHelper(
            true, masterCommand, masterControl, primaryBusMaster, primaryIrq);
        diskHelper(
            false, masterCommand, masterControl, primaryBusMaster, primaryIrq);
    }

    if (slaveControl)
    {
        diskHelper(
            true, slaveCommand, slaveControl, secondaryBusMaster, secondaryIrq);
        diskHelper(
            false, slaveCommand, slaveControl, secondaryBusMaster,
            secondaryIrq);
    }
#else
    ERROR("PCI ATA: no good, this machine has no port I/O");
#endif
}

PciAtaController::~PciAtaController()
{
}

void PciAtaController::diskHelper(
    bool master, IoBase *cmd, IoBase *ctl, BusMasterIde *dma, size_t irq)
{
    AtaDisk *pDisk = new AtaDisk(this, master, cmd, ctl, dma);
    pDisk->setInterruptNumber(irq);

    // Allow the initialisation to use sendCommand.
    size_t n = getNumChildren();
    addChild(pDisk);

    if (!pDisk->initialise(n))
    {
        removeChild(pDisk);
        delete pDisk;
    }
}

bool PciAtaController::sendCommand(
    size_t nUnit, uintptr_t pCommand, uint8_t nCommandSize,
    uintptr_t pRespBuffer, uint16_t nRespBytes, bool bWrite)
{
    Device *pChild = getChild(nUnit);
    if (!pChild)
    {
        ERROR("PCI ATA: sendCommand called with a bad unit number.");
        return false;
    }

    AtaDisk *pDisk = static_cast<AtaDisk *>(pChild);
    return pDisk->sendCommand(
        nUnit, pCommand, nCommandSize, pRespBuffer, nRespBytes, bWrite);
}

uint64_t PciAtaController::executeRequest(
    uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
    uint64_t p6, uint64_t p7, uint64_t p8)
{
    // Pin handling threads to the BSP as we depend on IRQs.
    Processor::information().getCurrentThread()->forceToStartupProcessor();

    AtaDisk *pDisk = reinterpret_cast<AtaDisk *>(p2);
    if (p1 == SCSI_REQUEST_READ)
        return pDisk->doRead(p3);
    else if (p1 == SCSI_REQUEST_WRITE)
        return pDisk->doWrite(p3);
    else
        return 0;
}

bool PciAtaController::irq(irq_id_t number, InterruptState &state)
{
    for (unsigned int i = 0; i < getNumChildren(); i++)
    {
        AtaDisk *pDisk = static_cast<AtaDisk *>(getChild(i));
        if (pDisk->getInterruptNumber() != number)
            continue;

        BusMasterIde *pBusMaster = pDisk->getBusMaster();
        if (pBusMaster && !pBusMaster->isActive())
        {
            // No active DMA transfer - clear interrupt/error bits.
            pBusMaster->commandComplete();
        }
        pDisk->irqReceived();
    }
    return true;
}
