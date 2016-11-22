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

// From musl
#include <errno.h>
#include <stdio.h>
#include <bits/syscall.h>
#include <stdarg.h>

// From the Pedigree source tree (syscall stubs). References musl errno.
#include <posix-syscall.h>
#include <posixSyscallNumbers.h>
#include <translate.h>

#define STUBBED(which) do { \
    char buf[32]; \
    snprintf(buf, 32, "linux=%ld", which); \
    syscall1(POSIX_STUBBED, (long)(buf)); \
} while(0)

long pedigree_translate_syscall(long which, long a1, long a2, long a3, long a4,
                                long a5, long a6)
{
    long pedigree_translation = posix_translate_syscall(which);
    if (pedigree_translation == -1)
    {
        STUBBED(which);
        return -ENOSYS;
    }
    else
    {
        long err = 0;
        long r = syscall6_err(pedigree_translation, a1, a2, a3, a4, a5, a6, &err);
        if (err)
        {
            return -err;
        }
        else
        {
            return r;
        }
    }
}

// Normally implemented in assembly - brought in here to avoid having to
// replace the .c file
long __syscall(long which, long a1, long a2, long a3, long a4,
               long a5, long a6)
{
    return pedigree_translate_syscall(which, a1, a2, a3, a4, a5, a6);
}

// Extension that provides write access to the kernel log.
int klog(int prio, const char *fmt, ...)
{
    static char print_temp[1024];
    va_list argptr;
    va_start(argptr, fmt);
    vsnprintf(print_temp, sizeof print_temp, fmt, argptr);
    syscall2(POSIX_SYSLOG, (long) print_temp, prio);
    va_end(argptr);
}
