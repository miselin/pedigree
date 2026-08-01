/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_THREADEDIRQDISPATCHER_H
#define PEDIGREE_KERNEL_MACHINE_THREADEDIRQDISPATCHER_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/types.h"

class Thread;

/**
 * Owns one coalescing worker for each physical IRQ line.
 *
 * The controller performs the trigger-specific hard-stage work before
 * publishing a monotonically increasing, nonzero cookie. The worker invokes
 * the callback in ordinary thread context. Interrupted machine state is never
 * retained or exposed to the callback.
 */
class EXPORTED_PUBLIC ThreadedIrqDispatcher
{
  public:
    static constexpr size_t MaxLines = 16;
    using DispatchCallback = void (*)(void *, uint8_t, size_t);

    struct Publication
    {
        bool accepted;
        bool wake;
    };

    ThreadedIrqDispatcher(
        size_t lineCount, DispatchCallback callback, void *callbackContext);
    ~ThreadedIrqDispatcher();

    /** Starts every stable per-line worker after scheduler initialisation. */
    bool initialise();

    /** Rejects new publications and joins every worker. */
    bool shutdown();

    bool isInitialised() const;

    /**
     * Coalesces a line occurrence without waking its worker yet.
     *
     * Controllers use this while their short state lock is held, then call
     * wake() after dropping that lock.
     */
    Publication markPending(uint8_t line, size_t cookie);

    /** Completes a split markPending() publication. */
    void wake(uint8_t line, Publication publication);

    /** Marks and wakes a line when no controller lock must be split. */
    bool publishFromInterrupt(uint8_t line, size_t cookie);

    /** Whether an occurrence arrived after the worker claimed its batch. */
    bool hasPending(uint8_t line) const;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    size_t completedBatchesForTest(uint8_t line) const;
    size_t completedCookieForTest(uint8_t line) const;
#endif

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
        Publication markPending(size_t cookie);
        void wake(Publication publication);
        bool hasPending() const;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        size_t completedBatchesForTest() const;
        size_t completedCookieForTest() const;
#endif

      private:
        static int workerEntry(void *context);
        int run();
        static bool generationReached(size_t current, size_t target);

        ThreadedIrqDispatcher *m_Owner;
        DispatchCallback m_Callback;
        void *m_CallbackContext;
        Thread *m_Thread;
        Semaphore m_Work;
        mutable Spinlock m_StateLock;
        uint8_t m_Line;
        size_t m_PendingCookie;
        bool m_WakePublished;
        bool m_Stopping;
        bool m_Started;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        size_t m_CompletedBatches;
        size_t m_CompletedCookie;
#endif

        Line(const Line &) = delete;
        Line &operator=(const Line &) = delete;
    };

    Line m_Lines[MaxLines];
    size_t m_LineCount;
    DispatchCallback m_Callback;
    void *m_CallbackContext;
    size_t m_Initialised;

    ThreadedIrqDispatcher(const ThreadedIrqDispatcher &) = delete;
    ThreadedIrqDispatcher &operator=(const ThreadedIrqDispatcher &) = delete;
};

#endif
