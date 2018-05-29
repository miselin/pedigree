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

#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/String.h"

Disk::Disk()
{
    m_SpecificType = "Generic Disk";
}

Disk::Disk(Device *p) : Device(p)
{
}

Disk::~Disk()
{
}

Device::Type Disk::getType()
{
    return Device::Disk;
}

Disk::SubType Disk::getSubType()
{
    return ATA;
}

void Disk::getName(String &str)
{
    str = "Generic disk";
}

void Disk::dump(String &str)
{
    str = "Generic disk";
}

uintptr_t Disk::read(uint64_t location)
{
    return ~0;
}

void Disk::write(uint64_t location)
{
}

void Disk::align(uint64_t location)
{
}

size_t Disk::getSize() const
{
    return 0;
}

size_t Disk::getBlockSize() const
{
    return 0;
}

void Disk::pin(uint64_t location)
{
}

void Disk::unpin(uint64_t location)
{
}

bool Disk::cacheIsCritical()
{
    return false;
}

void Disk::flush(uint64_t location)
{
}
