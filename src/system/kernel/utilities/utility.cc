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

#define IMPLEMENTING_LOG_FORMAT_FUNCTIONS

#include "pedigree/kernel/utilities/utility.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"

const char *SDirectoryName(const char *path, char *buf, size_t buflen)
{
    const char *last_slash = StringReverseFind(path, '/');
    if (last_slash == nullptr)
    {
        return nullptr;
    }

    size_t dirlength = last_slash - path;

    StringCopyN(buf, path, min(buflen, dirlength));

    return buf;
}

const char *SBaseName(const char *path, char *buf, size_t buflen)
{
    size_t len = StringLength(path);

    const char *last_slash = StringReverseFind(path, '/');
    if (last_slash == nullptr)
    {
        StringCopyN(buf, path, buflen);
        return buf;
    }

    if (!last_slash[1])
    {
        // Trailing slash, no basename
        return nullptr;
    }

    size_t baselength = len - (last_slash - path);
    StringCopyN(buf, last_slash + 1, min(buflen, baselength));

    return buf;
}

const char *DirectoryName(const char *path)
{
    size_t len = StringLength(path);
    char *buf = new char[len + 1];
    const char *result = SDirectoryName(path, buf, len);
    if (!result)
    {
        delete[] buf;
    }
    return result;
}

const char *BaseName(const char *path)
{
    size_t len = StringLength(path);
    char *buf = new char[len + 1];
    const char *result = SBaseName(path, buf, len);
    if (!result)
    {
        delete[] buf;
    }
    return result;
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

#define LOG_FORMAT_COMMON            \
    char buf[1024];                  \
    int i = 0;                       \
    va_list ap;                      \
    va_start(ap, fmt);               \
    i = VStringFormat(buf, fmt, ap); \
    va_end(ap);                      \
    if (i && (buf[i - 1] == '\n'))   \
        buf[i - 1] = '\0';

int Debugf(const char *fmt, ...)
{
    LOG_FORMAT_COMMON

    DEBUG_LOG("debugf: " << buf);

    return i;
}

int Noticef(const char *fmt, ...)
{
    LOG_FORMAT_COMMON

    NOTICE("noticef: " << buf);

    return i;
}

int Warningf(const char *fmt, ...)
{
    LOG_FORMAT_COMMON

    WARNING("warningf: " << buf);

    return i;
}

int Errorf(const char *fmt, ...)
{
    LOG_FORMAT_COMMON

    ERROR("errorf: " << buf);

    return i;
}

int Fatalf(const char *fmt, ...)
{
    LOG_FORMAT_COMMON

    FATAL("fatalf: " << buf);
}
