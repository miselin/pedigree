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

#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"

#include <PosixSubsystem.h>
#include <limits.h>

#include "net-syscalls.h"
#include "poll-syscalls.h"
#include "select-syscalls.h"

namespace {
constexpr short SelectReadReady = POLLIN | POLLRDNORM | POLLRDBAND | POLLHUP | POLLERR;
constexpr short SelectWriteReady = POLLOUT | POLLWRNORM | POLLWRBAND | POLLERR;
constexpr short SelectExceptionalReady = POLLPRI;

struct SelectProjection {
  bool read;
  bool write;
  bool exceptional;
  int count;
};

SelectProjection projectSelectReadiness(short revents, bool checkRead, bool checkWrite,
                                        bool checkExceptional) {
  SelectProjection result = {
      checkRead && (revents & SelectReadReady),
      checkWrite && (revents & SelectWriteReady),
      checkExceptional && (revents & SelectExceptionalReady),
      0,
  };
  result.count = static_cast<int>(result.read) + static_cast<int>(result.write) +
                 static_cast<int>(result.exceptional);
  return result;
}

int selectTimeoutMilliseconds(const timeval& timeout) {
  const int microsecondsMs = static_cast<int>((timeout.tv_usec + 999) / 1000);
  if (timeout.tv_sec > INT_MAX / 1000) {
    return INT_MAX;
  }

  const int secondsMs = static_cast<int>(timeout.tv_sec) * 1000;
  if (secondsMs > INT_MAX - microsecondsMs) {
    return INT_MAX;
  }
  return secondsMs + microsecondsMs;
}
}  // namespace

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
extern "C" EXPORTED_PUBLIC unsigned int posixSelectProjectionForTest(short revents, bool checkRead,
                                                                     bool checkWrite,
                                                                     bool checkExceptional) {
  const SelectProjection projection =
      projectSelectReadiness(revents, checkRead, checkWrite, checkExceptional);
  return static_cast<unsigned int>(projection.read) |
         (static_cast<unsigned int>(projection.write) << 1) |
         (static_cast<unsigned int>(projection.exceptional) << 2) |
         (static_cast<unsigned int>(projection.count) << 8);
}

extern "C" EXPORTED_PUBLIC int posixSelectTimeoutMillisecondsForTest(timeval timeout) {
  return selectTimeoutMilliseconds(timeout);
}
#endif

int posix_select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* errorfds, timeval* timeout) {
  POLL_NOTICE("select(" << nfds << ", " << readfds << ", " << writefds << ", " << errorfds << ", "
                        << timeout << ")");
  if (nfds < 0 || nfds > FD_SETSIZE) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  fd_set readSnapshot;
  fd_set writeSnapshot;
  fd_set errorSnapshot;
  timeval timeoutSnapshot;
  fd_set* reads = readfds ? &readSnapshot : nullptr;
  fd_set* writes = writefds ? &writeSnapshot : nullptr;
  fd_set* errors = errorfds ? &errorSnapshot : nullptr;

  const bool copiedInputs =
      (!readfds || PosixSubsystem::copyFromUser(reads, readfds, sizeof(fd_set))) &&
      (!writefds || PosixSubsystem::copyFromUser(writes, writefds, sizeof(fd_set))) &&
      (!errorfds || PosixSubsystem::copyFromUser(errors, errorfds, sizeof(fd_set))) &&
      (!timeout || PosixSubsystem::copyFromUser(&timeoutSnapshot, timeout, sizeof(timeval)));
  if (!copiedInputs) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  if (timeout && (timeoutSnapshot.tv_sec < 0 || timeoutSnapshot.tv_usec < 0 ||
                  timeoutSnapshot.tv_usec >= 1000000)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  // Count the actual number of fds we have.
  size_t trueFdCount = 0;
  for (int i = 0; i < nfds; ++i) {
    if ((reads && FD_ISSET(i, reads)) || (writes && FD_ISSET(i, writes)) ||
        (errors && FD_ISSET(i, errors))) {
      POLL_NOTICE("fd " << i << " is acceptable");
      ++trueFdCount;
    }
  }

  // Set up pollfds
  struct pollfd* fds = new struct pollfd[trueFdCount];
  size_t j = 0;
  for (int i = 0; i < nfds; ++i) {
    bool checkRead = reads && FD_ISSET(i, reads);
    bool checkWrite = writes && FD_ISSET(i, writes);
    bool checkError = errors && FD_ISSET(i, errors);

    if (!(checkRead || checkWrite || checkError)) {
      continue;
    }

    POLL_NOTICE("registering fd " << i << " in slot " << j);

    fds[j].fd = i;
    fds[j].events = 0;
    if (checkRead)
      fds[j].events |= POLLIN;
    if (checkWrite)
      fds[j].events |= POLLOUT;
    if (checkError)
      fds[j].events |= POLLPRI;
    fds[j].revents = 0;

    ++j;
  }

  // Default to infinite wait, but handle immediate wait or a specific timeout
  // too.
  int timeoutMs = -1;
  if (timeout) {
    timeoutMs = selectTimeoutMilliseconds(timeoutSnapshot);
  }

  // Go!
  POLL_NOTICE(" -> redirecting select() to poll() with " << trueFdCount << " actual fds");
  int r = posix_poll_safe(fds, trueFdCount, timeoutMs);

  if (r >= 0) {
    for (size_t i = 0; i < trueFdCount; ++i) {
      if (fds[i].revents & POLLNVAL) {
        SYSCALL_ERROR(BadFileDescriptor);
        r = -1;
        break;
      }
    }
  }

  // Fill fd_sets as needed. select() returns the number of result bits, not
  // poll()'s number of descriptors with at least one result.
  int readyCount = 0;
  j = 0;
  for (int i = 0; r >= 0 && i < nfds; ++i) {
    /// \todo this could be done MUCH better
    bool checkRead = reads && FD_ISSET(i, reads);
    bool checkWrite = writes && FD_ISSET(i, writes);
    bool checkError = errors && FD_ISSET(i, errors);

    if (!(checkRead || checkWrite || checkError)) {
      continue;
    }

    const SelectProjection projection =
        projectSelectReadiness(fds[j].revents, checkRead, checkWrite, checkError);

    if (checkRead) {
      if (projection.read) {
        FD_SET(i, reads);
      } else {
        FD_CLR(i, reads);
      }
    }

    if (checkWrite) {
      if (projection.write) {
        FD_SET(i, writes);
      } else {
        FD_CLR(i, writes);
      }
    }

    if (checkError) {
      if (projection.exceptional) {
        FD_SET(i, errors);
      } else {
        FD_CLR(i, errors);
      }
    }

    readyCount += projection.count;
    ++j;
  }

  delete[] fds;

  if (r >= 0) {
    r = readyCount;
  }

  if (r >= 0) {
    const bool copiedResults =
        (!readfds || PosixSubsystem::copyToUser(readfds, reads, sizeof(fd_set))) &&
        (!writefds || PosixSubsystem::copyToUser(writefds, writes, sizeof(fd_set))) &&
        (!errorfds || PosixSubsystem::copyToUser(errorfds, errors, sizeof(fd_set))) &&
        (!timeout || PosixSubsystem::copyToUser(timeout, &timeoutSnapshot, sizeof(timeval)));
    if (!copiedResults) {
      SYSCALL_ERROR(BadAddress);
      r = -1;
    }
  }

  POLL_NOTICE(" -> select via poll returns " << r);
  return r;
}
