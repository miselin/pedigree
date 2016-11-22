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
#include <vfs/VFS.h>

ConsolePhysicalFile::ConsolePhysicalFile(File *pTerminal, String consoleName, Filesystem *pFs) :
    ConsoleFile(~0U, consoleName, pFs), m_pTerminal(pTerminal)
{
}

uint64_t ConsolePhysicalFile::read(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    /// \todo input discipline
    return m_pTerminal->read(location, size, buffer, bCanBlock);
}

uint64_t ConsolePhysicalFile::write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    // we allocate a buffer to allow for a input buffer exclusively filled with
    // NL characters to be converted to CRNL
    char *outputBuffer = new char[size * 2];
    StringCopyN(outputBuffer, reinterpret_cast<char *>(buffer), size);
    size_t disciplineSize = outputLineDiscipline(reinterpret_cast<char *>(outputBuffer), size, size * 2, m_Flags);
    uint64_t count = m_pTerminal->write(location, disciplineSize, reinterpret_cast<uintptr_t>(outputBuffer), bCanBlock);
    delete [] outputBuffer;
    return count;
}
