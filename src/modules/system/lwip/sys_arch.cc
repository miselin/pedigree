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


#include <lwip/arch/sys_arch.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <lwip/errno.h>

#include <pedigree/kernel/utilities/pocketknife.h>
#include <pedigree/kernel/process/Semaphore.h>
#include <pedigree/kernel/process/Mutex.h>
#include <pedigree/kernel/process/Thread.h>
#include <pedigree/kernel/processor/Processor.h>
#include <pedigree/kernel/Log.h>
#include <pedigree/kernel/utilities/RingBuffer.h>

// errno for lwIP usage, this is not ideal as it'll be exposed to ALL modules.
int errno;

#ifdef UTILITY_LINUX
#include <time.h>

static Spinlock g_Protection(false);
#endif

struct pedigree_mbox
{
    pedigree_mbox() : buffer(64) {}

    RingBuffer<void *> buffer;
};

void sys_init()
{
}

u32_t sys_now()
{
#ifdef UTILITY_LINUX
    struct timespec spec;
    clock_gettime(CLOCK_REALTIME, &spec);

    return (spec.tv_sec * 1000) + (spec.tv_nsec / 1000000);
#else
    return Time::getTimeNanoseconds() / Time::Multiplier::MILLISECOND;
#endif
}

struct thread_meta
{
    lwip_thread_fn thread;
    void *arg;
    char name[64];
};

static int thread_shim(void *arg)
{
    struct thread_meta *meta = (struct thread_meta *) arg;
    meta->thread(meta->arg);
    return 0;
}

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio)
{
    /// \todo stacksize might be important
    auto meta = new struct thread_meta;
    meta->thread = thread;
    meta->arg = arg;
    StringCopy(meta->name, name);
    pocketknife::runConcurrently(thread_shim, meta);
    return meta;
}

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
#ifdef UTILITY_LINUX
    if (sem_init(sem, 0, count) != 0)
    {
        return ERR_ARG;
    }
    else
    {
        return ERR_OK;
    }
#else
    Semaphore *newsem = new Semaphore(count);
    *sem = reinterpret_cast<void *>(newsem);
    return ERR_OK;
#endif
}

void sys_sem_free(sys_sem_t *sem)
{
#ifdef UTILITY_LINUX
    sem_destroy(sem);
#else
    Semaphore *s = reinterpret_cast<Semaphore *>(*sem);
    delete s;
#endif
}

int sys_sem_valid(sys_sem_t *sem)
{
    return 1;
}

void sys_sem_set_invalid(sys_sem_t *sem)
{
#ifndef UTILITY_LINUX
    *sem = nullptr;
#endif
}

void sys_sem_signal(sys_sem_t *sem)
{
#ifdef UTILITY_LINUX
    sem_post(sem);
#else
    Semaphore *s = reinterpret_cast<Semaphore *>(*sem);
    s->release();
#endif
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
#ifdef UTILITY_LINUX
    if (!timeout)
    {
        if (sem_wait(sem) == 0)
        {
            return ERR_OK;
        }
        else
        {
            return ERR_ARG;  /// \todo incorrect for many cases
        }
    }

    struct timespec now, spec;
    clock_gettime(CLOCK_REALTIME, &now);

    int r = 0;
    if (timeout)
    {
        u32_t s = timeout / 1000;
        u32_t ms = timeout % 1000;

        spec.tv_sec = now.tv_sec + s;
        spec.tv_nsec = now.tv_nsec + (ms * 1000000);

        r = sem_timedwait(sem, &spec);
    }
    else
    {
        r = sem_wait(sem);
    }

    clock_gettime(CLOCK_REALTIME, &spec);

    if (r == 0)
    {
        uint64_t orig_ms = (now.tv_sec * 1000U) + (now.tv_nsec * 1000000U);
        uint64_t waited_ms = (spec.tv_sec * 1000U) + (spec.tv_nsec * 1000000U);

        // return time we had to wait for the semaphore
        return waited_ms - orig_ms;
    }
    else
    {
        /// \todo figure out if there's some better errors for this
        return SYS_ARCH_TIMEOUT;
    }
#else
    Semaphore *s = reinterpret_cast<Semaphore *>(*sem);
    if (!s->acquire(0, timeout * 1000))  // ms -> us
    {
        return SYS_ARCH_TIMEOUT;
    }

    /// \todo need to return the time waited for the semaphore!
    return 0;
#endif
}

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    *mbox = new pedigree_mbox;
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    delete *mbox;
    *mbox = nullptr;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    (*mbox)->buffer.write(msg);
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    if (!(*mbox)->buffer.dataReady())
    {
        return SYS_MBOX_EMPTY;
    }

    *msg = (*mbox)->buffer.read();
    return 0;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    Time::Timestamp begin = Time::getTimeNanoseconds();

    Time::Timestamp timeoutMs = 0;
    if (timeout == 0)
    {
        timeoutMs = Time::INFINITY;
    }
    else
    {
        timeout * Time::Multiplier::MILLISECOND;
    }

    *msg = (*mbox)->buffer.read(timeoutMs);
    if (*msg == NULL)
    {
        return SYS_ARCH_TIMEOUT;
    }

    Time::Timestamp end = Time::getTimeNanoseconds();
    return (end - begin) / Time::Multiplier::MILLISECOND;
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    if (!(*mbox)->buffer.canWrite())
    {
        return ERR_WOULDBLOCK;
    }

    (*mbox)->buffer.write(msg);

    return ERR_OK;
}

int sys_mbox_valid(sys_mbox_t *mbox)
{
    return *mbox != nullptr ? 1 : 0;
}

void sys_mbox_set_invalid(sys_mbox_t *mbox)
{
    *mbox = nullptr;
}

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    Mutex *m = new Mutex;
    *mutex = m;
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    Mutex *m = reinterpret_cast<Mutex *>(*mutex);
    while (!m->acquire())
        ;
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    Mutex *m = reinterpret_cast<Mutex *>(*mutex);
    m->release();
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    Mutex *m = reinterpret_cast<Mutex *>(*mutex);
    delete m;
    *mutex = nullptr;
}

int sys_mutex_valid(sys_mutex_t *mutex)
{
    return *mutex != nullptr ? 1 : 0;
}

void sys_mutex_set_invalid(sys_mutex_t *mutex)
{
    *mutex = nullptr;
}

sys_prot_t sys_arch_protect()
{
#ifdef UTILITY_LINUX
    while (!g_Protection.acquire(true))
        ;
#else
    bool was = Processor::getInterrupts();
    Processor::setInterrupts(false);
    return was ? 1 : 0;
#endif
}

void sys_arch_unprotect(sys_prot_t pval)
{
#ifdef UTILITY_LINUX
    g_Protection.release();
#else
    Processor::setInterrupts(pval);
#endif
}
