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

#include "pedigree/kernel/utilities/utility.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"

#include <stdio.h>

const char *DirectoryName(const char *path)
{
    const char *last_slash = StringReverseFind(path, '/');
    if (last_slash == nullptr)
    {
        return nullptr;
    }

    size_t dirlength = last_slash - path;

    char *new_str = new char[dirlength + 1];
    StringCopyN(new_str, path, dirlength);
    new_str[dirlength] = 0;

    return new_str;
}

const char *BaseName(const char *path)
{
    size_t len = StringLength(path);

    const char *last_slash = StringReverseFind(path, '/');
    if (last_slash == nullptr)
    {
        return path;
    }

    if (!last_slash[1])
    {
        // Trailing slash, no basename
        return nullptr;
    }

    size_t baselength = len - (last_slash - path);

    char *new_str = new char[baselength];
    StringCopy(new_str, last_slash + 1);
    new_str[baselength] = 0;

    return new_str;
}

uint8_t checksum(const uint8_t *pMemory, size_t sMemory)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < sMemory; i++)
        sum += pMemory[i];
    return (sum == 0);
}

uint16_t checksum16(const uint8_t *pMemory, size_t sMemory)
{
    uint16_t sum1 = 0, sum2 = 0;

    for (size_t i = 0; i < sMemory; ++i)
    {
        sum1 = (sum1 + pMemory[i]) % 255;
        sum2 = (sum2 + sum1) % 255;
    }

    return (sum2 << 8) | sum1;
}

uint32_t checksum32(const uint8_t *pMemory, size_t sMemory)
{
    uint32_t sum1 = 0, sum2 = 0;
    const uint16_t *mem = reinterpret_cast<const uint16_t *>(pMemory);

    for (size_t i = 0; i < sMemory / 2; ++i)
    {
        sum1 = (sum1 + mem[i]) % 65535;
        sum2 = (sum2 + sum1) % 65535;
    }

    return (sum2 << 16) | sum1;
}

uint32_t checksum32_naive(const uint8_t *pMemory, size_t sMemory)
{
    uint32_t sum1 = 0, sum2 = 0;
    const uint16_t *mem = reinterpret_cast<const uint16_t *>(pMemory);

    for (size_t i = 0; i < sMemory / 2; ++i)
    {
        sum1 = (sum1 + mem[i]) % 65535;
        sum2 = (sum2 + sum1) % 65535;
    }

    return (sum2 << 16) | sum1;
}

uint32_t checksumPage(uintptr_t address)
{
    // may be able to be inlined with the knowledge of the constant size
    return checksum32(
        reinterpret_cast<const uint8_t *>(address),
        PhysicalMemoryManager::getPageSize());
}

uint32_t elfHash(const char *buffer, size_t length)
{
    uint32_t h = 0, g = 0;
    for (size_t i = 0; i < length; ++i)
    {
        h = (h << 4) + buffer[i];
        g = h & 0xF0000000;
        h ^= g;
        h ^= g >> 24;
    }

    return h;
}

uint32_t jenkinsHash(const char *buffer, size_t length)
{
    uint32_t h = 0;
    for (size_t i = 0; i < length; ++i)
    {
        h += buffer[i];
        h += h << 10;
        h ^= h >> 6;
    }

    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}
