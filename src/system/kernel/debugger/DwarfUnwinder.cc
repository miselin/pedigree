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
void extractDwarfState(const DwarfState *endState, const DwarfState &startState, HostedProcessorState &outState, uint32_t nReturnAddressRegister)
{
    /// \todo
}

template <>
void fillDwarfState(DwarfState &outState, const HostedProcessorState &inState)
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
