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

#ifndef PEDIGREE_C_SYSCALL_NUMBERS_H
#define PEDIGREE_C_SYSCALL_NUMBERS_H
#include <config.h>

#define PEDIGREE_LOGIN 1
#define PEDIGREE_SIGRET 2
#define PEDIGREE_INIT_SIGRET 3
#define PEDIGREE_INIT_PTHREADS 4
#define PEDIGREE_LOAD_KEYMAP 5
#define PEDIGREE_GET_MOUNT 6
#define PEDIGREE_REBOOT 7

// System call numbers 8-20 are reserved.
#define PEDIGREE_MODULE_LOAD 21
#define PEDIGREE_MODULE_UNLOAD 22
#define PEDIGREE_MODULE_IS_LOADED 23
#define PEDIGREE_MODULE_GET_DEPENDING 24

// System call numbers 25-26 are reserved.

#define PEDIGREE_SYS_REQUEST_MEM 27

#define PEDIGREE_HALTFS 28
#define PEDIGREE_INPUT_INHIBIT_EVENTS 29

#define PEDIGREE_EVENT_RETURN 60
// System call numbers 64-79 are reserved.

#endif
