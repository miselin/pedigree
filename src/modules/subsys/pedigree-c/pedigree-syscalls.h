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

#ifndef PEDIGREE_SYSCALLS_H
#define PEDIGREE_SYSCALLS_H
#include "pedigree/kernel/processor/types.h"

/** Pedigree generic system calls **/

int pedigree_login(int uid);

int pedigree_reboot();

void pedigree_module_load(char* file);
int pedigree_module_unload(char* name);
int pedigree_module_is_loaded(char* name);
int pedigree_module_get_depending(char* name, char* buf, size_t bufsz);

int pedigree_get_mount(char* mount_buf, char* info_buf, size_t n);

void* pedigree_sys_request_mem(size_t len);

void pedigree_haltfs();

/** Pedigree input system calls */

#ifdef __cplusplus
extern "C" {
#endif

int pedigree_load_keymap(uint32_t* buffer, size_t len);
void pedigree_input_inhibit_events(int inhibit);

int pedigree_event_return();

#ifdef __cplusplus
};  // extern "C"
#endif

#endif
