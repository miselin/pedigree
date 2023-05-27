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

#include "IsaAtaController.h"
#include "AtaDisk.h"
#include "modules/drivers/common/scsi/ScsiController.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/new"

class Controller;
class IrqHandler;

IsaAtaController::IsaAtaController(Controller *pDev, int nController)
    : AtaController(pDev, nController)
{
    setSpecificType(String("ata-controller"));

    // Initialise our ports.
    bool bPortsFound = false;
    for (unsigned int i = 0; i < m_Addresses.count(); i++)
    {
        if (m_Addresses[i]->m_Name.compare("command") ||
            m_Addresses[i]->m_Name.compare("bar0"))
        {
            m_pCommandRegs = m_Addresses[i]->m_Io;
            bPortsFound = true;
        }
        if (m_Addresses[i]->m_Name.compare("control") ||
            m_Addresses[i]->m_Name.compare("bar1"))
        {
            m_pControlRegs = m_Addresses[i]->m_Io;
            bPortsFound = true;
        }
    }

    if (!bPortsFound)
    {
        ERROR("ISA ATA: No addresses found for this controller");
        return;
    }

    // Look for a floating bus
    if (m_pControlRegs->read8(6) == 0xFF || m_pCommandRegs->read8(7) == 0xFF)
    {
        // No devices on this controller
        return;
    }

    m_Children.clear();

    // Set up the RequestQueue
    initialise();

    // Perform a software reset.
    m_pControlRegs->write8(0x04, 6);  // Assert SRST
    Time::delay(5 * Time::Multiplier::Millisecond);

    m_pControlRegs->write8(0, 6);  // Negate SRST
    Time::delay(5 * Time::Multiplier::Millisecond);

    // Poll until BSY is clear. Until BSY is clear, no other bits in the
    // alternate status register are considered valid.
    uint8_t status = 0;
    while (1)  // ((status&0xC0) != 0) && ((status&0x9) == 0) )
    {
        status = m_pControlRegs->read8(6);
        if (status & 0x80)
            continue;
        else if (status & 0x1)
        {
            NOTICE("Error during ATA software reset, status = " << status);
            return;
        }
        else
            break;
    }

    // Create two disks - master and slave.
    AtaDisk *pMaster = new AtaDisk(this, true, m_pCommandRegs, m_pControlRegs);
    AtaDisk *pSlave = new AtaDisk(this, false, m_pCommandRegs, m_pControlRegs);

    pMaster->setInterruptNumber(getInterruptNumber());
    pSlave->setInterruptNumber(getInterruptNumber());

    size_t masterN = getNumChildren();
    addChild(pMaster);
    size_t slaveN = getNumChildren();
    addChild(pSlave);

    // Try and initialise the disks.
    bool masterInitialised = pMaster->initialise(masterN);
    bool slaveInitialised = pSlave->initialise(slaveN);

    Machine::instance().getIrqManager()->registerIsaIrqHandler(
        getInterruptNumber(), static_cast<IrqHandler *>(this));

    if (!masterInitialised)
    {
        removeChild(pMaster);
        delete pMaster;
    }

    if (!slaveInitialised)
    {
        removeChild(pSlave);
        delete pSlave;
    }
}

IsaAtaController::~IsaAtaController()
{
}

bool IsaAtaController::sendCommand(
    size_t nUnit, uintptr_t pCommand, uint8_t nCommandSize,
    uintptr_t pRespBuffer, uint16_t nRespBytes, bool bWrite)
{
    Device *pChild = getChild(nUnit);
    if (!pChild)
    {
        ERROR("ISA ATA: sendCommand called with a bad unit number.");
        return false;
    }

    AtaDisk *pDisk = static_cast<AtaDisk *>(pChild);
    return pDisk->sendCommand(
        nUnit, pCommand, nCommandSize, pRespBuffer, nRespBytes, bWrite);
}

uint64_t IsaAtaController::executeRequest(
    uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
    uint64_t p6, uint64_t p7, uint64_t p8)
{
    AtaDisk *pDisk = reinterpret_cast<AtaDisk *>(p2);
    if (p1 == SCSI_REQUEST_READ)
        return pDisk->doRead(p3);
    else if (p1 == SCSI_REQUEST_WRITE)
        return pDisk->doWrite(p3);
    else
        return 0;
}

bool IsaAtaController::irq(irq_id_t number, InterruptState &state)
{
    for (unsigned int i = 0; i < getNumChildren(); i++)
    {
        AtaDisk *pDisk = static_cast<AtaDisk *>(getChild(i));
        pDisk->irqReceived();
    }

    return true;
}
