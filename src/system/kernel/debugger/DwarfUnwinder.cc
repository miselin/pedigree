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

#include "pedigree/kernel/debugger/DwarfUnwinder.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/debugger/DwarfCfiAutomaton.h"
#include "pedigree/kernel/debugger/DwarfState.h"
#include "pedigree/kernel/processor/state.h"

DwarfUnwinder::DwarfUnwinder(uintptr_t nData, size_t nLength)
    : m_nData(nData), m_nLength(nLength)
{
}

DwarfUnwinder::~DwarfUnwinder()
{
}

bool DwarfUnwinder::unwind(
    const ProcessorState &inState, ProcessorState &outState,
    uintptr_t &frameBase)
{
    // Construct a DwarfState object and populate it.
    DwarfState startState;

    // Unfortunately the next few lines are highly architecture dependent.
    EMIT_IF(X64)
    {
        auto castedInState = reinterpret_cast<const X64ProcessorState *>(&inState);
        startState.m_R[DWARF_REG_RAX] = castedInState->rax;
        startState.m_R[DWARF_REG_RDX] = castedInState->rdx;
        startState.m_R[DWARF_REG_RCX] = castedInState->rcx;
        startState.m_R[DWARF_REG_RBX] = castedInState->rbx;
        startState.m_R[DWARF_REG_RSI] = castedInState->rsi;
        startState.m_R[DWARF_REG_RDI] = castedInState->rdi;
        startState.m_R[DWARF_REG_RBP] = castedInState->rbp;
        startState.m_R[DWARF_REG_RSP] = castedInState->rsp;
        startState.m_R[DWARF_REG_R8] = castedInState->r8;
        startState.m_R[DWARF_REG_R9] = castedInState->r9;
        startState.m_R[DWARF_REG_R10] = castedInState->r10;
        startState.m_R[DWARF_REG_R11] = castedInState->r11;
        startState.m_R[DWARF_REG_R12] = castedInState->r12;
        startState.m_R[DWARF_REG_R13] = castedInState->r13;
        startState.m_R[DWARF_REG_R14] = castedInState->r14;
        startState.m_R[DWARF_REG_R15] = castedInState->r15;
        startState.m_R[DWARF_REG_RFLAGS] = castedInState->rflags;
    }
    EMIT_IF(MIPS_COMMON)
    {
        auto castedInState = reinterpret_cast<const MIPS32ProcessorState *>(&inState);
        startState.m_R[DWARF_REG_AT] = castedInState->m_At;
        startState.m_R[DWARF_REG_V0] = castedInState->m_V0;
        startState.m_R[DWARF_REG_V1] = castedInState->m_V1;
        startState.m_R[DWARF_REG_A0] = castedInState->m_A0;
        startState.m_R[DWARF_REG_A1] = castedInState->m_A1;
        startState.m_R[DWARF_REG_A2] = castedInState->m_A2;
        startState.m_R[DWARF_REG_A3] = castedInState->m_A3;
        startState.m_R[DWARF_REG_T0] = castedInState->m_T0;
        startState.m_R[DWARF_REG_T1] = castedInState->m_T1;
        startState.m_R[DWARF_REG_T2] = castedInState->m_T2;
        startState.m_R[DWARF_REG_T3] = castedInState->m_T3;
        startState.m_R[DWARF_REG_T4] = castedInState->m_T4;
        startState.m_R[DWARF_REG_T5] = castedInState->m_T5;
        startState.m_R[DWARF_REG_T6] = castedInState->m_T6;
        startState.m_R[DWARF_REG_T7] = castedInState->m_T7;
        startState.m_R[DWARF_REG_S0] = castedInState->m_S0;
        startState.m_R[DWARF_REG_S1] = castedInState->m_S1;
        startState.m_R[DWARF_REG_S2] = castedInState->m_S2;
        startState.m_R[DWARF_REG_S3] = castedInState->m_S3;
        startState.m_R[DWARF_REG_S4] = castedInState->m_S4;
        startState.m_R[DWARF_REG_S5] = castedInState->m_S5;
        startState.m_R[DWARF_REG_S6] = castedInState->m_S6;
        startState.m_R[DWARF_REG_S7] = castedInState->m_S7;
        startState.m_R[DWARF_REG_T8] = castedInState->m_T8;
        startState.m_R[DWARF_REG_T9] = castedInState->m_T9;
        //   startState.m_R[DWARF_REG_K0] = castedInState->m_K0;
        //   startState.m_R[DWARF_REG_K1] = castedInState->m_K1;
        startState.m_R[DWARF_REG_GP] = castedInState->m_Gp;
        startState.m_R[DWARF_REG_SP] = castedInState->m_Sp;
        startState.m_R[DWARF_REG_FP] = castedInState->m_Fp;
        startState.m_R[DWARF_REG_RA] = castedInState->m_Ra;
    }
    EMIT_IF(PPC_COMMON)
    {
        auto castedInState = reinterpret_cast<const PPC32ProcessorState *>(&inState);
        startState.m_R[DWARF_REG_R0] = castedInState->m_R0;
        startState.m_R[DWARF_REG_R1] = castedInState->m_R1;
        startState.m_R[DWARF_REG_R2] = castedInState->m_R2;
        startState.m_R[DWARF_REG_R3] = castedInState->m_R3;
        startState.m_R[DWARF_REG_R4] = castedInState->m_R4;
        startState.m_R[DWARF_REG_R5] = castedInState->m_R5;
        startState.m_R[DWARF_REG_R6] = castedInState->m_R6;
        startState.m_R[DWARF_REG_R7] = castedInState->m_R7;
        startState.m_R[DWARF_REG_R8] = castedInState->m_R8;
        startState.m_R[DWARF_REG_R9] = castedInState->m_R9;
        startState.m_R[DWARF_REG_R10] = castedInState->m_R10;
        startState.m_R[DWARF_REG_R11] = castedInState->m_R11;
        startState.m_R[DWARF_REG_R12] = castedInState->m_R12;
        startState.m_R[DWARF_REG_R13] = castedInState->m_R13;
        startState.m_R[DWARF_REG_R14] = castedInState->m_R14;
        startState.m_R[DWARF_REG_R15] = castedInState->m_R15;
        startState.m_R[DWARF_REG_R16] = castedInState->m_R16;
        startState.m_R[DWARF_REG_R17] = castedInState->m_R17;
        startState.m_R[DWARF_REG_R18] = castedInState->m_R18;
        startState.m_R[DWARF_REG_R19] = castedInState->m_R19;
        startState.m_R[DWARF_REG_R20] = castedInState->m_R20;
        startState.m_R[DWARF_REG_R21] = castedInState->m_R21;
        startState.m_R[DWARF_REG_R22] = castedInState->m_R22;
        startState.m_R[DWARF_REG_R23] = castedInState->m_R23;
        startState.m_R[DWARF_REG_R24] = castedInState->m_R24;
        startState.m_R[DWARF_REG_R25] = castedInState->m_R25;
        startState.m_R[DWARF_REG_R26] = castedInState->m_R26;
        startState.m_R[DWARF_REG_R27] = castedInState->m_R27;
        startState.m_R[DWARF_REG_R28] = castedInState->m_R28;
        startState.m_R[DWARF_REG_R29] = castedInState->m_R29;
        startState.m_R[DWARF_REG_R30] = castedInState->m_R30;
        startState.m_R[DWARF_REG_R31] = castedInState->m_R31;
        startState.m_R[DWARF_REG_CR] = castedInState->m_Cr;
        startState.m_R[DWARF_REG_LR] = castedInState->m_Lr;
        // startState.m_R[DWARF_REG_CTR] = castedInState->m_Ctr;
    }

    // For each CIE or FDE...
    size_t nIndex = 0;
    while (nIndex < m_nLength)
    {
        // Get the length of this entry.
        uint32_t nLength = *reinterpret_cast<uint32_t *>(m_nData + nIndex);

        nIndex += sizeof(uint32_t);
        const uint32_t k_nCieId = 0xFFFFFFFF;

        if (nLength == 0xFFFFFFFF)
        {
            ERROR_NOLOCK("64-bit DWARF file detected, but not supported!");
            return false;
        }

        // Get the type of this entry (or CIE pointer if this is a FDE).
        uint32_t nCie = *reinterpret_cast<uint32_t *>(m_nData + nIndex);
        nIndex += sizeof(uint32_t);

        // Is this a CIE?
        if (nCie == k_nCieId)
        {
            // Skip over everything.
            nIndex += nLength - sizeof(processor_register_t);
            continue;
        }

        // This is a FDE. Get its initial location.
        uintptr_t nInitialLocation =
            *reinterpret_cast<uintptr_t *>(m_nData + nIndex);
        nIndex += sizeof(uintptr_t);

        // Get its addressing range.
        size_t nAddressRange = *reinterpret_cast<size_t *>(m_nData + nIndex);
        nIndex += sizeof(size_t);

        uintptr_t nInstructionStart = nIndex;
        size_t nInstructionLength =
            nLength - sizeof(uint32_t) - sizeof(uintptr_t) - sizeof(size_t);

        // Are we in this range?
        if ((inState.getInstructionPointer() < nInitialLocation) ||
            (inState.getInstructionPointer() >=
             nInitialLocation + nAddressRange))
        {
            nIndex += nInstructionLength;
            continue;
        }

        // This is a FDE. Get the CIE it corresponds to.
        uint32_t nCieEnd = *reinterpret_cast<uint32_t *>(m_nData + nCie) + nCie;
        nCie += sizeof(uint32_t);
        nCieEnd += sizeof(uint32_t);

        // Ensure our CIE ID is correct.
        uint32_t nCieId = *reinterpret_cast<uint32_t *>(m_nData + nCie);
        if (nCieId != k_nCieId)
        {
            WARNING_NOLOCK("DwarfUnwinder::unwind - CIE ID incorrect!");
            return false;
        }
        nCie += sizeof(uint32_t);
        nCie += 1;  // Increment over version byte.

        const char *pAugmentationString =
            reinterpret_cast<const char *>(m_nData + nCie);
        while (*pAugmentationString++)  // Pass over the augmentation string,
                                        // waiting for a NULL char.
            nCie++;
        nCie++;  // Step over null byte.

        uint8_t *pData = reinterpret_cast<uint8_t *>(m_nData);
        int32_t nCodeAlignmentFactor = decodeUleb128(pData, nCie);
        int32_t nDataAlignmentFactor = decodeSleb128(pData, nCie);
        uint32_t nReturnAddressRegister = 0;
        EMIT_IF(!HOSTED)
        {
            nReturnAddressRegister = decodeUleb128(pData, nCie);
        }

        DwarfCfiAutomaton automaton;
        automaton.initialise(
            startState, m_nData + nCie, nCieEnd - nCie, nCodeAlignmentFactor,
            nDataAlignmentFactor, nInitialLocation);
        DwarfState *endState = automaton.execute(
            m_nData + nInstructionStart, nInstructionLength,
            inState.getInstructionPointer());
        frameBase = endState->getCfa(startState);

        EMIT_IF(X64)
        {
            auto castedOutState = reinterpret_cast<X64ProcessorState *>(&outState);
            castedOutState->rax = endState->getRegister(DWARF_REG_RAX, startState);
            castedOutState->rdx = endState->getRegister(DWARF_REG_RDX, startState);
            castedOutState->rcx = endState->getRegister(DWARF_REG_RCX, startState);
            castedOutState->rbx = endState->getRegister(DWARF_REG_RBX, startState);
            castedOutState->rsi = endState->getRegister(DWARF_REG_RSI, startState);
            castedOutState->rdi = endState->getRegister(DWARF_REG_RDI, startState);
            castedOutState->rbp = endState->getRegister(DWARF_REG_RBP, startState);
            castedOutState->rsp = endState->getCfa(startState);  // Architectural rule.
            castedOutState->r8 = endState->getRegister(DWARF_REG_R8, startState);
            castedOutState->r9 = endState->getRegister(DWARF_REG_R9, startState);
            castedOutState->r10 = endState->getRegister(DWARF_REG_R10, startState);
            castedOutState->r11 = endState->getRegister(DWARF_REG_R11, startState);
            castedOutState->r12 = endState->getRegister(DWARF_REG_R12, startState);
            castedOutState->r13 = endState->getRegister(DWARF_REG_R13, startState);
            castedOutState->r14 = endState->getRegister(DWARF_REG_R14, startState);
            castedOutState->r15 = endState->getRegister(DWARF_REG_R15, startState);
            castedOutState->rflags = endState->getRegister(DWARF_REG_RFLAGS, startState);
            castedOutState->rip =
                endState->getRegister(nReturnAddressRegister, startState);
        }
        EMIT_IF(MIPS_COMMON)
        {
            auto castedOutState = reinterpret_cast<MIPS32ProcessorState *>(&outState);
            castedOutState->m_At = endState->getRegister(DWARF_REG_AT, startState);
            castedOutState->m_V0 = endState->getRegister(DWARF_REG_V0, startState);
            castedOutState->m_V1 = endState->getRegister(DWARF_REG_V1, startState);
            castedOutState->m_A0 = endState->getRegister(DWARF_REG_A0, startState);
            castedOutState->m_A1 = endState->getRegister(DWARF_REG_A1, startState);
            castedOutState->m_A2 = endState->getRegister(DWARF_REG_A2, startState);
            castedOutState->m_A3 = endState->getRegister(DWARF_REG_A3, startState);
            castedOutState->m_T0 = endState->getRegister(DWARF_REG_T0, startState);
            castedOutState->m_T1 = endState->getRegister(DWARF_REG_T1, startState);
            castedOutState->m_T2 = endState->getRegister(DWARF_REG_T2, startState);
            castedOutState->m_T3 = endState->getRegister(DWARF_REG_T3, startState);
            castedOutState->m_T4 = endState->getRegister(DWARF_REG_T4, startState);
            castedOutState->m_T5 = endState->getRegister(DWARF_REG_T5, startState);
            castedOutState->m_T6 = endState->getRegister(DWARF_REG_T6, startState);
            castedOutState->m_T7 = endState->getRegister(DWARF_REG_T7, startState);
            castedOutState->m_S0 = endState->getRegister(DWARF_REG_S0, startState);
            castedOutState->m_S1 = endState->getRegister(DWARF_REG_S1, startState);
            castedOutState->m_S2 = endState->getRegister(DWARF_REG_S2, startState);
            castedOutState->m_S3 = endState->getRegister(DWARF_REG_S3, startState);
            castedOutState->m_S4 = endState->getRegister(DWARF_REG_S4, startState);
            castedOutState->m_S5 = endState->getRegister(DWARF_REG_S5, startState);
            castedOutState->m_S6 = endState->getRegister(DWARF_REG_S6, startState);
            castedOutState->m_S7 = endState->getRegister(DWARF_REG_S7, startState);
            castedOutState->m_T8 = endState->getRegister(DWARF_REG_T8, startState);
            castedOutState->m_T9 = endState->getRegister(DWARF_REG_T9, startState);
            //     castedOutState->m_K0 = endState->getRegister(DWARF_REG_K0, startState);
            //     castedOutState->m_K1 = endState->getRegister(DWARF_REG_K1, startState);
            castedOutState->m_Gp = endState->getRegister(DWARF_REG_GP, startState);
            castedOutState->m_Sp = endState->getCfa(startState);  // Architectural rule.
            castedOutState->m_Fp = endState->getRegister(DWARF_REG_FP, startState);
            castedOutState->m_Ra = endState->getRegister(DWARF_REG_RA, startState);
            castedOutState->m_Epc =
                endState->getRegister(nReturnAddressRegister, startState);
        }
        EMIT_IF(PPC_COMMON)
        {
            auto castedOutState = reinterpret_cast<PPC32ProcessorState *>(&outState);
            castedOutState->m_R0 = endState->getRegister(DWARF_REG_R0, startState);
            castedOutState->m_R1 = endState->getCfa(startState);  // Architectural rule.
            castedOutState->m_R2 = endState->getRegister(DWARF_REG_R2, startState);
            castedOutState->m_R3 = endState->getRegister(DWARF_REG_R3, startState);
            castedOutState->m_R4 = endState->getRegister(DWARF_REG_R4, startState);
            castedOutState->m_R5 = endState->getRegister(DWARF_REG_R5, startState);
            castedOutState->m_R6 = endState->getRegister(DWARF_REG_R6, startState);
            castedOutState->m_R7 = endState->getRegister(DWARF_REG_R7, startState);
            castedOutState->m_R8 = endState->getRegister(DWARF_REG_R8, startState);
            castedOutState->m_R9 = endState->getRegister(DWARF_REG_R9, startState);
            castedOutState->m_R10 = endState->getRegister(DWARF_REG_R10, startState);
            castedOutState->m_R11 = endState->getRegister(DWARF_REG_R11, startState);
            castedOutState->m_R12 = endState->getRegister(DWARF_REG_R12, startState);
            castedOutState->m_R13 = endState->getRegister(DWARF_REG_R13, startState);
            castedOutState->m_R14 = endState->getRegister(DWARF_REG_R14, startState);
            castedOutState->m_R15 = endState->getRegister(DWARF_REG_R15, startState);
            castedOutState->m_R16 = endState->getRegister(DWARF_REG_R16, startState);
            castedOutState->m_R17 = endState->getRegister(DWARF_REG_R17, startState);
            castedOutState->m_R18 = endState->getRegister(DWARF_REG_R18, startState);
            castedOutState->m_R19 = endState->getRegister(DWARF_REG_R19, startState);
            castedOutState->m_R20 = endState->getRegister(DWARF_REG_R20, startState);
            castedOutState->m_R21 = endState->getRegister(DWARF_REG_R21, startState);
            castedOutState->m_R22 = endState->getRegister(DWARF_REG_R22, startState);
            castedOutState->m_R23 = endState->getRegister(DWARF_REG_R23, startState);
            castedOutState->m_R24 = endState->getRegister(DWARF_REG_R24, startState);
            castedOutState->m_R25 = endState->getRegister(DWARF_REG_R25, startState);
            castedOutState->m_R26 = endState->getRegister(DWARF_REG_R26, startState);
            castedOutState->m_R27 = endState->getRegister(DWARF_REG_R27, startState);
            castedOutState->m_R28 = endState->getRegister(DWARF_REG_R28, startState);
            castedOutState->m_R29 = endState->getRegister(DWARF_REG_R29, startState);
            castedOutState->m_R30 = endState->getRegister(DWARF_REG_R30, startState);
            castedOutState->m_R31 = endState->getRegister(DWARF_REG_R31, startState);
            castedOutState->m_Cr = endState->getRegister(DWARF_REG_CR, startState);
            castedOutState->m_Lr = endState->getRegister(DWARF_REG_LR, startState);
            //    castedOutState->m_Ctr = endState->getRegister(DWARF_REG_CTR, startState);
            // Ah so. G++ doesn't really support the DWARF standard (AGAIN) it
            // seems, it leaves the return address in LR, and doesn't use the
            // correct numbering. Nice.
            castedOutState->m_Srr0 =
                castedOutState->m_Lr;  // endState->getRegister(nReturnAddressRegister,
                                // startState);
        }
        return true;
    }

    return false;
}

uint32_t DwarfUnwinder::decodeUleb128(uint8_t *pBase, uint32_t &nOffset)
{
    uint32_t result = 0;
    uint32_t shift = 0;
    while (true)
    {
        uint8_t byte = pBase[nOffset++];
        result |= (byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
            break;
        shift += 7;
    }
    return result;
}

int32_t DwarfUnwinder::decodeSleb128(uint8_t *pBase, uint32_t &nOffset)
{
    int32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    while (true)
    {
        byte = pBase[nOffset++];
        result |= (byte & 0x7f) << shift;
        shift += 7;
        if ((byte & 0x80) == 0)
            break;
    }
    if ((shift < sizeof(int32_t) * 8) &&
        (byte & 0x40))           /* If sign bit of byte is set */
        result |= -(1 << shift); /* sign extend */
    return result;
}
