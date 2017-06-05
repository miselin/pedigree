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

extern const char defaultControl[MAX_CONTROL_CHAR];

ConsoleFile::ConsoleFile(
    size_t consoleNumber, String consoleName, Filesystem *pFs)
    : File(consoleName, 0, 0, 0, 0xdeadbeef, pFs, 0, 0), m_pOther(0),
      m_Flags(DEFAULT_FLAGS), m_Rows(25), m_Cols(80), m_LineBuffer(),
      m_LineBufferSize(0), m_LineBufferFirstNewline(~0), m_Last(0),
      m_Buffer(PTY_BUFFER_SIZE), m_ConsoleNumber(consoleNumber),
      m_Name(consoleName), m_pEvent(0), m_EventTrigger(true)
{
    MemoryCopy(m_ControlChars, defaultControl, MAX_CONTROL_CHAR);

    // r/w for all (todo: when a console is locked, it should become owned
    // by the locking user)
    setPermissionsOnly(
        FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
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

size_t ConsoleFile::outputLineDiscipline(
    char *buf, size_t len, size_t maxSz, size_t flags)
{
    // Make sure we always have the latest flags from the slave.
    size_t slaveFlags = flags;

    // Post-process output if enabled.
    if (slaveFlags & (ConsoleManager::OPostProcess))
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
            else if (
                pC[j] == '\n' && (slaveFlags & ConsoleManager::OMapNLToCRNL))
            {
                if (realSize >= maxSz)
                {
                    // We do not have any room to add in the mapped character.
                    // Drop it.
                    WARNING("Console ignored an NL -> CRNL conversion due to a "
                            "full buffer.");
                    tmpBuff[i++] = '\n';
                    continue;
                }

                realSize++;

                char *newBuff = new char[realSize];
                MemoryCopy(newBuff, tmpBuff, i);
                delete[] tmpBuff;
                tmpBuff = newBuff;

                // Add the newline and the caused carriage return
                tmpBuff[i++] = '\r';
                tmpBuff[i++] = '\n';

                continue;
            }

            // ONLRET: NL performs CR function
            if (pC[j] == '\n' && (slaveFlags & ConsoleManager::ONLCausesCR))
            {
                tmpBuff[i++] = '\r';
                continue;
            }

            if (bInsert)
            {
                tmpBuff[i++] = pC[j];
            }
        }

        MemoryCopy(buf, tmpBuff, realSize);
        delete[] tmpBuff;
        len = realSize;
    }

    return len;
}

size_t ConsoleFile::processInput(char *buf, size_t len)
{
    // Perform input processing.
    char *pC = buf;
    size_t realLen = len;
    for (size_t i = 0; i < len; i++)
    {
        if (m_Flags & ConsoleManager::IStripToSevenBits)
            pC[i] = static_cast<uint8_t>(pC[i]) & 0x7F;
        if (m_Flags & ConsoleManager::LCookedMode)
        {
            if (pC[i] == m_ControlChars[VEOF])
            {
                // Zero-length read: EOF.
                realLen = 0;
                break;
            }
        }

        if (pC[i] == '\n' && (m_Flags & ConsoleManager::IMapNLToCR))
            pC[i] = '\r';
        else if (pC[i] == '\r' && (m_Flags & ConsoleManager::IMapCRToNL))
            pC[i] = '\n';
        else if (pC[i] == '\r' && (m_Flags & ConsoleManager::IIgnoreCR))
        {
            MemoryCopy(buf + i, buf + i + 1, len - i - 1);
            i--;  // Need to process this byte again, its contents have changed.
            realLen--;
        }
    }

    return realLen;
}

void ConsoleFile::inputLineDiscipline(
    char *buf, size_t len, size_t flags, const char *controlChars)
{
    // Make sure we always have the latest flags from the slave.
    if (flags == ~0U)
    {
        flags = m_pOther->m_Flags;
    }
    size_t slaveFlags = flags;
    if (controlChars == 0)
    {
        controlChars = m_pOther->m_ControlChars;
    }
    const char *slaveControlChars = controlChars;

    size_t localWritten = 0;

    // Handle temios local modes
    if (slaveFlags & (ConsoleManager::LCookedMode | ConsoleManager::LEcho))
    {
        // Whether or not the application buffer has already been filled
        bool bAppBufferComplete = false;

        // Used for raw mode - just a buffer for erase echo etc
        char *destBuff = new char[len];
        size_t destBuffOffset = 0;

        // Iterate over the buffer
        while (!bAppBufferComplete)
        {
            for (size_t i = 0; i < len; i++)
            {
                // Handle incoming newline
                bool isCanonical = (slaveFlags & ConsoleManager::LCookedMode);
                if (isCanonical && (buf[i] == slaveControlChars[VEOF]))
                {
                    // EOF. Write it and it alone to the slave.
                    performInject(&buf[i], 1, true);
                    return;
                }

                if ((buf[i] == '\r') ||
                    (isCanonical && (buf[i] == slaveControlChars[VEOL])))
                {
                    // LEcho - output the newline. LCookedMode - handle line
                    // buffer.
                    if ((slaveFlags & ConsoleManager::LEcho) ||
                        (slaveFlags & ConsoleManager::LCookedMode))
                    {
                        // Only echo the newline if we are supposed to
                        m_LineBuffer[m_LineBufferSize++] = '\n';
                        if ((slaveFlags & ConsoleManager::LEchoNewline) ||
                            (slaveFlags & ConsoleManager::LEcho))
                        {
                            char buf[] = {'\n', 0};
                            m_Buffer.write(buf, 1);
                            ++localWritten;
                        }

                        if ((slaveFlags & ConsoleManager::LCookedMode) &&
                            !bAppBufferComplete)
                        {
                            // Transmit full buffer to slave.
                            size_t realSize = m_LineBufferSize;
                            if (m_LineBufferFirstNewline < realSize)
                            {
                                realSize = m_LineBufferFirstNewline;
                                m_LineBufferFirstNewline = ~0UL;
                            }

                            performInject(m_LineBuffer, realSize, true);

                            // And now move the buffer over the space we just
                            // consumed
                            uint64_t nConsumedBytes =
                                m_LineBufferSize - realSize;
                            if (nConsumedBytes)  // If zero, the buffer was
                                                 // consumed completely
                                MemoryCopy(
                                    m_LineBuffer, &m_LineBuffer[realSize],
                                    nConsumedBytes);

                            // Reduce the buffer size now
                            m_LineBufferSize -= realSize;

                            // The buffer has been filled!
                            bAppBufferComplete = true;
                        }
                        else if (
                            (slaveFlags & ConsoleManager::LCookedMode) &&
                            (m_LineBufferFirstNewline == ~0UL))
                        {
                            // Application buffer has already been filled, let
                            // future runs know where the limit is
                            m_LineBufferFirstNewline = m_LineBufferSize - 1;
                        }
                        else if (!(slaveFlags & ConsoleManager::LCookedMode))
                        {
                            // Inject this byte into the slave...
                            destBuff[destBuffOffset++] = buf[i];
                        }

                        // Ignore the \n if one is present
                        if (buf[i + 1] == '\n')
                            i++;
                    }
                }
                else if (buf[i] == m_ControlChars[VERASE])
                {
                    if (slaveFlags & (ConsoleManager::LCookedMode |
                                      ConsoleManager::LEchoErase))
                    {
                        if ((slaveFlags & ConsoleManager::LCookedMode) &&
                            m_LineBufferSize)
                        {
                            char ctl[3] = {'\x08', ' ', '\x08'};
                            m_Buffer.write(ctl, 3);
                            m_LineBufferSize--;
                            ++localWritten;
                        }
                        else if (
                            (!(slaveFlags & ConsoleManager::LCookedMode)) &&
                            destBuffOffset)
                        {
                            char ctl[3] = {'\x08', ' ', '\x08'};
                            m_Buffer.write(ctl, 3);
                            destBuffOffset--;
                            ++localWritten;
                        }
                    }
                }
                else
                {
                    // Do we need to handle this character differently?
                    if (checkForEvent(slaveFlags, buf[i], controlChars))
                    {
                        // So, normally we'll be fine to print nicely, but if
                        // we can't write to the ring buffer, we must not try
                        // to do so. This event may be necessary to unblock the
                        // buffer!
                        if (!m_Buffer.canWrite(false))
                        {
                            // Forcefully clear out bytes so we can write what
                            // we need to to the ring buffer.
                            WARNING("Console: dropping bytes to be able to "
                                    "render visual control code (e.g. ^C)");
                            char tmp[3];
                            m_Buffer.read(tmp, 3);
                        }

                        // Write it to the master nicely (eg, ^C, ^D)
                        char ctl_c = '@' + buf[i];
                        char ctl[3] = {'^', ctl_c, '\n'};
                        m_Buffer.write(ctl, 3);
                        ++localWritten;

                        // Trigger the actual event.
                        triggerEvent(buf[i]);
                        continue;
                    }

                    // Write the character to the slave
                    if (slaveFlags & ConsoleManager::LEcho)
                    {
                        m_Buffer.write(&buf[i], 1);
                        ++localWritten;
                    }

                    // Add to the buffer
                    if (slaveFlags & ConsoleManager::LCookedMode)
                        m_LineBuffer[m_LineBufferSize++] = buf[i];
                    else
                    {
                        destBuff[destBuffOffset++] = buf[i];
                    }
                }
            }

            // We appear to have hit the top of the line buffer!
            if (m_LineBufferSize >= LINEBUFFER_MAXIMUM)
            {
                // Our best bet is to return early, giving the application what
                // we can of the line buffer
                size_t numBytesToRemove = m_LineBufferSize;

                // Copy the buffer across
                performInject(m_LineBuffer, numBytesToRemove, true);

                // And now move the buffer over the space we just consumed
                uint64_t nConsumedBytes = m_LineBufferSize - numBytesToRemove;
                if (nConsumedBytes)  // If zero, the buffer was consumed
                                     // completely
                    MemoryCopy(
                        m_LineBuffer, &m_LineBuffer[numBytesToRemove],
                        nConsumedBytes);

                // Reduce the buffer size now
                m_LineBufferSize -= numBytesToRemove;
            }

            /// \todo remove me, this is because of the port
            break;
        }

        if (destBuffOffset)
        {
            performInject(destBuff, len, true);
        }

        delete[] destBuff;
    }
    else
    {
        for (size_t i = 0; i < len; ++i)
        {
            // Do we need to send an event?
            if (checkForEvent(slaveFlags, buf[i], controlChars))
            {
                triggerEvent(buf[i]);
                continue;
            }

            // No event. Simply write the character out.
            performInject(&buf[i], 1, true);
        }
    }

    // Wake up anything waiting on data to read from us.
    if (localWritten)
        dataChanged();
}

bool ConsoleFile::checkForEvent(
    size_t flags, char check, const char *controlChars)
{
    // ISIG?
    if (flags & ConsoleManager::LGenerateEvent)
    {
        if (check &&
            (check == controlChars[VINTR] || check == controlChars[VQUIT] ||
             check == controlChars[VSUSP]))
        {
            return true;
        }
    }
    return false;
}

void ConsoleFile::triggerEvent(char cause)
{
    if (m_pOther->m_pEvent)
    {
        Thread *pThread = Processor::information().getCurrentThread();
        m_Last = cause;
        pThread->sendEvent(m_pOther->m_pEvent);
        Scheduler::instance().yield();

        // Note that we do not release the mutex here.
        while (!m_EventTrigger.acquire())
            ;
    }
}

void ConsoleFile::performInject(char *buf, size_t len, bool canBlock)
{
    m_pOther->inject(buf, len, canBlock);
}

void ConsoleFile::performEventTrigger(char cause)
{
    triggerEvent(cause);
}
