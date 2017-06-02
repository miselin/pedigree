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
#include "modules/system/vfs/VFS.h"

#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/Processor.h"

ConsoleMasterFile::ConsoleMasterFile(
    size_t consoleNumber, String consoleName, Filesystem *pFs)
    : ConsoleFile(consoleNumber, consoleName, pFs), bLocked(false), pLocker(0)
{
}

uint64_t ConsoleMasterFile::read(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    // Check for NL->CRNL conversion which requires special logic.
    size_t slaveFlags = m_pOther->m_Flags;
    if (!(slaveFlags & ConsoleManager::OMapNLToCRNL))
    {
        // Easy read/write - output line discipline will not need to do any
        // conversions that involve expansion.
        uint64_t nBytes =
            m_Buffer.read(reinterpret_cast<char *>(buffer), size, bCanBlock);
        if (!nBytes)
        {
            return 0;
        }

        return outputLineDiscipline(
            reinterpret_cast<char *>(buffer), nBytes, size, m_pOther->m_Flags);
    }

    uint64_t totalBytes = 0;
    while (totalBytes < size)
    {
        // Check for no longer able to read as needed.
        if ((size / 2) == 0)
        {
            break;
        }

        // We assume that the worst-case buffer might be read, which contains
        // 100% newlines that would expand to carriage return + newline.
        // Eventually we'll reach a point where we can't halve size and then
        // we just return what's been read so far (assuming there's still
        // content in the buffer by that stage).
        // Note: the integer division will floor() which is intentional.
        uint64_t nBytes = m_Buffer.read(
            reinterpret_cast<char *>(buffer + totalBytes), size / 2, bCanBlock);
        if (!nBytes)
        {
            break;
        }

        // Perform line discipline using the full, unhalved size so we can
        // expand all available newlines.
        size_t disciplineSize = outputLineDiscipline(
            reinterpret_cast<char *>(buffer + totalBytes), nBytes, size,
            m_pOther->m_Flags);
        totalBytes += disciplineSize;
        size -= disciplineSize;

        // After the first iteration, disallow any further blocking so we read
        // the remainder of the buffer then terminate quickly.
        bCanBlock = false;
    }

    return totalBytes;
}

uint64_t ConsoleMasterFile::write(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    if (!m_pOther->m_Buffer.canWrite(bCanBlock))
    {
        return 0;
    }

    // Pass on to the input discipline, which will write to the slave.
    inputLineDiscipline(reinterpret_cast<char *>(buffer), size);

    return size;
}
