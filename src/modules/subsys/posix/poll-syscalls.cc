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
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"

#include <fcntl.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/epoll-syscalls.h"
#include "modules/subsys/posix/eventfd-syscalls.h"
#include "modules/system/vfs/File.h"
#include "net-syscalls.h"
#include "poll-syscalls.h"

enum TimeoutType { ReturnImmediately, SpecificTimeout, InfiniteTimeout };

namespace {
constexpr unsigned int MaxPollDescriptors = 16384;

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
  const bool copied = PosixSubsystem::copyToUser(fds, snapshot, nfds, sizeof(struct pollfd));
  delete[] snapshot;
  if (!copied) {
    POLL_NOTICE(" -> result address became invalid");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return result;
}

int posix_poll_safe(struct pollfd* fds, unsigned int nfds, int timeout) {
  POLL_NOTICE("poll_safe(" << Dec << nfds << ", " << timeout << Hex << ")");

  // Readiness registrations retain the observer and semaphore used by this
  // call. A terminal request may wake the wait, but cleanup must unregister
  // every target before the syscall stack can be consumed.
  TerminationDeferral registrationLifetime;

  // Investigate the timeout parameter.
  TimeoutType timeoutType;
  size_t timeoutSecs = timeout / 1000;
  size_t timeoutUSecs = (timeout % 1000) * 1000;
  if (timeout < 0) {
    timeoutType = InfiniteTimeout;

    // Fix timeout to be truly infinite
    // (negative timeout may divide incorrectly)
    timeoutSecs = 0;
    timeoutUSecs = 0;
  } else if (timeout == 0) {
    timeoutType = ReturnImmediately;
  } else {
    timeoutType = SpecificTimeout;
  }
  const Time::Timestamp deadline =
      timeoutType == SpecificTimeout
          ? Time::getTicks() + static_cast<Time::Timestamp>(timeout) * Time::Multiplier::Millisecond
          : 0;

  Thread* pThread = nullptr;

  EMIT_IF(!THREADS) {
    // can't time out without threads
    timeoutType = ReturnImmediately;
  }
  else {
    // Grab the subsystem for this process
    pThread = Processor::information().getCurrentThread();
    Process* pProcess = pThread->getParent();
    PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem) {
      ERROR("No subsystem for this process!");
      return -1;
    }
  }

  bool bError = false;
  bool bWillReturnImmediately = (timeoutType == ReturnImmediately);

  SharedPointer<Semaphore> pSem = nullptr;
  SharedPointer<ReadinessObserver> readinessObserver;
  ReadinessSubscription* readinessSubscriptions = nullptr;

  EMIT_IF(THREADS) {
    // Can be interrupted while waiting for sem - EINTR.
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

  for (unsigned int i = 0; i < nfds; i++) {
    // Grab the pollfd structure.
    struct pollfd* me = &fds[i];
    me->revents = 0;
    if (me->fd < 0) {
      continue;
    }

    // valid fd?
    const bool acquired = acquireDescriptor(me->fd, descriptors[i]);
    DescriptorLease& pFd = descriptors[i];
    if (!acquired) {
      // Error - no such file descriptor.
      POLL_NOTICE("poll: no such file descriptor (" << Dec << me->fd << ")");
      me->revents |= POLLNVAL;
      bWillReturnImmediately = true;
      continue;
    }

    ReadinessSource* readinessSource = descriptorReadinessSource(*pFd);
    if (!readinessSource) {
      me->revents |= POLLNVAL;
      bWillReturnImmediately = true;
      continue;
    }

    me->revents |= queryDescriptorPoll(*pFd, me->events);
    if (me->revents) {
      bWillReturnImmediately = true;
    }

    EMIT_IF(THREADS) {
      if (!bWillReturnImmediately) {
        const ReadyMask interest = pollInterest(me->events);
        if (!readinessSource->subscribeReadiness(interest, readinessObserver,
                                                 readinessSubscriptions[i])) {
          me->revents |= POLLNVAL;
          bWillReturnImmediately = true;
        } else {
          // Subscription precedes the second snapshot, closing the only
          // transition window in which a level could otherwise be missed.
          me->revents |= queryDescriptorPoll(*pFd, me->events);
          if (me->revents) {
            bWillReturnImmediately = true;
          }
        }
      }
    }
  }

  EMIT_IF(THREADS) {
    // Grunt work is done, now time to cleanup.
    while (!bWillReturnImmediately && !bError) {
      POLL_NOTICE("    -> no fds ready yet, poll will block");

      // We got here because there is a specific or infinite timeout and
      // no FD was ready immediately.
      //
      // Every subscribed source raises this semaphore when its predicate may
      // have changed. The exact level is always recomputed after the wake.
      size_t waitSecs = timeoutSecs;
      size_t waitUSecs = timeoutUSecs;
      if (timeoutType == SpecificTimeout) {
        const Time::Timestamp now = Time::getTicks();
        if (now >= deadline) {
          break;
        }

        const Time::Timestamp remaining = deadline - now;
        waitSecs = remaining / Time::Multiplier::Second;
        waitUSecs = (remaining % Time::Multiplier::Second + Time::Multiplier::Microsecond - 1) /
                    Time::Multiplier::Microsecond;
        if (waitUSecs >= 1000000) {
          ++waitSecs;
          waitUSecs = 0;
        }
      }

      Semaphore::SemaphoreError error = Semaphore::NoError;
      bool acquired = pSem->acquireWithError(1, waitSecs, waitUSecs, error);

      // Did we actually get the semaphore or did we timeout?
      if (acquired) {
        // We were signalled, so one more FD ready.
        // While the semaphore is nonzero, more FDs are ready.
        while (pSem->tryAcquire())
          ;

        bool ok = false;
        for (size_t i = 0; i < nfds; ++i) {
          struct pollfd* me = &fds[i];
          DescriptorLease& pFd = descriptors[i];
          if (!pFd) {
            continue;
          }

          me->revents |= queryDescriptorPoll(*pFd, me->events);
          if (me->revents) {
            ok = true;
          }
        }

        if (ok) {
          break;
        }
      } else {
        if (error == Semaphore::TimedOut) {
          // timed out, not an error
          POLL_NOTICE(" -> poll interrupted by timeout");
        } else {
          // generic interrupt
          POLL_NOTICE(" -> poll interrupted by external event");
          SYSCALL_ERROR(Interrupted);
          bError = true;
        }

        break;
      }
    }
  }

  // Prepare return value (number of fds with events).
  size_t nRet = 0;
  for (size_t i = 0; i < nfds; ++i) {
    POLL_NOTICE("    -> pollfd[" << i << "]: fd=" << fds[i].fd << ", events=" << fds[i].events
                                 << ", revents=" << fds[i].revents);

    if (fds[i].revents != 0) {
      ++nRet;
    }
  }

  POLL_NOTICE("    -> " << Dec << ((bError) ? -1 : (int)nRet) << Hex);
  POLL_NOTICE("    -> nRet is " << nRet << ", error is " << bError);

  const int result = bError ? -1 : static_cast<int>(nRet);
  removePollRegistrations(&cleanup);
  return result;
}
