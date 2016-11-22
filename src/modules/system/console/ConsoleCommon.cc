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

extern const char defaultControl[MAX_CONTROL_CHAR];

ConsoleFile::ConsoleFile(size_t consoleNumber, String consoleName, Filesystem *pFs) :
    File(consoleName, 0, 0, 0, 0xdeadbeef, pFs, 0, 0),
    m_Flags(DEFAULT_FLAGS), m_Rows(25), m_Cols(80),
    m_Buffer(PTY_BUFFER_SIZE), m_ConsoleNumber(consoleNumber),
    m_Name(consoleName), m_pEvent(0)
{
    MemoryCopy(m_ControlChars, defaultControl, MAX_CONTROL_CHAR);

    // r/w for all (todo: when a console is locked, it should become owned
    // by the locking user)
    setPermissionsOnly(FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
    setUidOnly(0);
    setGidOnly(0);
}

int ConsoleFile::select(bool bWriting, int timeout)
{
    if (bWriting)
    {
        return m_Buffer.canWrite(timeout > 0) ? 1 : 0;
    }
    else
    {
        return m_Buffer.canRead(timeout > 0) ? 1 : 0;
    }
}

void ConsoleFile::inject(char *buf, size_t len, bool canBlock)
{
    m_Buffer.write(buf, len, canBlock);
    dataChanged();
}

size_t ConsoleFile::outputLineDiscipline(char *buf, size_t len, size_t maxSz, size_t flags)
{
    // Make sure we always have the latest flags from the slave.
    size_t slaveFlags = flags;

    // Post-process output if enabled.
    if(slaveFlags & (ConsoleManager::OPostProcess))
    {
        char *tmpBuff = new char[len];
        size_t realSize = len;

        char *pC = buf;
        for (size_t i = 0, j = 0; j < len; j++)
        {
            bool bInsert = true;

            // OCRNL: Map CR to NL on output
            if (pC[j] == '\r' && (slaveFlags & ConsoleManager::OMapCRToNL))
            {
                tmpBuff[i++] = '\n';
                continue;
            }

            // ONLCR: Map NL to CR-NL on output
            else if (pC[j] == '\n' && (slaveFlags & ConsoleManager::OMapNLToCRNL))
            {
                if (realSize >= maxSz)
                {
                    // We do not have any room to add in the mapped character. Drop it.
                    WARNING("Console ignored an NL -> CRNL conversion due to a full buffer.");
                    tmpBuff[i++] = '\n';
                    continue;
                }

                realSize++;

                char *newBuff = new char[realSize];
                MemoryCopy(newBuff, tmpBuff, i);
                delete [] tmpBuff;
                tmpBuff = newBuff;

                // Add the newline and the caused carriage return
                tmpBuff[i++] = '\r';
                tmpBuff[i++] = '\n';

                continue;
            }

            // ONLRET: NL performs CR function
            if(pC[j] == '\n' && (slaveFlags & ConsoleManager::ONLCausesCR))
            {
                tmpBuff[i++] = '\r';
                continue;
            }

            if(bInsert)
            {
                tmpBuff[i++] = pC[j];
            }
        }

        MemoryCopy(buf, tmpBuff, realSize);
        delete [] tmpBuff;
        len = realSize;
    }

    return len;
}
