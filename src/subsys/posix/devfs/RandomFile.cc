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

#include "RandomFile.h"

uint64_t RandomFile::read(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    /// \todo Endianness issues?

    size_t realSize = size;

    if(size < sizeof(uint64_t))
    {
        uint64_t val = random_next();
        char *pBuffer = reinterpret_cast<char *>(buffer);
        while(size--)
        {
            *pBuffer++ = val & 0xFF;
            val >>= 8;
        }
    }
    else
    {
        // Align.
        char *pBuffer = reinterpret_cast<char *>(buffer);
        if(size % 8)
        {
            uint64_t align = random_next();
            while(size % 8)
            {
                *pBuffer++ = align & 0xFF;
                --size;
                align >>= 8;
            }
        }

        uint64_t *pBuffer64 = reinterpret_cast<uint64_t *>(buffer);
        while(size)
        {
            *pBuffer64++ = random_next();
            size -= 8;
        }
    }

    return realSize - size;
}

uint64_t RandomFile::write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    return 0;
}
