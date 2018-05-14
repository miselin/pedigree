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

#include "pedigree/kernel/utilities/ProducerConsumer.h"
#include "pedigree/kernel/Log.h"

#if PRODUCERCONSUMER_ASYNCHRONOUS
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/utilities/pocketknife.h"
#endif

ProducerConsumer::ProducerConsumer() = default;

ProducerConsumer::~ProducerConsumer()
{
#if PRODUCERCONSUMER_ASYNCHRONOUS
    m_Lock.acquire();
    if (m_Running)
    {
        m_Running = false;
        m_Condition.signal();
        m_Lock.release();

        pocketknife::attachTo(m_pThreadHandle);
    }
    else
    {
        m_Lock.release();
    }

    // Clean up tasks that didn't get executed.
    for (auto it : m_Tasks)
    {
        delete it;
    }
#endif
}

bool ProducerConsumer::initialise()
{
#if PRODUCERCONSUMER_ASYNCHRONOUS
    LockGuard<Mutex> guard(m_Lock);

    if (m_Running)
    {
        return true;
    }

    m_Running = true;
    m_pThreadHandle = pocketknife::runConcurrentlyAttached(thread, this);
    return m_pThreadHandle != nullptr;
#else
    return true;
#endif
}

void ProducerConsumer::produce(
    uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
    uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8)
{
#if PRODUCERCONSUMER_ASYNCHRONOUS
    Task *task = new Task;
    task->p0 = p0;
    task->p1 = p1;
    task->p2 = p2;
    task->p3 = p3;
    task->p4 = p4;
    task->p5 = p5;
    task->p6 = p6;
    task->p7 = p7;
    task->p8 = p8;

    m_Lock.acquire();
    m_Tasks.pushBack(task);
    m_Condition.signal();
    m_Lock.release();
#else
    consume(p0, p1, p2, p3, p4, p5, p6, p7, p8);
#endif
}

void ProducerConsumer::consumerThread()
{
    LockGuard<Mutex> guard(m_Lock);

    while (m_Running)
    {
        /// \todo should really use the result here.
        m_Condition.wait(m_Lock);

        if (!m_Running)
        {
            break;
        }
        else if (!m_Tasks.size())
        {
            continue;
        }

        Task *task = m_Tasks.popFront();

        // Don't hold lock while we actually perform the consume operation.
        m_Lock.release();

        consume(
            task->p0, task->p1, task->p2, task->p3, task->p4, task->p5,
            task->p6, task->p7, task->p8);

        delete task;

        m_Lock.acquire();
    }
}

int ProducerConsumer::thread(void *p)
{
    ProducerConsumer *pc = reinterpret_cast<ProducerConsumer *>(p);
    pc->consumerThread();

    return 0;
}
