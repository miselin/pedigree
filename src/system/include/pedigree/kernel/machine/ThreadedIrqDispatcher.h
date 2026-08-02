/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_THREADEDIRQDISPATCHER_H
#define PEDIGREE_KERNEL_MACHINE_THREADEDIRQDISPATCHER_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"

class Thread;
class PerProcessorScheduler;
struct IrqLineDiagnosticSnapshot;

/**
 * Owns one coalescing worker for each physical IRQ line.
 *
 * The controller performs the trigger-specific hard-stage work before
 * publishing a monotonically increasing, nonzero cookie. The worker invokes
 * the callback in ordinary thread context. Interrupted machine state is never
 * retained or exposed to the callback. Publication records atomics and rings
 * a scheduler doorbell; it never wakes a sleeping thread from hard context.
 */
class EXPORTED_PUBLIC ThreadedIrqDispatcher
{
  public:
    static constexpr size_t MaxLines = 16;
    using DispatchCallback = void (*)(void *, uint8_t, size_t);

    ThreadedIrqDispatcher(
        const String &name, size_t lineCount, DispatchCallback callback,
        void *callbackContext);
    ~ThreadedIrqDispatcher();

    /** Starts every stable per-line worker after scheduler initialisation. */
    bool initialise();

    /** Rejects new publications and joins every worker. */
    bool shutdown();

    bool isInitialised() const;

    /** True when called by one of this dispatcher's callback workers. */
    bool isCurrentWorker() const;

    /**
     * Marks a line and rings its owning scheduler in one bounded operation.
     * Concurrent or nested producers are coalesced by cookie generation. A
     * constant cookie is safe for multi-producer work-bit users.
     */
    bool publishFromInterrupt(uint8_t line, size_t cookie);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    using PublicationObservedHook = void (*)(
        ThreadedIrqDispatcher *, uint8_t, size_t, size_t);

    /** Pauses a publication after its initial pending-cookie observation. */
    void setPublicationObservedHookForTest(PublicationObservedHook hook);
#endif

    /** Whether an occurrence arrived after the worker claimed its batch. */
    bool hasPending(uint8_t line) const;

    /** Lock-free diagnostic state; invalid lines return zero/false. */
    size_t pendingCookie(uint8_t line) const;
    size_t activeCookie(uint8_t line) const;
    size_t completedBatches(uint8_t line) const;
    size_t completedCookie(uint8_t line) const;
    uintptr_t workerIdentity(uint8_t line) const;
    bool callbackActive(uint8_t line) const;
    bool publicationClosed(uint8_t line) const;

    /**
     * Copies lock-free timing and detached worker state for the debugger.
     * Live fields are sampled independently; stopped-world use is authoritative.
     */
    void snapshotDiagnostics(
        uint8_t line, IrqLineDiagnosticSnapshot &snapshot) const;

  private:
    class Line
    {
      public:
        Line();
        ~Line();

        void configure(
            ThreadedIrqDispatcher *owner, uint8_t line,
            DispatchCallback callback, void *callbackContext);
        bool start();
        void beginStop();
        bool join();
        bool publishFromInterrupt(size_t cookie);
        bool hasPending() const;
        bool isWorker(const Thread *thread) const;
        size_t pendingCookie() const;
        size_t activeCookie() const;
        size_t completedBatches() const;
        size_t completedCookie() const;
        uintptr_t workerIdentity() const;
        bool callbackActive() const;
        bool publicationClosed() const;
        void snapshotDiagnostics(IrqLineDiagnosticSnapshot &snapshot) const;

      private:
        static constexpr size_t PublicationClosed =
            static_cast<size_t>(1) << ((sizeof(size_t) * 8) - 1);
        static constexpr size_t PublicationCountMask = ~PublicationClosed;

        static int workerEntry(void *context);
        static bool workerReady(void *context);
        int run();
        static bool generationReached(size_t current, size_t target);

        ThreadedIrqDispatcher *m_Owner;
        DispatchCallback m_Callback;
        void *m_CallbackContext;
        Thread *m_Thread;
        PerProcessorScheduler *m_Scheduler;
        uint8_t m_Line;
        size_t m_PendingCookie;
        size_t m_ActiveCookie;
        size_t m_CallbackActive;
        mutable size_t m_PublicationState;
        size_t m_Started;
        size_t m_CompletedBatches;
        size_t m_CompletedCookie;
        size_t m_PendingSinceTimestamp;
        size_t m_ActiveCallbackStartedTimestamp;
        size_t m_LastWakeLatency;
        size_t m_MaximumWakeLatency;
        size_t m_LastCallbackRuntime;
        size_t m_MaximumCallbackRuntime;

        Line(const Line &) = delete;
        Line &operator=(const Line &) = delete;
    };

    Line m_Lines[MaxLines];
    /** Inline so global interrupt-controller constructors cannot enter the
     * heap. */
    NormalStaticString m_Name;
    size_t m_LineCount;
    DispatchCallback m_Callback;
    void *m_CallbackContext;
    size_t m_Initialised;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    PublicationObservedHook m_PublicationObservedHook;
#endif

    ThreadedIrqDispatcher(const ThreadedIrqDispatcher &) = delete;
    ThreadedIrqDispatcher &operator=(const ThreadedIrqDispatcher &) = delete;
};

#endif
