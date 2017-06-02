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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/utility.h"

#ifdef THREADS
#include "pedigree/kernel/process/Thread.h"
#endif

template <class T, bool allowShortOperation>
Buffer<T, allowShortOperation>::Buffer(size_t bufferSize)
    : m_BufferSize(bufferSize), m_DataSize(0), m_Lock(false),
      m_WriteCondition(), m_ReadCondition(), m_Segments(), m_MonitorTargets(),
      m_bCanRead(true), m_bCanWrite(true)
{
}

template <class T, bool allowShortOperation>
Buffer<T, allowShortOperation>::~Buffer()
{
    // Wake up all readers and writers to finish up existing operations.
    disableReads();
    disableWrites();

    // Clean up the entire buffer.
    wipe();

    // Clean up monitor targets.
    m_Lock.acquire();
    for (auto pTarget : m_MonitorTargets)
    {
        delete pTarget;
    }
    m_MonitorTargets.clear();
    m_Lock.release();
}

template <class T, bool allowShortOperation>
size_t
Buffer<T, allowShortOperation>::write(const T *buffer, size_t count, bool block)
{
    m_Lock.acquire();

    size_t countSoFar = 0;
    while (true)
    {
        // Can we write?
        if (!m_bCanWrite)
        {
            // No! Maybe not anymore, so return what we've written so far.
            break;
        }

        // Do we have space?
        size_t bytesAvailable = m_BufferSize - m_DataSize;
        if (!bytesAvailable)
        {
            if (!block)
            {
                // Cannot block!
                break;
            }

            // Can any reader get us out of this situation?
            if (!m_bCanRead)
            {
                // No. Return what we've written so far.
                break;
            }

            // No, we need to wait.
            while (!m_WriteCondition.wait(m_Lock))
                ;
            continue;
        }

        // Yes, we have room.
        size_t totalCount = bytesAvailable;
        if (totalCount > count)
        {
            totalCount = count;
        }

        // If we're allowed to just give up if we don't have enough bytes, do
        // so (e.g. for TCP buffers and the like).
        if (allowShortOperation && (totalCount > bytesAvailable))
        {
            totalCount = bytesAvailable;
            count = bytesAvailable;
            if (!totalCount)
            {
                break;
            }
        }

        // Add the segment.
        bool copiedData = false;
        while (totalCount)
        {
            size_t numberCopied = 0;
            if (totalCount >= m_SegmentSize)
            {
                // Ignore any existing segments, just create a new one.
                addSegment(buffer, m_SegmentSize);
                numberCopied = m_SegmentSize;
            }
            else if (!m_Segments.count())
            {
                // Just add this segment.
                addSegment(buffer, totalCount);
                numberCopied = totalCount;
            }
            else
            {
                // Can we modify the most recent segment?
                Segment *pSegment = m_Segments.popBack();
                if (pSegment->size == m_SegmentSize)
                {
                    m_Segments.pushBack(pSegment);

                    // Full already, need a new segment.
                    addSegment(buffer, totalCount);
                    numberCopied = totalCount;
                }
                else
                {
                    // There's room in this segment.
                    T *start = &pSegment->data[pSegment->size];
                    size_t availableSpace = m_SegmentSize - pSegment->size;
                    if (availableSpace > totalCount)
                    {
                        availableSpace = totalCount;
                    }
                    pedigree_std::copy(start, buffer, availableSpace);

                    // We just added more bytes to this segment.
                    pSegment->size += availableSpace;

                    // Done with this segment, so push it back for reading.
                    m_Segments.pushBack(pSegment);

                    // Was that enough?
                    if (availableSpace < totalCount)
                    {
                        // No, need one more segment.
                        addSegment(
                            &buffer[availableSpace],
                            totalCount - availableSpace);
                    }

                    // Done.
                    numberCopied = totalCount;
                }
            }

            countSoFar += numberCopied;
            m_DataSize += numberCopied;
            buffer += numberCopied;
            totalCount -= numberCopied;
            count -= numberCopied;

            if (!copiedData)
            {
                copiedData = numberCopied > 0;
            }
        }

        // Wake up a reader that was waiting for data.
        // We do this here rather than after we finish as we may need to block
        // again (e.g. if we're writing more than the size of the buffer), and
        // a reader is needed to unblock that.
        if (copiedData)
        {
            m_ReadCondition.signal();
        }

        if (!count)
        {
            // Complete.
            break;
        }
    }

    m_Lock.release();

    if (countSoFar)
    {
        // We've updated the buffer, so send events.
        notifyMonitors();
    }

    return countSoFar;
}

template <class T, bool allowShortOperation>
size_t Buffer<T, allowShortOperation>::read(T *buffer, size_t count, bool block)
{
    m_Lock.acquire();

    size_t countSoFar = 0;
    while (true)
    {
        // Can we read?
        if (!m_bCanRead)
        {
            // No! Maybe not anymore, so return what we've read so far.
            break;
        }

        // Do we have anything to read?
        if (!m_DataSize)
        {
            if (!block)
            {
                // Cannot block!
                break;
            }

            // Can any writer get us out of this situation?
            if (!m_bCanWrite)
            {
                // No. Return what we've read so far.
                break;
            }

            // No, we need to wait.
            while (!m_ReadCondition.wait(m_Lock))
                ;
            continue;
        }

        // Yes, we have room.
        size_t totalCount = count;
        if (totalCount > m_DataSize)
        {
            totalCount = m_DataSize;
        }

        size_t numberCopied = 0;
        while (m_Segments.count() && numberCopied < totalCount)
        {
            // Grab the first segment and read it.
            Segment *pSegment = m_Segments.popFront();
            size_t countToRead = pSegment->size - pSegment->reader;
            if ((numberCopied + countToRead) > totalCount)
            {
                countToRead = totalCount - numberCopied;
            }

            // Copy.
            pedigree_std::copy(
                buffer, &pSegment->data[pSegment->reader], countToRead);
            pSegment->reader += countToRead;

            // Do we need to re-add it?
            if (pSegment->reader < pSegment->size)
            {
                m_Segments.pushFront(pSegment);
            }
            else
            {
                delete pSegment;
            }

            numberCopied += countToRead;
            buffer += countToRead;
        }

        m_DataSize -= numberCopied;
        countSoFar += numberCopied;
        count -= numberCopied;

        // We read some bytes so writers may be able to continue, which may be
        // needed to unblock us if we loop back around and block.
        if (numberCopied)
        {
            // Wake up a writer that was waiting for space to write.
            m_WriteCondition.signal();
        }

        if (!count)
        {
            break;
        }

        // Once we've read at least some bytes, don't block - just return what
        // we've read so far if we loop back around and have no data.
        block = false;
    }

    m_Lock.release();

    if (countSoFar)
    {
        // We've updated the buffer, so send events.
        notifyMonitors();
    }

    return countSoFar;
}

template <class T, bool allowShortOperation>
void Buffer<T, allowShortOperation>::disableWrites()
{
    LockGuard<Mutex> guard(m_Lock);
    m_bCanWrite = false;

    // All pending readers need to now return.
    m_ReadCondition.broadcast();
}

template <class T, bool allowShortOperation>
void Buffer<T, allowShortOperation>::disableReads()
{
    LockGuard<Mutex> guard(m_Lock);
    m_bCanRead = false;

    // All pending writers need to now return.
    m_WriteCondition.broadcast();
}

template <class T, bool allowShortOperation>
bool Buffer<T, allowShortOperation>::enableWrites()
{
    LockGuard<Mutex> guard(m_Lock);
    bool previous = m_bCanWrite;
    m_bCanWrite = true;
    return previous;
}

template <class T, bool allowShortOperation>
bool Buffer<T, allowShortOperation>::enableReads()
{
    LockGuard<Mutex> guard(m_Lock);
    bool previous = m_bCanRead;
    m_bCanRead = true;
    return previous;
}

template <class T, bool allowShortOperation>
size_t Buffer<T, allowShortOperation>::getDataSize()
{
    LockGuard<Mutex> guard(m_Lock);
    return m_DataSize;
}

template <class T, bool allowShortOperation>
size_t Buffer<T, allowShortOperation>::getSize()
{
    return m_BufferSize;
}

template <class T, bool allowShortOperation>
bool Buffer<T, allowShortOperation>::canWrite(bool block)
{
    if (!block)
    {
        return m_bCanWrite && (m_DataSize < m_BufferSize);
    }

    LockGuard<Mutex> guard(m_Lock);

    if (!m_bCanWrite)
    {
        return false;
    }

    // We can get woken here if we stop being able to write.
    while (m_bCanWrite && m_DataSize >= m_BufferSize)
    {
        while (!m_WriteCondition.wait(m_Lock))
            ;
    }

    return m_bCanWrite;
}

template <class T, bool allowShortOperation>
bool Buffer<T, allowShortOperation>::canRead(bool block)
{
    if (!block)
    {
        return m_bCanRead && (m_DataSize > 0);
    }

    LockGuard<Mutex> guard(m_Lock);

    if (!m_bCanRead)
    {
        return false;
    }

    // We can get woken here if we stop being able to read.
    while (m_bCanRead && !m_DataSize)
    {
        while (!m_ReadCondition.wait(m_Lock))
            ;
    }

    return m_bCanRead;
}

template <class T, bool allowShortOperation>
void Buffer<T, allowShortOperation>::wipe()
{
    LockGuard<Mutex> guard(m_Lock);

    // Wipe out every segment we own.
    for (auto pSegment : m_Segments)
    {
        delete pSegment;
    }
    m_Segments.clear();
    m_DataSize = 0;

    // Notify writers that might have been waiting for space.
    m_WriteCondition.signal();
}

template <class T, bool allowShortOperation>
void Buffer<T, allowShortOperation>::monitor(Thread *pThread, Event *pEvent)
{
#ifdef THREADS
    LockGuard<Mutex> guard(m_Lock);
    MonitorTarget *pTarget = new MonitorTarget(pThread, pEvent);
    m_MonitorTargets.pushBack(pTarget);
#endif
}

template <class T, bool allowShortOperation>
void Buffer<T, allowShortOperation>::cullMonitorTargets(Thread *pThread)
{
#ifdef THREADS
    LockGuard<Mutex> guard(m_Lock);
    for (auto it = m_MonitorTargets.begin(); it != m_MonitorTargets.end(); ++it)
    {
        MonitorTarget *pMT = *it;

        if (pMT->pThread == pThread)
        {
            delete pMT;
            m_MonitorTargets.erase(it);
            it = m_MonitorTargets.begin();
            if (it == m_MonitorTargets.end())
                return;
        }
    }
#endif
}

template <class T, bool allowShortOperation>
void Buffer<T, allowShortOperation>::notifyMonitors()
{
#ifdef THREADS
    LockGuard<Mutex> guard(m_Lock);
    for (typename List<MonitorTarget *>::Iterator it = m_MonitorTargets.begin();
         it != m_MonitorTargets.end(); it++)
    {
        MonitorTarget *pMT = *it;

        pMT->pThread->sendEvent(pMT->pEvent);
        delete pMT;
    }
    m_MonitorTargets.clear();
#endif
}

template <class T, bool allowShortOperation>
void Buffer<T, allowShortOperation>::addSegment(const T *buffer, size_t count)
{
    // Called with lock taken.
    Segment *pNewSegment = new Segment();
    pedigree_std::copy(pNewSegment->data, buffer, count);
    pNewSegment->size = count;
    m_Segments.pushBack(pNewSegment);
}

template class Buffer<uint8_t, false>;
template class Buffer<uint8_t, true>;
template class Buffer<char, false>;
template class Buffer<char, true>;
template class Buffer<size_t, false>;
template class Buffer<size_t, true>;
