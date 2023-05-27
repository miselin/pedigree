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

#include "modules/system/linker/DynamicLinker.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/linker/SymbolTable.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

extern "C" void resolveSymbol(void);

/// \todo ARMify

void DynamicLinker::initPlt(Elf *pElf, uintptr_t value)
{
    // Value == loadBase. If this changes, add an extra parameter to get
    // loadBase here!
    uint32_t *got =
        reinterpret_cast<uint32_t *>(pElf->getGlobalOffsetTable() + value);
    if (reinterpret_cast<uintptr_t>(got) == value)
    {
        WARNING("DynamicLinker: Global offset table not found!");
        return;
    }

    got++;                      // Go to GOT+4
    *got = value & 0xFFFFFFFF;  // Library ID
    got++;                      // Got to GOT+8

    // Check if the resolve function has been set already...
    if (*got == 0)
    {
        uintptr_t resolveLocation = 0;

        // Grab a page to copy the PLT resolve function to.
        // Start at 0x20000000, looking for the next free page.
        /// \todo Change this to use the size of the elf!
        for (uintptr_t i = 0x40000000; i < 0x50000000;
             i += 0x1000)  /// \todo Page size here.
        {
            bool failed = false;
            if (Processor::information().getVirtualAddressSpace().isMapped(
                    reinterpret_cast<void *>(i)))
            {
                failed = true;
                continue;
            }

            resolveLocation = i;
            break;
        }

        if (resolveLocation == 0)
        {
            ERROR("DynamicLinker: nowhere to put resolve function.");
            return;
        }

        physical_uintptr_t physPage =
            PhysicalMemoryManager::instance().allocatePage();
        bool b = Processor::information().getVirtualAddressSpace().map(
            physPage, reinterpret_cast<void *>(resolveLocation),
            VirtualAddressSpace::Write);

        if (!b)
        {
            ERROR("DynamicLinker: Could not map resolve function.");
        }

        // Memcpy over the resolve function into the user address space.
        // resolveSymbol is an ASM function, defined in ./asm-i686.s
        MemoryCopy(
            reinterpret_cast<uint8_t *>(resolveLocation),
            reinterpret_cast<uint8_t *>(&::resolveSymbol),
            0x1000);  /// \todo Page size here.

        *got = resolveLocation;
    }
}

uintptr_t DynamicLinker::resolvePltSymbol(uintptr_t libraryId, uintptr_t symIdx)
{
    // Find the correct ELF to patch.
    Elf *pElf = 0;
    uintptr_t loadBase = libraryId;

    if (libraryId == 0)
        pElf = m_pProgramElf;
    else
    {
        SharedObject *pSo = m_Objects.lookup(libraryId);
        if (pSo)
            pElf = pSo->elf;
    }

    if (!pElf)
    {
        ERROR(
            "DynamicLinker::resolvePltSymbol: No library found for id `"
            << Hex << libraryId << "'");
        return 0;
    }

    uintptr_t result = pElf->applySpecificRelocation(
        symIdx, m_pProgramElf->getSymbolTable(), loadBase);
    if (result == 0)
        result = pElf->applySpecificRelocation(
            symIdx, m_pProgramElf->getSymbolTable(), loadBase,
            SymbolTable::NotOriginatingElf);

    return result;
}
