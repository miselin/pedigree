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

#include "pedigree/kernel/processor/MemoryMappedIo.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/utility.h"

MemoryMappedIo::MemoryMappedIo(
    const char *pName, uintptr_t offset, uintptr_t padding)
    : IoBase(), MemoryRegion(pName), m_Offset(offset), m_Padding(padding)
{
}

MemoryMappedIo::~MemoryMappedIo()
{
}

size_t MemoryMappedIo::size() const
{
    return MemoryRegion::size();
}

uint8_t MemoryMappedIo::read8(size_t offset)
{
#if ADDITIONAL_CHECKS
    if (offset >= size())
        Processor::halt();
#endif

    return *reinterpret_cast<volatile uint8_t *>(
        adjust_pointer(virtualAddress(), (offset * m_Padding) + m_Offset));
}

uint16_t MemoryMappedIo::read16(size_t offset)
{
#if ADDITIONAL_CHECKS
    if ((offset + 1) >= size())
        Processor::halt();
#endif

    return *reinterpret_cast<volatile uint16_t *>(
        adjust_pointer(virtualAddress(), (offset * m_Padding) + m_Offset));
}

uint32_t MemoryMappedIo::read32(size_t offset)
{
#if ADDITIONAL_CHECKS
    if ((offset + 3) >= size())
        Processor::halt();
#endif

    return *reinterpret_cast<volatile uint32_t *>(
        adjust_pointer(virtualAddress(), (offset * m_Padding) + m_Offset));
}

uint64_t MemoryMappedIo::read64(size_t offset)
{
#if ADDITIONAL_CHECKS
    if ((offset + 7) >= size())
        Processor::halt();
#endif

    return *reinterpret_cast<volatile uint64_t *>(
        adjust_pointer(virtualAddress(), (offset * m_Padding) + m_Offset));
}

void MemoryMappedIo::write8(uint8_t value, size_t offset)
{
#if ADDITIONAL_CHECKS
    if (offset >= size())
        Processor::halt();
#endif

    *reinterpret_cast<volatile uint8_t *>(adjust_pointer(
        virtualAddress(), (offset * m_Padding) + m_Offset)) = value;
}

void MemoryMappedIo::write16(uint16_t value, size_t offset)
{
#if ADDITIONAL_CHECKS
    if ((offset + 1) >= size())
        Processor::halt();
#endif

    *reinterpret_cast<volatile uint16_t *>(adjust_pointer(
        virtualAddress(), (offset * m_Padding) + m_Offset)) = value;
}

void MemoryMappedIo::write32(uint32_t value, size_t offset)
{
#if ADDITIONAL_CHECKS
    if ((offset + 3) >= size())
        Processor::halt();
#endif

    *reinterpret_cast<volatile uint32_t *>(adjust_pointer(
        virtualAddress(), (offset * m_Padding) + m_Offset)) = value;
}

void MemoryMappedIo::write64(uint64_t value, size_t offset)
{
#if ADDITIONAL_CHECKS
    if ((offset + 7) >= size())
        Processor::halt();
#endif

    *reinterpret_cast<volatile uint64_t *>(adjust_pointer(
        virtualAddress(), (offset * m_Padding) + m_Offset)) = value;
}

MemoryMappedIo::operator bool() const
{
    return MemoryRegion::operator bool();
}
