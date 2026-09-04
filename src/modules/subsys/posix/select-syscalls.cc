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
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"

#include <PosixSubsystem.h>
#include <signal.h>

#include "net-syscalls.h"
#include "poll-syscalls.h"
#include "select-syscalls.h"
#include <sys/time.h>

namespace {
constexpr int MaximumSelectDescriptors = 16384;
constexpr size_t SelectWordBits = sizeof(uintptr_t) * 8;
constexpr size_t LinuxKernelSigsetSize = sizeof(uint64_t);
constexpr uint64_t UnblockableSignals =
    (static_cast<uint64_t>(1) << (SIGKILL - 1)) | (static_cast<uint64_t>(1) << (SIGSTOP - 1));
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

size_t selectBitmapBytes(int nfds) {
  if (!nfds) {
    return 0;
  }
  return ((static_cast<size_t>(nfds) + SelectWordBits - 1) / SelectWordBits) * sizeof(uintptr_t);
}

bool selectBitIsSet(const uintptr_t* bitmap, size_t descriptor) {
  return bitmap[descriptor / SelectWordBits] &
         (static_cast<uintptr_t>(1) << (descriptor % SelectWordBits));
}

void setSelectBit(uintptr_t* bitmap, size_t descriptor) {
  bitmap[descriptor / SelectWordBits] |= static_cast<uintptr_t>(1) << (descriptor % SelectWordBits);
}

struct SelectBitmaps {
  explicit SelectBitmaps(size_t bitmapBytes)
      : storage(nullptr),
        readInput(nullptr),
        writeInput(nullptr),
        errorInput(nullptr),
        readResult(nullptr),
        writeResult(nullptr),
        errorResult(nullptr),
        wordCount(bitmapBytes / sizeof(uintptr_t)) {
    if (!wordCount) {
      return;
    }

    storage = new uintptr_t[wordCount * 6];
    readInput = storage;
    writeInput = readInput + wordCount;
    errorInput = writeInput + wordCount;
    readResult = errorInput + wordCount;
    writeResult = readResult + wordCount;
    errorResult = writeResult + wordCount;
    for (size_t i = 0; i < wordCount * 6; ++i) {
      storage[i] = 0;
    }
  }

  ~SelectBitmaps() {
    delete[] storage;
  }

  uintptr_t* storage;
  uintptr_t* readInput;
  uintptr_t* writeInput;
  uintptr_t* errorInput;
  uintptr_t* readResult;
  uintptr_t* writeResult;
  uintptr_t* errorResult;
  size_t wordCount;
};

bool importSelectTimeval(timeval* userTimeout, PollDeadline& deadline) {
  if (!userTimeout) {
    deadline = posix_poll_deadline(nullptr);
    return true;
  }

  timeval snapshot = {0, 0};
  if (!PosixSubsystem::copyFromUser(&snapshot, userTimeout, sizeof(snapshot))) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  if (snapshot.tv_sec < 0 || snapshot.tv_usec < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  const uint64_t microseconds = static_cast<uint64_t>(snapshot.tv_usec);
  const uint64_t additionalSeconds = microseconds / 1000000;
  const uint64_t seconds = static_cast<uint64_t>(snapshot.tv_sec);
  const uint64_t maximumSeconds = static_cast<uint64_t>(INT64_MAX);
  LinuxKernelTimespec normalized = {0, 0};
  if (seconds > maximumSeconds - additionalSeconds) {
    normalized.tv_sec = INT64_MAX;
    normalized.tv_nsec = static_cast<int64_t>(Time::Multiplier::Second - 1);
  } else {
    normalized.tv_sec = static_cast<int64_t>(seconds + additionalSeconds);
    normalized.tv_nsec = static_cast<int64_t>(microseconds % 1000000) *
                         static_cast<int64_t>(Time::Multiplier::Microsecond);
  }
  deadline = posix_poll_deadline(&normalized);
  return true;
}

bool importPselectTimespec(LinuxKernelTimespec* userTimeout, PollDeadline& deadline) {
  if (!userTimeout) {
    deadline = posix_poll_deadline(nullptr);
    return true;
  }

  LinuxKernelTimespec snapshot = {0, 0};
  if (!PosixSubsystem::copyFromUser(&snapshot, userTimeout, sizeof(snapshot))) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  if (snapshot.tv_sec < 0 || snapshot.tv_nsec < 0 ||
      snapshot.tv_nsec >= static_cast<int64_t>(Time::Multiplier::Second)) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  deadline = posix_poll_deadline(&snapshot);
  return true;
}

LinuxKernelTimespec selectTimeoutRemainder(const PollDeadline& deadline) {
  LinuxKernelTimespec remaining = {0, 0};
  if (deadline.type != PollDeadlineType::Finite) {
    return remaining;
  }

  const Time::Timestamp now = Time::getTicks();
  if (now < deadline.expires) {
    const Time::Timestamp nanoseconds = deadline.expires - now;
    remaining.tv_sec = static_cast<int64_t>(nanoseconds / Time::Multiplier::Second);
    remaining.tv_nsec = static_cast<int64_t>(nanoseconds % Time::Multiplier::Second);
  }
  return remaining;
}

void copySelectTimevalRemainder(timeval* userTimeout, const PollDeadline& deadline) {
  if (!userTimeout || deadline.type != PollDeadlineType::Finite) {
    return;
  }

  const LinuxKernelTimespec nanoseconds = selectTimeoutRemainder(deadline);
  timeval remaining = {
      static_cast<time_t>(nanoseconds.tv_sec),
      static_cast<suseconds_t>(nanoseconds.tv_nsec / Time::Multiplier::Microsecond),
  };
  PosixSubsystem::copyToUser(userTimeout, &remaining, sizeof(remaining));
}

void copyPselectTimespecRemainder(LinuxKernelTimespec* userTimeout, const PollDeadline& deadline) {
  if (!userTimeout || deadline.type != PollDeadlineType::Finite) {
    return;
  }

  const LinuxKernelTimespec remaining = selectTimeoutRemainder(deadline);
  PosixSubsystem::copyToUser(userTimeout, &remaining, sizeof(remaining));
}

bool importPselectSignalArgument(const LinuxPselectSigsetArgument* userArgument,
                                 LinuxPselectSigsetArgument& snapshot) {
  snapshot = {0, 0};
  if (!userArgument) {
    return true;
  }
  if (!PosixSubsystem::copyFromUser(&snapshot, userArgument, sizeof(snapshot))) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  return true;
}

bool importPselectSignalMask(const LinuxPselectSigsetArgument& argument, bool& hasTemporaryMask,
                             uint64_t& temporaryMask) {
  hasTemporaryMask = false;
  temporaryMask = 0;
  if (!argument.signalMask) {
    return true;
  }
  if (argument.signalMaskSize != LinuxKernelSigsetSize) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (!PosixSubsystem::copyFromUser(&temporaryMask,
                                    reinterpret_cast<const void*>(argument.signalMask),
                                    LinuxKernelSigsetSize)) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  temporaryMask &= ~UnblockableSignals;
  hasTemporaryMask = true;
  return true;
}

int selectWithDeadline(int nfds, fd_set* readfds, fd_set* writefds, fd_set* errorfds,
                       const PollDeadline& deadline) {
  if (nfds < 0 || nfds > MaximumSelectDescriptors) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  const size_t bitmapBytes = selectBitmapBytes(nfds);
  SelectBitmaps bitmaps(bitmapBytes);
  const bool copiedInputs =
      (!readfds || PosixSubsystem::copyFromUser(bitmaps.readInput, readfds, bitmapBytes)) &&
      (!writefds || PosixSubsystem::copyFromUser(bitmaps.writeInput, writefds, bitmapBytes)) &&
      (!errorfds || PosixSubsystem::copyFromUser(bitmaps.errorInput, errorfds, bitmapBytes));
  if (!copiedInputs) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  size_t selectedCount = 0;
  for (int descriptor = 0; descriptor < nfds; ++descriptor) {
    const size_t index = static_cast<size_t>(descriptor);
    if ((readfds && selectBitIsSet(bitmaps.readInput, index)) ||
        (writefds && selectBitIsSet(bitmaps.writeInput, index)) ||
        (errorfds && selectBitIsSet(bitmaps.errorInput, index))) {
      ++selectedCount;
    }
  }

  struct pollfd* descriptors = selectedCount ? new struct pollfd[selectedCount] : nullptr;
  size_t pollIndex = 0;
  for (int descriptor = 0; descriptor < nfds; ++descriptor) {
    const size_t index = static_cast<size_t>(descriptor);
    const bool checkRead = readfds && selectBitIsSet(bitmaps.readInput, index);
    const bool checkWrite = writefds && selectBitIsSet(bitmaps.writeInput, index);
    const bool checkError = errorfds && selectBitIsSet(bitmaps.errorInput, index);
    if (!(checkRead || checkWrite || checkError)) {
      continue;
    }

    descriptors[pollIndex].fd = descriptor;
    descriptors[pollIndex].events = 0;
    if (checkRead) {
      descriptors[pollIndex].events |= POLLIN;
    }
    if (checkWrite) {
      descriptors[pollIndex].events |= POLLOUT;
    }
    if (checkError) {
      descriptors[pollIndex].events |= POLLPRI;
    }
    descriptors[pollIndex].revents = 0;
    ++pollIndex;
  }

  int result = posix_poll_safe(descriptors, static_cast<unsigned int>(selectedCount), deadline);
  if (result >= 0) {
    for (size_t i = 0; i < selectedCount; ++i) {
      if (descriptors[i].revents & POLLNVAL) {
        SYSCALL_ERROR(BadFileDescriptor);
        result = -1;
        break;
      }
    }
  }

  int readyCount = 0;
  pollIndex = 0;
  for (int descriptor = 0; result >= 0 && descriptor < nfds; ++descriptor) {
    const size_t index = static_cast<size_t>(descriptor);
    const bool checkRead = readfds && selectBitIsSet(bitmaps.readInput, index);
    const bool checkWrite = writefds && selectBitIsSet(bitmaps.writeInput, index);
    const bool checkError = errorfds && selectBitIsSet(bitmaps.errorInput, index);
    if (!(checkRead || checkWrite || checkError)) {
      continue;
    }

    const SelectProjection projection =
        projectSelectReadiness(descriptors[pollIndex].revents, checkRead, checkWrite, checkError);
    if (projection.read) {
      setSelectBit(bitmaps.readResult, index);
    }
    if (projection.write) {
      setSelectBit(bitmaps.writeResult, index);
    }
    if (projection.exceptional) {
      setSelectBit(bitmaps.errorResult, index);
    }
    readyCount += projection.count;
    ++pollIndex;
  }

  delete[] descriptors;
  if (result < 0) {
    return result;
  }

  const bool copiedResults =
      (!readfds || PosixSubsystem::copyToUser(readfds, bitmaps.readResult, bitmapBytes)) &&
      (!writefds || PosixSubsystem::copyToUser(writefds, bitmaps.writeResult, bitmapBytes)) &&
      (!errorfds || PosixSubsystem::copyToUser(errorfds, bitmaps.errorResult, bitmapBytes));
  if (!copiedResults) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return readyCount;
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

#endif

int posix_select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* errorfds, timeval* timeout) {
  POLL_NOTICE("select(" << nfds << ", " << readfds << ", " << writefds << ", " << errorfds << ", "
                        << timeout << ")");
  PollDeadline deadline = {PollDeadlineType::Infinite, 0};
  if (!importSelectTimeval(timeout, deadline)) {
    return -1;
  }

  const int result = selectWithDeadline(nfds, readfds, writefds, errorfds, deadline);
  copySelectTimevalRemainder(timeout, deadline);
  POLL_NOTICE(" -> select via poll returns " << result);
  return result;
}

int posix_pselect6(int nfds, fd_set* readfds, fd_set* writefds, fd_set* errorfds,
                   LinuxKernelTimespec* timeout, const LinuxPselectSigsetArgument* signalArgument) {
  LinuxPselectSigsetArgument signalArgumentSnapshot = {0, 0};
  if (!importPselectSignalArgument(signalArgument, signalArgumentSnapshot)) {
    return -1;
  }

  PollDeadline deadline = {PollDeadlineType::Infinite, 0};
  if (!importPselectTimespec(timeout, deadline)) {
    return -1;
  }

  bool hasTemporaryMask = false;
  uint64_t temporaryMask = 0;
  if (!importPselectSignalMask(signalArgumentSnapshot, hasTemporaryMask, temporaryMask)) {
    return -1;
  }

  int result = 0;
  if (hasTemporaryMask) {
    Thread* thread = Processor::information().getCurrentThread();
    if (!thread) {
      FATAL("pselect6 has no current Thread.");
    }

    Thread::TemporarySignalMask signalWait(*thread, temporaryMask);
    result = selectWithDeadline(nfds, readfds, writefds, errorfds, deadline);
    const bool signalInterrupted = signalWait.finish();
    if (!result && signalInterrupted) {
      SYSCALL_ERROR(Interrupted);
      result = -1;
    }
  } else {
    result = selectWithDeadline(nfds, readfds, writefds, errorfds, deadline);
  }

  copyPselectTimespecRemainder(timeout, deadline);
  return result;
}
