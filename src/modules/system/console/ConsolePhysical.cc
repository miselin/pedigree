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
#include "modules/system/vfs/File.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

class Filesystem;

ConsolePhysicalFile::ConsolePhysicalFile(
    size_t nth, File *pTerminal, String consoleName, Filesystem *pFs)
    : ConsoleFile(~0U, consoleName, pFs), m_pTerminal(pTerminal),
      m_ProcessedInput(PTY_BUFFER_SIZE), m_TerminalNumber(nth)
{
}

uint64_t ConsolePhysicalFile::readBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    // read from terminal and perform line discipline as needed
    // we loop because we need to perform line discipline even though a
    // terminal might give us input a byte a time (e.g. cooked mode won't have
    // real input to return until we've done line discipline for every
    // character including the carriage return)
    while (true)
    {
        if (!m_ProcessedInput.canRead(false))
        {
            char *temp = new char[size];
            size_t nRead = m_pTerminal->read(
                location, size, reinterpret_cast<uintptr_t>(temp), bCanBlock);

            if (nRead)
            {
                inputLineDiscipline(temp, nRead, m_Flags, m_ControlChars);
            }
            delete[] temp;
        }

        // handle any bytes that the input discipline created
        while (m_Buffer.canRead(false))
        {
            char *buff = new char[512];
            size_t nTransfer = m_Buffer.read(buff, 512);
            write(0, nTransfer, reinterpret_cast<uintptr_t>(buff), true);
            delete[] buff;
        }

        // and then return the processed content to the caller when ready
        if (m_ProcessedInput.canRead(false))
        {
            return m_ProcessedInput.read(
                reinterpret_cast<char *>(buffer), size, bCanBlock);
        }
        else if (!bCanBlock)
        {
            return 0;
        }
    }
}

uint64_t ConsolePhysicalFile::writeBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    // we allocate a buffer to allow for a input buffer exclusively filled with
    // NL characters to be converted to CRNL
    char *outputBuffer = new char[size * 2];
    ByteSet(outputBuffer, 0, size * 2);
    StringCopyN(outputBuffer, reinterpret_cast<char *>(buffer), size);
    size_t disciplineSize =
        outputLineDiscipline(outputBuffer, size, size * 2, m_Flags);
    /// \todo handle small writes
    /// \todo disciplineSize can be bigger than size due to edits, how do we
    /// manage this instead of lying?
    m_pTerminal->write(
        location, disciplineSize, reinterpret_cast<uintptr_t>(outputBuffer),
        bCanBlock);
    delete[] outputBuffer;
    return size;
}

void ConsolePhysicalFile::performInject(char *buf, size_t len, bool canBlock)
{
    m_ProcessedInput.write(buf, len, canBlock);
    dataChanged();
}

int ConsolePhysicalFile::select(bool bWriting, int timeout)
{
    // if we're writing, we only care about the attached terminal
    if (bWriting)
    {
        return m_pTerminal->select(true, timeout);
    }

    // if we're reading, though, we might be able to return quickly
    if (m_ProcessedInput.canRead(false))
    {
        return 1;
    }

    // or maybe not
    return m_pTerminal->select(false, timeout);
}
