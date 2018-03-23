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

#include "Pc.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#if defined(ACPI)
#include "Acpi.h"
#endif
#if defined(SMP)
#include "Smp.h"
#endif
#if defined(APIC)
#include "Apic.h"
#endif
#include "pedigree/kernel/machine/Bus.h"
#include "pedigree/kernel/machine/Controller.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/machine/Pci.h"

Pc Pc::m_Instance;

void Pc::initialise()
{
    // Initialise Vga
    if (m_Vga.initialise() == false)
        panic("Pc: Vga initialisation failed");

    // Initialise the Real-time Clock / CMOS (without IRQs).
    Rtc &rtc = Rtc::instance();
    if (rtc.initialise1() == false)
        panic("Pc: Rtc initialisation phase 1 failed");

// Initialise ACPI
#if defined(ACPI)
    Acpi &acpi = Acpi::instance();
    acpi.initialise();
#endif

// Initialise SMP
#if defined(SMP)
    Smp &smp = Smp::instance();
    smp.initialise();
#endif

// Check for a local APIC
#if defined(APIC)

    // Physical address of the local APIC
    uint64_t localApicAddress = 0;

    // Get the Local APIC address & I/O APIC list from either the ACPI or the
    // SMP tables
    bool bLocalApicValid = false;
#if defined(ACPI)
    if ((bLocalApicValid = acpi.validApicInfo()) == true)
        localApicAddress = acpi.getLocalApicAddress();
#endif
#if defined(SMP)
    if (bLocalApicValid == false && (bLocalApicValid = smp.valid()) == true)
        localApicAddress = smp.getLocalApicAddress();
#endif

    // Initialise the local APIC, if we have gotten valid data from
    // the ACPI/SMP structures
    if (bLocalApicValid == true && localApicAddress &&
        m_LocalApic.initialise(localApicAddress))
    {
        NOTICE("Local APIC initialised");
    }

#endif

// Check for an I/O APIC
#if defined(APIC)

    // TODO: Check for I/O Apic
    // TODO: Initialise the I/O Apic
    // TODO: IMCR?
    // TODO: Mask the PICs?
    if (false)
    {
    }

    // Fall back to dual 8259 PICs
    else
    {
#endif

        NOTICE("Falling back to dual 8259 PIC Mode");

        // Initialise PIC
        Pic &pic = Pic::instance();
        if (pic.initialise() == false)
            panic("Pc: Pic initialisation failed");

#if defined(APIC)
    }
#endif

    // Initialise serial ports.
    m_pSerial[0].setBase(0x3F8);
    m_pSerial[1].setBase(0x2F8);
    m_pSerial[2].setBase(0x3E8);
    m_pSerial[3].setBase(0x2E8);

    // Initialse the Real-time Clock / CMOS IRQs.
    if (rtc.initialise2() == false)
        panic("Pc: Rtc initialisation phase 2 failed");

    // Initialise the PIT
    Pit &pit = Pit::instance();
    if (pit.initialise() == false)
        panic("Pc: Pit initialisation failed");

    // Set up PS/2
    m_Ps2Controller.initialise();
    m_Keyboard.initialise();

// Find and parse the SMBIOS tables
#if defined(SMBIOS)
    m_SMBios.initialise();
#endif

    m_bInitialised = true;
}

void Pc::deinitialise()
{
    m_bInitialised = false;
}

#if defined(MULTIPROCESSOR)
void Pc::initialiseProcessor()
{
    // TODO: we might need to initialise per-processor ACPI shit, no idea atm

    // Initialise the local APIC
    if (m_LocalApic.initialiseProcessor() == false)
        panic("Pc::initialiseProcessor(): Failed to initialise the local APIC");
}
#endif

void Pc::initialise3()
{
    m_Keyboard.startReaderThread();
}

void Pc::initialiseDeviceTree()
{
    // Firstly add the ISA bus.
    m_IsaBus.setSpecificType(String("isa"));

    // ATA controllers.
    m_AtaMaster.setSpecificType(String("ata"));
    m_AtaMaster.addresses().pushBack(
        new Device::Address(String("command"), 0x1F0, 8, true));
    m_AtaMaster.addresses().pushBack(
        new Device::Address(String("control"), 0x3F0, 8, true));
    m_AtaMaster.setInterruptNumber(14);
    m_IsaBus.addChild(&m_AtaMaster);
    m_AtaMaster.setParent(&m_IsaBus);

    m_AtaSlave.setSpecificType(String("ata"));
    m_AtaSlave.addresses().pushBack(
        new Device::Address(String("command"), 0x170, 8, true));
    m_AtaSlave.addresses().pushBack(
        new Device::Address(String("control"), 0x370, 8, true));
    m_AtaSlave.setInterruptNumber(15);
    m_IsaBus.addChild(&m_AtaSlave);
    m_AtaSlave.setParent(&m_IsaBus);

    // PS/2
    m_Ps2Controller.setSpecificType(String("ps2"));
    m_Ps2Controller.addresses().pushBack(
        new Device::Address(String("ps2-base"), 0x60, 5, true));
    m_Ps2Controller.setInterruptNumber(1);  // 12 for mouse, handled by the driver
    m_IsaBus.addChild(&m_Ps2Controller);
    m_Ps2Controller.setParent(&m_IsaBus);

    // IB700 Watchdog Timer
    m_Watchdog.addresses().pushBack(
        new Device::Address(String("ib700-base"), 0x441, 4, true));
    m_IsaBus.addChild(&m_Watchdog);
    m_Watchdog.setParent(&m_IsaBus);

    Device::addToRoot(&m_IsaBus);

    // Initialise the PCI interface
    PciBus::instance().initialise();
}

Serial *Pc::getSerial(size_t n)
{
    return &m_pSerial[n];
}

size_t Pc::getNumSerial()
{
    return 4;
}

Vga *Pc::getVga(size_t n)
{
    return &m_Vga;
}

size_t Pc::getNumVga()
{
    return 1;
}

IrqManager *Pc::getIrqManager()
{
    return &Pic::instance();
}

SchedulerTimer *Pc::getSchedulerTimer()
{
#ifdef MULTIPROCESSOR
    return &m_LocalApic;
#else
    return &Pit::instance();
#endif
}

Timer *Pc::getTimer()
{
    return &Rtc::instance();
}

Keyboard *Pc::getKeyboard()
{
    return m_pKeyboard;
}

void Pc::setKeyboard(Keyboard *kb)
{
    m_pKeyboard = kb;
}

#ifdef MULTIPROCESSOR
void Pc::stopAllOtherProcessors()
{
    m_LocalApic.interProcessorInterruptAllExcludingThis(
        IPI_HALT_VECTOR, 0 /* Fixed delivery mode */);
}
#endif

Pc::Pc()
    : m_Vga(0x3C0, 0xB8000), m_pKeyboard(0)
#if defined(SMBIOS)
      ,
      m_SMBios()
#endif
#if defined(APIC)
      ,
      m_LocalApic()
#endif
      , m_Keyboard(&m_Ps2Controller), m_IsaBus("ISA"), m_Ps2Controller()
{
    m_pKeyboard = &m_Keyboard;
}
Pc::~Pc()
{
}
