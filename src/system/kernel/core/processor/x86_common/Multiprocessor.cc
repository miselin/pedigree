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

#include "Multiprocessor.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "../x64/VirtualAddressSpace.h"

#include <machine/mach_pc/Acpi.h>
#include <machine/mach_pc/LocalApic.h>
#include <machine/mach_pc/Pc.h>
#include <machine/mach_pc/Smp.h>

// Don't track these locks - they are never going to be "correct" (they are for
// synchronisation, not for protecting a specific resource).
Spinlock Multiprocessor::m_ProcessorLock1(false, true);
Spinlock Multiprocessor::m_ProcessorLock2(true, true);

extern "C" void mp_trampoline16(void);
extern "C" void mp_trampoline32(void);
extern "C" void *trampolinegdt;
extern "C" void *trampolinegdtr;
extern "C" void *trampolinegdt64;
extern "C" void *trampolinegdtr64;

size_t Multiprocessor::initialise1()
{
    // Did we find a processor list?
    bool bMPInfoFound = false;
    // List of information about each usable processor
    const Vector<ProcessorInformation *> *Processors = 0;

    EMIT_IF(ACPI)
    {
        // Search through the ACPI tables
        Acpi &acpi = Acpi::instance();
        if ((bMPInfoFound = acpi.validProcessorInfo()) == true)
            Processors = &acpi.getProcessorList();
    }

    EMIT_IF(SMP)
    {
        // Search through the SMP tables
        Smp &smp = Smp::instance();
        if (bMPInfoFound == false && (bMPInfoFound = smp.valid()) == true)
            Processors = &smp.getProcessorList();
    }

    // No processor list found
    if (bMPInfoFound == false || !Processors)
    {
        NOTICE("Multiprocessor: couldn't find any information about multiple "
               "processors");
        return 1;
    }

    NOTICE(
        "Multiprocessor: Found " << Dec << Processors->count() << Hex
                                 << " processors");

    // Copy the trampoline code to 0x7000
    /// \note This is a slightly hacky way to have the code linked directly to
    /// the
    ///       kernel - we hard-code specific offsets. Avoids the "relocation
    ///       truncated to fit" error from ld.
    MemoryCopy(
        reinterpret_cast<void *>(0x7000),
        reinterpret_cast<void *>(&mp_trampoline16), 0x100);
    MemoryCopy(
        reinterpret_cast<void *>(0x7100),
        reinterpret_cast<void *>(&mp_trampoline32), 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x7200), &trampolinegdtr64, 0x10);
    MemoryCopy(reinterpret_cast<void *>(0x7210), &trampolinegdt64, 0xF0);

    volatile uintptr_t *trampolineStack;
    volatile uintptr_t *trampolineKernelEntry;

    // Parameters for the trampoline code
    EMIT_IF(X86)
    {
        // dead code path
    }
    else
    {
        trampolineStack =
            reinterpret_cast<volatile uintptr_t *>(0x7FF0);
        trampolineKernelEntry =
            reinterpret_cast<volatile uintptr_t *>(0x7FE8);

        // Set the virtual address space
        *reinterpret_cast<volatile uintptr_t *>(0x7FF8) =
            static_cast<X64VirtualAddressSpace &>(
                VirtualAddressSpace::getKernelAddressSpace())
                .m_PhysicalPML4;
    }

    // Set the entry point
    *trampolineKernelEntry =
        reinterpret_cast<uintptr_t>(&applicationProcessorStartup);

    LocalApic &localApic = Pc::instance().getLocalApic();
    VirtualAddressSpace &kernelSpace =
        VirtualAddressSpace::getKernelAddressSpace();
    // Startup the application processors through startup interprocessor
    // interrupt
    for (size_t i = 0; i < Processors->count(); i++)
    {
        // Add a ProcessorInformation object
        ::ProcessorInformation *pProcessorInfo = 0;

        // Startup the processor
        if (localApic.getId() != (*Processors)[i]->apicId)
        {
            // AP: set up a proper information structure
            pProcessorInfo = new ::ProcessorInformation(
                (*Processors)[i]->processorId, (*Processors)[i]->apicId);
            Processor::m_ProcessorInformation.pushBack(pProcessorInfo);

            // Allocate kernel stack
            VirtualAddressSpace::Stack *pStack = kernelSpace.allocateStack();

            // Set trampoline stack
            *trampolineStack = reinterpret_cast<uintptr_t>(pStack->getTop());

            NOTICE(
                " Booting processor #"
                << Dec << (*Processors)[i]->processorId << ", stack at 0x"
                << Hex << reinterpret_cast<uintptr_t>(pStack->getTop()));

            /// \todo 10 ms delay between INIT IPI and Startup IPI, and we may
            /// need to
            ///       send the Startup IPI twice on some hardware.

            // Acquire the lock
            m_ProcessorLock1.acquire(false);

            localApic.interProcessorInterrupt(
                (*Processors)[i]->apicId, 0x07, LocalApic::deliveryModeInit,
                true, true);
            for (int z = 0; z < 0x10000; z++)
                ;

            // Send the Startup IPI to the processor
            localApic.interProcessorInterrupt(
                (*Processors)[i]->apicId, 0x07, LocalApic::deliveryModeStartup,
                true, false);

            // Wait until the processor is started and has unlocked the lock
            m_ProcessorLock1.acquire(false, false);
            m_ProcessorLock1.release();
        }
        else
        {
            NOTICE(
                "Currently running on CPU #"
                << Dec << localApic.getId() << Hex
                << ", skipping boot (not necessary)");

            Processor::m_ProcessorInformation.pushBack(
                &Processor::m_SafeBspProcessorInformation);
            Processor::m_SafeBspProcessorInformation.setIds(
                (*Processors)[i]->processorId, (*Processors)[i]->apicId);
        }
    }

    return Processors->count();
}

void Multiprocessor::initialise2()
{
    m_ProcessorLock2.release();
}
