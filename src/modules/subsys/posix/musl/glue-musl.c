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

#include <errno.h>
#include <stdio.h>
#include <stdarg.h>

#include <posix-syscall.h>
#include <posixSyscallNumbers.h>

#if HOSTED
// Hosted userspace cannot issue raw syscalls without entering the host OS.
#include <pedigree/kernel/processor/Syscalls.h>

long pedigree_translate_syscall(long which, long a1, long a2, long a3, long a4,
                                long a5, long a6)
{
    long err = 0;
    long r = syscall6_for_service_err(
        linuxCompat, which, a1, a2, a3, a4, a5, a6, &err);
    if (err)
    {
        return -err;
    }
    return r;
}

__attribute__((noreturn, visibility("hidden")))
void pedigree_musl_thread_exit(long status)
{
    syscall1(POSIX_PTHREAD_RETURN, status);
    __builtin_trap();
}
#endif

// Extension that provides write access to the kernel log.
int klog(int prio, const char *fmt, ...)
{
    static char print_temp[1024];
    va_list argptr;
    va_start(argptr, fmt);
    vsnprintf(print_temp, sizeof print_temp, fmt, argptr);
    int result = (int) syscall2(POSIX_SYSLOG, (long) print_temp, prio);
    va_end(argptr);
    return result;
}
