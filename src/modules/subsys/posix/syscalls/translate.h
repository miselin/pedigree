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

#ifndef _POSIX_SYSCALLS_TRANSLATE_H
#define _POSIX_SYSCALLS_TRANSLATE_H

#include <posixSyscallNumbers.h>

enum PedigreeLinuxAmd64SyscallNumber {
#define PEDIGREE_LINUX_AMD64_SYSCALL(name, number, target) \
  PedigreeLinuxAmd64Syscall_##name = number,
#include "linuxSyscallMappings-amd64.h"
#undef PEDIGREE_LINUX_AMD64_SYSCALL
};

#if ARM64
enum PedigreeLinuxArm64SyscallNumber {
#define PEDIGREE_LINUX_ARM64_SYSCALL(name, number, target) \
  PedigreeLinuxArm64Syscall_##name = number,
#include "linuxSyscallMappings-arm64.h"
#undef PEDIGREE_LINUX_ARM64_SYSCALL
};
#elif ARMV7
enum PedigreeLinuxArmv7SyscallNumber {
#define PEDIGREE_LINUX_ARMV7_SYSCALL(name, number, target) \
  PedigreeLinuxArmv7Syscall_##name = number,
#include "linuxSyscallMappings-armv7.h"
#undef PEDIGREE_LINUX_ARMV7_SYSCALL
};
#endif

static inline long posix_translate_syscall(long which) {
  switch (which) {
#if ARM64
#define PEDIGREE_LINUX_ARM64_SYSCALL(name, number, target) \
  case PedigreeLinuxArm64Syscall_##name:                   \
    return target;
#include "linuxSyscallMappings-arm64.h"
#undef PEDIGREE_LINUX_ARM64_SYSCALL
#elif ARMV7
#define PEDIGREE_LINUX_ARMV7_SYSCALL(name, number, target) \
  case PedigreeLinuxArmv7Syscall_##name:                   \
    return target;
#include "linuxSyscallMappings-armv7.h"
#undef PEDIGREE_LINUX_ARMV7_SYSCALL
#else
#define PEDIGREE_LINUX_AMD64_SYSCALL(name, number, target) \
  case PedigreeLinuxAmd64Syscall_##name:                   \
    return target;
#include "linuxSyscallMappings-amd64.h"
#undef PEDIGREE_LINUX_AMD64_SYSCALL
#endif
  }
  return -1;
}

#endif  // _POSIX_SYSCALLS_TRANSLATE_H
