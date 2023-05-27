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

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/MemoryPool.h"
#include "pedigree/kernel/utilities/TimeoutGuard.h"

void *g_pBootstrapInfo = 0;

Scheduler Scheduler::m_Instance;

static int g_DevZero = -1;

static int getDevZero()
{
    if (g_DevZero != -1)
    {
        return g_DevZero;
    }

    g_DevZero = open("/dev/zero", O_RDWR);
    return g_DevZero;
}

static void closeDevZero()
{
    if (g_DevZero == -1)
    {
        return;
    }

    close(g_DevZero);
    g_DevZero = -1;
}

namespace Time
{
Timestamp getTime(bool sync)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    return ts.tv_sec;
}

bool delay(Timestamp nanoseconds)
{
    /// \todo this isn't quite right
    struct timespec ts;
    ts.tv_sec = nanoseconds / Multiplier::Second;
    ts.tv_nsec = nanoseconds % Multiplier::Second;
    nanosleep(&ts, nullptr);
    return true;
}

Timestamp getTimeNanoseconds(bool sync)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    return (ts.tv_sec * Multiplier::Second) + ts.tv_nsec;
}
}  // namespace Time

extern "C" void panic(const char *s)
{
    fprintf(stderr, "PANIC: %s\n", s);
    abort();
}

namespace SlamSupport
{
static const size_t heapSize = 0x40000000ULL;
uintptr_t getHeapBase()
{
    static void *base = 0;
    if (base)
    {
        return reinterpret_cast<uintptr_t>(base);
    }

    base = mmap(
        0, heapSize, PROT_NONE, MAP_PRIVATE | MAP_NORESERVE | MAP_ANONYMOUS, -1,
        0);
    if (base == MAP_FAILED)
    {
        fprintf(
            stderr, "cannot get a region of memory for SlamAllocator: %s\n",
            strerror(errno));
        abort();
    }

    return reinterpret_cast<uintptr_t>(base);
}

uintptr_t getHeapEnd()
{
    return getHeapBase() + heapSize;
}

void getPageAt(void *addr)
{
    void *r = mmap(
        addr, 0x1000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_FIXED | MAP_NORESERVE | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED)
    {
        fprintf(stderr, "map failed: %s\n", strerror(errno));
        abort();
    }
}

void unmapPage(void *page)
{
    munmap(page, 0x1000);
}

void unmapAll()
{
    munmap((void *) getHeapBase(), heapSize);
}
}  // namespace SlamSupport

/** Spinlock implementation. */

Spinlock::Spinlock() = default;

Spinlock::Spinlock(bool bLocked, bool bAvoidTracking)
    : m_bInterrupts(), m_Atom(!bLocked), m_CpuState(0), m_Ra(0),
      m_bAvoidTracking(bAvoidTracking), m_Magic(0xdeadbaba), m_pOwner(0),
      m_bOwned(false), m_Level(0), m_OwnedProcessor(~0)
{
}

bool Spinlock::acquire(bool recurse, bool safe)
{
    while (!m_Atom.compareAndSwap(true, false))
        ;

    return true;
}

void Spinlock::release()
{
    exit();
}

void Spinlock::exit()
{
    m_Atom.compareAndSwap(false, true);
}

/** ConditionVariable implementation. */

ConditionVariable::ConditionVariable()
    : m_Lock(false), m_Waiters(), m_Private(0)
{
    pthread_cond_t *cond = new pthread_cond_t;
    *cond = PTHREAD_COND_INITIALIZER;

    pthread_cond_init(cond, 0);

    m_Private = reinterpret_cast<void *>(cond);
}

ConditionVariable::~ConditionVariable()
{
    pthread_cond_t *cond = reinterpret_cast<pthread_cond_t *>(m_Private);
    pthread_cond_destroy(cond);

    delete cond;
}

ConditionVariable::WaitResult ConditionVariable::wait(Mutex &mutex)
{
    Time::Timestamp zero = Time::Infinity;
    return wait(mutex, zero);
}

ConditionVariable::WaitResult
ConditionVariable::wait(Mutex &mutex, Time::Timestamp &timeout)
{
    pthread_cond_t *cond = reinterpret_cast<pthread_cond_t *>(m_Private);
    pthread_mutex_t *m =
        reinterpret_cast<pthread_mutex_t *>(mutex.getPrivate());

    Error err = NoError;

    int r = 0;
    if (timeout == Time::Infinity)
    {
        r = pthread_cond_wait(cond, m);
    }
    else
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout / Time::Multiplier::Second;
        ts.tv_nsec += timeout % Time::Multiplier::Second;

        r = pthread_cond_timedwait(cond, m, &ts);

        if (r == ETIMEDOUT)
        {
            // no more time remaining
            timeout = 0;
            err = TimedOut;
        }
        else
        {
            struct timespec ts2;
            clock_gettime(CLOCK_REALTIME, &ts2);

            // Need to calculate the time remaining.
            uint64_t sec = ts.tv_sec - ts2.tv_sec;
            uint64_t nsec = ts.tv_nsec - ts2.tv_nsec;

            timeout = (sec * Time::Multiplier::Second) + nsec;
        }
    }

    if (err != NoError)
    {
        return Result<bool, Error>::withError(err);
    }
    else
    {
        /// \todo should capture more error states
        return Result<bool, Error>::withValue(r == 0);
    }
}

void ConditionVariable::signal()
{
    pthread_cond_t *cond = reinterpret_cast<pthread_cond_t *>(m_Private);
    pthread_cond_signal(cond);
}

void ConditionVariable::broadcast()
{
    pthread_cond_t *cond = reinterpret_cast<pthread_cond_t *>(m_Private);
    pthread_cond_broadcast(cond);
}

/** Mutex implementation. */

Mutex::Mutex(bool bLocked) : m_Private(0)
{
    pthread_mutex_t *mutex = new pthread_mutex_t;
    *mutex = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_init(mutex, 0);

    m_Private = reinterpret_cast<void *>(mutex);
}

Mutex::~Mutex()
{
    pthread_mutex_t *mutex = reinterpret_cast<pthread_mutex_t *>(m_Private);
    pthread_mutex_destroy(mutex);

    delete mutex;
}

bool Mutex::acquire()
{
    errno = 0;

    pthread_mutex_t *mutex = reinterpret_cast<pthread_mutex_t *>(m_Private);
    int r = pthread_mutex_lock(mutex);
    if (r == 0)
    {
        return true;
    }
    else
    {
        perror("pthread_mutex_lock");
    }

    return false;
}

bool Mutex::tryAcquire()
{
    pthread_mutex_t *mutex = reinterpret_cast<pthread_mutex_t *>(m_Private);
    int r = pthread_mutex_trylock(mutex);
    if (r == 0)
    {
        return true;
    }
    else if (r != EBUSY)
    {
        errno = r;
        perror("pthread_mutex_trylock");
    }

    return false;
}

void Mutex::release()
{
    errno = 0;

    pthread_mutex_t *mutex = reinterpret_cast<pthread_mutex_t *>(m_Private);
    int r = pthread_mutex_unlock(mutex);
    if (r != 0)
    {
        perror("pthread_mutex_unlock");
    }
}

ssize_t Mutex::getValue()
{
    // ugh.
    pthread_mutex_t *mutex = reinterpret_cast<pthread_mutex_t *>(m_Private);
    int r = pthread_mutex_trylock(mutex);
    if (r == 0)
    {
        // was unlocked
        pthread_mutex_unlock(mutex);
        return 1;
    }
    else
    {
        return 0;
    }
}

/** Cache implementation. */
void Cache::discover_range(uintptr_t &start, uintptr_t &end)
{
    EMIT_IF(!STANDALONE_CACHE)
    {
        return;
    }

    static uintptr_t alloc_start = 0;
    const size_t length = 0x80000000U;

    if (alloc_start)
    {
        start = alloc_start;
        end = start + length;
        return;
    }

    int fd = getDevZero();
    void *p = mmap(0, length, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (p != MAP_FAILED)
    {
        alloc_start = reinterpret_cast<uintptr_t>(p);

        start = alloc_start;
        end = start + length;
    }
}

MemoryPool::MemoryPool()
    : m_BufferSize(4096), m_BufferCount(0), m_bInitialised(false),
      m_AllocBitmap()
{
}

MemoryPool::MemoryPool(const char *poolName) : MemoryPool()
{
}

MemoryPool::~MemoryPool()
{
}

bool MemoryPool::initialise(size_t poolSize, size_t bufferSize)
{
    m_BufferSize = bufferSize;
    m_bInitialised = true;
    return true;
}

uintptr_t MemoryPool::allocate()
{
    return reinterpret_cast<uintptr_t>(malloc(m_BufferSize));
}

uintptr_t MemoryPool::allocateNow()
{
    return allocate();
}

void MemoryPool::free(uintptr_t buffer)
{
    ::free(reinterpret_cast<void *>(buffer));
}

bool MemoryPool::trim()
{
    return true;
}

void syscallError(int e)
{
    errno = e;
}

Scheduler::Scheduler() = default;

void Scheduler::yield()
{
    sched_yield();
}

TimeoutGuard::TimeoutGuard(size_t timeoutSecs) : m_bTimedOut(false)
{
    jmp_buf *buf = reinterpret_cast<jmp_buf *>(malloc(sizeof(jmp_buf)));
    if (sigsetjmp(*buf, 0))
    {
        m_bTimedOut = true;
        return;
    }

    m_State = buf;
}

TimeoutGuard::~TimeoutGuard()
{
    jmp_buf *buf = reinterpret_cast<jmp_buf *>(m_State);
    free(buf);
}

void TimeoutGuard::cancel()
{
    jmp_buf *buf = reinterpret_cast<jmp_buf *>(m_State);
    siglongjmp(*buf, 1);
}

size_t Processor::id()
{
    return 0;
}

void Processor::pause()
{
}

bool normalisePath(String &nameToOpen, const char *name, bool *onDevFs)
{
    if (onDevFs)
    {
        *onDevFs = false;
    }

    nameToOpen = name;
    return true;
}
