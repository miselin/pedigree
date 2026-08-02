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
      m_ActiveCookie(0), m_CallbackActive(0),
      m_PublicationState(PublicationClosed), m_Started(0),
      m_CompletedBatches(0), m_CompletedCookie(0)
{
}

ThreadedIrqDispatcher::Line::~Line()
{
    if (__atomic_load_n(&m_Started, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&m_Thread, __ATOMIC_ACQUIRE))
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
    if (__atomic_load_n(&m_Started, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&m_Thread, __ATOMIC_ACQUIRE) || !m_Owner || !m_Callback)
    {
        return false;
    }

    __atomic_store_n(&m_PendingCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(&m_ActiveCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(&m_CallbackActive, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_PublicationState, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_CompletedBatches, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_CompletedCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);

    m_Scheduler = &Processor::information().getScheduler();
    Thread *thread = new Thread(
        Scheduler::instance().getKernelProcess(), workerEntry, this, nullptr,
        false, true, true);
    __atomic_store_n(&m_Thread, thread, __ATOMIC_RELEASE);
    const String workerName(
        static_cast<const char *>(m_Owner->m_Name), m_Owner->m_Name.length());
    thread->setName(workerName);
    if (!thread->setSchedulerReadyPredicate(workerReady, this))
    {
        FATAL("A threaded IRQ worker could not install its ready predicate.");
        return false;
    }

    __atomic_store_n(&m_Started, static_cast<size_t>(1), __ATOMIC_RELEASE);
    if (!thread->start())
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
        return __atomic_load_n(&m_Thread, __ATOMIC_ACQUIRE) == nullptr;
    }

    Thread *thread = __atomic_load_n(&m_Thread, __ATOMIC_ACQUIRE);
    if (!thread || !thread->joinForCompletion())
    {
        return false;
    }

    __atomic_store_n(
        &m_Thread, static_cast<Thread *>(nullptr), __ATOMIC_RELEASE);
    __atomic_store_n(&m_Started, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(&m_PendingCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(&m_ActiveCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
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
    return pendingCookie() != 0;
}

bool ThreadedIrqDispatcher::Line::isWorker(const Thread *thread) const
{
    return thread && thread == __atomic_load_n(&m_Thread, __ATOMIC_ACQUIRE);
}

size_t ThreadedIrqDispatcher::Line::pendingCookie() const
{
    return __atomic_load_n(&m_PendingCookie, __ATOMIC_ACQUIRE);
}

size_t ThreadedIrqDispatcher::Line::activeCookie() const
{
    return __atomic_load_n(&m_ActiveCookie, __ATOMIC_ACQUIRE);
}

size_t ThreadedIrqDispatcher::Line::completedBatches() const
{
    return __atomic_load_n(&m_CompletedBatches, __ATOMIC_ACQUIRE);
}

size_t ThreadedIrqDispatcher::Line::completedCookie() const
{
    return __atomic_load_n(&m_CompletedCookie, __ATOMIC_ACQUIRE);
}

uintptr_t ThreadedIrqDispatcher::Line::workerIdentity() const
{
    return reinterpret_cast<uintptr_t>(
        __atomic_load_n(&m_Thread, __ATOMIC_ACQUIRE));
}

bool ThreadedIrqDispatcher::Line::callbackActive() const
{
    return __atomic_load_n(&m_CallbackActive, __ATOMIC_ACQUIRE) != 0;
}

bool ThreadedIrqDispatcher::Line::publicationClosed() const
{
    return (__atomic_load_n(&m_PublicationState, __ATOMIC_ACQUIRE) &
            PublicationClosed) != 0;
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
            __atomic_store_n(&m_ActiveCookie, cookie, __ATOMIC_RELEASE);
            m_Callback(m_CallbackContext, m_Line, cookie);
            __atomic_add_fetch(
                &m_CompletedBatches, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
            __atomic_store_n(&m_CompletedCookie, cookie, __ATOMIC_RELEASE);
            __atomic_store_n(
                &m_ActiveCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
            __atomic_store_n(
                &m_CallbackActive, static_cast<size_t>(0), __ATOMIC_RELEASE);
            // A continuously asserted source must not turn its threaded
            // bottom half into an unbounded softirq loop. One completed batch
            // is the scheduling budget before ordinary peers get a chance.
            Scheduler::instance().yield();
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

ThreadedIrqDispatcher::ThreadedIrqDispatcher(
    const String &name, size_t lineCount, DispatchCallback callback,
    void *callbackContext)
    : m_Lines(), m_Name(name), m_LineCount(lineCount), m_Callback(callback),
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

size_t ThreadedIrqDispatcher::pendingCookie(uint8_t line) const
{
    return line < m_LineCount ? m_Lines[line].pendingCookie() : 0;
}

size_t ThreadedIrqDispatcher::activeCookie(uint8_t line) const
{
    return line < m_LineCount ? m_Lines[line].activeCookie() : 0;
}

size_t ThreadedIrqDispatcher::completedBatches(uint8_t line) const
{
    return line < m_LineCount ? m_Lines[line].completedBatches() : 0;
}

size_t ThreadedIrqDispatcher::completedCookie(uint8_t line) const
{
    return line < m_LineCount ? m_Lines[line].completedCookie() : 0;
}

uintptr_t ThreadedIrqDispatcher::workerIdentity(uint8_t line) const
{
    return line < m_LineCount ? m_Lines[line].workerIdentity() : 0;
}

bool ThreadedIrqDispatcher::callbackActive(uint8_t line) const
{
    return line < m_LineCount && m_Lines[line].callbackActive();
}

bool ThreadedIrqDispatcher::publicationClosed(uint8_t line) const
{
    return line < m_LineCount && m_Lines[line].publicationClosed();
}
