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

#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"

class Thread;

/// \todo rewrite this in the same way as TcpBuffer!

namespace RingBufferWait
{
enum WaitType
{
    Reading,
    Writing
};
}

/**
 * \brief Utility class to provide a ring buffer.
 *
 * Using this class provides safety in accessing the ring buffer as well as
 * the ability to check (with and without blocking) whether the buffer can
 * be read or written to at this time.
 *
 * The idea of the waitFor function is to provide a way for applications
 * desiring integration with a select()-style interface to block until the
 * condition is met.
 */
template <class T>
class RingBuffer
{
  public:
    RingBuffer();  // Not implemented, use RingBuffer(size_t)

    /// Constructor - pass in the desired size of the ring buffer.
    RingBuffer(size_t ringSize)
        : m_RingSize(ringSize), m_WriteCondition(), m_ReadCondition(), m_Ring()
    {
    }

    /// Destructor - destroys the ring; ensure nothing is calling waitFor.
    virtual ~RingBuffer()
    {
    }

    /// write - write a byte to the ring buffer.
    void write(T obj)
    {
        m_Lock.acquire();
        while (true)
        {
            // Wait for room in the buffer if we're full.
            if (m_Ring.count() >= m_RingSize)
            {
                while (!m_WriteCondition.wait(m_Lock))
                    ;
                continue;
            }

            m_Ring.pushBack(obj);
            break;
        }

        m_Lock.release();

        notifyMonitors();

        // Signal readers waiting for objects to read.
        m_ReadCondition.signal();
    }

    /// write - write the given number of objects to the ring buffer.
    size_t write(T *obj, size_t n)
    {
        if (n > m_RingSize)
            n = m_RingSize;

        for (size_t i = 0; i < n; ++i)
        {
            write(obj[i]);
        }

        return n;
    }

    /// read - read a byte from the ring buffer.
    T read()
    {
        T ret;

        m_Lock.acquire();
        while (true)
        {
            // Wait for room in the buffer if we're full.
            if (m_Ring.count() >= m_RingSize)
            {
                while (!m_ReadCondition.wait(m_Lock))
                    ;
                continue;
            }

            ret = m_Ring.popFront();
            break;
        }

        m_Lock.release();

        notifyMonitors();

        // Signal writers that may be waiting for buffer space.
        m_WriteCondition.signal();

        return ret;
    }

    /// read - read up to the given number of objects from the ring buffer
    size_t read(T *out, size_t n)
    {
        if (n > m_RingSize)
            n = m_RingSize;

        for (size_t i = 0; i < n; ++i)
        {
            out[i] = read();
        }

        return n;
    }

    /// dataReady - is data ready for reading from the ring buffer?
    bool dataReady()
    {
        LockGuard<Mutex> guard(m_Lock);
        return m_Ring.count() > 0;
    }

    /// canWrite - is it possible to write to the ring buffer without blocking?
    bool canWrite()
    {
        LockGuard<Mutex> guard(m_Lock);
        return m_Ring.count() < m_RingSize;
    }

    /// waitFor - block until the given condition is true (readable/writeable)
    bool waitFor(RingBufferWait::WaitType wait)
    {
        m_Lock.acquire();
        if (wait == RingBufferWait::Writing)
        {
            while (true)
            {
                if (m_Ring.count() < m_RingSize)
                {
                    m_Lock.release();
                    return true;
                }

                while (!m_WriteCondition.wait(m_Lock))
                    ;
            }
        }
        else
        {
            while (true)
            {
                if (m_Ring.count())
                {
                    m_Lock.release();
                    return true;
                }

                while (!m_ReadCondition.wait(m_Lock))
                    ;
            }
        }

        m_Lock.release();
        return false;
    }

    /**
     * \brief monitor - add a new Event to be fired when something happens
     *
     * This could be a read or a write event; after receiving the event be
     * sure to call dataReady() and/or canWrite() to determine the state
     * of the buffer.
     *
     * Do not assume that an event means both a read and write will not
     * block. In fact, never assume an event means either will not block.
     * You may need to re-subscribe to the event if something else reads
     * or writes to the ring buffer between the event trigger and your
     * handling.
     */
    void monitor(Thread *pThread, Event *pEvent)
    {
        m_Lock.acquire();
        m_MonitorTargets.pushBack(new MonitorTarget(pThread, pEvent));
        m_Lock.release();
    }

    /// Cull all monitor targets pointing to \p pThread.
    void cullMonitorTargets(Thread *pThread)
    {
        m_Lock.acquire();
        for (typename List<MonitorTarget *>::Iterator it =
                 m_MonitorTargets.begin();
             it != m_MonitorTargets.end(); it++)
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
        m_Lock.release();
    }

  private:
    /// Trigger event for threads waiting on us.
    void notifyMonitors()
    {
        m_Lock.acquire();
        for (typename List<MonitorTarget *>::Iterator it =
                 m_MonitorTargets.begin();
             it != m_MonitorTargets.end(); it++)
        {
            MonitorTarget *pMT = *it;

            pMT->pThread->sendEvent(pMT->pEvent);
            delete pMT;
        }
        m_MonitorTargets.clear();
        m_Lock.release();
    }

    size_t m_RingSize;

    ConditionVariable m_WriteCondition;
    ConditionVariable m_ReadCondition;

    List<T> m_Ring;

    Mutex m_Lock;

    struct MonitorTarget
    {
        MonitorTarget(Thread *pT, Event *pE) : pThread(pT), pEvent(pE)
        {
        }

        Thread *pThread;
        Event *pEvent;
    };

    List<MonitorTarget *> m_MonitorTargets;
};

extern template class RingBuffer<char>;
extern template class RingBuffer<void *>;

#endif
