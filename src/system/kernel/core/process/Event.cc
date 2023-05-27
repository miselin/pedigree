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

#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

Event::Event(
    uintptr_t handlerAddress, bool isDeletable, size_t specificNestingLevel)
    : m_HandlerAddress(handlerAddress), m_bIsDeletable(isDeletable),
      m_NestingLevel(specificNestingLevel), m_Magic(EVENT_MAGIC),
      m_Threads(), m_Lock(false)
{
}

Event::~Event()
{
    EMIT_IF(THREADS)
    {
        LockGuard<Spinlock> guard(m_Lock);

        if (m_Threads.count())
        {
            ERROR("UNSAFE EVENT DELETION");
            for (auto it : m_Threads)
            {
                ERROR(
                    " => Pending delivery to thread "
                    << it << " (" << it->getParent()->getId() << ":" << it->getId()
                    << ").");
            }
            FATAL(
                "Unsafe event deletion: " << m_Threads.count()
                                          << " threads reference it!");

            m_Threads.clear();
        }
    }
}

uintptr_t Event::getTrampoline()
{
    EMIT_IF(THREADS)
    {
        return VirtualAddressSpace::getKernelAddressSpace()
            .getKernelEventBlockStart();
    }
}

uintptr_t Event::getSecondaryTrampoline()
{
    return getTrampoline() + 0x100;
}

uintptr_t Event::getHandlerBuffer()
{
    return getTrampoline() + 0x1000;
}

uintptr_t Event::getLastHandlerBuffer()
{
    return getHandlerBuffer() +
           ((EVENT_TID_MAX * MAX_NESTED_EVENTS) * EVENT_LIMIT);
}

bool Event::isDeletable()
{
    return m_bIsDeletable;
}

bool Event::unserialize(uint8_t *pBuffer, Event &event)
{
    ERROR("Event::unserialize is abstract, should never be called.");
    return false;
}

size_t Event::getEventType(uint8_t *pBuffer)
{
    void *alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(size_t));
    size_t *pBufferSize_t = reinterpret_cast<size_t *>(alignedBuffer);
    return pBufferSize_t[0];
}

Event::Event(const Event &other)
    : Event(other.m_HandlerAddress, other.m_bIsDeletable, other.m_NestingLevel)
{
    ConstexprLockGuard<Spinlock, THREADS> guard(m_Lock);
    m_Threads.clear();
}

Event &Event::operator=(const Event &other)
{
    m_HandlerAddress = other.m_HandlerAddress;
    m_bIsDeletable = other.m_bIsDeletable;
    m_NestingLevel = other.m_NestingLevel;
    {
        ConstexprLockGuard<Spinlock, THREADS> guard(m_Lock);
        m_Threads.clear();
    }
    return *this;
}

void Event::registerThread(Thread *thread)
{
    LockGuard<Spinlock> guard(m_Lock);
    m_Threads.pushBack(thread);
}

void Event::deregisterThread(Thread *thread)
{
    LockGuard<Spinlock> guard(m_Lock);

    for (List<Thread *>::Iterator it = m_Threads.begin();
         it != m_Threads.end();)
    {
        if (*it == thread)
        {
            it = m_Threads.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

size_t Event::pendingCount()
{
    LockGuard<Spinlock> guard(m_Lock);

    return m_Threads.count();
}

void Event::waitForDeliveries()
{
    // no-op if no threads
    EMIT_IF(!THREADS)
    {
        return;
    }

    while (pendingCount())
    {
        // Each yield will check event states on the new thread(s).
        Scheduler::instance().yield();
    }
}
