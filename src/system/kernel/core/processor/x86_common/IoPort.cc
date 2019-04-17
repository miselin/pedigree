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

#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/panic.h"

uint8_t IoPort::read8(size_t offset)
{
#if ADDITIONAL_CHECKS
    if (offset >= m_Size)
        panic("8-bit IO read past allocated space.");
#endif

    uint8_t value;
    asm volatile("inb %%dx, %%al" : "=a"(value) : "dN"(m_IoPort + offset));
    return value;
}
uint16_t IoPort::read16(size_t offset)
{
#if ADDITIONAL_CHECKS
    if ((offset + 1) >= m_Size)
        panic("16-bit IO read past allocated space.");
#endif

    uint16_t value;
    asm volatile("inw %%dx, %%ax" : "=a"(value) : "dN"(m_IoPort + offset));
    return value;
}
uint32_t IoPort::read32(size_t offset)
{
#if ADDITIONAL_CHECKS
    if ((offset + 3) >= m_Size)
        panic("32-bit IO read past allocated space.");
#endif

    uint32_t value;
    asm volatile("in %%dx, %%eax" : "=a"(value) : "dN"(m_IoPort + offset));
    return value;
}
#if BITS_64
uint64_t IoPort::read64(size_t offset)
{
    Processor::halt();
    return 0;
}
#endif
void IoPort::write8(uint8_t value, size_t offset)
{
#if ADDITIONAL_CHECKS
    if (offset >= m_Size)
        panic("8-bit IO write past allocated space.");
#endif

    asm volatile("outb %%al, %%dx" ::"dN"(m_IoPort + offset), "a"(value));
}
void IoPort::write16(uint16_t value, size_t offset)
{
#if ADDITIONAL_CHECKS
    if ((offset + 1) >= m_Size)
        panic("16-bit IO write past allocated space.");
#endif

    asm volatile("outw %%ax, %%dx" ::"dN"(m_IoPort + offset), "a"(value));
}
void IoPort::write32(uint32_t value, size_t offset)
{
#if ADDITIONAL_CHECKS
    if (offset >= m_Size)
        panic("32-bit IO write past allocated space.");
#endif

    asm volatile("out %%eax, %%dx" ::"dN"(m_IoPort + offset), "a"(value));
}
#if BITS_64
void IoPort::write64(uint64_t value, size_t offset)
{
    Processor::halt();
}
#endif
