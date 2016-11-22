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

#include <processor/Processor.h>
#include <process/Scheduler.h>

ConsoleMasterFile::ConsoleMasterFile(size_t consoleNumber, String consoleName, Filesystem *pFs) :
    ConsoleFile(consoleNumber, consoleName, pFs), bLocked(false), pLocker(0), m_LineBuffer(), m_LineBufferSize(0),
    m_LineBufferFirstNewline(~0), m_Last(0), m_EventTrigger(true)
{
}

uint64_t ConsoleMasterFile::read(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    // Check for NL->CRNL conversion which requires special logic.
    size_t slaveFlags = m_pOther->m_Flags;
    if (!(slaveFlags & ConsoleManager::OMapNLToCRNL))
    {
        // Easy read/write - output line discipline will not need to do any
        // conversions that involve expansion.
        uint64_t nBytes = m_Buffer.read(reinterpret_cast<char *>(buffer), size, bCanBlock);
        if (!nBytes)
        {
            return 0;
        }

        return outputLineDiscipline(reinterpret_cast<char *>(buffer), nBytes, size, m_pOther->m_Flags);
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
        uint64_t nBytes = m_Buffer.read(reinterpret_cast<char *>(buffer + totalBytes), size / 2, bCanBlock);
        if (!nBytes)
        {
            break;
        }

        // Perform line discipline using the full, unhalved size so we can
        // expand all available newlines.
        size_t disciplineSize = outputLineDiscipline(reinterpret_cast<char *>(buffer + totalBytes), nBytes, size, m_pOther->m_Flags);
        totalBytes += disciplineSize;
        size -= disciplineSize;

        // After the first iteration, disallow any further blocking so we read
        // the remainder of the buffer then terminate quickly.
        bCanBlock = false;
    }

    return totalBytes;
}

uint64_t ConsoleMasterFile::write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    if(!m_pOther->m_Buffer.canWrite(bCanBlock))
    {
        return 0;
    }

    // Pass on to the input discipline, which will write to the slave.
    inputLineDiscipline(reinterpret_cast<char *>(buffer), size);

    return size;
}

void ConsoleMasterFile::inputLineDiscipline(char *buf, size_t len)
{
    // Make sure we always have the latest flags from the slave.
    size_t slaveFlags = m_pOther->m_Flags;
    const char *slaveControlChars = m_pOther->m_ControlChars;

    size_t localWritten = 0;

    // Handle temios local modes
    if(slaveFlags & (ConsoleManager::LCookedMode|ConsoleManager::LEcho))
    {
        // Whether or not the application buffer has already been filled
        bool bAppBufferComplete = false;

        // Used for raw mode - just a buffer for erase echo etc
        char *destBuff = new char[len];
        size_t destBuffOffset = 0;

        // Iterate over the buffer
        while(!bAppBufferComplete)
        {
            for(size_t i = 0; i < len; i++)
            {
                // Handle incoming newline
                bool isCanonical = (slaveFlags & ConsoleManager::LCookedMode);
                if(isCanonical && (buf[i] == slaveControlChars[VEOF]))
                {
                    // EOF. Write it and it alone to the slave.
                    m_pOther->inject(&buf[i], 1, true);
                    return;
                }

                if((buf[i] == '\r') || (isCanonical && (buf[i] == slaveControlChars[VEOL])))
                {
                    // LEcho - output the newline. LCookedMode - handle line buffer.
                    if((slaveFlags & ConsoleManager::LEcho) || (slaveFlags & ConsoleManager::LCookedMode))
                    {
                        // Only echo the newline if we are supposed to
                        m_LineBuffer[m_LineBufferSize++] = '\n';
                        if((slaveFlags & ConsoleManager::LEchoNewline) || (slaveFlags & ConsoleManager::LEcho))
                        {
                            char buf[] = {'\n', 0};
                            m_Buffer.write(buf, 1);
                            ++localWritten;
                        }

                        if((slaveFlags & ConsoleManager::LCookedMode) && !bAppBufferComplete)
                        {
                            // Transmit full buffer to slave.
                            size_t realSize = m_LineBufferSize;
                            if(m_LineBufferFirstNewline < realSize)
                            {
                                realSize = m_LineBufferFirstNewline;
                                m_LineBufferFirstNewline = ~0UL;
                            }

                            m_pOther->inject(m_LineBuffer, realSize, true);

                            // And now move the buffer over the space we just consumed
                            uint64_t nConsumedBytes = m_LineBufferSize - realSize;
                            if(nConsumedBytes) // If zero, the buffer was consumed completely
                                MemoryCopy(m_LineBuffer, &m_LineBuffer[realSize], nConsumedBytes);

                            // Reduce the buffer size now
                            m_LineBufferSize -= realSize;

                            // The buffer has been filled!
                            bAppBufferComplete = true;
                        }
                        else if((slaveFlags & ConsoleManager::LCookedMode) && (m_LineBufferFirstNewline == ~0UL))
                        {
                            // Application buffer has already been filled, let future runs know where the limit is
                            m_LineBufferFirstNewline = m_LineBufferSize - 1;
                        }
                        else if(!(slaveFlags & ConsoleManager::LCookedMode))
                        {
                            // Inject this byte into the slave...
                            destBuff[destBuffOffset++] = buf[i];
                        }

                        // Ignore the \n if one is present
                        if(buf[i+1] == '\n')
                            i++;
                    }
                }
                else if(buf[i] == m_ControlChars[VERASE])
                {
                    if(slaveFlags & (ConsoleManager::LCookedMode|ConsoleManager::LEchoErase))
                    {
                        if((slaveFlags & ConsoleManager::LCookedMode) && m_LineBufferSize)
                        {
                            char ctl[3] = {'\x08', ' ', '\x08'};
                            m_Buffer.write(ctl, 3);
                            m_LineBufferSize--;
                            ++localWritten;
                        }
                        else if((!(slaveFlags & ConsoleManager::LCookedMode)) && destBuffOffset)
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
                    if(checkForEvent(slaveFlags, buf[i]))
                    {
                        // So, normally we'll be fine to print nicely, but if
                        // we can't write to the ring buffer, we must not try
                        // to do so. This event may be necessary to unblock the
                        // buffer!
                        if (!m_Buffer.canWrite(false))
                        {
                            // Forcefully clear out bytes so we can write what
                            // we need to to the ring buffer.
                            WARNING("Console: dropping bytes to be able to render visual control code (e.g. ^C)");
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
                    if(slaveFlags & ConsoleManager::LEcho)
                    {
                        m_Buffer.write(&buf[i], 1);
                        ++localWritten;
                    }

                    // Add to the buffer
                    if(slaveFlags & ConsoleManager::LCookedMode)
                        m_LineBuffer[m_LineBufferSize++] = buf[i];
                    else
                    {
                        destBuff[destBuffOffset++] = buf[i];
                    }
                }
            }

            // We appear to have hit the top of the line buffer!
            if(m_LineBufferSize >= LINEBUFFER_MAXIMUM)
            {
                // Our best bet is to return early, giving the application what we can of the line buffer
                size_t numBytesToRemove = m_LineBufferSize;

                // Copy the buffer across
                m_pOther->inject(m_LineBuffer, numBytesToRemove, true);

                // And now move the buffer over the space we just consumed
                uint64_t nConsumedBytes = m_LineBufferSize - numBytesToRemove;
                if(nConsumedBytes) // If zero, the buffer was consumed completely
                    MemoryCopy(m_LineBuffer, &m_LineBuffer[numBytesToRemove], nConsumedBytes);

                // Reduce the buffer size now
                m_LineBufferSize -= numBytesToRemove;
            }

            /// \todo remove me, this is because of the port
            break;
        }

        if(destBuffOffset)
        {
            m_pOther->inject(destBuff, len, true);
        }

        delete [] destBuff;
    }
    else
    {
        for(size_t i = 0; i < len; ++i)
        {
            // Do we need to send an event?
            if(checkForEvent(slaveFlags, buf[i]))
            {
                triggerEvent(buf[i]);
                continue;
            }

            // No event. Simply write the character out.
            m_pOther->inject(&buf[i], 1, true);
        }
    }

    // Wake up anything waiting on data to read from us.
    if(localWritten)
        dataChanged();
}

bool ConsoleMasterFile::checkForEvent(size_t flags, char check)
{
    const char *slaveControlChars = m_pOther->m_ControlChars;

    // ISIG?
    if(flags & ConsoleManager::LGenerateEvent)
    {
        if(check && (
                    check == slaveControlChars[VINTR] ||
                    check == slaveControlChars[VQUIT] ||
                    check == slaveControlChars[VSUSP]))
        {
            return true;
        }
    }
    return false;
}

void ConsoleMasterFile::triggerEvent(char cause)
{
    if(m_pOther->m_pEvent)
    {
        Thread *pThread = Processor::information().getCurrentThread();
        m_Last = cause;
        pThread->sendEvent(m_pOther->m_pEvent);
        Scheduler::instance().yield();

        // Note that we do not release the mutex here.
        while(!m_EventTrigger.acquire());
    }
}
