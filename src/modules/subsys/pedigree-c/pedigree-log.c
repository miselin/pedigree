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

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "modules/subsys/posix/syscalls/posix-syscall.h"
#include "modules/subsys/posix/syscalls/posixSyscallNumbers.h"
#include <pedigree/log.h>

EXPORTED_PUBLIC int pedigree_log(int priority, const char* format, ...) {
  char message[1024];
  va_list arguments;
  va_start(arguments, format);
  int result = vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);

  if (result < 0) {
    return result;
  }

  return (int)syscall2(POSIX_SYSLOG, (long)message, priority);
}
