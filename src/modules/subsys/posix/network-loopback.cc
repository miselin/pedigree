/* Copyright (c) 2026, Pedigree Developers. */
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include <errno.h>
#include <stddef.h>

#include "FileDescriptor.h"
#include "net-syscalls.h"
#include "network-namespace.h"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>

namespace {
constexpr uint32_t Loopback = 0x7f000001;
constexpr socklen_t EndpointLength = offsetof(sockaddr_un, sun_path) + 16;

void endpoint(sockaddr_storage& output, int type, uint32_t address, uint16_t port, uint64_t token) {
  ByteSet(&output, 0, sizeof(output));
  auto* local = reinterpret_cast<sockaddr_un*>(&output);
  local->sun_family = AF_UNIX;
  local->sun_path[1] = type;
  MemoryCopy(local->sun_path + 2, &address, 4);
  MemoryCopy(local->sun_path + 6, &port, 2);
  MemoryCopy(local->sun_path + 8, &token, 8);
}
sockaddr_in decode(const sockaddr_storage& address, socklen_t length) {
  sockaddr_in result = {};
  result.sin_family = AF_INET;
  if (length >= EndpointLength) {
    const auto* local = reinterpret_cast<const sockaddr_un*>(&address);
    uint32_t ip;
    uint16_t port;
    MemoryCopy(&ip, local->sun_path + 2, 4);
    MemoryCopy(&port, local->sun_path + 6, 2);
    result.sin_addr.s_addr = HOST_TO_BIG32(ip);
    result.sin_port = HOST_TO_BIG16(port);
  }
  return result;
}
void copyAddress(const sockaddr_in& source, sockaddr_storage* destination, socklen_t* length) {
  if (destination) {
    MemoryCopy(destination, &source, *length < sizeof(source) ? *length : sizeof(source));
  }
  *length = sizeof(source);
}
bool internetAddress(const sockaddr_storage* input, socklen_t length, uint32_t& ip,
                     uint16_t& port) {
  if (!input || length < sizeof(sockaddr_in) || input->ss_family != AF_INET) {
    syscallError(EINVAL);
    return false;
  }
  const auto* address = reinterpret_cast<const sockaddr_in*>(input);
  ip = BIG_TO_HOST32(address->sin_addr.s_addr);
  port = BIG_TO_HOST16(address->sin_port);
  return true;
}

class LoopbackSocket final : public UnixSocketSyscalls {
 public:
  LoopbackSocket(int type, int protocol, const NetworkNamespaceRef& space, bool accepted = false)
      : UnixSocketSyscalls(AF_INET, type, protocol), m_Accepted(accepted) {
    m_NetworkNamespace = space;
  }
  ~LoopbackSocket() override {
    lastDescriptorClosed();
  }
  bool create() override {
    if ((getType() != SOCK_STREAM && getType() != SOCK_DGRAM) ||
        (getProtocol() &&
         getProtocol() != (getType() == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP))) {
      syscallError(EPROTONOSUPPORT);
      return false;
    }
    return UnixSocketSyscalls::create();
  }
  int bind(const sockaddr_storage* address, socklen_t length) override {
    uint32_t ip;
    uint16_t port;
    if (!internetAddress(address, length, ip, port)) {
      return -1;
    }
    LockGuard<Mutex> guard(m_BindLock);
    if (m_Closing) {
      syscallError(EBADF);
      return -1;
    }
    if (m_Token || m_Accepted) {
      syscallError(EINVAL);
      return -1;
    }
    return bindLocked(ip, port);
  }
  int listen(int backlog) override {
    if (!ensureBound(0)) {
      return -1;
    }
    return UnixSocketSyscalls::listen(backlog);
  }
  int connect(const sockaddr_storage* address, socklen_t length) override {
    sockaddr_storage destination;
    if (!destinationEndpoint(address, length, destination) || !ensureBound(Loopback)) {
      return -1;
    }
    const int result = UnixSocketSyscalls::connect(&destination, EndpointLength);
    if (result == 0 || Processor::information().getCurrentThread()->getErrno() == EINPROGRESS) {
      LockGuard<Mutex> guard(m_BindLock);
      MemoryCopy(&m_Peer, address, sizeof(m_Peer));
      if (!m_Peer.sin_addr.s_addr) {
        m_Peer.sin_addr.s_addr = HOST_TO_BIG32(Loopback);
      }
      m_HasPeer = true;
    }
    return result;
  }
  ssize_t sendto_msg(const msghdr* message, const SharedPointer<SocketRights>& rights) override {
    if (rights) {
      syscallError(EOPNOTSUPP);
      return -1;
    }
    if (!m_NetworkNamespace->usable()) {
      syscallError(ENETUNREACH);
      return -1;
    }
    if (!ensureBound(Loopback)) {
      return -1;
    }
    sockaddr_storage destination;
    msghdr translated = *message;
    if (message->msg_name) {
      if (!destinationEndpoint(reinterpret_cast<const sockaddr_storage*>(message->msg_name),
                               message->msg_namelen, destination)) {
        return -1;
      }
      translated.msg_name = &destination;
      translated.msg_namelen = EndpointLength;
    }
    return UnixSocketSyscalls::sendto_msg(&translated, {});
  }
  ssize_t recvfrom_msg(msghdr* message, SharedPointer<SocketRights>* rights) override {
    sockaddr_storage source = {};
    msghdr translated = *message;
    translated.msg_name = &source;
    translated.msg_namelen = sizeof(source);
    const ssize_t result = UnixSocketSyscalls::recvfrom_msg(&translated, rights);
    if (result < 0) {
      return result;
    }
    message->msg_flags = translated.msg_flags;
    message->msg_controllen = 0;
    if (message->msg_name && getType() == SOCK_DGRAM) {
      auto decoded = decode(source, translated.msg_namelen);
      if (!decoded.sin_addr.s_addr) {
        decoded.sin_addr.s_addr = HOST_TO_BIG32(Loopback);
      }
      socklen_t length = message->msg_namelen;
      copyAddress(decoded, reinterpret_cast<sockaddr_storage*>(message->msg_name), &length);
      message->msg_namelen = length;
    } else {
      message->msg_namelen = 0;
    }
    return result;
  }
  int accept(sockaddr_storage* address, socklen_t* length, int flags,
             DescriptorLease* accepted) override {
    sockaddr_storage source = {};
    socklen_t sourceLength = sizeof(source);
    const int result = UnixSocketSyscalls::accept(&source, &sourceLength, flags, accepted);
    if (result >= 0 && length) {
      copyAddress(decode(source, sourceLength), address, length);
    }
    return result;
  }
  int getsockname(sockaddr_storage* address, socklen_t* length) override {
    sockaddr_storage source = {};
    socklen_t sourceLength = sizeof(source);
    if (UnixSocketSyscalls::getsockname(&source, &sourceLength)) {
      return -1;
    }
    auto decoded = decode(source, sourceLength);
    if (m_Accepted && !decoded.sin_addr.s_addr) {
      decoded.sin_addr.s_addr = HOST_TO_BIG32(Loopback);
    }
    copyAddress(decoded, address, length);
    return 0;
  }
  int getpeername(sockaddr_storage* address, socklen_t* length) override {
    if (!m_Accepted) {
      LockGuard<Mutex> guard(m_BindLock);
      if (!m_HasPeer) {
        syscallError(ENOTCONN);
        return -1;
      }
      copyAddress(m_Peer, address, length);
      return 0;
    }
    sockaddr_storage source = {};
    socklen_t sourceLength = sizeof(source);
    if (UnixSocketSyscalls::getpeername(&source, &sourceLength)) {
      return -1;
    }
    auto decoded = decode(source, sourceLength);
    if (!decoded.sin_addr.s_addr) {
      decoded.sin_addr.s_addr = HOST_TO_BIG32(Loopback);
    }
    copyAddress(decoded, address, length);
    return 0;
  }
  int setsockopt(int level, int option, const void* value, socklen_t length) override {
    if ((level == IPPROTO_TCP && option == TCP_NODELAY && getType() == SOCK_STREAM) ||
        (level == SOL_SOCKET && option == SO_REUSEADDR)) {
      if (length < sizeof(int)) {
        syscallError(EINVAL);
        return -1;
      }
      LockGuard<Mutex> guard(m_BindLock);
      (level == IPPROTO_TCP ? m_NoDelay : m_ReuseAddress) = *static_cast<const int*>(value) != 0;
      return 0;
    }
    syscallError(ENOPROTOOPT);
    return -1;
  }
  int getsockopt(int level, int option, void* value, socklen_t* length) override {
    if ((level == IPPROTO_TCP && option == TCP_NODELAY) ||
        (level == SOL_SOCKET && option == SO_REUSEADDR)) {
      if (*length < sizeof(int)) {
        syscallError(EINVAL);
        return -1;
      }
      LockGuard<Mutex> guard(m_BindLock);
      *static_cast<int*>(value) = level == IPPROTO_TCP ? m_NoDelay : m_ReuseAddress;
      *length = sizeof(int);
      return 0;
    }
    if (level == SOL_SOCKET && (option == SO_TYPE || option == SO_ERROR)) {
      return UnixSocketSyscalls::getsockopt(level, option, value, length);
    }
    syscallError(ENOPROTOOPT);
    return -1;
  }
  void lastDescriptorClosed() override {
    uint64_t token;
    {
      LockGuard<Mutex> guard(m_BindLock);
      if (m_Closing) {
        return;
      }
      m_Closing = true;
      token = m_Token;
      m_Token = 0;
    }
    const auto space = m_NetworkNamespace;
    UnixSocketSyscalls::lastDescriptorClosed();
    if (token) {
      space->release(token);
    }
  }

 protected:
  UnixSocketSyscalls* createAcceptedSocket() override {
    return new LoopbackSocket(getType(), getProtocol(), m_NetworkNamespace, true);
  }

 private:
  int bindLocked(uint32_t ip, uint16_t port) {
    const uint64_t token = m_NetworkNamespace->reserve(getType(), ip, port);
    if (!token) {
      return -1;
    }
    sockaddr_storage local;
    endpoint(local, getType(), ip, port, token);
    if (UnixSocketSyscalls::bind(&local, EndpointLength)) {
      m_NetworkNamespace->release(token);
      return -1;
    }
    m_Token = token;
    return 0;
  }
  bool ensureBound(uint32_t address) {
    LockGuard<Mutex> guard(m_BindLock);
    if (m_Closing) {
      syscallError(EBADF);
      return false;
    }
    return m_Accepted || m_Token || bindLocked(address, 0) == 0;
  }
  bool destinationEndpoint(const sockaddr_storage* address, socklen_t length,
                           sockaddr_storage& output) {
    uint32_t ip, boundIp;
    uint16_t port;
    uint64_t token;
    if (!internetAddress(address, length, ip, port)) {
      return false;
    }
    if (!ip) {
      ip = Loopback;
    }
    if (!m_NetworkNamespace->find(getType(), ip, port, boundIp, token)) {
      return false;
    }
    endpoint(output, getType(), boundIp, port, token);
    return true;
  }
  Mutex m_BindLock;
  uint64_t m_Token = 0;
  const bool m_Accepted;
  bool m_HasPeer = false, m_NoDelay = false, m_ReuseAddress = false, m_Closing = false;
  sockaddr_in m_Peer = {};
};
}  // namespace

NetworkSyscalls* posix_network_socket(int domain, int type, int protocol,
                                      const NetworkNamespaceRef& space) {
  if (domain == AF_INET) {
    auto* socket = new LoopbackSocket(type, protocol, space);
    if (!socket) {
      syscallError(ENOMEM);
    }
    return socket;
  }
  if (domain == 16) {
    return posix_route_netlink_socket(type, protocol, space);
  }
  syscallError(EAFNOSUPPORT);
  return nullptr;
}
