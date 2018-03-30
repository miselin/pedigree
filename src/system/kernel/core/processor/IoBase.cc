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

#include "pedigree/kernel/processor/IoBase.h"

IoBase::IoBase() = default;
IoBase::~IoBase() = default;

uint64_t IoBase::read64LowFirst(size_t offset)
{
    uint64_t low = read32(offset);
    uint64_t high = read32(offset + 4);
    return low | (high << 32);
}

uint64_t IoBase::read64HighFirst(size_t offset)
{
    uint64_t high = read32(offset + 4);
    uint64_t low = read32(offset);
    return low | (high << 32);
}

void IoBase::write64LowFirst(uint64_t value, size_t offset)
{
    write32(value & 0xFFFFFFFF, offset);
    write32(value >> 32, offset + 4);
}

void IoBase::write64HighFirst(uint64_t value, size_t offset)
{
    write32(value >> 32, offset + 4);
    write32(value & 0xFFFFFFFF, offset);
}
