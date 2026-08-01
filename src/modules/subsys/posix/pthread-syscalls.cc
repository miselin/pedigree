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

#include "PosixSubsystem.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"
#include <pthread-syscalls.h>

/// \todo add paths to include from path/to/musl-<vers>/src/internal/futex.h
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_FD 2
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_WAIT_BITSET 9
#define FUTEX_PRIVATE 128
#define FUTEX_CLOCK_REALTIME 256

extern "C" {
extern void pthread_stub();
extern char pthread_stub_end;
}

struct FutexKey
{
    FutexKey() : addressSpace(0), address(0)
    {
    }

    FutexKey(Process *pProcess, int *pAddress)
        : addressSpace(
              reinterpret_cast<uintptr_t>(pProcess->getAddressSpace())),
          address(reinterpret_cast<uintptr_t>(pAddress))
    {
    }

    bool operator==(const FutexKey &other) const
    {
        return addressSpace == other.addressSpace && address == other.address;
    }

    bool operator>(const FutexKey &other) const
    {
        return addressSpace > other.addressSpace ||
               (addressSpace == other.addressSpace && address > other.address);
    }

    uintptr_t addressSpace;
    uintptr_t address;
};

static WaitQueue g_FutexWaiters;

static WaitQueue::Channel futexChannel(const FutexKey &key)
{
    return WaitQueue::Channel(
        reinterpret_cast<const void *>(key.addressSpace), key.address);
}

int posix_futex(
    int *uaddr, int futex_op, int val, const struct timespec *timeout)
{
    Thread *pThread = Processor::information().getCurrentThread();
    Process *pProcess = pThread->getParent();
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    PT_NOTICE(
        "futex(" << Hex << uaddr << ", " << futex_op << ", " << val << ", "
                 << timeout << ")");

    if (!(futex_op & FUTEX_PRIVATE))
    {
        PT_NOTICE(" -> public futexes are not yet supported");
        SYSCALL_ERROR(Unimplemented);
        return -1;
    }

    if (futex_op & FUTEX_CLOCK_REALTIME)
    {
        PT_NOTICE(" -> realtime futex waits are not yet supported");
        SYSCALL_ERROR(Unimplemented);
        return -1;
    }

    futex_op &= ~FUTEX_PRIVATE;

    if (reinterpret_cast<uintptr_t>(uaddr) % alignof(int))
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }
    if (
        !PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(uaddr), sizeof(*uaddr),
            PosixSubsystem::SafeRead))
    {
        SYSCALL_ERROR(BadAddress);
        return -1;
    }

    int r = 0;
    const FutexKey key(pProcess, uaddr);

    switch (futex_op)
    {
        case FUTEX_WAIT:
        {
            PT_NOTICE(" -> FUTEX_WAIT");

            if (
                timeout &&
                !PosixSubsystem::checkAddress(
                    reinterpret_cast<uintptr_t>(timeout), sizeof(*timeout),
                    PosixSubsystem::SafeRead))
            {
                SYSCALL_ERROR(BadAddress);
                return -1;
            }

            Time::Timestamp timeoutNanoseconds = Time::Infinity;
            if (timeout)
            {
                if (
                    timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
                    timeout->tv_nsec >=
                        static_cast<decltype(timeout->tv_nsec)>(
                            Time::Multiplier::Second))
                {
                    SYSCALL_ERROR(InvalidArgument);
                    return -1;
                }

                const Time::Timestamp nanoseconds =
                    static_cast<Time::Timestamp>(timeout->tv_nsec);
                const Time::Timestamp seconds =
                    static_cast<Time::Timestamp>(timeout->tv_sec);
                if (
                    seconds >
                    (Time::Infinity - nanoseconds) /
                        Time::Multiplier::Second)
                {
                    SYSCALL_ERROR(InvalidArgument);
                    return -1;
                }

                timeoutNanoseconds =
                    seconds * Time::Multiplier::Second + nanoseconds;
            }

            auto guard = g_FutexWaiters.acquire();

            if (*uaddr != val)
            {
                PT_NOTICE(" -> value changed");
                SYSCALL_ERROR(NoMoreProcesses);  // EAGAIN
                r = -1;
            }
            else
            {
                pThread->clearInterruption();
                void *pAlarm = nullptr;
                if (timeout)
                {
                    pAlarm = Time::addAlarm(timeoutNanoseconds);
                }

                PT_NOTICE(" -> waiting...");
                WaitQueue::WakeReason wakeReason = guard.wait(
                    futexChannel(key), Thread::FutexWait,
                    reinterpret_cast<uintptr_t>(__builtin_return_address(0)),
                    pAlarm ? &Time::removeAlarm : nullptr, pAlarm);
                PT_NOTICE(" -> waiting complete!");

                const Thread::InterruptionReason interruption =
                    pThread->getInterruptionReason();
                if (pAlarm)
                {
                    Time::removeAlarm(pAlarm);
                }
                pThread->clearInterruption();

                if (wakeReason != WaitQueue::WakeReason::Signalled)
                {
                    if (
                        timeout &&
                        interruption == Thread::InterruptedByTimeout)
                    {
                        SYSCALL_ERROR(TimedOut);
                    }
                    else
                    {
                        SYSCALL_ERROR(Interrupted);
                    }
                    r = -1;
                }
            }
            break;
        }

        case FUTEX_WAKE:
        {
            PT_NOTICE(" -> FUTEX_WAKE");

            if (val < 0)
            {
                SYSCALL_ERROR(InvalidArgument);
                r = -1;
                break;
            }

            auto guard = g_FutexWaiters.acquire();
            int woken = 0;
            for (int i = 0; i < val; ++i)
            {
                if (!guard.wakeOne(
                        WaitQueue::WakeReason::Signalled, futexChannel(key)))
                {
                    break;
                }
                ++woken;
            }
            PT_NOTICE(" -> woke " << Dec << woken << " threads.");
            r = woken;
            break;
        }

        default:
            PT_NOTICE(" -> unsupported futex operation");
            SYSCALL_ERROR(Unimplemented);
            r = -1;
    }

    PT_NOTICE(" -> " << Dec << r);
    return r;
}

/**
 * Forcefully registers the given thread with the given PosixSubsystem.
 */
void pedigree_copy_posix_thread(
    Thread *origThread, PosixSubsystem *origSubsystem, Thread *newThread,
    PosixSubsystem *newSubsystem)
{
    PosixSubsystem::PosixThread *pOldPosixThread =
        origSubsystem->getThread(origThread->getId());
    if (!pOldPosixThread)
    {
        // Nothing to see here.
        return;
    }

    PosixSubsystem::PosixThread *pNewPosixThread =
        new PosixSubsystem::PosixThread;
    pNewPosixThread->pThread = newThread;
    pNewPosixThread->returnValue = 0;

    // Copy thread-specific data across.
    for (Tree<size_t, PosixSubsystem::PosixThreadKey *>::Iterator it =
             pOldPosixThread->m_ThreadData.begin();
         it != pOldPosixThread->m_ThreadData.end(); ++it)
    {
        size_t key = it.key();
        PosixSubsystem::PosixThreadKey *data = it.value();

        pNewPosixThread->addThreadData(key, data);
        pNewPosixThread->m_ThreadKeys.set(key);
    }

    pNewPosixThread->lastDataKey = pOldPosixThread->lastDataKey;
    pNewPosixThread->nextDataKey = pOldPosixThread->nextDataKey;

    newSubsystem->insertThread(newThread->getId(), pNewPosixThread);
}

/**
 * pedigree_init_pthreads
 *
 * This function copies the user mode thread wrapper from the kernel to a known
 * user mode location. The location is already mapped by pedigree_init_signals
 * which must be called before this function.
 */
void pedigree_init_pthreads()
{
    PT_NOTICE("init_pthreads");
    // Make sure we can write to the trampoline area.
    Processor::information().getVirtualAddressSpace().setFlags(
        reinterpret_cast<void *>(Event::getTrampoline()),
        VirtualAddressSpace::Write);
    MemoryCopy(
        reinterpret_cast<void *>(Event::getSecondaryTrampoline()),
        reinterpret_cast<void *>(pthread_stub),
        (reinterpret_cast<uintptr_t>(&pthread_stub_end) -
         reinterpret_cast<uintptr_t>(pthread_stub)));
    Processor::information().getVirtualAddressSpace().setFlags(
        reinterpret_cast<void *>(Event::getTrampoline()),
        VirtualAddressSpace::Execute | VirtualAddressSpace::Shared);

    // Make sure the main thread is actually known.
    Thread *pThread = Processor::information().getCurrentThread();
    Process *pProcess = pThread->getParent();
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return;
    }

    PosixSubsystem::PosixThread *pPosixThread = new PosixSubsystem::PosixThread;
    pPosixThread->pThread = pThread;
    pPosixThread->returnValue = 0;
    pSubsystem->insertThread(pThread->getId(), pPosixThread);
}

void *posix_pedigree_create_waiter()
{
    PT_NOTICE("posix_pedigree_create_waiter");

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return 0;
    }

    Semaphore *sem = new Semaphore(0);
    void *descriptor = pSubsystem->insertThreadWaiter(sem);
    if (!descriptor)
    {
        delete sem;
    }

    return descriptor;
}

int posix_pedigree_thread_wait_for(void *waiter)
{
    PT_NOTICE("posix_pedigree_thread_wait_for");

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    Semaphore *sem = pSubsystem->getThreadWaiter(waiter);
    if (!sem)
    {
        return -1;
    }

    // Deadlock detection - don't wait if nothing can wake this waiter.
    /// \todo Check for more than just one thread - there's probably other
    ///       detections we can do here.
    if (pProcess->getNumThreads() <= 1)
    {
        SYSCALL_ERROR(Deadlock);
        return -1;
    }

    // This descriptor remains owned by the subsystem until its matching
    // trigger. A signal may run while blocked, but cannot abandon the waiter
    // storage or consume the eventual notification.
    if (!sem->acquireForCompletion())
    {
        FATAL("POSIX thread-waiter completion barrier failed.");
    }

    return 0;
}

int posix_pedigree_thread_trigger(void *waiter)
{
    PT_NOTICE("posix_pedigree_thread_trigger");

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return 0;
    }

    Semaphore *sem = pSubsystem->getThreadWaiter(waiter);
    if (!sem)
        return 0;
    if (sem->getValue())
        return 0;  // Nothing to wake up.

    // Wake up a waiter.
    sem->release();
    return 1;
}

void posix_pedigree_destroy_waiter(void *waiter)
{
    PT_NOTICE("posix_pedigree_destroy_waiter");

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return;
    }

    Semaphore *sem = pSubsystem->getThreadWaiter(waiter);
    if (!sem)
    {
        return;
    }
    pSubsystem->removeThreadWaiter(waiter);
    delete sem;
}

pid_t posix_gettid()
{
    // Go caches this value before creating another thread, so it must not
    // change when the process transitions from one thread to several.
    return Processor::information().getCurrentThread()->getId();
}
