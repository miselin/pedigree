/* Copyright (c) 2026, Pedigree Developers. */
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include <errno.h>

#include "net-syscalls.h"
#include "network-namespace.h"

namespace {
struct Header {
  uint32_t length;
  uint16_t type, flags;
  uint32_t sequence, pid;
};
struct Address {
  uint16_t family, padding;
  uint32_t pid, groups;
};
struct Reply {
  Header header;
  int32_t error;
  Header request;
};
static_assert(sizeof(Header) == 16 && sizeof(Reply) == 36, "Linux netlink ABI");
int unsupported() {
  syscallError(EOPNOTSUPP);
  return -1;
}
uint32_t currentPid() {
  auto* process = Processor::information().getCurrentThread()->getParent();
  return process->getUserspaceId(process->pidNamespace().get());
}

class RouteSocket final : public NetworkSyscalls {
 public:
  RouteSocket(int type, int protocol, const NetworkNamespaceRef& space)
      : NetworkSyscalls(16, type, protocol) {
    m_NetworkNamespace = space;
  }
  ~RouteSocket() override {
    lastDescriptorClosed();
  }
  bool create() override {
    if ((getType() != SOCK_RAW && getType() != SOCK_DGRAM) || getProtocol()) {
      syscallError(EPROTONOSUPPORT);
      return false;
    }
    return true;
  }
  int bind(const sockaddr_storage* address, socklen_t length) override {
    if (length < sizeof(Address) || address->ss_family != 16) {
      syscallError(EINVAL);
      return -1;
    }
    Address value;
    MemoryCopy(&value, address, sizeof(value));
    if (value.groups) {
      syscallError(EOPNOTSUPP);
      return -1;
    }
    LockGuard<Mutex> guard(m_Lock);
    if (m_Closed) {
      syscallError(EBADF);
      return -1;
    }
    if (m_Bound) {
      syscallError(EINVAL);
      return -1;
    }
    m_Pid =
        m_NetworkNamespace->reserveRoutePort(value.pid ? value.pid : currentPid(), value.pid != 0);
    if (!m_Pid) {
      return -1;
    }
    m_Bound = true;
    return 0;
  }
  ssize_t sendto_msg(const msghdr* message, const SharedPointer<SocketRights>& rights) override {
    if (rights || message->msg_flags) {
      return unsupported();
    }
    if (message->msg_name) {
      Address destination;
      if (message->msg_namelen < sizeof(destination)) {
        syscallError(EINVAL);
        return -1;
      }
      MemoryCopy(&destination, message->msg_name, sizeof(destination));
      if (destination.family != 16 || destination.pid || destination.groups) {
        return unsupported();
      }
    }
    uint8_t bytes[4096];
    size_t length = 0;
    for (size_t i = 0; i < static_cast<size_t>(message->msg_iovlen); ++i) {
      const auto& vector = message->msg_iov[i];
      if (vector.iov_len > sizeof(bytes) - length) {
        syscallError(EMSGSIZE);
        return -1;
      }
      MemoryCopy(bytes + length, vector.iov_base, vector.iov_len);
      length += vector.iov_len;
    }
    Header header;
    if (length < sizeof(header)) {
      syscallError(EINVAL);
      return -1;
    }
    MemoryCopy(&header, bytes, sizeof(header));
    if (header.length < sizeof(header) || header.length > length || length - header.length > 3 ||
        !(header.flags & 1)) {
      syscallError(EINVAL);
      return -1;
    }
    {
      LockGuard<Mutex> guard(m_Lock);
      if (m_Closed) {
        syscallError(EBADF);
        return -1;
      }
      if (m_Count == 16) {
        syscallError(ENOBUFS);
        return -1;
      }
      if (!m_Bound) {
        m_Pid = m_NetworkNamespace->reserveRoutePort(currentPid(), false);
        if (!m_Pid) {
          return -1;
        }
        m_Bound = true;
      }
      const int error = request(header, bytes + sizeof(header), header.length - sizeof(header));
      if (error || (header.flags & 4)) {
        Reply& reply = m_Replies[(m_Head + m_Count) % 16];
        reply = {{sizeof(Reply), 2, 0, header.sequence, m_Pid}, -error, header};
        if (!m_Count++) {
          ++m_Generations.read;
        }
      }
    }
    m_Changed.broadcast();
    notifyReadiness(ReadyRead);
    return length;
  }
  ssize_t recvfrom_msg(msghdr* message, SharedPointer<SocketRights>* rights) override {
    TerminationDeferral lifetime;
    if (rights) {
      rights->reset();
    }
    const int inputFlags = message->msg_flags;
    if (inputFlags & ~(MSG_DONTWAIT | MSG_TRUNC | MSG_WAITALL)) {
      return unsupported();
    }
    m_Lock.acquire();
    while (!m_Count && !m_Closed) {
      if (!isBlocking() || (inputFlags & MSG_DONTWAIT)) {
        m_Lock.release();
        syscallError(EAGAIN);
        return -1;
      }
      ConditionVariable::Error error = ConditionVariable::NoError;
      if (!m_Changed.wait(m_Lock, error)) {
        if (ConditionVariable::mutexAcquired(error)) {
          m_Lock.release();
        }
        syscallError(EINTR);
        return -1;
      }
    }
    if (m_Closed) {
      m_Lock.release();
      syscallError(EBADF);
      return -1;
    }
    const Reply reply = m_Replies[m_Head];
    m_Head = (m_Head + 1) % 16;
    if (m_Count-- == 16) {
      ++m_Generations.write;
    }
    m_Lock.release();
    size_t copied = 0;
    for (size_t i = 0; i < static_cast<size_t>(message->msg_iovlen) && copied < sizeof(reply);
         ++i) {
      const auto& vector = message->msg_iov[i];
      const size_t amount =
          vector.iov_len < sizeof(reply) - copied ? vector.iov_len : sizeof(reply) - copied;
      MemoryCopy(vector.iov_base, reinterpret_cast<const uint8_t*>(&reply) + copied, amount);
      copied += amount;
    }
    if (message->msg_name) {
      const Address address = {16, 0, 0, 0};
      MemoryCopy(message->msg_name, &address,
                 message->msg_namelen < sizeof(address) ? message->msg_namelen : sizeof(address));
      message->msg_namelen = sizeof(address);
    }
    message->msg_controllen = 0;
    message->msg_flags = copied < sizeof(reply) ? MSG_TRUNC : 0;
    notifyReadiness(ReadyWrite);
    return inputFlags & MSG_TRUNC ? sizeof(reply) : copied;
  }
  int getsockname(sockaddr_storage* address, socklen_t* length) override {
    LockGuard<Mutex> guard(m_Lock);
    const Address value = {16, 0, m_Pid, 0};
    MemoryCopy(address, &value, *length < sizeof(value) ? *length : sizeof(value));
    *length = sizeof(value);
    return 0;
  }
  int getpeername(sockaddr_storage*, socklen_t*) override {
    syscallError(ENOTCONN);
    return -1;
  }
  int connect(const sockaddr_storage*, socklen_t) override {
    return unsupported();
  }
  int listen(int) override {
    return unsupported();
  }
  int accept(sockaddr_storage*, socklen_t*, int, DescriptorLease*) override {
    return unsupported();
  }
  int shutdown(int) override {
    return unsupported();
  }
  int setsockopt(int, int, const void*, socklen_t) override {
    syscallError(ENOPROTOOPT);
    return -1;
  }
  int getsockopt(int level, int option, void* value, socklen_t* length) override {
    if (level != SOL_SOCKET || (option != SO_TYPE && option != SO_ERROR) || *length < sizeof(int)) {
      syscallError(ENOPROTOOPT);
      return -1;
    }
    *static_cast<int*>(value) = option == SO_TYPE ? getType() : 0;
    *length = sizeof(int);
    return 0;
  }
  bool canPoll() const override {
    return true;
  }
  ReadyMask queryReady(bool reading, bool writing) override {
    LockGuard<Mutex> guard(m_Lock);
    return (m_Closed ? ReadyInvalid | ReadyHangup
                     : (reading && m_Count ? ReadyRead : ReadyNone) |
                           (writing && m_Count < 16 ? ReadyWrite : ReadyNone)) |
           pendingReceiveReadiness();
  }
  ReadinessGenerations readinessGenerations() override {
    LockGuard<Mutex> guard(m_Lock);
    return withReceiveErrorGeneration(m_Generations);
  }
  void lastDescriptorClosed() override {
    if (!beginDescriptorClose()) {
      return;
    }
    {
      LockGuard<Mutex> guard(m_Lock);
      m_Closed = true;
      if (m_Bound) {
        m_NetworkNamespace->releaseRoutePort(m_Pid);
      }
    }
    m_Changed.broadcast();
    m_ReadinessNotifications.closeAndWait();
    closeReadiness();
  }

 private:
  int request(const Header& header, const uint8_t* payload, size_t length) {
    if (header.type == 16) {
      struct Link {
        uint8_t family, padding;
        uint16_t type;
        int32_t index;
        uint32_t flags, change;
      } link;
      if (length != sizeof(link)) {
        return EINVAL;
      }
      MemoryCopy(&link, payload, sizeof(link));
      if (link.index != 1) {
        return ENODEV;
      }
      if (link.family || link.type) {
        return EAFNOSUPPORT;
      }
      return m_NetworkNamespace->configureLink(link.flags, link.change);
    }
    if (header.type == 20) {
      struct InterfaceAddress {
        uint8_t family, prefix, flags, scope;
        uint32_t index;
      } address;
      if (length < sizeof(address)) {
        return EINVAL;
      }
      MemoryCopy(&address, payload, sizeof(address));
      if (address.index != 1) {
        return ENODEV;
      }
      if (address.family != AF_INET || address.scope != 254) {
        return EAFNOSUPPORT;
      }
      if (!(header.flags & 0x400) || (address.flags & ~0x80)) {
        return EOPNOTSUPP;
      }
      uint32_t local = 0, peer = 0;
      size_t offset = sizeof(address);
      while (offset < length) {
        struct Attribute {
          uint16_t length, type;
        } attribute;
        if (length - offset < sizeof(attribute)) {
          return EINVAL;
        }
        MemoryCopy(&attribute, payload + offset, sizeof(attribute));
        if (attribute.length != 8 || attribute.length > length - offset) {
          return EINVAL;
        }
        uint32_t value;
        MemoryCopy(&value, payload + offset + sizeof(attribute), sizeof(value));
        if (attribute.type == 1) {
          peer = BIG_TO_HOST32(value);
        } else if (attribute.type == 2) {
          local = BIG_TO_HOST32(value);
        } else {
          return EOPNOTSUPP;
        }
        offset += (attribute.length + 3) & ~size_t(3);
      }
      if (!local || (peer && peer != local)) {
        return EINVAL;
      }
      return m_NetworkNamespace->configureAddress(local, address.prefix, header.flags & 0x200);
    }
    return EOPNOTSUPP;
  }
  Mutex m_Lock;
  ConditionVariable m_Changed;
  Reply m_Replies[16] = {};
  size_t m_Head = 0, m_Count = 0;
  uint32_t m_Pid = 0;
  bool m_Bound = false, m_Closed = false;
  ReadinessGenerations m_Generations;
};
}  // namespace

NetworkSyscalls* posix_route_netlink_socket(int type, int protocol,
                                            const NetworkNamespaceRef& space) {
  auto* socket = new RouteSocket(type, protocol, space);
  if (!socket) {
    syscallError(ENOMEM);
  }
  return socket;
}
