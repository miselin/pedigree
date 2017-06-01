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

static long
syscall6(long function, long p1, long p2, long p3, long p4, long p5, long p6)
{
    long eax = ((SERVICE & 0xFFFF) << 16) | (function & 0xFFFF);
    long ret;
    long err;
    register long p5_r __asm__("r8") = p5;
    register long p6_r __asm__("r9") = p6;

    SERVICE_INIT;

    __asm__ __volatile__("syscall"
                         : "=a"(ret), "=b"(err)
                         : "0"(eax), "1"(p1), "d"(p2), "S"(p3), "D"(p4),
                           "r"(p5_r), "r"(p6_r)
                         : "rcx", "r11", "memory");

    if (err)
    {
        SERVICE_ERROR = err;
    }
    return ret;
}

static long syscall6_err(
    long function, long p1, long p2, long p3, long p4, long p5, long p6,
    long *err)
{
    long eax = ((SERVICE & 0xFFFF) << 16) | (function & 0xFFFF);
    long ret;
    register long p5_r __asm__("r8") = p5;
    register long p6_r __asm__("r9") = p6;

    *err = 0;
    __asm__ __volatile__("syscall"
                         : "=a"(ret), "=b"(*err)
                         : "0"(eax), "1"(p1), "d"(p2), "S"(p3), "D"(p4),
                           "r"(p5_r), "r"(p6_r)
                         : "rcx", "r11", "memory");

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
