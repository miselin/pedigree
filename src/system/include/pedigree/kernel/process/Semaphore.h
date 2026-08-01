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

#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Result.h"
#include "pedigree/kernel/utilities/new"

/**
 * A counting semaphore.
 */
class EXPORTED_PUBLIC Semaphore
{
  public:
    enum SemaphoreError
    {
        TimedOut,
        Interrupted,
        NoError,
    };

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    enum MutexTransitionWindow
    {
        MutexCounterAcquired,
        MutexOwnerReleased,
    };
    using MutexTransitionHook = void (*)(MutexTransitionWindow window);

    /** Installs a hosted-only observer for the two mutex transition windows. */
    static void setMutexTransitionHook(MutexTransitionHook hook);

    static size_t getHostedTimeoutCreateCount();
    static size_t getHostedTimeoutDestroyCount();
#endif

    /** Constructor
     * \param nInitialValue The initial value of the semaphore.
     * \param canInterrupt If false, acquire() retries after interrupt rather
     *      than returning a failure status.*/
    Semaphore(size_t nInitialValue, bool canInterrupt = true);
    /** Destructor */
    virtual ~Semaphore();

    /** Acquires n items and reports the exact failure in error. */
    MUST_USE_RESULT bool acquireWithError(
        size_t n, size_t timeoutSecs, size_t timeoutUsecs,
        SemaphoreError &error);

    /**
     * Acquires a semaphore used as an internal completion barrier.
     *
     * Signal events are delivered while blocked, but do not complete the
     * barrier. The signal interruption marker is restored after the semaphore
     * is acquired (or the original timeout expires), allowing an outer syscall
     * boundary to report EINTR without abandoning storage still owned by the
     * asynchronous operation.
     */
    MUST_USE_RESULT bool acquireForCompletion(
        size_t n = 1, size_t timeoutSecs = 0, size_t timeoutUsecs = 0);

    /** Convenience wrapper for acquire(). */
    bool acquire(
        size_t n = 1, size_t timeoutSecs = 0, size_t timeoutUsecs = 0);

    /** Attempts to acquire n items from the semaphore. This will not block.
     * \param n The number of semaphore items required. Must be non-zero.
     * \return True if acquire succeeded, false otherwise. */
    bool tryAcquire(size_t n = 1);

    /** Releases n items from the semaphore.
     * \param n The number of semaphore items to release. Must be non-zero. */
    void release(size_t n = 1);

    /** Gets the current value of the semaphore */
    ssize_t getValue();

    /**
     * Returns the owning thread address when this is a locked Mutex.
     *
     * This is a read-only, best-effort debugger snapshot. A plain Semaphore
     * or unlocked Mutex returns nullptr.
     */
    const void *getDebugMutexOwner() const;

  protected:
    /**
     * Reuses this semaphore's storage for Mutex ownership state. These hooks
     * keep Mutex layout-compatible with its historical empty subclass while
     * ensuring calls through either the Mutex or Semaphore interface enforce
     * mutex ownership.
     */
    void initialiseMutex(bool locked);
    void destroyMutex();
    bool mutexOwnedByCurrentThread() const;

  private:
    typedef Result<bool, SemaphoreError> SemaphoreResult;

    MUST_USE_RESULT SemaphoreResult acquireWithResult(
        size_t n = 1, size_t timeoutSecs = 0, size_t timeoutUsecs = 0,
        bool deferTerminal = false);

    /** Private copy constructor
        \note NOT implemented. */
    Semaphore(const Semaphore &);
    /** Private operator=
        \note NOT implemented. */
    void operator=(const Semaphore &);

    /** Internal event class - just interrupts the calling thread
        (sets wasInterrupted and sets the thread status to Ready). */
    class SemaphoreEvent : public Event
    {
      public:
        explicit SemaphoreEvent(size_t nestingLevel);
        virtual ~SemaphoreEvent();
        virtual size_t serialize(uint8_t *pBuffer);
        static bool unserialize(uint8_t *pBuffer, SemaphoreEvent &event);
        virtual size_t getNumber();
    };

    size_t magic;

    Atomic<ssize_t> m_Counter;
    WaitQueue m_Waiters;
    bool m_bCanInterrupt;
};

#endif
