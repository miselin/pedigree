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

#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

_BEGIN_STD_C

#define PROT_READ 0
#define PROT_WRITE 1
#define PROT_EXEC 2
#define PROT_NONE 4

#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 4
#define MAP_ANON 8

#define MAP_USERSVD         0x10000
#define MAP_PHYS_OFFSET     0x20000

#define MAP_FAILED ((void*) 0)

#define MS_ASYNC        0x1
#define MS_SYNC         0x2
#define MS_INVALIDATE   0x4

#include <sys/types.h>

void  *mmap(void *, size_t, int, int, int, off_t);
int    munmap(void *, size_t);
int    mprotect(void *addr, size_t len, int prot);
int    msync(void *addr, size_t len, int flags);

_END_STD_C

#endif
