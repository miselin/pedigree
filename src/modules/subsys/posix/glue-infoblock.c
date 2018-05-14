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

// Contains implementations of syscalls that use global info blocks instead of
// native syscalls proper.

#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/InfoBlock.h"

/// \todo this is hardcoded for x64
static struct InfoBlock *infoBlock = (struct InfoBlock *) 0xFFFFFFFF8FFF0000;

struct getcpu_cache;

int __vdso_clock_gettime(clockid_t clock_id, struct timespec *tp);
int __vdso_gettimeofday(struct timeval *tv, void *tz);
int __vdso_getcpu(unsigned *cpu, unsigned *node, struct getcpu_cache *cache);
time_t __vdso_time(time_t *tloc);

int __vdso_clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    // 'now' is in nanoseconds.
    /// \todo only do this for CLOCK_MONOTONIC and CLOCK_REALTIME
    uint64_t now = infoBlock->now;
    tp->tv_sec = now / 1000000000U;
    tp->tv_nsec = now % 1000000000U;

    return 0;
}

int __vdso_gettimeofday(struct timeval *tv, void *tz)
{
    if (tv)
    {
        // 'now' is in nanoseconds.
        uint64_t now = infoBlock->now;
        tv->tv_sec = now / 1000000000U;
        tv->tv_usec = now / 1000U;
    }

    /// \todo use tz

    return 0;
}

int __vdso_getcpu(unsigned *cpu, unsigned *node, struct getcpu_cache *cache)
{
    if (cpu)
    {
        *cpu = 0;
    }

    if (node)
    {
        *node = 0;
    }

    return 0;
}

time_t __vdso_time(time_t *tloc)
{
    if (tloc)
    {
        *tloc = infoBlock->now_s;
    }

    return infoBlock->now_s;
}

__asm__(".symver __vdso_clock_gettime,__vdso_clock_gettime@LINUX_2.6");
__asm__(".symver __vdso_gettimeofday,__vdso_gettimeofday@LINUX_2.6");
__asm__(".symver __vdso_getcpu,__vdso_getcpu@LINUX_2.6");
__asm__(".symver __vdso_time,__vdso_time@LINUX_2.6");

int clock_gettime(clockid_t, struct timespec *)
    __attribute__((weak, alias("__vdso_clock_gettime")));
int gettimeofday(struct timeval *__restrict, void *__restrict)
    __attribute__((weak, alias("__vdso_gettimeofday")));
int getcpu(unsigned *, unsigned *, struct getcpu_cache *)
    __attribute__((weak, alias("__vdso_getcpu")));
time_t time(time_t *) __attribute__((weak, alias("__vdso_time")));
