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

#include "pedigree/kernel/processor/state.h"

const char *ARMV7InterruptStateRegisterName[17] = {
    "r0", "r1",  "r2",  "r3",  "r4", "r5", "r6",     "r7",     "r8",
    "r9", "r10", "r11", "r12", "lr", "pc", "usersp", "userlr",
};

uintptr_t ARMV7InterruptState::getStackPointer() const
{
    return m_usersp;
}

void ARMV7InterruptState::setStackPointer(uintptr_t stackPointer)
{
    m_usersp = stackPointer;
}

uintptr_t ARMV7InterruptState::getInstructionPointer() const
{
    return m_pc;
}

void ARMV7InterruptState::setInstructionPointer(uintptr_t instructionPointer)
{
    m_pc = instructionPointer;
}

uintptr_t ARMV7InterruptState::getBasePointer() const
{
    return m_r11;
}

void ARMV7InterruptState::setBasePointer(uintptr_t basePointer)
{
    m_r11 = basePointer;
}

size_t ARMV7InterruptState::getRegisterSize(size_t index) const
{
#if BITS_32
    return 4;
#else
    return 4;  // TODO: handle other bits sizes (this is mainly here)
               // in order to help future development if ARM ends up
               // requiring 64-bit or something
#endif
}

bool ARMV7InterruptState::kernelMode() const
{
    uint32_t cpsr;
    asm volatile("mrs %0, cpsr" : "=r"(cpsr));
    return ((cpsr & 0x1F) != 0x10);
}

size_t ARMV7InterruptState::getInterruptNumber() const
{
    /// \todo implement
    return 0;
}

size_t ARMV7InterruptState::getSyscallService() const
{
    /// \todo implement
    return 0;
}

size_t ARMV7InterruptState::getSyscallNumber() const
{
    /// \todo implement
    return 0;
}

uintptr_t ARMV7InterruptState::getSyscallParameter(size_t n) const
{
    return 0;
}

void ARMV7InterruptState::setSyscallReturnValue(uintptr_t val)
{
}

ARMV7InterruptState::ARMV7InterruptState()
    : m_usersp(), m_userlr(), m_r0(), m_r1(), m_r2(), m_r3(), m_r4(), m_r5(),
      m_r6(), m_r7(), m_r8(), m_r9(), m_r10(), m_r11(), m_r12(), m_lr(), m_pc(),
      m_spsr()
{
}

ARMV7InterruptState::ARMV7InterruptState(const ARMV7InterruptState &is)
    : m_usersp(is.m_usersp), m_userlr(is.m_userlr), m_r0(is.m_r0),
      m_r1(is.m_r1), m_r2(is.m_r2), m_r3(is.m_r3), m_r4(is.m_r4), m_r5(is.m_r5),
      m_r6(is.m_r6), m_r7(is.m_r7), m_r8(is.m_r8), m_r9(is.m_r9),
      m_r10(is.m_r10), m_r11(is.m_r11), m_r12(is.m_r12), m_lr(is.m_lr),
      m_pc(is.m_pc), m_spsr(is.m_spsr)
{
}

ARMV7InterruptState &ARMV7InterruptState::
operator=(const ARMV7InterruptState &is)
{
    m_spsr = is.m_spsr;
    m_r0 = is.m_r0;
    m_r1 = is.m_r1;
    m_r2 = is.m_r2;
    m_r3 = is.m_r3;
    m_r4 = is.m_r4;
    m_r5 = is.m_r5;
    m_r6 = is.m_r6;
    m_r7 = is.m_r7;
    m_r8 = is.m_r8;
    m_r9 = is.m_r9;
    m_r10 = is.m_r10;
    m_r11 = is.m_r11;
    m_r12 = is.m_r12;
    m_lr = is.m_lr;
    m_pc = is.m_pc;
    return *this;
}

size_t ARMV7InterruptState::getRegisterCount() const
{
    return 18;
}
processor_register_t ARMV7InterruptState::getRegister(size_t index) const
{
    switch (index)
    {
        case 0:
            return m_r0;
        case 1:
            return m_r1;
        case 2:
            return m_r2;
        case 3:
            return m_r3;
        case 4:
            return m_r4;
        case 5:
            return m_r5;
        case 6:
            return m_r6;
        case 7:
            return m_r7;
        case 8:
            return m_r8;
        case 9:
            return m_r9;
        case 10:
            return m_r10;
        case 11:
            return m_r11;
        case 12:
            return m_r12;
        case 13:
            return m_lr;
        case 14:
            return m_pc;
        case 15:
            return m_usersp;
        case 16:
            return m_userlr;
        case 17:
            return m_spsr;
        default:
            return 0;
    }
}
const char *ARMV7InterruptState::getRegisterName(size_t index) const
{
    return ARMV7InterruptStateRegisterName[index];
}

ARMV7SyscallState::ARMV7SyscallState(const ARMV7InterruptState &state)
{
    m_spsr = state.m_spsr;
    m_r0 = state.m_r0;
    m_r1 = state.m_r1;
    m_r2 = state.m_r2;
    m_r3 = state.m_r3;
    m_r4 = state.m_r4;
    m_r5 = state.m_r5;
    m_r6 = state.m_r6;
    m_r7 = state.m_r7;
    m_r8 = state.m_r8;
    m_r9 = state.m_r9;
    m_r10 = state.m_r10;
    m_r11 = state.m_r11;
    m_r12 = state.m_r12;
    m_lr = state.m_lr;
    m_pc = state.m_pc;
}

ARMV7ProcessorState::ARMV7ProcessorState(const ARMV7InterruptState &state)
{
    m_spsr = state.m_spsr;
    m_r0 = state.m_r0;
    m_r1 = state.m_r1;
    m_r2 = state.m_r2;
    m_r3 = state.m_r3;
    m_r4 = state.m_r4;
    m_r5 = state.m_r5;
    m_r6 = state.m_r6;
    m_r7 = state.m_r7;
    m_r8 = state.m_r8;
    m_r9 = state.m_r9;
    m_r10 = state.m_r10;
    m_r11 = state.m_r11;
    m_r12 = state.m_r12;
    m_lr = state.m_lr;
    m_pc = state.m_pc;
}
