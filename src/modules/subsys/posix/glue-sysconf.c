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

#define _PEDIGREE_COMPILING_SUBSYS

#include "pedigree/kernel/processor/types.h"

#define _COMPILING_NEWLIB
#include <newlib.h>

#include <errno.h>
#include <limits.h>
#include <sys/config.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

// Ensure errno is usable
#define errno (*__errno())
extern int *__errno(void);

long sysconf(int name)
{
    syslog(LOG_NOTICE, "[%d] sysconf(%d)", getpid(), name);

    switch (name)
    {
        case _SC_AIO_LISTIO_MAX:
#ifdef AIO_LISTIO_MAX
            return AIO_LISTIO_MAX;
#else
            return -1;
#endif

        case _SC_AIO_MAX:
#ifdef AIO_MAX
            return AIO_MAX;
#else
            return -1;
#endif

        case _SC_AIO_PRIO_DELTA_MAX:
#ifdef AIO_PRIO_DELTA_MAX
            return AIO_PRIO_DELTA_MAX;
#else
            return -1;
#endif

        case _SC_ARG_MAX:
#ifdef ARG_MAX
            return ARG_MAX;
#else
            return -1;
#endif

        case _SC_CHILD_MAX:
#ifdef CHILD_MAX
            return CHILD_MAX;
#else
            return -1;
#endif

        case _SC_CLK_TCK:
            //#ifdef CLK_TCK
            return CLK_TCK;
            //#else
            //      return -1;
            //#endif

        case _SC_DELAYTIMER_MAX:
#ifdef DELAYTIMER_MAX
            return DELAYTIMER_MAX;
#else
            return -1;
#endif

        case _SC_GETGR_R_SIZE_MAX:
#ifdef _GETGR_R_SIZE_MAX
            return _GETGR_R_SIZE_MAX;
#else
            return -1;
#endif

        case _SC_GETPW_R_SIZE_MAX:
#ifdef _GETPW_R_SIZE_MAX
            return _GETPW_R_SIZE_MAX;
#else
            return -1;
#endif

        case _SC_LOGIN_NAME_MAX:
#ifdef LOGIN_NAME_MAX
            return LOGIN_NAME_MAX;
#else
            return -1;
#endif

        case _SC_MQ_OPEN_MAX:
#ifdef MQ_OPEN_MAX
            return MQ_OPEN_MAX;
#else
            return -1;
#endif

        case _SC_MQ_PRIO_MAX:
#ifdef MQ_PRIO_MAX
            return MQ_PRIO_MAX;
#else
            return -1;
#endif

        case _SC_NGROUPS_MAX:
#ifdef NGROUPS_MAX
            return NGROUPS_MAX;
#else
            return -1;
#endif

        case _SC_OPEN_MAX:
#ifdef OPEN_MAX
            return OPEN_MAX;
#else
            /// \note This is a hacked in value, OPEN_MAX should really be
            /// defined
            return 0xffff;
#endif

        case _SC_PAGESIZE:
#ifdef PAGESIZE
            return PAGESIZE;
#else
#ifdef PAGE_SIZE
            return PAGE_SIZE;
#else
            return -1;
#endif
#endif

        case _SC_RTSIG_MAX:
#ifdef RTSIG_MAX
            return RTSIG_MAX;
#else
            return -1;
#endif

        case _SC_SEM_NSEMS_MAX:
#ifdef SEM_NSEMS_MAX
            return SEM_NSEMS_MAX;
#else
            return -1;
#endif

        case _SC_SEM_VALUE_MAX:
#ifdef SEM_VALUE_MAX
            return SEM_VALUE_MAX;
#else
            return -1;
#endif

        case _SC_SIGQUEUE_MAX:
#ifdef SIGQUEUE_MAX
            return SIGQUEUE_MAX;
#else
            return -1;
#endif

        case _SC_STREAM_MAX:
#ifdef STREAM_MAX
            return STREAM_MAX;
#else
            return -1;
#endif

        case _SC_THREAD_DESTRUCTOR_ITERATIONS:
#ifdef PTHREAD_DESTRUCTOR_ITERATIONS
            return PTHREAD_DESTRUCTOR_ITERATIONS;
#else
            return -1;
#endif

        case _SC_THREAD_KEYS_MAX:
#ifdef PTHREAD_KEYS_MAX
            return PTHREAD_KEYS_MAX;
#else
            return -1;
#endif

        case _SC_THREAD_STACK_MIN:
#ifdef PTHREAD_STACK_MIN
            return PTHREAD_STACK_MIN;
#else
            return -1;
#endif

        case _SC_THREAD_THREADS_MAX:
#ifdef PTHREAD_THREADS_MAX
            return PTHREAD_THREADS_MAX;
#else
            return -1;
#endif

        case _SC_TIMER_MAX:
#ifdef TIMER_MAX
            return TIMER_MAX;
#else
            return -1;
#endif

        case _SC_TTY_NAME_MAX:
#ifdef TTY_NAME_MAX
            return TTY_NAME_MAX;
#else
            return -1;
#endif

        case _SC_TZNAME_MAX:
#ifdef TZNAME_MAX
            return TZNAME_MAX;
#else
            return -1;
#endif

        case _SC_ASYNCHRONOUS_IO:
#ifdef _POSIX_ASYNCHRONOUS_IO
            return 1;
#else
            return -1;
#endif

        case _SC_FSYNC:
#ifdef _POSIX_FSYNC
            return 1;
#else
            return -1;
#endif

        case _SC_JOB_CONTROL:
#ifdef _POSIX_JOB_CONTROL
            return 1;
#else
            return -1;
#endif

        case _SC_MAPPED_FILES:
#ifdef _POSIX_MAPPED_FILES
            return 1;
#else
            return -1;
#endif

        case _SC_MEMLOCK:
#ifdef _POSIX_MEMLOCK
            return 1;
#else
            return -1;
#endif

        case _SC_MEMLOCK_RANGE:
#ifdef _POSIX_MEMLOCK_RANGE
            return _POSIX_MEMLOCK_RANGE;
#else
            return -1;
#endif

        case _SC_MEMORY_PROTECTION:
#ifdef _POSIX_MEMORY_PROTECTION
            return 1;
#else
            return -1;
#endif

        case _SC_MESSAGE_PASSING:
#ifdef _POSIX_MESSAGE_PASSING
            return 1;
#else
            return -1;
#endif

        case _SC_PRIORITIZED_IO:
#ifdef _POSIX_PRIORITIZED_IO
            return 1;
#else
            return -1;
#endif

        case _SC_PRIORITY_SCHEDULING:
#ifdef _POSIX_PRIORITY_SCHEDULING
            return 1;
#else
            return -1;
#endif

        case _SC_REALTIME_SIGNALS:
#ifdef _POSIX_REALTIME_SIGNALS
            return 1;
#else
            return -1;
#endif

        case _SC_SAVED_IDS:
#ifdef _POSIX_SAVED_IDS
            return 1;
#else
            return -1;
#endif

        case _SC_SEMAPHORES:
#ifdef _POSIX_SEMAPHORES
            return 1;
#else
            return -1;
#endif

        case _SC_SHARED_MEMORY_OBJECTS:
#ifdef _POSIX_SHARED_MEMORY_OBJECTS
            return 1;
#else
            return -1;
#endif

        case _SC_SYNCHRONIZED_IO:
#ifdef _POSIX_SYNCHRONIZED_IO
            return 1;
#else
            return -1;
#endif

        case _SC_TIMERS:
#ifdef _POSIX_TIMERS
            return 1;
#else
            return -1;
#endif

        case _SC_THREADS:
#ifdef _POSIX_THREADS
            return 1;
#else
            return -1;
#endif

        case _SC_THREAD_ATTR_STACKADDR:
#ifdef _POSIX_THREAD_ATTR_STACKADDR
            return 1;
#else
            return -1;
#endif

        case _SC_THREAD_ATTR_STACKSIZE:
#ifdef _POSIX_THREAD_ATTR_STACKSIZE
            return 1;
#else
            return -1;
#endif

        case _SC_THREAD_PRIORITY_SCHEDULING:
#ifdef _POSIX_THREAD_PRIORITY_SCHEDULING
            return 1;
#else
            return -1;
#endif

        case _SC_THREAD_PRIO_INHERIT:
#ifdef _POSIX_THREAD_PRIO_INHERIT
            return 1;
#else
            return -1;
#endif

        case _SC_THREAD_PRIO_PROTECT:
#ifdef _POSIX_THREAD_PRIO_PROTECT
            return 1;
#else
            return -1;
#endif

        case _SC_THREAD_PROCESS_SHARED:
#ifdef _POSIX_THREAD_PROCESS_SHARED
            return 1;
#else
            return -1;
#endif

        case _SC_THREAD_SAFE_FUNCTIONS:
#ifdef _POSIX_THREAD_SAFE_FUNCTIONS
            return 1;
#else
            return -1;
#endif

        case _SC_VERSION:
#ifdef _POSIX_VERSION
            return _POSIX_VERSION;
#else
            return -1;
#endif

        default:
            errno = EINVAL;
            return -1;
    }

    return -1; /* can't get here */
}
