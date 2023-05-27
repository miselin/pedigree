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

#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/KernelElf.h"

#define VERBOSE_X64_ELF 0

#if VERBOSE_X64_ELF
#define VERBOSE_NOTICE(x) NOTICE(x)
#else
#define VERBOSE_NOTICE(x)
#endif

// http://www.caldera.com/developers/devspecs/abi386-4.pdf

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_GOT32 3
#define R_X86_64_PLT32 4
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_GOTPCREL 9
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_PC64 24
#define R_X86_64_GOTOFF64 25
#define R_X86_64_GOTPC32 26
#define R_X86_64_GOT64 27
#define R_X86_64_GOTPCREL64 28
#define R_X86_64_GOTPC64 29
#define R_X86_64_GOTPLT64 30
#define R_X86_64_PLTOFF64 31

bool Elf::applyRelocation(
    ElfRel_t rel, ElfSectionHeader_t *pSh, SymbolTable *pSymtab,
    uintptr_t loadBase, SymbolTable::Policy policy)
{
    ERROR("The X64 architecture does not use REL entries!");
    return false;
}

bool Elf::applyRelocation(
    ElfRela_t rel, ElfSectionHeader_t *pSh, SymbolTable *pSymtab,
    uintptr_t loadBase, SymbolTable::Policy policy)
{
    // Section not loaded?
    if (pSh && pSh->addr == 0)
    {
        return true;  // Not a fatal error.
    }

    // Avoid NONE relocations.
    if (R_TYPE(rel.info) == R_X86_64_NONE)
    {
        return true;
    }

    if (!loadBase)
    {
        loadBase = pSh ? pSh->addr - pSh->offset : 0;
        if (!loadBase)
        {
            ERROR("Cannot apply relocation, no load base given.");
            return false;
        }
    }

    // Get the address of the unit to be relocated.
    // NOTE: offsets are against the binary start, not the section
    uint64_t address = loadBase + rel.offset;

    // Addend is the value currently at the given address.
    Elf_Sxword A = rel.addend;

    // 'Place' is the address.
    uint64_t P = address;

    // Symbol location.
    uint64_t S = 0;
    ElfSymbol_t *pSymbols = 0;
    if (!m_pDynamicSymbolTable)
    {
        pSymbols = m_pSymbolTable;
    }
    else
    {
        pSymbols = m_pDynamicSymbolTable;
    }

    const char *pStringTable = 0;
    if (!m_pDynamicStringTable)
    {
        pStringTable = reinterpret_cast<const char *>(m_pStringTable);
    }
    else
    {
        pStringTable = m_pDynamicStringTable;
    }

    String symbolName("(unknown)");

    size_t symbolSize = 0;

    // If this is a section header, patch straight to it.
    if (pSymbols && ST_TYPE(pSymbols[R_SYM(rel.info)].info) == 3)
    {
        // Section type - the name will be the name of the section header it
        // refers to.
        int shndx = pSymbols[R_SYM(rel.info)].shndx;
        ElfSectionHeader_t *pReferencedSh = &m_pSectionHeaders[shndx];
        S = pReferencedSh->addr;
        symbolSize = pSymbols[R_SYM(rel.info)].size;
    }
    else if (pSymbols && R_TYPE(rel.info) != R_X86_64_RELATIVE)  // Relative
                                                                 // doesn't need
                                                                 // a symbol!
    {
        const char *pStr = pStringTable + pSymbols[R_SYM(rel.info)].name;

        if (pSymtab == 0)
            pSymtab = &m_SymbolTable;

        if (R_TYPE(rel.info) == R_X86_64_COPY)
            policy = SymbolTable::NotOriginatingElf;
        S = pSymtab->lookup(String(pStr), this, policy);
        if (S == 0)
        {
            // Failed to find - fall back to kernel symbol table.
            S = KernelElf::instance().getSymbolTable()->lookup(
                String(pStr), this, policy);
        }

        if (S == 0 && ST_BIND(pSymbols[R_SYM(rel.info)].info) == 2)
        {
            // Weak relocation that couldn't be found, which is OK.
            S = ~0UL;
        }

        if (S == 0)
        {
            WARNING(
                "Relocation failed for symbol \""
                << pStr << "\" (relocation=" << R_TYPE(rel.info) << ")");
            WARNING(
                "Relocation at " << Hex << address << " (offset=" << rel.offset
                                 << ")...");
        }

        symbolName.assign(pStr);
        symbolSize = pSymbols[R_SYM(rel.info)].size;
    }

    if (S == 0 && (R_TYPE(rel.info) != R_X86_64_RELATIVE))
        return false;
    if (S == ~0UL)
        S = 0;  // weak relocation, undefined

    // Base address
    uint64_t B = loadBase;

    uint64_t *pResult = reinterpret_cast<uint64_t *>(address);
    uint64_t result = *pResult;

    uint64_t tmp = 0;
    uint8_t r_type = R_TYPE(rel.info);

    VERBOSE_NOTICE("");
    VERBOSE_NOTICE("Relocation for " << symbolName);
    VERBOSE_NOTICE("A=" << Hex << A << " B=" << B << " S=" << S << " P=" << P);

    switch (r_type)
    {
        case R_X86_64_NONE:
            VERBOSE_NOTICE("R_X86_64_NONE");
            break;
        case R_X86_64_64:
            VERBOSE_NOTICE("R_X86_64_64");
            result = S + A;
            break;
        case R_X86_64_PC32:
            VERBOSE_NOTICE("R_X86_64_PC32");
            result = (result & 0xFFFFFFFF00000000) | ((S + A - P) & 0xFFFFFFFF);
            break;
        case R_X86_64_COPY:
            VERBOSE_NOTICE("R_X86_64_COPY");
            if (!S)
            {
                ERROR("Cannot perform a R_X86_64_COPY relocation for a weak "
                      "symbol.");
                return false;
            }

            NOTICE("Copy needed, " << symbolSize << " bytes wanted");
            result = *reinterpret_cast<uintptr_t *>(S);
            break;
        case R_X86_64_JUMP_SLOT:
        case R_X86_64_GLOB_DAT:
            VERBOSE_NOTICE("R_X86_64_JUMP_SLOT/R_X86_64_GLOB_DAT");

            result = S;
            break;
        case R_X86_64_RELATIVE:
            VERBOSE_NOTICE("R_X86_64_RELATIVE");
            result = B + A;
            break;
        case R_X86_64_32:
        case R_X86_64_32S:
            VERBOSE_NOTICE("R_X86_64_32(S)");
            tmp = S + A;

            if ((r_type == R_X86_64_32) && ((tmp & 0xFFFFFFFF00000000ULL) != 0))
            {
                ERROR(
                    "Relocation for symbol '" << symbolName
                                              << "' will be truncated to fit!");
            }
            else if (r_type == R_X86_64_32S)
            {
                // did this sign extend?
                uint64_t sign = (tmp & 0x80000000ULL) >> 31ULL;
                uint64_t top = (tmp & 0xFFFFFFFF00000000ULL) >> 32ULL;
                if ((sign * 0xFFFFFFFFUL) != top)
                {
                    ERROR(
                        "Relocation for symbol '"
                        << symbolName
                        << "' will be truncated to fit (sign-extension was "
                           "incorrect)");
                }
            }

            result = (result & 0xFFFFFFFF00000000) | (tmp & 0xFFFFFFFFUL);
            break;
        default:
            ERROR(
                "Relocation not supported for symbol \""
                << symbolName << "\": " << Dec << R_TYPE(rel.info));
    }

    VERBOSE_NOTICE("result=" << Hex << result);
    VERBOSE_NOTICE("");

    // Write back the result.
    *pResult = result;
    return true;
}
