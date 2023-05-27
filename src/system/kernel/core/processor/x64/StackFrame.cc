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

#include "pedigree/kernel/processor/StackFrame.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/types.h"
#include <stdarg.h>

#if DEBUGGER

uintptr_t X64StackFrame::getParameter(size_t n)
{
    if (n == 0)
        return m_State.rdi;
    if (n == 1)
        return m_State.rsi;
    if (n == 2)
        return m_State.rdx;
    if (n == 3)
        return m_State.rcx;
    if (n == 4)
        return m_State.r8;
    if (n == 5)
        return m_State.r9;

#if OMIT_FRAMEPOINTER
    uint64_t *pPtr = reinterpret_cast<uint64_t *>(
        m_State.rbp + (n - 6 - 1) * sizeof(uint64_t));
#else
    uint64_t *pPtr =
        reinterpret_cast<uint64_t *>(m_State.rbp + (n - 6) * sizeof(uint64_t));
#endif
    return *pPtr;
}

#endif

void X64StackFrame::construct(
    ProcessorState &state, uintptr_t returnAddress, unsigned int nParams, ...)
{
    // Align the stack to a 16-byte boundary. SysV AMD64 ABI supplement, Sn
    // 3.2.2.
    uintptr_t stack = state.getStackPointer();
    if (stack & 0xF)
    {
        WARNING("StackFrame: given stack was not ABI-compliant, fixing.");
        stack &= ~0xF;
    }

    // Obtain the stack pointer.
    uintptr_t *pStack = reinterpret_cast<uintptr_t *>(stack);

    // How many parameters do we need to push?
    // We push in reverse order but must iterate through the va_list in forward
    // order, so we decrement the stack pointer here.
    ssize_t nToPush = static_cast<ssize_t>(nParams) -
                      6;  // 6 Params can be passed in registers.

    if (nToPush < 0)
        nToPush = 0;
    nToPush++;  // But we always have to push our return address.

    pStack -= nToPush;
    uintptr_t *pStackLowWaterMark = pStack;

    *pStack++ = returnAddress;

    va_list list;
    va_start(list, nParams);

    for (size_t i = 0; i < nParams; i++)
    {
        uintptr_t arg = va_arg(list, uintptr_t);
        switch (i)
        {
            case 0:
                state.rdi = arg;
                break;
            case 1:
                state.rsi = arg;
                break;
            case 2:
                state.rdx = arg;
                break;
            case 3:
                state.rcx = arg;
                break;
            case 4:
                state.r8 = arg;
                break;
            case 5:
                state.r9 = arg;
                break;
            default:
                *pStack++ = arg;
        }
    }

    va_end(list);

    // Write the new stack pointer back.
    state.setStackPointer(reinterpret_cast<uintptr_t>(pStackLowWaterMark));
}
