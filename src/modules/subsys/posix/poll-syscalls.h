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

#ifndef POLL_SYSCALLS_H
#define POLL_SYSCALLS_H

#include <poll.h>  // for pollfd
#include <stddef.h>
#include <stdint.h>

#include "linux-wait-abi.h"

enum class PollDeadlineType { Immediate, Finite, Infinite };

struct PollDeadline {
  PollDeadlineType type;
  uint64_t expires;
};

int posix_poll(struct pollfd* fds, unsigned int nfds, int timeout);

int posix_ppoll(struct pollfd* fds, unsigned int nfds, LinuxKernelTimespec* timeout,
                const uint64_t* signalMask, size_t signalMaskSize);

/** Create one absolute monotonic deadline from a validated relative timeout. */
PollDeadline posix_poll_deadline(const LinuxKernelTimespec* timeout);

/** Like posix_poll, but doesn't check for safe memory regions. */
int posix_poll_safe(struct pollfd* fds, unsigned int nfds, int timeout);

/** Like posix_poll_safe, using an exact absolute monotonic deadline. */
int posix_poll_safe(struct pollfd* fds, unsigned int nfds, const PollDeadline& deadline);

#endif
