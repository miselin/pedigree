/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"

ThreadedIrqDispatcher::Line::Line()
    : m_Owner(nullptr), m_Callback(nullptr), m_CallbackContext(nullptr),
      m_Thread(nullptr), m_Work(0, false), m_StateLock(false), m_Line(0),
      m_PendingCookie(0), m_WakePublished(false), m_Stopping(true),
      m_Started(false)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_CompletedBatches(0), m_CompletedCookie(0)
#endif
{
}

ThreadedIrqDispatcher::Line::~Line()
{
    if (m_Started || m_Thread)
    {
        FATAL("A threaded IRQ worker was destroyed while active.");
    }
}

void ThreadedIrqDispatcher::Line::configure(
    ThreadedIrqDispatcher *owner, uint8_t line, DispatchCallback callback,
    void *callbackContext)
{
    m_Owner = owner;
    m_Line = line;
    m_Callback = callback;
    m_CallbackContext = callbackContext;
}

bool ThreadedIrqDispatcher::Line::start()
{
#if THREADS
    {
        LockGuard<Spinlock> guard(m_StateLock);
        if (m_Started || m_Thread || !m_Owner || !m_Callback)
        {
            return false;
        }

        m_PendingCookie = 0;
        m_WakePublished = false;
        m_Stopping = false;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        m_CompletedBatches = 0;
        m_CompletedCookie = 0;
#endif
    }
    const size_t staleWakeCount = m_Work.drainAvailable();
    (void) staleWakeCount;

    m_Thread = new Thread(
        Scheduler::instance().getKernelProcess(), workerEntry, this, nullptr,
        false, true, true);
    m_Thread->setName("threaded IRQ line");
    if (!m_Thread->start())
    {
        // This can only fail if a freshly-created delayed Thread has already
        // entered an impossible lifecycle state. Continuing would strand a
        // registered kernel Thread which cannot be safely reclaimed here.
        FATAL("A threaded IRQ worker could not be started.");
        return false;
    }

    {
        LockGuard<Spinlock> guard(m_StateLock);
        m_Started = true;
    }
    return true;
#else
    return false;
#endif
}

void ThreadedIrqDispatcher::Line::beginStop()
{
    {
        LockGuard<Spinlock> guard(m_StateLock);
        if (!m_Started)
        {
            return;
        }
        m_Stopping = true;
    }
    m_Work.release();
}

bool ThreadedIrqDispatcher::Line::join()
{
    {
        LockGuard<Spinlock> guard(m_StateLock);
        if (!m_Started)
        {
            return m_Thread == nullptr;
        }
    }

    if (!m_Thread || !m_Thread->joinForCompletion())
    {
        return false;
    }

    {
        LockGuard<Spinlock> guard(m_StateLock);
        m_Thread = nullptr;
        m_Started = false;
        m_PendingCookie = 0;
        m_WakePublished = false;
    }
    const size_t staleWakeCount = m_Work.drainAvailable();
    (void) staleWakeCount;
    return true;
}

ThreadedIrqDispatcher::Publication
ThreadedIrqDispatcher::Line::markPending(size_t cookie)
{
    Publication result = {false, false};
    if (!cookie)
    {
        return result;
    }

    LockGuard<Spinlock> guard(m_StateLock);
    if (!m_Started || m_Stopping)
    {
        return result;
    }

    if (!m_PendingCookie || generationReached(cookie, m_PendingCookie))
    {
        m_PendingCookie = cookie;
    }

    result.accepted = true;
    result.wake = !m_WakePublished;
    m_WakePublished = true;
    return result;
}

void ThreadedIrqDispatcher::Line::wake(Publication publication)
{
    if (publication.accepted && publication.wake)
    {
        m_Work.release();
    }
}

bool ThreadedIrqDispatcher::Line::hasPending() const
{
    LockGuard<Spinlock> guard(m_StateLock);
    return m_PendingCookie != 0;
}

int ThreadedIrqDispatcher::Line::workerEntry(void *context)
{
    return reinterpret_cast<Line *>(context)->run();
}

int ThreadedIrqDispatcher::Line::run()
{
    // Manager-owned workers are retired only by shutdown(). A terminal
    // request must not strand a line which still accepts publications.
    TerminationDeferral workerLifetime;
    while (true)
    {
        if (!m_Work.acquireForCompletion())
        {
            continue;
        }

        size_t cookie = 0;
        {
            LockGuard<Spinlock> guard(m_StateLock);
            // Clear the wake claim before consuming work. A later publisher
            // records both a new predicate and a fresh semaphore wake.
            m_WakePublished = false;
            cookie = m_PendingCookie;
            m_PendingCookie = 0;
        }
        if (cookie)
        {
            m_Callback(m_CallbackContext, m_Line, cookie);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            LockGuard<Spinlock> guard(m_StateLock);
            ++m_CompletedBatches;
            m_CompletedCookie = cookie;
#endif
        }

        {
            LockGuard<Spinlock> guard(m_StateLock);
            if (m_Stopping && !m_PendingCookie)
            {
                break;
            }
        }
    }

    return 0;
}

bool ThreadedIrqDispatcher::Line::generationReached(
    size_t current, size_t target)
{
    return static_cast<intptr_t>(current - target) >= 0;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
size_t ThreadedIrqDispatcher::Line::completedBatchesForTest() const
{
    LockGuard<Spinlock> guard(m_StateLock);
    return m_CompletedBatches;
}

size_t ThreadedIrqDispatcher::Line::completedCookieForTest() const
{
    LockGuard<Spinlock> guard(m_StateLock);
    return m_CompletedCookie;
}
#endif

ThreadedIrqDispatcher::ThreadedIrqDispatcher(
    size_t lineCount, DispatchCallback callback, void *callbackContext)
    : m_Lines(), m_LineCount(lineCount), m_Callback(callback),
      m_CallbackContext(callbackContext), m_Initialised(false)
{
    if (m_LineCount > MaxLines)
    {
        m_LineCount = MaxLines;
    }
}

ThreadedIrqDispatcher::~ThreadedIrqDispatcher()
{
    if (__atomic_load_n(&m_Initialised, __ATOMIC_ACQUIRE))
    {
        FATAL("Threaded IRQ dispatcher was destroyed before shutdown.");
    }
}

bool ThreadedIrqDispatcher::initialise()
{
#if THREADS
    if (__atomic_load_n(&m_Initialised, __ATOMIC_ACQUIRE) || !m_LineCount ||
        !m_Callback)
    {
        return false;
    }

    for (size_t i = 0; i < m_LineCount; ++i)
    {
        m_Lines[i].configure(
            this, static_cast<uint8_t>(i), m_Callback, m_CallbackContext);
        if (!m_Lines[i].start())
        {
            for (size_t j = 0; j < i; ++j)
            {
                m_Lines[j].beginStop();
            }
            for (size_t j = 0; j < i; ++j)
            {
                m_Lines[j].join();
            }
            return false;
        }
    }

    __atomic_store_n(&m_Initialised, static_cast<size_t>(1), __ATOMIC_RELEASE);
    return true;
#else
    return true;
#endif
}

bool ThreadedIrqDispatcher::shutdown()
{
#if THREADS
    if (!__atomic_load_n(&m_Initialised, __ATOMIC_ACQUIRE))
    {
        return true;
    }

    for (size_t i = 0; i < m_LineCount; ++i)
    {
        m_Lines[i].beginStop();
    }

    bool joined = true;
    for (size_t i = 0; i < m_LineCount; ++i)
    {
        joined &= m_Lines[i].join();
    }
    if (joined)
    {
        __atomic_store_n(
            &m_Initialised, static_cast<size_t>(0), __ATOMIC_RELEASE);
    }
    return joined;
#else
    return true;
#endif
}

bool ThreadedIrqDispatcher::isInitialised() const
{
    return __atomic_load_n(&m_Initialised, __ATOMIC_ACQUIRE) != 0;
}

ThreadedIrqDispatcher::Publication
ThreadedIrqDispatcher::markPending(uint8_t line, size_t cookie)
{
    if (!isInitialised() || line >= m_LineCount)
    {
        return {false, false};
    }
    return m_Lines[line].markPending(cookie);
}

void ThreadedIrqDispatcher::wake(uint8_t line, Publication publication)
{
    if (line < m_LineCount)
    {
        m_Lines[line].wake(publication);
    }
}

bool ThreadedIrqDispatcher::publishFromInterrupt(uint8_t line, size_t cookie)
{
    const Publication publication = markPending(line, cookie);
    wake(line, publication);
    return publication.accepted;
}

bool ThreadedIrqDispatcher::hasPending(uint8_t line) const
{
    return isInitialised() && line < m_LineCount && m_Lines[line].hasPending();
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
size_t ThreadedIrqDispatcher::completedBatchesForTest(uint8_t line) const
{
    return line < m_LineCount ? m_Lines[line].completedBatchesForTest() : 0;
}

size_t ThreadedIrqDispatcher::completedCookieForTest(uint8_t line) const
{
    return line < m_LineCount ? m_Lines[line].completedCookieForTest() : 0;
}
#endif
