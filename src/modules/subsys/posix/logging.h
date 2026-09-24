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

#ifndef _POSIX_KERNEL_LOGGING_H
#define _POSIX_KERNEL_LOGGING_H
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

#include <config.h>

// Logs to the kernel log with the current PID.
#if THREADS
#define POSIX_VERBOSE_LOG(f, x)                                                             \
  do {                                                                                      \
    auto ____tid = Processor::information().getCurrentThread()->getId();                    \
    auto ____level = Processor::information().getCurrentThread()->getStateLevel();          \
    auto ____pid = Processor::information().getCurrentThread()->getParent()->getId();       \
    NOTICE("[" << f << ":\t" << Dec << ____pid << ":" << ____tid << "." << ____level << Hex \
               << "]\t" << x);                                                              \
  } while (0)
#else
#define POSIX_VERBOSE_LOG(f, x)     \
  do {                              \
    NOTICE("[" << f << "]\t" << x); \
  } while (0)
#endif

// Each facility is one bit in the build's verbose logging mask.
#ifndef POSIX_LOG_FACILITIES
#define POSIX_LOG_FACILITIES 0
#endif

#define POSIX_LOG_IF(mask, facility, text)               \
  do {                                                   \
    EMIT_IF((POSIX_LOG_FACILITIES & (mask)) == (mask)) { \
      POSIX_VERBOSE_LOG(facility, text);                 \
    }                                                    \
  } while (0)

#define SC_NOTICE(x) POSIX_LOG_IF(2, "sys", x)
#define F_NOTICE(x) POSIX_LOG_IF(1, "io", x)
#define PT_NOTICE(x) POSIX_LOG_IF(4, "thr", x)
#define N_NOTICE(x) POSIX_LOG_IF(8, "net", x)
#define SG_NOTICE(x) POSIX_LOG_IF(32, "sig", x)
#define PS_NOTICE(x) POSIX_LOG_IF(16, "sub", x)
#define SG_VERBOSE_NOTICE(x) POSIX_LOG_IF(32 | 64, "sig", x)
#define POLL_NOTICE(x) POSIX_LOG_IF(256, "poll", x)

#endif  // _POSIX_KERNEL_LOGGING_H
