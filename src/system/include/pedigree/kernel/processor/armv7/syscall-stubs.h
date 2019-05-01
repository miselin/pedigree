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

#ifndef SERVICE
#error syscall-stubs.h requires SERVICE to be defined
#endif
#ifndef SERVICE_ERROR
#error syscall-stubs.h requires SERVICE_ERROR to be defined
#endif
#ifndef SERVICE_INIT
#error syscall-stubs.h requires SERVICE_INIT to be defined
#endif

struct SyscallParams
{
    long p1;
    long p2;
    long p3;
    long p4;
    long p5;
    long p6;
};

static long syscall6(long function, long p1, long p2, long p3, long p4, long p5, long p6)
{
    long num = ((SERVICE & 0xFFFF) << 16) | (function & 0xFFFF);
    long ret;
    long err;

    SERVICE_INIT;

    struct SyscallParams params;
    params.p1 = p1;
    params.p2 = p2;
    params.p3 = p3;
    params.p4 = p4;
    params.p5 = p5;
    params.p6 = p6;

    __asm__ __volatile__ ("mov r0, %1; \
                  mov r1, %1; \
                  swi #0; \
                  mov %0, r0; \
                  mov %0, r1"
                 : "=r"(ret), "=r"(err)
                 : "r"(num), "r"(&params));
    return ret;
}

static long syscall6_err(long function, long p1, long p2, long p3, long p4, long p5, long p6, long *err)
{
    long num = ((SERVICE & 0xFFFF) << 16) | (function & 0xFFFF);
    long ret;

    SERVICE_INIT;

    struct SyscallParams params;
    params.p1 = p1;
    params.p2 = p2;
    params.p3 = p3;
    params.p4 = p4;
    params.p5 = p5;
    params.p6 = p6;

    __asm__ __volatile__ ("mov r0, %1; \
                  mov r1, %1; \
                  swi #0; \
                  mov %0, r0; \
                  mov %0, r1"
                 : "=r"(ret), "=r"(err)
                 : "r"(num), "r"(&params));
    return ret;
}

static long syscall0(long function)
{
    return syscall6(function, 0, 0, 0, 0, 0, 0);
}

static long syscall1(long function, long p1)
{
    return syscall6(function, p1, 0, 0, 0, 0, 0);
}

static long syscall2(long function, long p1, long p2)
{
    return syscall6(function, p1, p2, 0, 0, 0, 0);
}

static long syscall3(long function, long p1, long p2, long p3)
{
    return syscall6(function, p1, p2, p3, 0, 0, 0);
}

static long syscall4(long function, long p1, long p2, long p3, long p4)
{
    return syscall6(function, p1, p2, p3, p4, 0, 0);
}

static long syscall5(long function, long p1, long p2, long p3, long p4, long p5)
{
    return syscall6(function, p1, p2, p3, p4, p5, 0);
}
