/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"

#include <errno.h>
#include <limits.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "net-syscalls.h"
#include "poll-syscalls.h"
#include "recvmmsg-syscalls.h"

namespace {
class ReceiveObserver final : public ReadinessObserver {
 public:
  explicit ReceiveObserver(const SharedPointer<Semaphore>& semaphore) : m_Semaphore(semaphore) {}
  void readinessChanged(ReadyMask) override {
    m_Semaphore->release();
  }

 private:
  SharedPointer<Semaphore> m_Semaphore;
};

struct ReceiveResult {
  int count;
  int error;
};

bool deadlineExpired(const PollDeadline& deadline) {
  return deadline.type == PollDeadlineType::Immediate ||
         (deadline.type == PollDeadlineType::Finite && Time::getTicks() >= deadline.expires);
}

// The socket registration outlives every receive attempt and wakeup. Numeric
// descriptor reuse cannot redirect either the predicate or its cleanup.
class ReceiveWait {
 public:
  bool prepare(NetworkSyscalls& socket) {
    if (m_Subscription)
      return true;
    m_Semaphore = SharedPointer<Semaphore>::tryAllocate(0, true);
    if (!m_Semaphore)
      return false;
    m_Observer = SharedPointer<ReadinessObserver>::tryAdopt(new ReceiveObserver(m_Semaphore));
    return m_Observer && socket.subscribeReadiness(
                             ReadyRead | ReadyError | ReadyHangup | ReadyReadHangup | ReadyInvalid,
                             m_Observer, m_Subscription);
  }

  int wait(NetworkSyscalls& socket, const PollDeadline& deadline, Thread& thread) {
    for (;;) {
      if (socket.queryReady(true, false) &
          (ReadyRead | ReadyError | ReadyHangup | ReadyReadHangup | ReadyInvalid))
        return 1;
      if (thread.getInterruptionReason() == Thread::InterruptedBySignal)
        return -1;
      if (deadlineExpired(deadline))
        return 0;

      size_t seconds = 0, microseconds = 0;
      if (deadline.type == PollDeadlineType::Finite) {
        const uint64_t now = Time::getTicks();
        if (now >= deadline.expires)
          return 0;
        const uint64_t remaining = deadline.expires - now;
        const uint64_t rounded = remaining / Time::Multiplier::Microsecond +
                                 (remaining % Time::Multiplier::Microsecond != 0);
        seconds = static_cast<size_t>(rounded / 1000000);
        microseconds = static_cast<size_t>(rounded % 1000000);
      }
      Semaphore::SemaphoreError error = Semaphore::NoError;
      const bool signalled = m_Semaphore->acquireWithError(1, seconds, microseconds, error);
      if (signalled) {
        while (m_Semaphore->tryAcquire()) {
        }
      } else if (error != Semaphore::TimedOut) {
        // A final readiness query lets already-received data win a signal race.
        return socket.queryReady(true, false) &
                       (ReadyRead | ReadyError | ReadyHangup | ReadyReadHangup | ReadyInvalid)
                   ? 1
                   : -1;
      }
    }
  }

 private:
  SharedPointer<Semaphore> m_Semaphore;
  SharedPointer<ReadinessObserver> m_Observer;
  ReadinessSubscription m_Subscription;
};

ReceiveResult receiveMessages(int fd, LinuxMmsghdr* messages, unsigned int count,
                              unsigned int flags, LinuxKernelTimespec* timeout) {
  TerminationDeferral lifetime;
  Thread& thread = *Processor::information().getCurrentThread();
  thread.clearInterruption();
  LinuxKernelTimespec duration = {};
  if (timeout) {
    if (!PosixSubsystem::copyFromUser(&duration, timeout, sizeof(duration)))
      return {-1, EFAULT};
    if (duration.tv_sec < 0 || duration.tv_nsec < 0 ||
        duration.tv_nsec >= static_cast<int64_t>(Time::Multiplier::Second))
      return {-1, EINVAL};
  }
  const PollDeadline deadline = posix_poll_deadline(timeout ? &duration : nullptr);
  auto* subsystem = static_cast<PosixSubsystem*>(thread.getParent()->getSubsystem());
  DescriptorLease descriptor;
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor))
    return {-1, EBADF};
  if (!descriptor->networkImpl)
    return {-1, ENOTSOCK};
  NetworkSyscalls& socket = *descriptor->networkImpl;
  const int deferred = socket.takeReceiveError();
  if (deferred)
    return {-1, deferred};
  if (!count)
    return {0, 0};
  if (count > static_cast<unsigned int>(INT_MAX))
    return {-1, EINVAL};

  ReceiveWait wait;
  unsigned int received = 0;
  int error = 0;
  bool interrupted = false;
  uintptr_t address = reinterpret_cast<uintptr_t>(messages);
  while (received < count) {
    if (address > ~uintptr_t(0) - sizeof(LinuxMmsghdr)) {
      error = EFAULT;
      break;
    }
    auto* message = reinterpret_cast<LinuxMmsghdr*>(address);
    const int receiveFlags = static_cast<int>(flags & ~MSG_WAITFORONE) | MSG_DONTWAIT;
    interrupted |= thread.getInterruptionReason() == Thread::InterruptedBySignal;
    syscallError(0);
    const ssize_t bytes = posix_recvmsg_user_descriptor(descriptor, &message->msg_hdr, receiveFlags,
                                                        &message->msg_len);
    if (bytes >= 0) {
      ++received;
      address += sizeof(LinuxMmsghdr);
      if (timeout && deadlineExpired(deadline))
        break;
      continue;
    }
    error = thread.getErrno();
    if (!error)
      error = EIO;
    if (error != EAGAIN)
      break;
    if (interrupted) {
      error = EINTR;
      break;
    }
    if ((flags & MSG_DONTWAIT) || !socket.isBlocking() || (received && (flags & MSG_WAITFORONE)))
      break;
    if (deadlineExpired(deadline)) {
      error = 0;
      break;
    }
    const ReadyMask readyState = socket.queryReady(true, false);
    if (readyState & ReadyInvalid) {
      error = EBADF;
      break;
    }
    if (readyState & ReadyError) {
      int socketError = 0;
      socklen_t length = sizeof(socketError);
      if (socket.getsockopt(SOL_SOCKET, SO_ERROR, &socketError, &length) < 0) {
        error = thread.getErrno() ? thread.getErrno() : EIO;
        break;
      }
      if (socketError) {
        error = socketError;
        break;
      }
    }
    if (!wait.prepare(socket)) {
      error = ENOMEM;
      break;
    }
    const int ready = wait.wait(socket, deadline, thread);
    if (ready <= 0) {
      error = ready ? EINTR : 0;
      break;
    }
  }

  if (received && error && error != EAGAIN)
    socket.deferReceiveError(error);
  if (timeout && received) {
    LinuxKernelTimespec remaining = {};
    if (deadline.type == PollDeadlineType::Finite) {
      const uint64_t now = Time::getTicks();
      if (now < deadline.expires) {
        const uint64_t nanoseconds = deadline.expires - now;
        remaining.tv_sec = nanoseconds / Time::Multiplier::Second;
        remaining.tv_nsec = nanoseconds % Time::Multiplier::Second;
      }
    }
    if (!PosixSubsystem::copyToUser(timeout, &remaining, sizeof(remaining)))
      return {-1, EFAULT};
  }
  if (received || !error) {
    thread.clearInterruption();
    return {static_cast<int>(received), 0};
  }
  return {-1, error};
}
}  // namespace

int posix_recvmmsg(int fd, LinuxMmsghdr* messages, unsigned int count, unsigned int flags,
                   LinuxKernelTimespec* timeout) {
  if (flags & 0x80000000U) {
    syscallError(EINVAL);
    return -1;
  }
  const ReceiveResult result = receiveMessages(fd, messages, count, flags, timeout);
  syscallError(result.error);
  return result.count;
}
