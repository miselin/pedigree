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

#include "pedigree/kernel/processor/MemoryRegion.h"

MemoryRegion::MemoryRegion(const char *pName)
    : m_VirtualAddress(0), m_PhysicalAddress(0), m_Size(0), m_pName(pName),
      m_bNonRamMemory(false), m_bForced(false)
{
}

MemoryRegion::~MemoryRegion()
{
    PhysicalMemoryManager::instance().unmapRegion(this);
}

void MemoryRegion::free()
{
    PhysicalMemoryManager::instance().unmapRegion(this);
}

void *MemoryRegion::virtualAddress() const
{
    return m_VirtualAddress;
}

physical_uintptr_t MemoryRegion::physicalAddress() const
{
    return m_PhysicalAddress;
}

size_t MemoryRegion::size() const
{
    return m_Size;
}

const char *MemoryRegion::name() const
{
    return m_pName;
}

MemoryRegion::operator bool() const
{
    return (m_Size != 0);
}

bool MemoryRegion::physicalBoundsCheck(physical_uintptr_t address)
{
    if (address >= m_PhysicalAddress && address < (m_PhysicalAddress + m_Size))
        return true;
    return false;
}

void MemoryRegion::setNonRamMemory(bool b)
{
    m_bNonRamMemory = b;
}

bool MemoryRegion::getNonRamMemory()
{
    return m_bNonRamMemory;
}

void MemoryRegion::setForced(bool b)
{
    m_bForced = b;
}

bool MemoryRegion::getForced()
{
    return m_bForced;
}
