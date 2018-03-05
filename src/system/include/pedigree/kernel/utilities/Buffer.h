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

#ifndef KERNEL_UTILITIES_BUFFER_H
#define KERNEL_UTILITIES_BUFFER_H

#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"

class Thread;
class Event;

/**
 * Provides a buffer of a specific size and utility functions for integration
 * with e.g. File or other kernel systems.
 *
 * allowShortOperation defines the action to take on overflow. If true, the
 * buffer's write() operation is permitted to return a size less than requested
 * if the buffer would overflow. Otherwise, the implementation is required to
 * block until bytes are present (unless blocking has been explicitly denied).
 *
 * Note that an attempt to read when writing is disabled that would block will
 * always return the number of bytes read so far (or zero if none yet). The
 * same is true for an attempt to write when reading is disabled if that would
 * block.
 */
template <class T, bool allowShortOperation = false>
class Buffer
{
  public:
    Buffer(size_t bufferSize);
    virtual ~Buffer();

    /**
     * Write \param count values from \param buffer, optionally blocking
     * before writing if there is insufficient space.
     */
    size_t write(const T *buffer, size_t count, bool block = true);

    /**
     * Read \param count values into \param buffer, optionally blocking
     * if no more values are available to be read yet.
     */
    size_t read(T *buffer, size_t count, bool block = true);

    /**
     * Disable further writes to the buffer.
     * This will wake up all readers waiting on a writer.
     */
    void disableWrites();

    /**
     * Disable further reads from the buffer.
     * This will wake up all writers waiting on reader.
     */
    void disableReads();

    /**
     * Enable writes to the buffer.
     *
     * \return the previous state of writes.
     */
    bool enableWrites();

    /**
     * Enable reads from the buffer.
     *
     * \return the previous state of reads.
     */
    bool enableReads();

    /**
     * Get the number of bytes in the buffer now.
     */
    size_t getDataSize();

    /**
     * Get the full size of the buffer (potential storage).
     */
    size_t getSize();

    /**
     * Check if the buffer can be written to.
     * \note This does not guarantee the next write() will succeed.
     */
    bool canWrite(bool block);

    /**
     * Check if the buffer can be read from.
     */
    bool canRead(bool block);

    /**
     * Wipes the buffer.
     */
    void wipe();

    /**
     * Add an event to be sent to the given thread upon a data change.
     *
     * \note An event does not guarantee the next operation will succeed.
     */
    void monitor(Thread *pThread, Event *pEvent);

    /**
     * Add a Semaphore to be signaled when data changes.
     */
    void monitor(Semaphore *pSemaphore);

    /**
     * Remove monitoring targets for the given thread.
     */
    void cullMonitorTargets(Thread *pThread);

    /**
     * Remove monitoring targets for the given Semaphore.
     */
    void cullMonitorTargets(Semaphore *pSemaphore);

  private:
    WITHOUT_IMPLICIT_CONSTRUCTORS(Buffer);

    /**
     * Helper function to send events upon completing an action.
     *
     * Clears all monitors as a side effect.
     */
    void notifyMonitors();

    /**
     * Create a new segment with the given data.
     */
    void addSegment(const T *buffer, size_t count);

    /**
     * Controls the size of each segment.
     */
    static const size_t m_SegmentSize = 32768;

    /**
     * Holds a segment of data; more data can be written into this segment
     * until it reaches capacity.
     */
    struct Segment
    {
        Segment() : data(), reader(0), size(0)
        {
        }

        /** Segment data. */
        T data[m_SegmentSize];

        /** Reader offset (the next reader starts here). */
        size_t reader;

        /** Segment size so far. */
        size_t size;
    };

    /**
     * Contains information about a particular target to send events to.
     */
    struct MonitorTarget
    {
        MonitorTarget() : pThread(0), pEvent(0), pSemaphore(0)
        {
        }

        MonitorTarget(Thread *thread, Event *event)
            : pThread(thread), pEvent(event), pSemaphore(0)
        {
        }

        MonitorTarget(Semaphore *sem)
            : pThread(0), pEvent(0), pSemaphore(sem)
        {
        }

        Thread *pThread;
        Event *pEvent;
        Semaphore *pSemaphore;
    };

    size_t m_BufferSize;
    size_t m_DataSize;

    Mutex m_Lock;

    ConditionVariable m_WriteCondition;
    ConditionVariable m_ReadCondition;

    List<Segment *> m_Segments;
    List<MonitorTarget *> m_MonitorTargets;

    bool m_bCanRead;
    bool m_bCanWrite;
};

// Specializations are in a .cc file.
extern template class Buffer<uint8_t, false>;
extern template class Buffer<uint8_t, true>;
extern template class Buffer<char, false>;
extern template class Buffer<char, true>;
extern template class Buffer<size_t, false>;
extern template class Buffer<size_t, true>;

#endif  // KERNEL_UTILITIES_BUFFER_H
