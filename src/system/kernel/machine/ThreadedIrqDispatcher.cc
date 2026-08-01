/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

static_assert(
    __atomic_always_lock_free(sizeof(size_t), nullptr),
    "IRQ doorbell words must be lock-free");

ThreadedIrqDispatcher::Line::Line()
    : m_Owner(nullptr), m_Callback(nullptr), m_CallbackContext(nullptr),
      m_Thread(nullptr), m_Scheduler(nullptr), m_Line(0), m_PendingCookie(0),
      m_CallbackActive(0), m_PublicationState(PublicationClosed), m_Started(0)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_CompletedBatches(0), m_CompletedCookie(0)
#endif
{
}

ThreadedIrqDispatcher::Line::~Line()
{
    if (__atomic_load_n(&m_Started, __ATOMIC_ACQUIRE) || m_Thread)
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
    if (__atomic_load_n(&m_Started, __ATOMIC_ACQUIRE) || m_Thread ||
        !m_Owner || !m_Callback)
    {
        return false;
    }

    __atomic_store_n(&m_PendingCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(&m_CallbackActive, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_PublicationState, static_cast<size_t>(0), __ATOMIC_RELEASE);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    __atomic_store_n(
        &m_CompletedBatches, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_CompletedCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
#endif

    m_Scheduler = &Processor::information().getScheduler();
    m_Thread = new Thread(
        Scheduler::instance().getKernelProcess(), workerEntry, this, nullptr,
        false, true, true);
    m_Thread->setName("threaded IRQ line");
    m_Thread->setPriority(0);
    if (!m_Thread->setSchedulerReadyPredicate(workerReady, this))
    {
        FATAL("A threaded IRQ worker could not install its ready predicate.");
        return false;
    }

    __atomic_store_n(&m_Started, static_cast<size_t>(1), __ATOMIC_RELEASE);
    if (!m_Thread->start())
    {
        // This can only fail if a freshly-created delayed Thread has already
        // entered an impossible lifecycle state. Continuing would strand a
        // registered kernel Thread which cannot be safely reclaimed here.
        FATAL("A threaded IRQ worker could not be started.");
        return false;
    }
    return true;
#else
    return false;
#endif
}

void ThreadedIrqDispatcher::Line::beginStop()
{
    if (!__atomic_load_n(&m_Started, __ATOMIC_ACQUIRE))
    {
        return;
    }
    // One atomic word closes admission and counts publishers already inside
    // publishFromInterrupt(). The worker does not exit until that count drains.
    __atomic_fetch_or(
        &m_PublicationState, PublicationClosed, __ATOMIC_ACQ_REL);
    m_Scheduler->ringIrqWorkDoorbell();
}

bool ThreadedIrqDispatcher::Line::join()
{
    if (!__atomic_load_n(&m_Started, __ATOMIC_ACQUIRE))
    {
        return m_Thread == nullptr;
    }

    if (!m_Thread || !m_Thread->joinForCompletion())
    {
        return false;
    }

    m_Thread = nullptr;
    __atomic_store_n(&m_Started, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(&m_PendingCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(&m_CallbackActive, static_cast<size_t>(0), __ATOMIC_RELEASE);
    return true;
}

bool ThreadedIrqDispatcher::Line::publishFromInterrupt(size_t cookie)
{
    if (!cookie)
    {
        return false;
    }

    const size_t admission = __atomic_fetch_add(
        &m_PublicationState, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
    if (admission & PublicationClosed)
    {
        __atomic_fetch_sub(
            &m_PublicationState, static_cast<size_t>(1), __ATOMIC_RELEASE);
        return false;
    }

    // Controller dispatch serialises the one hard producer for each physical
    // line. The worker is the only consumer and can only exchange the value
    // to zero, so this store cannot overwrite a newer producer publication.
    const size_t pending =
        __atomic_load_n(&m_PendingCookie, __ATOMIC_ACQUIRE);
    if (!pending || generationReached(cookie, pending))
    {
        __atomic_store_n(&m_PendingCookie, cookie, __ATOMIC_RELEASE);
    }

    // The worker is pinned to this scheduler. Remote delivery can still
    // require a reschedule IPI for immediate service, but it must never ring
    // an unrelated CPU's doorbell.
    m_Scheduler->ringIrqWorkDoorbell();

    __atomic_fetch_sub(
        &m_PublicationState, static_cast<size_t>(1), __ATOMIC_RELEASE);
    return true;
}

bool ThreadedIrqDispatcher::Line::hasPending() const
{
    return __atomic_load_n(&m_PendingCookie, __ATOMIC_ACQUIRE) != 0;
}

bool ThreadedIrqDispatcher::Line::isWorker(const Thread *thread) const
{
    return thread && thread == m_Thread;
}

int ThreadedIrqDispatcher::Line::workerEntry(void *context)
{
    return reinterpret_cast<Line *>(context)->run();
}

bool ThreadedIrqDispatcher::Line::workerReady(void *context)
{
    Line *line = reinterpret_cast<Line *>(context);
    return __atomic_load_n(&line->m_PendingCookie, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&line->m_CallbackActive, __ATOMIC_ACQUIRE) ||
           (__atomic_load_n(
                &line->m_PublicationState, __ATOMIC_ACQUIRE) &
            PublicationClosed);
}

int ThreadedIrqDispatcher::Line::run()
{
    // Manager-owned workers are retired only by shutdown(). A terminal
    // request must not strand a line which still accepts publications.
    TerminationDeferral workerLifetime;
    while (true)
    {
        // Stay scheduler-eligible from before the claim until all callback
        // completion bookkeeping is published. Otherwise a timer preemption
        // can park this worker after it clears the only pending predicate.
        __atomic_store_n(
            &m_CallbackActive, static_cast<size_t>(1), __ATOMIC_RELEASE);
        const size_t cookie = __atomic_exchange_n(
            &m_PendingCookie, static_cast<size_t>(0), __ATOMIC_ACQ_REL);
        if (cookie)
        {
            m_Callback(m_CallbackContext, m_Line, cookie);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            __atomic_add_fetch(
                &m_CompletedBatches, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
            __atomic_store_n(&m_CompletedCookie, cookie, __ATOMIC_RELEASE);
#endif
            __atomic_store_n(
                &m_CallbackActive, static_cast<size_t>(0), __ATOMIC_RELEASE);
            continue;
        }

        __atomic_store_n(
            &m_CallbackActive, static_cast<size_t>(0), __ATOMIC_RELEASE);
        const size_t publicationState = __atomic_load_n(
            &m_PublicationState, __ATOMIC_ACQUIRE);
        if (publicationState & PublicationClosed)
        {
            if (publicationState & PublicationCountMask)
            {
                Scheduler::instance().yield();
                continue;
            }

            // An admitted publisher stores its cookie before dropping the
            // final count. Recheck after observing zero so close cannot race
            // the worker past an already-accepted occurrence.
            if (!__atomic_load_n(&m_PendingCookie, __ATOMIC_ACQUIRE))
            {
                break;
            }
            continue;
        }

        Scheduler::instance().yield();
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
    return __atomic_load_n(&m_CompletedBatches, __ATOMIC_ACQUIRE);
}

size_t ThreadedIrqDispatcher::Line::completedCookieForTest() const
{
    return __atomic_load_n(&m_CompletedCookie, __ATOMIC_ACQUIRE);
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
            Processor::information().getScheduler().serviceIrqWorkDoorbell();
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
    if (isCurrentWorker())
    {
        return false;
    }

    for (size_t i = 0; i < m_LineCount; ++i)
    {
        m_Lines[i].beginStop();
    }

    Processor::information().getScheduler().serviceIrqWorkDoorbell();

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

bool ThreadedIrqDispatcher::isCurrentWorker() const
{
#if THREADS
    const Thread *current = Processor::information().getCurrentThread();
    for (size_t i = 0; i < m_LineCount; ++i)
    {
        if (m_Lines[i].isWorker(current))
        {
            return true;
        }
    }
#endif
    return false;
}

bool ThreadedIrqDispatcher::publishFromInterrupt(uint8_t line, size_t cookie)
{
    return isInitialised() && line < m_LineCount &&
           m_Lines[line].publishFromInterrupt(cookie);
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
