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

#include "Console.h"
#include "ConsoleDefines.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/String.h"

class Filesystem;

extern const char defaultControl[MAX_CONTROL_CHAR];

ConsoleSlaveFile::ConsoleSlaveFile(
    size_t consoleNumber, String consoleName, Filesystem *pFs)
    : ConsoleFile(consoleNumber, consoleName, pFs)
{
}

uint64_t ConsoleSlaveFile::readBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    uint64_t nBytes =
        m_Buffer.read(reinterpret_cast<char *>(buffer), size, bCanBlock);
    if (!nBytes)
    {
        return 0;
    }

    size_t endSize = processInput(reinterpret_cast<char *>(buffer), nBytes);

    return endSize;
}

uint64_t ConsoleSlaveFile::writeBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    // Send straight to the master.
    m_pOther->inject(reinterpret_cast<char *>(buffer), size, bCanBlock);

    return size;
}
