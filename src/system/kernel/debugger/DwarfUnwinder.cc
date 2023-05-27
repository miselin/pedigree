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

template <class T>
void fillDwarfState(DwarfState &outState, const T &inState);

template <class T>
void extractDwarfState(const DwarfState *endState, const DwarfState &startState, T &outState, uint32_t nReturnAddressRegister);

template <>
void fillDwarfState(DwarfState &outState, const X64ProcessorState &inState)
{
    outState.m_R[DWARF_REG_RAX] = inState.rax;
    outState.m_R[DWARF_REG_RDX] = inState.rdx;
    outState.m_R[DWARF_REG_RCX] = inState.rcx;
    outState.m_R[DWARF_REG_RBX] = inState.rbx;
    outState.m_R[DWARF_REG_RSI] = inState.rsi;
    outState.m_R[DWARF_REG_RDI] = inState.rdi;
    outState.m_R[DWARF_REG_RBP] = inState.rbp;
    outState.m_R[DWARF_REG_RSP] = inState.rsp;
    outState.m_R[DWARF_REG_R8] = inState.r8;
    outState.m_R[DWARF_REG_R9] = inState.r9;
    outState.m_R[DWARF_REG_R10] = inState.r10;
    outState.m_R[DWARF_REG_R11] = inState.r11;
    outState.m_R[DWARF_REG_R12] = inState.r12;
    outState.m_R[DWARF_REG_R13] = inState.r13;
    outState.m_R[DWARF_REG_R14] = inState.r14;
    outState.m_R[DWARF_REG_R15] = inState.r15;
    outState.m_R[DWARF_REG_RFLAGS] = inState.rflags;
}

template <>
void extractDwarfState(const DwarfState *endState, const DwarfState &startState, X64ProcessorState &outState, uint32_t nReturnAddressRegister)
{
    outState.rax = endState->getRegister(DWARF_REG_RAX, startState);
    outState.rdx = endState->getRegister(DWARF_REG_RDX, startState);
    outState.rcx = endState->getRegister(DWARF_REG_RCX, startState);
    outState.rbx = endState->getRegister(DWARF_REG_RBX, startState);
    outState.rsi = endState->getRegister(DWARF_REG_RSI, startState);
    outState.rdi = endState->getRegister(DWARF_REG_RDI, startState);
    outState.rbp = endState->getRegister(DWARF_REG_RBP, startState);
    outState.rsp = endState->getCfa(startState);  // Architectural rule.
    outState.r8 = endState->getRegister(DWARF_REG_R8, startState);
    outState.r9 = endState->getRegister(DWARF_REG_R9, startState);
    outState.r10 = endState->getRegister(DWARF_REG_R10, startState);
    outState.r11 = endState->getRegister(DWARF_REG_R11, startState);
    outState.r12 = endState->getRegister(DWARF_REG_R12, startState);
    outState.r13 = endState->getRegister(DWARF_REG_R13, startState);
    outState.r14 = endState->getRegister(DWARF_REG_R14, startState);
    outState.r15 = endState->getRegister(DWARF_REG_R15, startState);
    outState.rflags = endState->getRegister(DWARF_REG_RFLAGS, startState);
    outState.rip =
        endState->getRegister(nReturnAddressRegister, startState);
}

template <>
void fillDwarfState(DwarfState &outState, const MIPS32ProcessorState &inState)
{
    outState.m_R[DWARF_REG_AT] = inState.m_At;
    outState.m_R[DWARF_REG_V0] = inState.m_V0;
    outState.m_R[DWARF_REG_V1] = inState.m_V1;
    outState.m_R[DWARF_REG_A0] = inState.m_A0;
    outState.m_R[DWARF_REG_A1] = inState.m_A1;
    outState.m_R[DWARF_REG_A2] = inState.m_A2;
    outState.m_R[DWARF_REG_A3] = inState.m_A3;
    outState.m_R[DWARF_REG_T0] = inState.m_T0;
    outState.m_R[DWARF_REG_T1] = inState.m_T1;
    outState.m_R[DWARF_REG_T2] = inState.m_T2;
    outState.m_R[DWARF_REG_T3] = inState.m_T3;
    outState.m_R[DWARF_REG_T4] = inState.m_T4;
    outState.m_R[DWARF_REG_T5] = inState.m_T5;
    outState.m_R[DWARF_REG_T6] = inState.m_T6;
    outState.m_R[DWARF_REG_T7] = inState.m_T7;
    outState.m_R[DWARF_REG_S0] = inState.m_S0;
    outState.m_R[DWARF_REG_S1] = inState.m_S1;
    outState.m_R[DWARF_REG_S2] = inState.m_S2;
    outState.m_R[DWARF_REG_S3] = inState.m_S3;
    outState.m_R[DWARF_REG_S4] = inState.m_S4;
    outState.m_R[DWARF_REG_S5] = inState.m_S5;
    outState.m_R[DWARF_REG_S6] = inState.m_S6;
    outState.m_R[DWARF_REG_S7] = inState.m_S7;
    outState.m_R[DWARF_REG_T8] = inState.m_T8;
    outState.m_R[DWARF_REG_T9] = inState.m_T9;
    //   outState.m_R[DWARF_REG_K0] = inState.m_K0;
    //   outState.m_R[DWARF_REG_K1] = inState.m_K1;
    outState.m_R[DWARF_REG_GP] = inState.m_Gp;
    outState.m_R[DWARF_REG_SP] = inState.m_Sp;
    outState.m_R[DWARF_REG_FP] = inState.m_Fp;
    outState.m_R[DWARF_REG_RA] = inState.m_Ra;
}

template <>
void extractDwarfState(const DwarfState *endState, const DwarfState &startState, MIPS32ProcessorState &outState, uint32_t nReturnAddressRegister)
{
    outState.m_At = endState->getRegister(DWARF_REG_AT, startState);
    outState.m_V0 = endState->getRegister(DWARF_REG_V0, startState);
    outState.m_V1 = endState->getRegister(DWARF_REG_V1, startState);
    outState.m_A0 = endState->getRegister(DWARF_REG_A0, startState);
    outState.m_A1 = endState->getRegister(DWARF_REG_A1, startState);
    outState.m_A2 = endState->getRegister(DWARF_REG_A2, startState);
    outState.m_A3 = endState->getRegister(DWARF_REG_A3, startState);
    outState.m_T0 = endState->getRegister(DWARF_REG_T0, startState);
    outState.m_T1 = endState->getRegister(DWARF_REG_T1, startState);
    outState.m_T2 = endState->getRegister(DWARF_REG_T2, startState);
    outState.m_T3 = endState->getRegister(DWARF_REG_T3, startState);
    outState.m_T4 = endState->getRegister(DWARF_REG_T4, startState);
    outState.m_T5 = endState->getRegister(DWARF_REG_T5, startState);
    outState.m_T6 = endState->getRegister(DWARF_REG_T6, startState);
    outState.m_T7 = endState->getRegister(DWARF_REG_T7, startState);
    outState.m_S0 = endState->getRegister(DWARF_REG_S0, startState);
    outState.m_S1 = endState->getRegister(DWARF_REG_S1, startState);
    outState.m_S2 = endState->getRegister(DWARF_REG_S2, startState);
    outState.m_S3 = endState->getRegister(DWARF_REG_S3, startState);
    outState.m_S4 = endState->getRegister(DWARF_REG_S4, startState);
    outState.m_S5 = endState->getRegister(DWARF_REG_S5, startState);
    outState.m_S6 = endState->getRegister(DWARF_REG_S6, startState);
    outState.m_S7 = endState->getRegister(DWARF_REG_S7, startState);
    outState.m_T8 = endState->getRegister(DWARF_REG_T8, startState);
    outState.m_T9 = endState->getRegister(DWARF_REG_T9, startState);
    //     outState.m_K0 = endState->getRegister(DWARF_REG_K0, startState);
    //     outState.m_K1 = endState->getRegister(DWARF_REG_K1, startState);
    outState.m_Gp = endState->getRegister(DWARF_REG_GP, startState);
    outState.m_Sp = endState->getCfa(startState);  // Architectural rule.
    outState.m_Fp = endState->getRegister(DWARF_REG_FP, startState);
    outState.m_Ra = endState->getRegister(DWARF_REG_RA, startState);
    outState.m_Epc =
        endState->getRegister(nReturnAddressRegister, startState);
}

template <>
void fillDwarfState(DwarfState &outState, const PPC32ProcessorState &inState)
{
    outState.m_R[DWARF_REG_R0] = inState.m_R0;
    outState.m_R[DWARF_REG_R1] = inState.m_R1;
    outState.m_R[DWARF_REG_R2] = inState.m_R2;
    outState.m_R[DWARF_REG_R3] = inState.m_R3;
    outState.m_R[DWARF_REG_R4] = inState.m_R4;
    outState.m_R[DWARF_REG_R5] = inState.m_R5;
    outState.m_R[DWARF_REG_R6] = inState.m_R6;
    outState.m_R[DWARF_REG_R7] = inState.m_R7;
    outState.m_R[DWARF_REG_R8] = inState.m_R8;
    outState.m_R[DWARF_REG_R9] = inState.m_R9;
    outState.m_R[DWARF_REG_R10] = inState.m_R10;
    outState.m_R[DWARF_REG_R11] = inState.m_R11;
    outState.m_R[DWARF_REG_R12] = inState.m_R12;
    outState.m_R[DWARF_REG_R13] = inState.m_R13;
    outState.m_R[DWARF_REG_R14] = inState.m_R14;
    outState.m_R[DWARF_REG_R15] = inState.m_R15;
    outState.m_R[DWARF_REG_R16] = inState.m_R16;
    outState.m_R[DWARF_REG_R17] = inState.m_R17;
    outState.m_R[DWARF_REG_R18] = inState.m_R18;
    outState.m_R[DWARF_REG_R19] = inState.m_R19;
    outState.m_R[DWARF_REG_R20] = inState.m_R20;
    outState.m_R[DWARF_REG_R21] = inState.m_R21;
    outState.m_R[DWARF_REG_R22] = inState.m_R22;
    outState.m_R[DWARF_REG_R23] = inState.m_R23;
    outState.m_R[DWARF_REG_R24] = inState.m_R24;
    outState.m_R[DWARF_REG_R25] = inState.m_R25;
    outState.m_R[DWARF_REG_R26] = inState.m_R26;
    outState.m_R[DWARF_REG_R27] = inState.m_R27;
    outState.m_R[DWARF_REG_R28] = inState.m_R28;
    outState.m_R[DWARF_REG_R29] = inState.m_R29;
    outState.m_R[DWARF_REG_R30] = inState.m_R30;
    outState.m_R[DWARF_REG_R31] = inState.m_R31;
    outState.m_R[DWARF_REG_CR] = inState.m_Cr;
    outState.m_R[DWARF_REG_LR] = inState.m_Lr;
    // outState.m_R[DWARF_REG_CTR] = inState.m_Ctr;
}

template <>
void extractDwarfState(const DwarfState *endState, const DwarfState &startState, PPC32ProcessorState &outState, uint32_t nReturnAddressRegister)
{
    outState.m_R0 = endState->getRegister(DWARF_REG_R0, startState);
    outState.m_R1 = endState->getCfa(startState);  // Architectural rule.
    outState.m_R2 = endState->getRegister(DWARF_REG_R2, startState);
    outState.m_R3 = endState->getRegister(DWARF_REG_R3, startState);
    outState.m_R4 = endState->getRegister(DWARF_REG_R4, startState);
    outState.m_R5 = endState->getRegister(DWARF_REG_R5, startState);
    outState.m_R6 = endState->getRegister(DWARF_REG_R6, startState);
    outState.m_R7 = endState->getRegister(DWARF_REG_R7, startState);
    outState.m_R8 = endState->getRegister(DWARF_REG_R8, startState);
    outState.m_R9 = endState->getRegister(DWARF_REG_R9, startState);
    outState.m_R10 = endState->getRegister(DWARF_REG_R10, startState);
    outState.m_R11 = endState->getRegister(DWARF_REG_R11, startState);
    outState.m_R12 = endState->getRegister(DWARF_REG_R12, startState);
    outState.m_R13 = endState->getRegister(DWARF_REG_R13, startState);
    outState.m_R14 = endState->getRegister(DWARF_REG_R14, startState);
    outState.m_R15 = endState->getRegister(DWARF_REG_R15, startState);
    outState.m_R16 = endState->getRegister(DWARF_REG_R16, startState);
    outState.m_R17 = endState->getRegister(DWARF_REG_R17, startState);
    outState.m_R18 = endState->getRegister(DWARF_REG_R18, startState);
    outState.m_R19 = endState->getRegister(DWARF_REG_R19, startState);
    outState.m_R20 = endState->getRegister(DWARF_REG_R20, startState);
    outState.m_R21 = endState->getRegister(DWARF_REG_R21, startState);
    outState.m_R22 = endState->getRegister(DWARF_REG_R22, startState);
    outState.m_R23 = endState->getRegister(DWARF_REG_R23, startState);
    outState.m_R24 = endState->getRegister(DWARF_REG_R24, startState);
    outState.m_R25 = endState->getRegister(DWARF_REG_R25, startState);
    outState.m_R26 = endState->getRegister(DWARF_REG_R26, startState);
    outState.m_R27 = endState->getRegister(DWARF_REG_R27, startState);
    outState.m_R28 = endState->getRegister(DWARF_REG_R28, startState);
    outState.m_R29 = endState->getRegister(DWARF_REG_R29, startState);
    outState.m_R30 = endState->getRegister(DWARF_REG_R30, startState);
    outState.m_R31 = endState->getRegister(DWARF_REG_R31, startState);
    outState.m_Cr = endState->getRegister(DWARF_REG_CR, startState);
    outState.m_Lr = endState->getRegister(DWARF_REG_LR, startState);
    //    outState.m_Ctr = endState->getRegister(DWARF_REG_CTR, startState);
    // Ah so. G++ doesn't really support the DWARF standard (AGAIN) it
    // seems, it leaves the return address in LR, and doesn't use the
    // correct numbering. Nice.
    outState.m_Srr0 =
        outState.m_Lr;  // endState->getRegister(nReturnAddressRegister,
                        // startState);
}

template <>
void fillDwarfState(DwarfState &outState, const ARMV7ProcessorState &inState)
{
    /// \todo
}

template <>
void extractDwarfState(const DwarfState *endState, const DwarfState &startState, ARMV7ProcessorState &outState, uint32_t nReturnAddressRegister)
{
    /// \todo
}

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

    fillDwarfState(startState, inState);

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

        extractDwarfState(endState, startState, outState, nReturnAddressRegister);

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
