/* Copyright (c) 2026, Pedigree Developers. */
#include "mqueue-netlink.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include <errno.h>

namespace {
int unsupported() {
  SYSCALL_ERROR(OperationNotSupported);
  return -1;
}
}  // namespace

MqueueNetlinkSocket::MqueueNetlinkSocket(int type, int protocol)
    : NetworkSyscalls(16, type, protocol),
      m_Lock(),
      m_Changed(),
      m_Cookies(),
      m_Head(0),
      m_Count(0),
      m_Reserved(0),
      m_Closed(false),
      m_Generations() {}

MqueueNetlinkSocket::~MqueueNetlinkSocket() {
  lastDescriptorClosed();
}

bool MqueueNetlinkSocket::create() {
  if ((getType() != SOCK_RAW && getType() != SOCK_DGRAM) || getProtocol()) {
    SYSCALL_ERROR(ProtocolNotAvailable);
    return false;
  }
  return true;
}

bool MqueueNetlinkSocket::reserveCookie() {
  LockGuard<Mutex> guard(m_Lock);
  if (m_Closed) {
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }
  if (m_Count + m_Reserved == 64) {
    SYSCALL_ERROR(NoMoreBuffers);
    return false;
  }
  ++m_Reserved;
  return true;
}

void MqueueNetlinkSocket::deliverCookie(const uint8_t cookie[32], bool removed) {
  {
    LockGuard<Mutex> guard(m_Lock);
    assert(m_Reserved);
    --m_Reserved;
    if (m_Closed) {
      return;
    }
    uint8_t* target = m_Cookies[(m_Head + m_Count) % 64];
    MemoryCopy(target, cookie, 32);
    target[31] = removed ? 2 : 1;
    if (!m_Count++) {
      ++m_Generations.read;
    }
  }
  m_Changed.broadcast();
  notifyReadiness(ReadyRead);
}

void MqueueNetlinkSocket::lastDescriptorClosed() {
  if (!beginDescriptorClose()) {
    return;
  }
  {
    LockGuard<Mutex> guard(m_Lock);
    m_Closed = true;
  }
  m_Changed.broadcast();
  // beginDescriptorClose() makes the base destructor skip its close path.
  // Every derived close must therefore drain the inherited barrier itself.
  m_ReadinessNotifications.closeAndWait();
  closeReadiness();
}

bool MqueueNetlinkSocket::canPoll() const {
  return true;
}

ReadyMask MqueueNetlinkSocket::queryReady(bool reading, bool writing) {
  (void)writing;
  LockGuard<Mutex> guard(m_Lock);
  const ReadyMask ready = m_Closed             ? ReadyInvalid | ReadyHangup
                          : reading && m_Count ? ReadyRead
                                               : ReadyNone;
  return ready | pendingReceiveReadiness();
}

ReadinessGenerations MqueueNetlinkSocket::readinessGenerations() {
  LockGuard<Mutex> guard(m_Lock);
  return withReceiveErrorGeneration(m_Generations);
}

ssize_t MqueueNetlinkSocket::recvfrom_msg(struct msghdr* message,
                                          SharedPointer<SocketRights>* rights) {
  TerminationDeferral lifetime;
  if (rights) {
    rights->reset();
  }
  const int inputFlags = message->msg_flags;
  const int allowedFlags = MSG_DONTWAIT | MSG_NOSIGNAL | MSG_WAITALL | MSG_PEEK | MSG_TRUNC;
  if (inputFlags & ~allowedFlags) {
    return unsupported();
  }
  size_t capacity = 0;
  for (size_t n = 0; n < static_cast<size_t>(message->msg_iovlen); ++n) {
    capacity += message->msg_iov[n].iov_len;
  }
  m_Lock.acquire();
  while (!m_Count) {
    if (m_Closed) {
      m_Lock.release();
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    if (!isBlocking() || (inputFlags & MSG_DONTWAIT)) {
      m_Lock.release();
      SYSCALL_ERROR(NoMoreProcesses);
      return -1;
    }
    ConditionVariable::Error error = ConditionVariable::NoError;
    if (!m_Changed.wait(m_Lock, error)) {
      if (ConditionVariable::mutexAcquired(error)) {
        m_Lock.release();
      }
      SYSCALL_ERROR(Interrupted);
      return -1;
    }
  }
  const size_t length = capacity < 32 ? capacity : 32;
  size_t copied = 0;
  for (size_t n = 0; n < static_cast<size_t>(message->msg_iovlen) && copied < length; ++n) {
    const auto& vector = message->msg_iov[n];
    const size_t amount = vector.iov_len < length - copied ? vector.iov_len : length - copied;
    MemoryCopy(vector.iov_base, m_Cookies[m_Head] + copied, amount);
    copied += amount;
  }
  if (!(inputFlags & MSG_PEEK)) {
    m_Head = (m_Head + 1) % 64;
    --m_Count;
  }
  m_Lock.release();
  message->msg_flags = length < 32 ? MSG_TRUNC : 0;
  message->msg_controllen = 0;
  message->msg_namelen = 0;
  return inputFlags & MSG_TRUNC ? 32 : static_cast<ssize_t>(length);
}

int MqueueNetlinkSocket::connect(const struct sockaddr_storage*, socklen_t) {
  return unsupported();
}
ssize_t MqueueNetlinkSocket::sendto_msg(const struct msghdr*, const SharedPointer<SocketRights>&) {
  return unsupported();
}
int MqueueNetlinkSocket::listen(int) {
  return unsupported();
}
int MqueueNetlinkSocket::bind(const struct sockaddr_storage*, socklen_t) {
  return unsupported();
}
int MqueueNetlinkSocket::accept(struct sockaddr_storage*, socklen_t*, int, DescriptorLease*) {
  return unsupported();
}
int MqueueNetlinkSocket::shutdown(int) {
  return unsupported();
}
int MqueueNetlinkSocket::getpeername(struct sockaddr_storage*, socklen_t*) {
  SYSCALL_ERROR(NotConnected);
  return -1;
}
int MqueueNetlinkSocket::getsockname(struct sockaddr_storage* address, socklen_t* length) {
  struct NetlinkAddress {
    uint16_t family, padding;
    uint32_t pid, groups;
  } result = {16, 0, 0, 0};
  const size_t amount = *length < sizeof(result) ? *length : sizeof(result);
  MemoryCopy(address, &result, amount);
  *length = sizeof(result);
  return 0;
}
int MqueueNetlinkSocket::setsockopt(int, int, const void*, socklen_t) {
  SYSCALL_ERROR(ProtocolNotAvailable);
  return -1;
}
int MqueueNetlinkSocket::getsockopt(int level, int option, void* value, socklen_t* length) {
  if (level == SOL_SOCKET && (option == SO_TYPE || option == SO_ERROR) && *length >= sizeof(int)) {
    const int result = option == SO_TYPE ? getType() : 0;
    MemoryCopy(value, &result, sizeof(result));
    *length = sizeof(result);
    return 0;
  }
  SYSCALL_ERROR(ProtocolNotAvailable);
  return -1;
}
