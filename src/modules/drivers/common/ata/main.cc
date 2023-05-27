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
#include "PciAtaController.h"
#include "modules/Module.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Controller.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/new"

static int nController = 0;

static bool bFound = false;

// Try for a PIIX IDE controller first. We prefer the PIIX as it enables us
// to use DMA (and is a little easier to use for device detection).
static bool bPiixControllerFound = false;
static int piixLevel = -1;
static bool bFallBackISA = false;

static bool allowProbing = false;

static Device *probeIsaDevice(Controller *pDev)
{
    // Create a new AtaController device node.
    IsaAtaController *pController = new IsaAtaController(pDev, nController++);

    bFound = true;

    return pController;
}

static Device *probePiixController(Device *pDev)
{
    EMIT_IF(PEDIGREE_MACHINE_HASPCI)
    {
        static uint8_t interrupt = 14;

        // Create a new AtaController device node.
        Controller *pDevController = new Controller(pDev);
        uintptr_t intnum = pDevController->getInterruptNumber();
        if (intnum == 0)
        {
            // No valid interrupt, handle
            pDevController->setInterruptNumber(interrupt);
            if (interrupt < 15)
                interrupt++;
            else
            {
                ERROR("PCI IDE: Controller found with no IRQ and IRQs 14 and 15 "
                      "are already allocated");
                delete pDevController;

                return pDev;
            }
        }

        PciAtaController *pController =
            new PciAtaController(pDevController, nController++);

        bFound = true;

        return pController;
    }
    else
    {
        return nullptr;
    }
}

/// Removes the ISA ATA controllers added early in boot
static Device *removeIsaAta(Device *dev)
{
    if (dev->getType() == Device::Controller)
    {
        // Get its addresses, and search for "command" and "control".
        bool foundCommand = false;
        bool foundControl = false;
        for (unsigned int j = 0; j < dev->addresses().count(); j++)
        {
            /// \todo Problem with String::operator== - fix.
            if (dev->addresses()[j]->m_Name == "command")
                foundCommand = true;
            if (dev->addresses()[j]->m_Name == "control")
                foundControl = true;
        }

        if (foundCommand && foundControl)
        {
            // Destroy and remove this device.
            return 0;
        }
    }

    return dev;
}

static Device *probeDisk(Device *pDev)
{
    // Check to see if this is an AHCI controller.
    // Class 1 = Mass Storage. Subclass 6 = SATA.
    if ((!allowProbing) &&
        (pDev->getPciClassCode() == 0x01 && pDev->getPciSubclassCode() == 0x06))
    {
        // No AHCI support yet, so just log and keep going.
        WARNING(
            "Found a SATA controller of some sort, hoping for ISA fallback.");
    }

    // Look for a PIIX controller
    // Class/subclasss 1:1 == Mass storage + IDE.
    if (pDev->getPciVendorId() == 0x8086 && pDev->getPciClassCode() == 1 &&
        pDev->getPciSubclassCode() == 1)
    {
        // Ensure we probe the most modern PIIX that is present and available.
        // This is important as there may be a PIIX3 in a system that also has
        // a PIIX4, but the drives are likely to be attached to the PIIX4.
        bool shouldProbe = false;
        switch (pDev->getPciDeviceId())
        {
            case 0x1230:  // PIIX
                if (allowProbing && piixLevel == 0)
                {
                    shouldProbe = true;
                }
                else if (piixLevel < 0)
                {
                    piixLevel = 0;
                }
                break;

            case 0x7010:  // PIIX3
                if (allowProbing && piixLevel == 3)
                {
                    shouldProbe = true;
                }
                else if (piixLevel < 3)
                {
                    piixLevel = 3;
                }
                break;

            case 0x7111:  // PIIX4
                if (allowProbing && piixLevel == 4)
                {
                    shouldProbe = true;
                }
                else if (piixLevel < 4)
                {
                    piixLevel = 4;
                }
                break;
        }

        if (piixLevel != -1)
        {
            bPiixControllerFound = true;
        }

        if (allowProbing && shouldProbe)
        {
            return probePiixController(pDev);
        }
    }

    // No PIIX controller found, fall back to ISA
    /// \todo Could also fall back to ICH?
    if (!bPiixControllerFound && bFallBackISA)
    {
        // Is this a controller?
        if (pDev->getType() == Device::Controller)
        {
            // Check it's not an ATA controller already.
            // Get its addresses, and search for "command" and "control".
            bool foundCommand = false;
            bool foundControl = false;
            for (unsigned int j = 0; j < pDev->addresses().count(); j++)
            {
                /// \todo Problem with String::operator== - fix.
                if (pDev->addresses()[j]->m_Name == "command")
                    foundCommand = true;
                if (pDev->addresses()[j]->m_Name == "control")
                    foundControl = true;
            }
            if (allowProbing && foundCommand && foundControl)
                return probeIsaDevice(static_cast<Controller *>(pDev));
        }
    }

    return pDev;
}

static bool entry()
{
    /// \todo this iterates the device tree up to FOUR times.
    /// Needs some more thinking about how to do this better.

    // Walk the device tree looking for controllers that have
    // "control" and "command" addresses.
    Device::foreach (probeDisk);

    // Done initial probe to find out what exists, action the findings now.
    allowProbing = true;
    if (bPiixControllerFound)
    {
        // Right, we found a PIIX controller. Let's remove the ATA
        // controllers that are created early in the boot (ISA) now
        // so that when we probe the controller we don't run into used
        // ports.
        Device::foreach (removeIsaAta);
        Device::foreach (probeDisk);
    }
    if (!bFound)
    {
        // Try again, allowing ISA devices this time.
        bFallBackISA = true;
        Device::foreach (probeDisk);
    }

    return bFound;
}

static void exit()
{
}

#if PPC_COMMON
MODULE_INFO("ata", &entry, &exit, "scsi", "ata-specific", 0);
#elif X86_COMMON
MODULE_INFO("ata", &entry, &exit, "scsi", "pci", 0);
#endif
