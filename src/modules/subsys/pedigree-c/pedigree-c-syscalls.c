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

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

#include <unistd.h>

#include "pedigree-c-syscall.h"
#include "pedigreecSyscallNumbers.h"

/// \todo make all these available in a header somewhere that isn't the POSIX
/// subsystem
EXPORTED_PUBLIC int pedigree_load_keymap(char* buf, size_t sz);

EXPORTED_PUBLIC void pedigree_reboot(void);

EXPORTED_PUBLIC void pedigree_haltfs(void);

EXPORTED_PUBLIC int pedigree_get_mount(char* mount_buf, char* info_buf, size_t n);

EXPORTED_PUBLIC void* pedigree_sys_request_mem(size_t len);

EXPORTED_PUBLIC void pedigree_input_inhibit_events(int inhibit);

EXPORTED_PUBLIC int pedigree_event_return(void);

EXPORTED_PUBLIC void pedigree_module_load(char* file);

EXPORTED_PUBLIC void pedigree_module_unload(char* name);

EXPORTED_PUBLIC int pedigree_module_is_loaded(char* name);

EXPORTED_PUBLIC int pedigree_module_get_depending(char* name, char* buf, size_t bufsz);

EXPORTED_PUBLIC int pedigree_login(uid_t uid);

int pedigree_load_keymap(char* buf, size_t sz) {
  return syscall2(PEDIGREE_LOAD_KEYMAP, (long)buf, (long)sz);
}

void pedigree_reboot() {
  syscall0(PEDIGREE_REBOOT);
}

void pedigree_haltfs() {
  syscall0(PEDIGREE_HALTFS);
}

int pedigree_get_mount(char* mount_buf, char* info_buf, size_t n) {
  return syscall3(PEDIGREE_GET_MOUNT, (long)mount_buf, (long)info_buf, n);
}

void* pedigree_sys_request_mem(size_t len) {
  uintptr_t result = syscall1(PEDIGREE_SYS_REQUEST_MEM, (long)len);
  return (void*)result;
}

void pedigree_input_inhibit_events(int inhibit) {
  syscall1(PEDIGREE_INPUT_INHIBIT_EVENTS, inhibit);
}

int pedigree_event_return() {
  return (int)syscall0(PEDIGREE_EVENT_RETURN);
}

void pedigree_module_load(char* file) {
  syscall1(PEDIGREE_MODULE_LOAD, (long)file);
}

void pedigree_module_unload(char* name) {
  syscall1(PEDIGREE_MODULE_UNLOAD, (long)name);
}

int pedigree_module_is_loaded(char* name) {
  return syscall1(PEDIGREE_MODULE_IS_LOADED, (long)name);
}

int pedigree_module_get_depending(char* name, char* buf, size_t bufsz) {
  return syscall3(PEDIGREE_MODULE_GET_DEPENDING, (long)name, (long)buf, bufsz);
}

// Pedigree-specific function: switch this process to a known uid.
int pedigree_login(uid_t uid) {
  return (long)syscall1(PEDIGREE_LOGIN, uid);
}
