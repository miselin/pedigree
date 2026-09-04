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
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"

#include <fcntl.h>
#include <signal.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/epoll-syscalls.h"
#include "modules/subsys/posix/eventfd-syscalls.h"
#include "modules/system/vfs/File.h"
#include "net-syscalls.h"
#include "poll-syscalls.h"

namespace {
constexpr unsigned int MaxPollDescriptors = 16384;
constexpr size_t LinuxKernelSigsetSize = sizeof(uint64_t);
constexpr uint64_t UnblockableSignals =
    (static_cast<uint64_t>(1) << (SIGKILL - 1)) | (static_cast<uint64_t>(1) << (SIGSTOP - 1));

PollDeadline pollDeadlineAfter(Time::Timestamp duration) {
  if (!duration) {
    return {PollDeadlineType::Immediate, 0};
  }

  const Time::Timestamp now = Time::getTicks();
  const Time::Timestamp maximum = Time::Infinity - 1;
  const Time::Timestamp expires =
      now >= maximum || duration > maximum - now ? maximum : now + duration;
  return {PollDeadlineType::Finite, expires};
}

PollDeadline pollDeadlineFromMilliseconds(int timeout) {
  if (timeout < 0) {
    return {PollDeadlineType::Infinite, 0};
  }

  return pollDeadlineAfter(static_cast<Time::Timestamp>(timeout) * Time::Multiplier::Millisecond);
}

PollDeadline pollDeadlineFromTimespec(const LinuxKernelTimespec& timeout) {
  const Time::Timestamp maximum = Time::Infinity - 1;
  Time::Timestamp duration = maximum;
  if (static_cast<uint64_t>(timeout.tv_sec) <= maximum / Time::Multiplier::Second) {
    duration = static_cast<Time::Timestamp>(timeout.tv_sec) * Time::Multiplier::Second;
    const Time::Timestamp nanoseconds = static_cast<Time::Timestamp>(timeout.tv_nsec);
    duration = nanoseconds > maximum - duration ? maximum : duration + nanoseconds;
  }
  return pollDeadlineAfter(duration);
}

bool deadlineSemaphoreTimeout(const PollDeadline& deadline, size_t& seconds, size_t& microseconds) {
  seconds = 0;
  microseconds = 0;
  if (deadline.type == PollDeadlineType::Infinite) {
    return true;
  }
  if (deadline.type == PollDeadlineType::Immediate) {
    return false;
  }

  const Time::Timestamp now = Time::getTicks();
  if (now >= deadline.expires) {
    return false;
  }

  const Time::Timestamp remaining = deadline.expires - now;
  const Time::Timestamp remainingMicroseconds =
      (remaining / Time::Multiplier::Microsecond) +
      ((remaining % Time::Multiplier::Microsecond) ? 1 : 0);
  const Time::Timestamp microsecondsPerSecond =
      Time::Multiplier::Second / Time::Multiplier::Microsecond;
  const Time::Timestamp wholeSeconds = remainingMicroseconds / microsecondsPerSecond;
  const size_t maximumSeconds = ~static_cast<size_t>(0);
  if (wholeSeconds > maximumSeconds) {
    // A shorter saturated chunk is safe: the absolute deadline is checked
    // again after this timer wakes.
    seconds = maximumSeconds;
    microseconds = 0;
  } else {
    seconds = static_cast<size_t>(wholeSeconds);
    microseconds = static_cast<size_t>(remainingMicroseconds % microsecondsPerSecond);
  }
  return true;
}

class PollReadinessObserver final : public ReadinessObserver {
 public:
  explicit PollReadinessObserver(const SharedPointer<Semaphore>& semaphore)
      : m_Semaphore(semaphore) {}

  void readinessChanged(ReadyMask) override {
    m_Semaphore->release();
  }

 private:
  SharedPointer<Semaphore> m_Semaphore;
};

ReadyMask pollInterest(short events) {
  ReadyMask interest = ReadyError;
  if (events & (POLLIN | POLLRDNORM)) {
    interest |= ReadyRead;
  }
  if (events & (POLLPRI | POLLRDBAND)) {
    interest |= ReadyPriority;
  }
  if (events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
    interest |= ReadyWrite;
  }
#ifdef POLLRDHUP
  if (events & POLLRDHUP) {
    interest |= ReadyReadHangup;
  }
#endif
  return interest;
}

short readyMaskToPoll(ReadyMask ready, short events) {
  short result = 0;

  if (ready & ReadyRead) {
    result |= events & (POLLIN | POLLRDNORM);
  }
  if (ready & ReadyPriority) {
    result |= events & (POLLPRI | POLLRDBAND);
  }
  if (ready & ReadyWrite) {
    result |= events & (POLLOUT | POLLWRNORM | POLLWRBAND);
  }
  if (ready & ReadyError) {
    result |= POLLERR;
  }
  if (ready & ReadyHangup) {
    result |= POLLHUP;
  }
#ifdef POLLRDHUP
  if ((ready & ReadyReadHangup) && (events & POLLRDHUP)) {
    result |= POLLRDHUP;
  }
#endif
  if (ready & ReadyInvalid) {
    result |= POLLNVAL;
  }
  return result;
}

short queryDescriptorPoll(const FileDescriptor& descriptor, short events) {
  if (descriptor.epollImpl) {
    return readyMaskToPoll(descriptor.epollImpl->queryReady(), events);
  }
  SharedPointer<EventFd> eventFd = descriptor.getEventFdImpl();
  if (eventFd) {
    return readyMaskToPoll(eventFd->queryReady(), events);
  }
  if (descriptor.file) {
    const int accessMode = descriptor.getStatusFlags() & O_ACCMODE;
    const bool canRead = accessMode != O_WRONLY;
    const bool canWrite = accessMode != O_RDONLY;
    return readyMaskToPoll(descriptor.file->queryReady(canRead, canWrite), events);
  }

  if (descriptor.networkImpl) {
    const bool checkRead = events & (POLLIN | POLLRDNORM | POLLPRI | POLLRDBAND);
    const bool checkWrite = events & (POLLOUT | POLLWRNORM | POLLWRBAND);
    return readyMaskToPoll(descriptor.networkImpl->queryReady(checkRead, checkWrite), events);
  }

  return 0;
}

ReadinessSource* descriptorReadinessSource(const FileDescriptor& descriptor) {
  if (descriptor.epollImpl) {
    return descriptor.epollImpl.get();
  }
  SharedPointer<EventFd> eventFd = descriptor.getEventFdImpl();
  if (eventFd) {
    return eventFd.get();
  }
  if (descriptor.file) {
    return descriptor.file;
  }
  if (descriptor.networkImpl) {
    return descriptor.networkImpl.get();
  }
  return nullptr;
}

struct PollCleanupContext {
  SharedPointer<Semaphore>* semaphore;
  SharedPointer<ReadinessObserver>* readinessObserver;
  ReadinessSubscription** readinessSubscriptions;
  DescriptorLease** descriptors;
  size_t descriptorCount;
  bool active;
};

void removeReadinessSubscriptions(PollCleanupContext& cleanup) {
  ReadinessSubscription* subscriptions = *cleanup.readinessSubscriptions;
  if (!subscriptions) {
    return;
  }

  *cleanup.readinessSubscriptions = nullptr;
  for (size_t i = 0; i < cleanup.descriptorCount; ++i) {
    subscriptions[i].reset();
  }
  delete[] subscriptions;
  cleanup.readinessObserver->reset();
}

void removePollRegistrations(void* context) {
  PollCleanupContext* cleanup = reinterpret_cast<PollCleanupContext*>(context);
  if (!cleanup->active) {
    return;
  }
  cleanup->active = false;

  removeReadinessSubscriptions(*cleanup);

  DescriptorLease* descriptors = *cleanup->descriptors;
  *cleanup->descriptors = nullptr;
  delete[] descriptors;
  cleanup->semaphore->reset();
}

bool copyPollReventsToUser(struct pollfd* userFds, const struct pollfd* snapshot, size_t nfds) {
  // fd and events remain concurrent user-owned input fields.
  for (size_t i = 0; i < nfds; ++i) {
    if (!PosixSubsystem::copyToUser(&userFds[i].revents, &snapshot[i].revents,
                                    sizeof(snapshot[i].revents))) {
      return false;
    }
  }
  return true;
}
}  // namespace

/** poll: determine if a set of file descriptors are writable/readable.
 *
 *  Permits any number of descriptors, unlike select().
 */
int posix_poll(struct pollfd* fds, unsigned int nfds, int timeout) {
  POLL_NOTICE("poll(" << Dec << nfds << ", " << timeout << Hex << ")");
  if (nfds > MaxPollDescriptors) {
    POLL_NOTICE(" -> too many descriptors");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  size_t extent = 0;
  if (!PosixSubsystem::checkedUserBufferSize(nfds, sizeof(struct pollfd), extent)) {
    POLL_NOTICE(" -> descriptor array size overflow");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  struct pollfd* snapshot = nfds ? new struct pollfd[nfds] : nullptr;
  if (!PosixSubsystem::copyFromUser(snapshot, fds, nfds, sizeof(struct pollfd))) {
    POLL_NOTICE(" -> invalid address");
    delete[] snapshot;
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  const int result = posix_poll_safe(snapshot, nfds, timeout);

  // posix_poll_safe has retired every registration before it returns, so no
  // callback can retain a pointer into this snapshot during copyout.
  const bool copied = copyPollReventsToUser(fds, snapshot, nfds);
  delete[] snapshot;
  if (!copied) {
    POLL_NOTICE(" -> result address became invalid");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return result;
}

namespace {
bool refreshPollDescriptors(struct pollfd* fds, size_t nfds, DescriptorLease* descriptors) {
  bool ready = false;
  for (size_t i = 0; i < nfds; ++i) {
    struct pollfd* me = &fds[i];
    DescriptorLease& descriptor = descriptors[i];
    if (descriptor) {
      me->revents |= queryDescriptorPoll(*descriptor, me->events);
    }
    ready |= me->revents != 0;
  }
  return ready;
}

int pollWithDeadline(struct pollfd* fds, unsigned int nfds, const PollDeadline& deadline) {
  // Readiness registrations retain the observer and semaphore used by this
  // call. A terminal request may wake the wait, but cleanup must unregister
  // every target before the syscall stack can be consumed.
  TerminationDeferral registrationLifetime;

  Thread* pThread = nullptr;
  bool returnImmediately = deadline.type == PollDeadlineType::Immediate;
  EMIT_IF(!THREADS) {
    // No scheduler is available to complete a blocking wait.
    returnImmediately = true;
  }
  else {
    pThread = Processor::information().getCurrentThread();
    Process* pProcess = pThread->getParent();
    PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem) {
      ERROR("No subsystem for this process!");
      return -1;
    }
  }

  SharedPointer<Semaphore> pSem = nullptr;
  SharedPointer<ReadinessObserver> readinessObserver;
  ReadinessSubscription* readinessSubscriptions = nullptr;
  EMIT_IF(THREADS) {
    pSem.reset(new Semaphore(0, true));
    readinessObserver.reset(new PollReadinessObserver(pSem));
    readinessSubscriptions = new ReadinessSubscription[nfds];
  }

  // Keep the exact descriptor generation used during registration pinned
  // until every file event and socket waiter has been removed. Re-looking up
  // the numeric fd during wakeup or cleanup could target a reused descriptor
  // and leave a registration pointing into this stack behind.
  DescriptorLease* descriptors = new DescriptorLease[nfds];
  PollCleanupContext cleanup = {
      &pSem, &readinessObserver, &readinessSubscriptions, &descriptors, nfds, true};
  Thread::StackDiscardScope discardScope(THREADS ? &removePollRegistrations : nullptr, &cleanup);

  for (unsigned int i = 0; i < nfds; ++i) {
    struct pollfd* me = &fds[i];
    me->revents = 0;
    if (me->fd < 0) {
      continue;
    }

    const bool acquired = acquireDescriptor(me->fd, descriptors[i]);
    DescriptorLease& descriptor = descriptors[i];
    if (!acquired) {
      POLL_NOTICE("poll: no such file descriptor (" << Dec << me->fd << ")");
      me->revents |= POLLNVAL;
      returnImmediately = true;
      continue;
    }

    ReadinessSource* readinessSource = descriptorReadinessSource(*descriptor);
    if (!readinessSource) {
      me->revents |= POLLNVAL;
      returnImmediately = true;
      continue;
    }

    me->revents |= queryDescriptorPoll(*descriptor, me->events);
    if (me->revents) {
      returnImmediately = true;
    }

    EMIT_IF(THREADS) {
      if (!returnImmediately) {
        const ReadyMask interest = pollInterest(me->events);
        if (!readinessSource->subscribeReadiness(interest, readinessObserver,
                                                 readinessSubscriptions[i])) {
          me->revents |= POLLNVAL;
          returnImmediately = true;
        } else {
          // Subscription precedes the second snapshot, closing the only
          // transition window in which a level could otherwise be missed.
          me->revents |= queryDescriptorPoll(*descriptor, me->events);
          if (me->revents) {
            returnImmediately = true;
          }
        }
      }
    }
  }

  bool waited = false;
  bool interrupted = false;
  bool timedOut = deadline.type == PollDeadlineType::Immediate;
  EMIT_IF(THREADS) {
    while (!returnImmediately) {
      POLL_NOTICE("    -> no fds ready yet, poll will block");

      size_t waitSecs = 0;
      size_t waitUSecs = 0;
      if (!deadlineSemaphoreTimeout(deadline, waitSecs, waitUSecs)) {
        timedOut = true;
        break;
      }

      Semaphore::SemaphoreError error = Semaphore::NoError;
      waited = true;
      const bool acquired = pSem->acquireWithError(1, waitSecs, waitUSecs, error);
      if (acquired) {
        while (pSem->tryAcquire())
          ;
      }

      // Recompute the predicate after every wake outcome. A readiness change
      // racing a timeout or signal owns the result even if the semaphore
      // reported the other event first.
      if (refreshPollDescriptors(fds, nfds, descriptors)) {
        break;
      }

      if (!acquired) {
        if (error == Semaphore::TimedOut) {
          POLL_NOTICE(" -> poll interrupted by timeout");
          timedOut = true;
        } else {
          POLL_NOTICE(" -> poll interrupted by external event");
          interrupted = true;
        }
        break;
      }

      if (pThread->getInterruptionReason() == Thread::InterruptedBySignal) {
        interrupted = true;
        break;
      }
    }
  }

  // One last level snapshot closes a change racing the terminal decision.
  refreshPollDescriptors(fds, nfds, descriptors);
  if (waited && pThread->getInterruptionReason() == Thread::InterruptedBySignal) {
    interrupted = true;
  }

  size_t readyCount = 0;
  for (size_t i = 0; i < nfds; ++i) {
    POLL_NOTICE("    -> pollfd[" << i << "]: fd=" << fds[i].fd << ", events=" << fds[i].events
                                 << ", revents=" << fds[i].revents);
    if (fds[i].revents != 0) {
      ++readyCount;
    }
  }

  int result = static_cast<int>(readyCount);
  if (!readyCount && interrupted) {
    SYSCALL_ERROR(Interrupted);
    result = -1;
  } else if (readyCount && interrupted &&
             pThread->getInterruptionReason() == Thread::InterruptedBySignal) {
    // The handler ran, but readiness won this syscall's terminal collision.
    pThread->clearInterruption();
  }

  POLL_NOTICE("    -> " << Dec << result << Hex);
  POLL_NOTICE("    -> ready is " << readyCount << ", interrupted is " << interrupted
                                 << ", timed out is " << timedOut);

  removePollRegistrations(&cleanup);
  return result;
}

void copyPpollTimeoutRemainder(LinuxKernelTimespec* userTimeout, const PollDeadline& deadline) {
  if (deadline.type != PollDeadlineType::Finite) {
    return;
  }

  LinuxKernelTimespec remaining = {0, 0};
  const Time::Timestamp now = Time::getTicks();
  if (now < deadline.expires) {
    const Time::Timestamp nanoseconds = deadline.expires - now;
    remaining.tv_sec = static_cast<int64_t>(nanoseconds / Time::Multiplier::Second);
    remaining.tv_nsec = static_cast<int64_t>(nanoseconds % Time::Multiplier::Second);
  }

  // Linux treats timeout writeback as advisory: a late output fault does not
  // replace the poll result or a descriptor-copy EFAULT.
  PosixSubsystem::copyToUser(userTimeout, &remaining, sizeof(remaining));
}

bool finishPpoll(Thread::TemporarySignalMask* temporarySignalMask, LinuxKernelTimespec* userTimeout,
                 const PollDeadline& deadline) {
  const bool signalInterrupted = temporarySignalMask && temporarySignalMask->finish();
  if (userTimeout) {
    copyPpollTimeoutRemainder(userTimeout, deadline);
  }
  return signalInterrupted;
}

int ppollWithDeadline(struct pollfd* fds, unsigned int nfds, LinuxKernelTimespec* timeout,
                      const PollDeadline& deadline,
                      Thread::TemporarySignalMask* temporarySignalMask) {
  if (nfds > MaxPollDescriptors) {
    finishPpoll(temporarySignalMask, timeout, deadline);
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  size_t extent = 0;
  if (!PosixSubsystem::checkedUserBufferSize(nfds, sizeof(struct pollfd), extent)) {
    finishPpoll(temporarySignalMask, timeout, deadline);
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  struct pollfd* snapshot = nfds ? new struct pollfd[nfds] : nullptr;
  if (!PosixSubsystem::copyFromUser(snapshot, fds, nfds, sizeof(struct pollfd))) {
    delete[] snapshot;
    finishPpoll(temporarySignalMask, timeout, deadline);
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  int result = pollWithDeadline(snapshot, nfds, deadline);
  // Linux publishes revents while the temporary mask still governs signal
  // delivery. Timeout writeback happens only after the caller's mask is
  // restored.
  const bool copiedResults = copyPollReventsToUser(fds, snapshot, nfds);
  const bool signalInterrupted = finishPpoll(temporarySignalMask, timeout, deadline);
  if (!result && signalInterrupted) {
    SYSCALL_ERROR(Interrupted);
    result = -1;
  }

  delete[] snapshot;

  if (!copiedResults) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return result;
}
}  // namespace

PollDeadline posix_poll_deadline(const LinuxKernelTimespec* timeout) {
  if (!timeout) {
    return {PollDeadlineType::Infinite, 0};
  }
  return pollDeadlineFromTimespec(*timeout);
}

int posix_poll_safe(struct pollfd* fds, unsigned int nfds, const PollDeadline& deadline) {
  POLL_NOTICE("poll_safe_deadline(" << Dec << nfds << Hex << ")");
  return pollWithDeadline(fds, nfds, deadline);
}

int posix_poll_safe(struct pollfd* fds, unsigned int nfds, int timeout) {
  POLL_NOTICE("poll_safe(" << Dec << nfds << ", " << timeout << Hex << ")");
  return pollWithDeadline(fds, nfds, pollDeadlineFromMilliseconds(timeout));
}

int posix_ppoll(struct pollfd* fds, unsigned int nfds, LinuxKernelTimespec* timeout,
                const uint64_t* signalMask, size_t signalMaskSize) {
  LinuxKernelTimespec timeoutSnapshot = {0, 0};
  const LinuxKernelTimespec* timeoutValue = nullptr;
  if (timeout) {
    if (!PosixSubsystem::copyFromUser(&timeoutSnapshot, timeout, sizeof(timeoutSnapshot))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (timeoutSnapshot.tv_sec < 0 || timeoutSnapshot.tv_nsec < 0 ||
        timeoutSnapshot.tv_nsec >= static_cast<int64_t>(Time::Multiplier::Second)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    timeoutValue = &timeoutSnapshot;
  }
  const PollDeadline deadline = posix_poll_deadline(timeoutValue);

  uint64_t temporarySignalMask = 0;
  if (signalMask) {
    if (signalMaskSize != LinuxKernelSigsetSize) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (!PosixSubsystem::copyFromUser(&temporarySignalMask, signalMask, LinuxKernelSigsetSize)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    temporarySignalMask &= ~UnblockableSignals;
    Thread* thread = Processor::information().getCurrentThread();
    if (!thread) {
      FATAL("ppoll has no current Thread.");
    }

    Thread::TemporarySignalMask signalWait(*thread, temporarySignalMask);
    return ppollWithDeadline(fds, nfds, timeout, deadline, &signalWait);
  }

  return ppollWithDeadline(fds, nfds, timeout, deadline, nullptr);
}
