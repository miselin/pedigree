/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"

static_assert(
    __atomic_always_lock_free(sizeof(size_t), nullptr),
    "IRQ doorbell words must be lock-free");
static_assert(
    __atomic_always_lock_free(sizeof(uintptr_t), nullptr),
    "IRQ diagnostic identities must be lock-free");
static_assert(
    __atomic_always_lock_free(sizeof(Thread::DebugState), nullptr),
    "IRQ worker debug state must be lock-free");

namespace
{
size_t elapsedSince(size_t now, size_t then)
{
    return then && now >= then ? now - then : 0;
}

void updateMaximum(size_t &maximum, size_t value)
{
    if (value > __atomic_load_n(&maximum, __ATOMIC_RELAXED))
    {
        // Each physical line has exactly one worker updating its maxima.
        __atomic_store_n(&maximum, value, __ATOMIC_RELEASE);
    }
}

IrqWorkerDebugState workerDebugState(Thread::DebugState state)
{
    switch (state)
    {
        case Thread::None:
            return IrqWorkerDebugState::None;
        case Thread::SemWait:
            return IrqWorkerDebugState::SemaphoreWait;
        case Thread::CondWait:
            return IrqWorkerDebugState::ConditionWait;
        case Thread::Joining:
            return IrqWorkerDebugState::Joining;
        case Thread::FutexWait:
            return IrqWorkerDebugState::FutexWait;
        case Thread::EventWait:
            return IrqWorkerDebugState::EventWait;
        case Thread::ProcessWait:
            return IrqWorkerDebugState::ProcessWait;
        case Thread::CallbackDrain:
            return IrqWorkerDebugState::CallbackDrain;
    }
    return IrqWorkerDebugState::Unavailable;
}

IrqWorkerWaitReason workerWaitReason(WaitQueue::WakeReason reason)
{
    switch (reason)
    {
        case WaitQueue::WakeReason::Waiting:
            return IrqWorkerWaitReason::Waiting;
        case WaitQueue::WakeReason::Signalled:
            return IrqWorkerWaitReason::Signalled;
        case WaitQueue::WakeReason::Event:
            return IrqWorkerWaitReason::Event;
        case WaitQueue::WakeReason::Unwinding:
            return IrqWorkerWaitReason::Unwinding;
        case WaitQueue::WakeReason::Terminating:
            return IrqWorkerWaitReason::Terminating;
        case WaitQueue::WakeReason::Spurious:
            return IrqWorkerWaitReason::Spurious;
    }
    return IrqWorkerWaitReason::Unavailable;
}
}  // namespace

ThreadedIrqDispatcher::Line::Line()
    : m_Owner(nullptr), m_Callback(nullptr), m_CallbackContext(nullptr),
      m_Thread(nullptr), m_Scheduler(nullptr), m_Line(0),
      m_PendingCookies(nullptr), m_PendingCookieCount(0), m_ActiveCookie(0),
      m_CallbackActive(0),
      m_PublicationState(PublicationClosed), m_Started(0),
      m_CompletedBatches(0), m_CompletedCookie(0), m_PendingSinceTimestamp(0),
      m_ActiveCallbackStartedTimestamp(0), m_LastWakeLatency(0),
      m_MaximumWakeLatency(0), m_LastCallbackRuntime(0),
      m_MaximumCallbackRuntime(0)
{
}

ThreadedIrqDispatcher::Line::~Line()
{
    if (__atomic_load_n(&m_Started, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&m_Thread, __ATOMIC_ACQUIRE) || m_PendingCookies)
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
        __atomic_load_n(&m_Thread, __ATOMIC_ACQUIRE) || m_PendingCookies ||
        !m_Owner || !m_Callback)
    {
        return false;
    }

    size_t pendingCookieCount = Processor::getCount();
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    const size_t testPendingCookieCount = __atomic_load_n(
        &m_Owner->m_PendingSlotCountForTest, __ATOMIC_ACQUIRE);
    if (testPendingCookieCount)
    {
        pendingCookieCount = testPendingCookieCount;
    }
#endif
    if (!pendingCookieCount)
    {
        return false;
    }
    size_t *pendingCookies = new size_t[pendingCookieCount];
    for (size_t i = 0; i < pendingCookieCount; ++i)
    {
        __atomic_store_n(
            &pendingCookies[i], static_cast<size_t>(0), __ATOMIC_RELAXED);
    }
    m_PendingCookies = pendingCookies;
    m_PendingCookieCount = pendingCookieCount;
    __atomic_store_n(&m_ActiveCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_CallbackActive, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_PublicationState, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_CompletedBatches, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_CompletedCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_PendingSinceTimestamp, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_ActiveCallbackStartedTimestamp, static_cast<size_t>(0),
        __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_LastWakeLatency, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_MaximumWakeLatency, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_LastCallbackRuntime, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_MaximumCallbackRuntime, static_cast<size_t>(0), __ATOMIC_RELEASE);

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
    __atomic_fetch_or(&m_PublicationState, PublicationClosed, __ATOMIC_ACQ_REL);
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
    delete[] m_PendingCookies;
    m_PendingCookies = nullptr;
    m_PendingCookieCount = 0;
    __atomic_store_n(&m_ActiveCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(
        &m_CallbackActive, static_cast<size_t>(0), __ATOMIC_RELEASE);
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

    size_t processor = Processor::index();
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    const size_t processorSlot = __atomic_load_n(
        &m_Owner->m_PublicationSlotForTest, __ATOMIC_ACQUIRE);
    if (processorSlot != static_cast<size_t>(-1))
    {
        processor = processorSlot;
    }
#endif
    if (!m_PendingCookies || processor >= m_PendingCookieCount)
    {
        __atomic_fetch_sub(
            &m_PublicationState, static_cast<size_t>(1), __ATOMIC_RELEASE);
        // Processor topology is fixed before dispatcher initialisation. A
        // failure here is therefore a lifecycle/configuration rejection, not
        // a contention fallback which can lose an admitted edge.
        return false;
    }

    // Maskable hard interrupts cannot run concurrently on one processor.
    // Per-processor slots make publication one wait-free exchange. A nested
    // publication happens after this store and may replace it with a later
    // generation; the outer publisher never writes again when it resumes.
    const size_t pending = __atomic_exchange_n(
        &m_PendingCookies[processor], cookie, __ATOMIC_ACQ_REL);
    if (!pending)
    {
        __atomic_store_n(
            &m_PendingSinceTimestamp, static_cast<size_t>(Time::getTicks()),
            __ATOMIC_RELEASE);
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    PublicationObservedHook hook = __atomic_load_n(
        &m_Owner->m_PublicationObservedHook, __ATOMIC_ACQUIRE);
    if (hook)
    {
        hook(m_Owner, m_Line, cookie, pending);
    }
#endif

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
    // External diagnostics can race orderly dispatcher shutdown. Share the
    // publisher admission word so join cannot release the slot array while a
    // detached scan is in progress.
    const size_t admission = __atomic_fetch_add(
        &m_PublicationState, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
    if (admission & PublicationClosed)
    {
        __atomic_fetch_sub(
            &m_PublicationState, static_cast<size_t>(1), __ATOMIC_RELEASE);
        return 0;
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    PendingScanAdmittedHook hook = __atomic_load_n(
        &m_Owner->m_PendingScanAdmittedHook, __ATOMIC_ACQUIRE);
    if (hook)
    {
        hook(m_Owner, m_Line);
    }
#endif

    const size_t pending = pendingCookieForWorker();
    __atomic_fetch_sub(
        &m_PublicationState, static_cast<size_t>(1), __ATOMIC_RELEASE);
    return pending;
}

size_t ThreadedIrqDispatcher::Line::pendingCookieForWorker() const
{
    size_t newest = 0;
    for (size_t i = 0; i < m_PendingCookieCount; ++i)
    {
        const size_t candidate =
            __atomic_load_n(&m_PendingCookies[i], __ATOMIC_ACQUIRE);
        if (candidate && (!newest || generationReached(candidate, newest)))
        {
            newest = candidate;
        }
    }
    return newest;
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

void ThreadedIrqDispatcher::Line::snapshotDiagnostics(
    IrqLineDiagnosticSnapshot &snapshot) const
{
    snapshot.workerDiagnosticAvailable = false;
    snapshot.workerDebugState = IrqWorkerDebugState::Unavailable;
    snapshot.workerDebugAddress = 0;
    snapshot.workerWaitActive = false;
    snapshot.workerWaitQueue = 0;
    snapshot.workerWaitChannelOwner = 0;
    snapshot.workerWaitChannelValue = 0;
    snapshot.workerWaitReason = IrqWorkerWaitReason::Unavailable;
    snapshot.workerWaitStateLevel = 0;
    snapshot.workerWaitQueued = false;
    snapshot.observationTimestamp = static_cast<size_t>(Time::getTicks());
    snapshot.pendingSinceTimestamp =
        __atomic_load_n(&m_PendingSinceTimestamp, __ATOMIC_ACQUIRE);
    snapshot.activeCallbackStartedTimestamp =
        __atomic_load_n(&m_ActiveCallbackStartedTimestamp, __ATOMIC_ACQUIRE);
    snapshot.lastWakeLatency =
        __atomic_load_n(&m_LastWakeLatency, __ATOMIC_ACQUIRE);
    snapshot.maximumWakeLatency =
        __atomic_load_n(&m_MaximumWakeLatency, __ATOMIC_ACQUIRE);
    snapshot.lastCallbackRuntime =
        __atomic_load_n(&m_LastCallbackRuntime, __ATOMIC_ACQUIRE);
    snapshot.maximumCallbackRuntime =
        __atomic_load_n(&m_MaximumCallbackRuntime, __ATOMIC_ACQUIRE);

    // Shutdown closes this shared admission word before joining the worker.
    // A diagnostic reader admitted here therefore pins m_Thread until its
    // detached copy is complete without taking a lock which the debugger may
    // have interrupted another CPU while holding.
    const size_t admission = __atomic_fetch_add(
        &m_PublicationState, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
    if (admission & PublicationClosed)
    {
        __atomic_fetch_sub(
            &m_PublicationState, static_cast<size_t>(1), __ATOMIC_RELEASE);
        return;
    }

    Thread *thread = __atomic_load_n(&m_Thread, __ATOMIC_ACQUIRE);
    if (thread)
    {
        snapshot.workerDiagnosticAvailable = true;
        uintptr_t debugAddress = 0;
        snapshot.workerDebugState =
            workerDebugState(thread->getDebugState(debugAddress));
        snapshot.workerDebugAddress = debugAddress;

        Thread::WaitDebugInfo wait = {};
        if (thread->getWaitDebugInfo(wait))
        {
            snapshot.workerWaitActive = true;
            snapshot.workerWaitQueue = reinterpret_cast<uintptr_t>(wait.queue);
            snapshot.workerWaitChannelOwner =
                reinterpret_cast<uintptr_t>(wait.channelOwner);
            snapshot.workerWaitChannelValue = wait.channelValue;
            snapshot.workerWaitReason = workerWaitReason(wait.reason);
            snapshot.workerWaitStateLevel = wait.stateLevel;
            snapshot.workerWaitQueued = wait.queued;
        }
    }

    __atomic_fetch_sub(
        &m_PublicationState, static_cast<size_t>(1), __ATOMIC_RELEASE);
}

int ThreadedIrqDispatcher::Line::workerEntry(void *context)
{
    return reinterpret_cast<Line *>(context)->run();
}

bool ThreadedIrqDispatcher::Line::workerReady(void *context)
{
    Line *line = reinterpret_cast<Line *>(context);
    return line->hasPendingForWorker() ||
           __atomic_load_n(&line->m_CallbackActive, __ATOMIC_ACQUIRE) ||
           (__atomic_load_n(&line->m_PublicationState, __ATOMIC_ACQUIRE) &
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
        const size_t pendingSince =
            __atomic_load_n(&m_PendingSinceTimestamp, __ATOMIC_ACQUIRE);
        const size_t cookie = takePendingCookie();
        if (cookie)
        {
            const size_t completedCookie = __atomic_load_n(
                &m_CompletedCookie, __ATOMIC_ACQUIRE);
            if (
                completedCookie &&
                !generationReached(cookie, completedCookie))
            {
                // A cross-CPU scan can claim an older slot after another
                // batch has already completed. The delivered high-water
                // suppresses that stale generation without suppressing
                // equal-cookie work-bit publications.
                __atomic_store_n(
                    &m_CallbackActive, static_cast<size_t>(0),
                    __ATOMIC_RELEASE);
                Scheduler::instance().yield();
                continue;
            }

            const size_t started = static_cast<size_t>(Time::getTicks());
            const size_t wakeLatency = elapsedSince(started, pendingSince);
            __atomic_store_n(&m_LastWakeLatency, wakeLatency, __ATOMIC_RELEASE);
            updateMaximum(m_MaximumWakeLatency, wakeLatency);
            __atomic_store_n(
                &m_ActiveCallbackStartedTimestamp, started, __ATOMIC_RELEASE);
            __atomic_store_n(&m_ActiveCookie, cookie, __ATOMIC_RELEASE);
            m_Callback(m_CallbackContext, m_Line, cookie);
            const size_t completed = static_cast<size_t>(Time::getTicks());
            const size_t runtime = elapsedSince(completed, started);
            __atomic_store_n(&m_LastCallbackRuntime, runtime, __ATOMIC_RELEASE);
            updateMaximum(m_MaximumCallbackRuntime, runtime);
            __atomic_add_fetch(
                &m_CompletedBatches, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
            __atomic_store_n(&m_CompletedCookie, cookie, __ATOMIC_RELEASE);
            __atomic_store_n(
                &m_ActiveCookie, static_cast<size_t>(0), __ATOMIC_RELEASE);
            __atomic_store_n(
                &m_ActiveCallbackStartedTimestamp, static_cast<size_t>(0),
                __ATOMIC_RELEASE);
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
        const size_t publicationState =
            __atomic_load_n(&m_PublicationState, __ATOMIC_ACQUIRE);
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
            if (!hasPendingForWorker())
            {
                break;
            }
            continue;
        }

        Scheduler::instance().yield();
    }

    return 0;
}

bool ThreadedIrqDispatcher::Line::hasPendingForWorker() const
{
    return pendingCookieForWorker() != 0;
}

size_t ThreadedIrqDispatcher::Line::takePendingCookie()
{
    size_t newest = 0;
    for (size_t i = 0; i < m_PendingCookieCount; ++i)
    {
        const size_t candidate = __atomic_exchange_n(
            &m_PendingCookies[i], static_cast<size_t>(0), __ATOMIC_ACQ_REL);
        if (candidate && (!newest || generationReached(candidate, newest)))
        {
            newest = candidate;
        }
    }
    return newest;
}

bool ThreadedIrqDispatcher::Line::generationReached(
    size_t current, size_t target)
{
    // Cookies advance monotonically and no live publication can span half of
    // the size_t range, so signed modular distance preserves wrap ordering.
    return static_cast<intptr_t>(current - target) >= 0;
}

ThreadedIrqDispatcher::ThreadedIrqDispatcher(
    const String &name, size_t lineCount, DispatchCallback callback,
    void *callbackContext)
    : m_Lines(), m_Name(name), m_LineCount(lineCount), m_Callback(callback),
      m_CallbackContext(callbackContext), m_Initialised(false)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_PublicationObservedHook(nullptr), m_PendingScanAdmittedHook(nullptr),
      m_PendingSlotCountForTest(0),
      m_PublicationSlotForTest(static_cast<size_t>(-1))
#endif
{
    if (m_LineCount > MaxLines)
    {
        m_LineCount = MaxLines;
    }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void ThreadedIrqDispatcher::setPublicationObservedHookForTest(
    PublicationObservedHook hook)
{
    __atomic_store_n(&m_PublicationObservedHook, hook, __ATOMIC_RELEASE);
}

bool ThreadedIrqDispatcher::setPendingSlotCountForTest(size_t slotCount)
{
    if (isInitialised() || !slotCount)
    {
        return false;
    }

    __atomic_store_n(
        &m_PendingSlotCountForTest, slotCount, __ATOMIC_RELEASE);
    return true;
}

bool ThreadedIrqDispatcher::publishFromSlotForTest(
    uint8_t line, size_t slot, size_t cookie)
{
    if (!isInitialised() || line >= m_LineCount)
    {
        return false;
    }

    size_t expected = static_cast<size_t>(-1);
    if (!__atomic_compare_exchange_n(
            &m_PublicationSlotForTest, &expected, slot, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        return false;
    }
    const bool published = m_Lines[line].publishFromInterrupt(cookie);
    __atomic_store_n(
        &m_PublicationSlotForTest, static_cast<size_t>(-1), __ATOMIC_RELEASE);
    return published;
}

void ThreadedIrqDispatcher::setPendingScanAdmittedHookForTest(
    PendingScanAdmittedHook hook)
{
    __atomic_store_n(&m_PendingScanAdmittedHook, hook, __ATOMIC_RELEASE);
}
#endif

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
    if (!canShutdown())
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

bool ThreadedIrqDispatcher::canShutdown() const
{
    Thread *current = Processor::information().getCurrentThread();
    bool safe = current && Processor::getInterrupts() &&
                !Processor::inDeviceHardIrq() && !isCurrentWorker();
#if HOSTED
    safe = safe && !current->getHostedSignalDepth();
#endif
    return safe;
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

void ThreadedIrqDispatcher::snapshotDiagnostics(
    uint8_t line, IrqLineDiagnosticSnapshot &snapshot) const
{
    if (line < m_LineCount)
    {
        m_Lines[line].snapshotDiagnostics(snapshot);
    }
}
