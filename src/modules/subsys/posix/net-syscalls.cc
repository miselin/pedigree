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

#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1  // don't need them here

#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Tree.h"

#include <fcntl.h>
#include <stddef.h>

#include "file-syscalls.h"
#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/UnixFilesystem.h"
#include "modules/system/lwip/include/lwip/api.h"
#include "modules/system/lwip/include/lwip/ip.h"
#include "modules/system/lwip/include/lwip/ip_addr.h"
#include "modules/system/lwip/include/lwip/tcp.h"
#include "modules/system/lwip/include/lwip/tcpip.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/VFS.h"
#include "net-syscalls.h"

#ifndef UTILITY_LINUX
#include <netdb.h>

#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <netinet/in.h>
#include <sys/un.h>

// Set to 1 to also log the contents of send() and recv() buffers ala strace
#define LOG_SEND_RECV_BUFFERS 0

Tree<struct netconn*, LwipSocketSyscalls*> LwipSocketSyscalls::m_SyscallObjects;
Mutex LwipSocketSyscalls::m_SyscallObjectsLock;

extern UnixFilesystem* g_pUnixFilesystem;

static File* findTrackedUnixSocket(const String& pathname) {
  LockGuard<Mutex> guard(UnixFilesystem::namespaceLock());
  File* file = VFS::instance().find(pathname);
  if (file) {
    VFS::instance().trackFile(file);
  }
  return file;
}

static void releaseTrackedUnixSocket(File* file) {
  if (!file) {
    return;
  }

  LockGuard<Mutex> guard(UnixFilesystem::namespaceLock());
  VFS::instance().untrackFile(file);
}

static Thread* beginInterruptibleSocketCall() {
#if defined(PEDIGREE_EXTERNAL_SOURCE)
  // The standalone syscall harness has no Pedigree Thread or event source.
  return nullptr;
#else
  Thread* thread = Processor::information().getCurrentThread();
  thread->clearInterruption();
  return thread;
#endif
}

bool finishInterruptibleSocketCall(Thread* thread, ssize_t result) {
#if defined(PEDIGREE_EXTERNAL_SOURCE)
  (void)thread;
  (void)result;
  return true;
#else
  const bool interrupted = thread->getInterruptionReason() == Thread::InterruptedBySignal;
  thread->clearInterruption();

  if (interrupted && result < 0) {
    SYSCALL_ERROR(Interrupted);
    return false;
  }

  return true;
#endif
}

/// Pass is_create = true to indicate that the operation is permitted to
// operate if the socket does not yet have valid members (i.e. before a bind).
static bool isSaneSocket(const DescriptorLease& f, bool is_create = false) {
  if (!f) {
    N_NOTICE(" -> isSaneSocket: descriptor is null");
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }

  if (is_create) {
    return true;
  }

  if (!f->networkImpl) {
    N_NOTICE(" -> isSaneSocket: no network implementation found");
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }

  return true;
}

static const int socketTypeMask = 0xF;
static const int socketCreationFlags = SOCK_NONBLOCK | SOCK_CLOEXEC;
static const int linuxTcpNoDelay = 1;

static bool splitSocketType(int argument, int& type, int& flags) {
  type = argument & socketTypeMask;
  flags = argument & socketCreationFlags;
  if (argument != (type | flags)) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  return true;
}

static void setSocketDescriptorFlags(FileDescriptor* descriptor, int flags) {
  descriptor->setFlags((flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
  descriptor->setStatusFlags((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
}

static bool unixSocketPath(const struct sockaddr_storage* address, socklen_t addressLength,
                           String& path, bool allowUnnamed) {
  const size_t pathOffset = offsetof(struct sockaddr_un, sun_path);
  if (!address || addressLength < pathOffset || addressLength > sizeof(struct sockaddr_un)) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  const struct sockaddr_un* un = reinterpret_cast<const struct sockaddr_un*>(address);
  const size_t pathLength = addressLength - pathOffset;
  if (!pathLength) {
    if (allowUnnamed) {
      path = String();
      return true;
    }

    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  // Linux abstract sockets have unrelated lifetime and namespace semantics.
  // Keep the first Go milestone explicitly pathname-only.
  if (!un->sun_path[0]) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }

  char boundedPath[sizeof(un->sun_path) + 1];
  ByteSet(boundedPath, 0, sizeof(boundedPath));
  MemoryCopy(boundedPath, un->sun_path, pathLength);
  normalisePath(path, boundedPath);
  return true;
}

static uint8_t lwipSocketOption(int option) {
  switch (option) {
    case SO_REUSEADDR:
      return SOF_REUSEADDR;
    case SO_KEEPALIVE:
      return SOF_KEEPALIVE;
    case SO_BROADCAST:
      return SOF_BROADCAST;
    default:
      return 0;
  }
}

static int lwipErrorNumber(err_t error) {
  const int result = err_to_errno(error);
  return result < 0 ? Error::IoError : result;
}

static err_t sockaddrToIpaddr(const struct sockaddr_storage* saddr, uint16_t& port,
                              ip_addr_t* result, bool isbind = true) {
  ByteSet(result, 0, sizeof(*result));

  if (saddr->ss_family == AF_INET) {
    const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(saddr);
    result->u_addr.ip4.addr = sin->sin_addr.s_addr;
    result->type = IPADDR_TYPE_V4;

    if (!isbind) {
      // do some extra sanity checks for client connections
      if (!sin->sin_addr.s_addr) {
        // rebind to 127.0.0.1 (localhost)
        result->u_addr.ip4.addr = HOST_TO_BIG32(INADDR_LOOPBACK);
      }
    }

    port = BIG_TO_HOST16(sin->sin_port);

    return ERR_OK;
  } else {
    ERROR("sockaddrToIpaddr: only AF_INET is supported at the moment.");
  }

  return ERR_VAL;
}

int posix_socket(int domain, int type, int protocol) {
  N_NOTICE("socket(" << domain << ", " << type << ", " << protocol << ")");

  int socketType = 0;
  int flags = 0;
  if (!splitSocketType(type, socketType, flags)) {
    return -1;
  }

  size_t fd = getAvailableDescriptor();

  netconn_type connType = NETCONN_INVALID;

  File* file = nullptr;
  struct netconn* conn = nullptr;
  bool valid = true;

  NetworkSyscalls* syscalls;

  if (domain == AF_UNIX) {
    if (socketType != SOCK_STREAM && socketType != SOCK_DGRAM) {
      SYSCALL_ERROR(OperationNotSupported);
      return -1;
    }
    syscalls = new UnixSocketSyscalls(domain, socketType, protocol);
  } else {
    /// \todo handle non-lwIP domains
    syscalls = new LwipSocketSyscalls(domain, socketType, protocol);
  }

  if (!syscalls->create()) {
    return -1;
  }

  FileDescriptor* f = new FileDescriptor;
  f->networkImpl = syscalls;
  f->fd = fd;
  setSocketDescriptorFlags(f, flags);
  addDescriptor(fd, f);
  syscalls->associate(f);

  N_NOTICE("  -> " << Dec << fd << Hex);
  return static_cast<int>(fd);
}

int posix_socketpair(int domain, int type, int protocol, int sv[2]) {
  N_NOTICE("socketpair");

  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(sv), sizeof(int) * 2,
                                    PosixSubsystem::SafeWrite)) {
    N_NOTICE("socketpair -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  if (domain != AF_UNIX) {
    /// \todo syscall error for EAFNOSUPPORT
    N_NOTICE(" -> bad domain");
    return -1;
  }

  int socketType = 0;
  int flags = 0;
  if (!splitSocketType(type, socketType, flags)) {
    return -1;
  }
  if (socketType != SOCK_STREAM && socketType != SOCK_DGRAM) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }

  UnixSocketSyscalls* syscallsA = new UnixSocketSyscalls(domain, socketType, protocol);
  if (!syscallsA->create()) {
    delete syscallsA;
    N_NOTICE(" -> failed to create first socket");
    return -1;
  }

  UnixSocketSyscalls* syscallsB = new UnixSocketSyscalls(domain, socketType, protocol);
  if (!syscallsB->create()) {
    delete syscallsA;
    delete syscallsB;
    N_NOTICE(" -> failed to create second socket");
    return -1;
  }

  if (!syscallsA->pairWith(syscallsB)) {
    delete syscallsA;
    delete syscallsB;
    N_NOTICE(" -> failed to pair");
    return -1;
  }

  FileDescriptor* fA = new FileDescriptor;
  FileDescriptor* fB = new FileDescriptor;

  size_t fdA = getAvailableDescriptor();
  size_t fdB = getAvailableDescriptor();

  fA->networkImpl = syscallsA;
  fA->fd = fdA;
  fB->networkImpl = syscallsB;
  fB->fd = fdB;

  setSocketDescriptorFlags(fA, flags);
  setSocketDescriptorFlags(fB, flags);

  addDescriptor(fdA, fA);
  addDescriptor(fdB, fB);

  syscallsA->associate(fA);
  syscallsB->associate(fB);

  sv[0] = static_cast<int>(fdA);
  sv[1] = static_cast<int>(fdB);

  N_NOTICE(" -> " << sv[0] << ", " << sv[1]);
  return 0;
}

int posix_connect(int sock, const struct sockaddr_storage* address, socklen_t addrlen) {
  N_NOTICE("connect");

  if (!address || addrlen < sizeof(sa_family_t) ||
      !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address), addrlen,
                                    PosixSubsystem::SafeRead)) {
    N_NOTICE("connect -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  N_NOTICE("connect(" << sock << ", " << reinterpret_cast<uintptr_t>(address) << ", " << addrlen
                      << ")");

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f, true)) {
    return -1;
  }

  if (address->ss_family != f->networkImpl->getDomain()) {
    // EAFNOSUPPORT
    N_NOTICE(" -> incorrect address family passed to connect()");
    return -1;
  }

  Thread* thread = beginInterruptibleSocketCall();
  const int result = f->networkImpl->connect(address, addrlen);
  return finishInterruptibleSocketCall(thread, static_cast<ssize_t>(result)) ? result : -1;
}

ssize_t posix_send(int sock, const void* buff, size_t bufflen, int flags) {
  N_NOTICE("send");

  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen,
                                    PosixSubsystem::SafeRead)) {
    N_NOTICE("send -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  N_NOTICE("send(" << sock << ", " << buff << ", " << bufflen << ", " << flags << ")");

  EMIT_IF(LOG_SEND_RECV_BUFFERS) {
    if (buff && bufflen) {
      String debug;
      debug.assign(reinterpret_cast<const char*>(buff), bufflen, true);
      N_NOTICE(" -> sending: '" << debug << "'");
    }
  }

  DescriptorLease f;
  acquireDescriptor(sock, f);
  return posix_send_descriptor(f, buff, bufflen, flags);
}

ssize_t posix_send_descriptor(const DescriptorLease& f, const void* buff, size_t bufflen,
                              int flags) {
  if (!isSaneSocket(f)) {
    return -1;
  }

  Thread* thread = beginInterruptibleSocketCall();
  const ssize_t result = f->networkImpl->sendto(buff, bufflen, flags, nullptr, 0);
  return finishInterruptibleSocketCall(thread, result) ? result : -1;
}

ssize_t posix_sendto(int sock, const void* buff, size_t bufflen, int flags,
                     struct sockaddr_storage* address, socklen_t addrlen) {
  N_NOTICE("sendto");

  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen,
                                    PosixSubsystem::SafeRead)) {
    N_NOTICE("sendto -> invalid address for transmission buffer");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (address && (addrlen < sizeof(sa_family_t) ||
                  !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address), addrlen,
                                                PosixSubsystem::SafeRead))) {
    N_NOTICE("sendto -> invalid destination address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  N_NOTICE("sendto(" << sock << ", " << buff << ", " << bufflen << ", " << flags << ", " << address
                     << ", " << addrlen << ")");

  EMIT_IF(LOG_SEND_RECV_BUFFERS) {
    if (buff && bufflen) {
      String debug;
      debug.assign(reinterpret_cast<const char*>(buff), bufflen, true);
      N_NOTICE(" -> sending: '" << debug << "'");
    }
  }

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  Thread* thread = beginInterruptibleSocketCall();
  const ssize_t result = f->networkImpl->sendto(buff, bufflen, flags, address, addrlen);
  return finishInterruptibleSocketCall(thread, result) ? result : -1;
}

ssize_t posix_recv(int sock, void* buff, size_t bufflen, int flags) {
  N_NOTICE("recv");

  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen,
                                    PosixSubsystem::SafeWrite)) {
    N_NOTICE("recv -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  N_NOTICE("recv(" << sock << ", " << buff << ", " << bufflen << ", " << flags << ")");

  DescriptorLease f;
  acquireDescriptor(sock, f);
  ssize_t n = posix_recv_descriptor(f, buff, bufflen, flags);

  EMIT_IF(LOG_SEND_RECV_BUFFERS) {
    if (buff && n > 0) {
      String debug;
      debug.assign(reinterpret_cast<const char*>(buff), n, true);
      N_NOTICE(" -> received: '" << debug << "'");
    }
  }

  N_NOTICE(" -> " << n);
  return n;
}

ssize_t posix_recv_descriptor(const DescriptorLease& f, void* buff, size_t bufflen, int flags) {
  if (!isSaneSocket(f)) {
    return -1;
  }

  Thread* thread = beginInterruptibleSocketCall();
  ssize_t n = f->networkImpl->recvfrom(buff, bufflen, flags, nullptr, nullptr);
  if (!finishInterruptibleSocketCall(thread, n)) {
    return -1;
  }
  return n;
}

ssize_t posix_recvfrom(int sock, void* buff, size_t bufflen, int flags,
                       struct sockaddr_storage* address, socklen_t* addrlen) {
  N_NOTICE("recvfrom");

  if (!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen,
                                     PosixSubsystem::SafeWrite) &&
        ((!address) ||
         PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(addrlen), sizeof(socklen_t),
                                      PosixSubsystem::SafeWrite)))) {
    N_NOTICE(
        "recvfrom -> invalid address for receive buffer or addrlen "
        "parameter");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  N_NOTICE("recvfrom(" << sock << ", " << buff << ", " << bufflen << ", " << flags << ", "
                       << address << ", " << addrlen);

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  Thread* thread = beginInterruptibleSocketCall();
  ssize_t n = f->networkImpl->recvfrom(buff, bufflen, flags, address, addrlen);
  if (!finishInterruptibleSocketCall(thread, n)) {
    return -1;
  }

  EMIT_IF(LOG_SEND_RECV_BUFFERS) {
    if (buff && n > 0) {
      String debug;
      debug.assign(reinterpret_cast<const char*>(buff), n, true);
      N_NOTICE(" -> received: '" << debug << "'");
    }
  }

  N_NOTICE(" -> " << n);
  return n;
}

int posix_bind(int sock, const struct sockaddr_storage* address, socklen_t addrlen) {
  N_NOTICE("bind");

  if (!address || addrlen < sizeof(sa_family_t) ||
      !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address), addrlen,
                                    PosixSubsystem::SafeRead)) {
    N_NOTICE("bind -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  N_NOTICE("bind(" << sock << ", " << address << ", " << addrlen << ")");

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f, true)) {
    return -1;
  }

  if (f->networkImpl->getDomain() != address->ss_family) {
    // EAFNOSUPPORT
    return -1;
  }

  return f->networkImpl->bind(address, addrlen);
}

int posix_listen(int sock, int backlog) {
  N_NOTICE("listen(" << sock << ", " << backlog << ")");

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  if (f->networkImpl->getType() != SOCK_STREAM) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  return f->networkImpl->listen(backlog);
}

int posix_accept(int sock, struct sockaddr_storage* address, socklen_t* addrlen) {
  return posix_accept4(sock, address, addrlen, 0);
}

int posix_accept4(int sock, struct sockaddr_storage* address, socklen_t* addrlen, int flags) {
  N_NOTICE("accept4");

  if (flags & ~socketCreationFlags) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  struct sockaddr_storage acceptedAddress;
  ByteSet(&acceptedAddress, 0, sizeof(acceptedAddress));
  socklen_t acceptedLength = sizeof(acceptedAddress);
  socklen_t addressCapacity = 0;
  const bool returnAddress = address != nullptr;
  if (returnAddress) {
    if (!addrlen || !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(addrlen),
                                                  sizeof(socklen_t), PosixSubsystem::SafeWrite)) {
      N_NOTICE("accept4 -> invalid address length");
      SYSCALL_ERROR(BadAddress);
      return -1;
    }

    addressCapacity = *addrlen;
    const size_t writableLength = addressCapacity < sizeof(acceptedAddress)
                                      ? static_cast<size_t>(addressCapacity)
                                      : sizeof(acceptedAddress);
    if (writableLength &&
        !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address), writableLength,
                                      PosixSubsystem::SafeWrite)) {
      N_NOTICE("accept4 -> invalid address");
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  N_NOTICE("accept4(" << sock << ", " << address << ", " << addrlen << ", " << flags << ")");

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  if (f->networkImpl->getType() != SOCK_STREAM) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }

  Thread* thread = beginInterruptibleSocketCall();
  int r = f->networkImpl->accept(&acceptedAddress, &acceptedLength, flags);
  if (!finishInterruptibleSocketCall(thread, static_cast<ssize_t>(r))) {
    return -1;
  }
  if (r >= 0 && returnAddress) {
    const size_t copyLength = addressCapacity < acceptedLength
                                  ? static_cast<size_t>(addressCapacity)
                                  : static_cast<size_t>(acceptedLength);
    if (copyLength) {
      MemoryCopy(address, &acceptedAddress, copyLength);
    }
    *addrlen = acceptedLength;
  }
  N_NOTICE(" -> " << Dec << r);
  return r;
}

int posix_shutdown(int socket, int how) {
  N_NOTICE("shutdown(" << socket << ", " << how << ")");

  DescriptorLease f;
  acquireDescriptor(socket, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  return f->networkImpl->shutdown(how);
}

int posix_getpeername(int socket, struct sockaddr_storage* address, socklen_t* address_len) {
  N_NOTICE("getpeername");

  if (!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address),
                                     sizeof(struct sockaddr_storage), PosixSubsystem::SafeWrite) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address_len), sizeof(socklen_t),
                                     PosixSubsystem::SafeWrite))) {
    N_NOTICE("getpeername -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  N_NOTICE("getpeername(" << socket << ", " << address << ", " << address_len << ")");

  DescriptorLease f;
  acquireDescriptor(socket, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  return f->networkImpl->getpeername(address, address_len);
}

int posix_getsockname(int socket, struct sockaddr_storage* address, socklen_t* address_len) {
  N_NOTICE("getsockname");

  if (!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address),
                                     sizeof(struct sockaddr_storage), PosixSubsystem::SafeWrite) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address_len), sizeof(socklen_t),
                                     PosixSubsystem::SafeWrite))) {
    N_NOTICE("getsockname -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  N_NOTICE("getsockname(" << socket << ", " << address << ", " << address_len << ")");

  DescriptorLease f;
  acquireDescriptor(socket, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  return f->networkImpl->getsockname(address, address_len);
}

int posix_setsockopt(int sock, int level, int optname, const void* optvalue, socklen_t optlen) {
  N_NOTICE("setsockopt(" << sock << ", " << level << ", " << optname << ", " << optvalue << ", "
                         << optlen << ")");

  if (!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(optvalue), optlen,
                                     PosixSubsystem::SafeRead))) {
    N_NOTICE("setsockopt -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  return f->networkImpl->setsockopt(level, optname, optvalue, optlen);
}

int posix_getsockopt(int sock, int level, int optname, void* optvalue, socklen_t* optlen) {
  N_NOTICE("getsockopt(" << sock << ", " << level << ", " << optname << ")");

  // Check optlen first, then use it to check optvalue.
  if (!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(optlen), sizeof(socklen_t),
                                     PosixSubsystem::SafeRead) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(optlen), sizeof(socklen_t),
                                     PosixSubsystem::SafeWrite))) {
    N_NOTICE("getsockopt -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(optvalue), *optlen,
                                     PosixSubsystem::SafeWrite))) {
    N_NOTICE("getsockopt -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  return f->networkImpl->getsockopt(level, optname, optvalue, optlen);
}

int posix_sethostname(const char* name, size_t len) {
  N_NOTICE("sethostname");

  if (!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(name), len,
                                     PosixSubsystem::SafeRead))) {
    N_NOTICE(" -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  N_NOTICE("sethostname(" << String(name, len) << ")");

  /// \todo integrate this

  return 0;
}

ssize_t posix_sendmsg(int sockfd, const struct msghdr* msg, int flags) {
  N_NOTICE("sendmsg(" << sockfd << ", " << msg << ", " << flags << ")");

  /// \todo check address

  DescriptorLease f;
  acquireDescriptor(sockfd, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  Thread* thread = beginInterruptibleSocketCall();
  ssize_t n = f->networkImpl->sendto_msg(msg);
  if (!finishInterruptibleSocketCall(thread, n)) {
    return -1;
  }
  N_NOTICE(" -> " << n);
  return n;
}

ssize_t posix_recvmsg(int sockfd, struct msghdr* msg, int flags) {
  N_NOTICE("recvmsg(" << sockfd << ", " << msg << ", " << flags << ")");

  /// \todo check address

  DescriptorLease f;
  acquireDescriptor(sockfd, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  Thread* thread = beginInterruptibleSocketCall();
  ssize_t n = f->networkImpl->recvfrom_msg(msg);
  if (!finishInterruptibleSocketCall(thread, n)) {
    return -1;
  }
  N_NOTICE(" -> " << n);
  return n;
}

NetworkSyscalls::NetworkSyscalls(int domain, int type, int protocol)
    : m_Domain(domain), m_Type(type), m_Protocol(protocol), m_Blocking(true) {}

NetworkSyscalls::~NetworkSyscalls() {}

bool NetworkSyscalls::create() {
  return true;
}

ssize_t NetworkSyscalls::sendto(const void* buffer, size_t bufferlen, int flags,
                                const struct sockaddr_storage* address, socklen_t addrlen) {
  struct iovec iov;
  iov.iov_base = const_cast<void*>(buffer);
  iov.iov_len = bufferlen;

  struct msghdr msg;
  msg.msg_name = const_cast<struct sockaddr_storage*>(address);
  msg.msg_namelen = addrlen;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = nullptr;
  msg.msg_controllen = 0;
  msg.msg_flags = flags;

  return sendto_msg(&msg);
}

ssize_t NetworkSyscalls::recvfrom(void* buffer, size_t bufferlen, int flags,
                                  struct sockaddr_storage* address, socklen_t* addrlen) {
  struct iovec iov;
  iov.iov_base = buffer;
  iov.iov_len = bufferlen;

  struct msghdr msg;
  msg.msg_name = address;
  msg.msg_namelen = addrlen ? *addrlen : 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = nullptr;
  msg.msg_controllen = 0;
  msg.msg_flags = flags;

  ssize_t result = recvfrom_msg(&msg);
  if (result >= 0) {
    // Copy result address length if needed.
    if (addrlen) {
      *addrlen = msg.msg_namelen;
    }
  }

  return result;
}

int NetworkSyscalls::shutdown(int how) {
  return 0;
}

bool NetworkSyscalls::canPoll() const {
  return false;
}

bool NetworkSyscalls::poll(bool& read, bool& write, bool& error, Semaphore* waiter) {
  read = false;
  write = false;
  error = false;
  return false;
}

void NetworkSyscalls::unPoll(Semaphore* waiter) {}

bool NetworkSyscalls::monitor(Thread* pThread, Event* pEvent) {
  return false;
}

bool NetworkSyscalls::unmonitor(Event* pEvent) {
  return false;
}

void NetworkSyscalls::associate(FileDescriptor* fd) {
  m_Blocking = !fd || !(fd->flflags & O_NONBLOCK);
}

bool NetworkSyscalls::isBlocking() const {
  return m_Blocking;
}

void NetworkSyscalls::setBlocking(bool blocking) {
  m_Blocking = blocking;
}

LwipSocketSyscalls::LwipSocketSyscalls(int domain, int type, int protocol)
    : NetworkSyscalls(domain, type, protocol), m_Socket(nullptr), m_Metadata() {}

LwipSocketSyscalls::~LwipSocketSyscalls() {
  if (m_Socket) {
    LOCK_TCPIP_CORE();
    {
      ConstexprLockGuard<Mutex, THREADS> guard(m_SyscallObjectsLock);
      m_SyscallObjects.remove(m_Socket);
    }
    UNLOCK_TCPIP_CORE();

    netconn_delete(m_Socket);
    m_Socket = nullptr;
  }
}

void LwipSocketSyscalls::setBlocking(bool blocking) {
  NetworkSyscalls::setBlocking(blocking);
  if (m_Socket) {
    netconn_set_nonblocking(m_Socket, blocking ? 0 : 1);
  }
}

void LwipSocketSyscalls::registerSocket() {
  LOCK_TCPIP_CORE();
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_SyscallObjectsLock);
    if (!m_SyscallObjects.lookup(m_Socket)) {
      // lwIP counts receive events in socket while an accepted
      // connection has no userspace descriptor. Transfer those events
      // before exposing it so early request data remains readable.
      if (m_Socket->socket < 0) {
        m_Metadata.recv += -1 - m_Socket->socket;
        m_Socket->socket = 0;
      }
      m_SyscallObjects.insert(m_Socket, this);
    }
  }
  UNLOCK_TCPIP_CORE();
}

bool LwipSocketSyscalls::create() {
  netconn_type connType = NETCONN_INVALID;

  // fix up some defaults that make sense for inet[6] sockets
  if (!m_Protocol) {
    N_NOTICE("LwipSocketSyscalls: using default protocol for socket type");
    if (m_Type == SOCK_DGRAM) {
      m_Protocol = IPPROTO_UDP;
    } else if (m_Type == SOCK_STREAM) {
      m_Protocol = IPPROTO_TCP;
    }
  }

  if (m_Domain == AF_INET) {
    switch (m_Protocol) {
      case IPPROTO_TCP:
        connType = NETCONN_TCP;
        break;
      case IPPROTO_UDP:
        connType = NETCONN_UDP;
        break;
    }
  } else if (m_Domain == AF_INET6) {
    switch (m_Protocol) {
      case IPPROTO_TCP:
        connType = NETCONN_TCP_IPV6;
        break;
      case IPPROTO_UDP:
        connType = NETCONN_UDP_IPV6;
        break;
    }
  } else if (m_Domain == AF_PACKET) {
    connType = NETCONN_RAW;
  } else {
    WARNING("LwipSocketSyscalls: domain " << m_Domain << " is not known!");
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  if (connType == NETCONN_INVALID) {
    N_NOTICE("LwipSocketSyscalls: invalid socket creation parameters");
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  // Socket already exists? No need to do the rest.
  if (m_Socket) {
    registerSocket();
    return true;
  }

  m_Socket = netconn_new_with_callback(connType, netconnCallback);
  if (!m_Socket) {
    /// \todo need an error here...
    return false;
  }

  if (NETCONNTYPE_GROUP(m_Socket->type) != NETCONN_TCP) {
    m_Metadata.send = 1;
  }

  registerSocket();

  return true;
}

int LwipSocketSyscalls::connect(const struct sockaddr_storage* address, socklen_t addrlen) {
  ip_addr_t ipaddr;
  ByteSet(&ipaddr, 0, sizeof(ipaddr));
  uint16_t port = 0;
  err_t err = sockaddrToIpaddr(address, port, &ipaddr, false);
  if (err != ERR_OK) {
    N_NOTICE("failed to convert sockaddr");
    lwipToSyscallError(err);
    return -1;
  }

  // set blocking status if needed
  bool blocking = isBlocking();
  netconn_set_nonblocking(m_Socket, blocking ? 0 : 1);

  N_NOTICE("using socket " << m_Socket << "!");
  N_NOTICE(" -> connecting to remote " << ipaddr_ntoa(&ipaddr) << " on port " << Dec << port);

  err = netconn_connect(m_Socket, &ipaddr, port);
  if (err != ERR_OK) {
    N_NOTICE(" -> lwip error");
    lwipToSyscallError(err);
    return -1;
  }

  // need to allow writing immediately for non-tcp sockets
  /// \todo for accept() we need to do this too
  if (NETCONNTYPE_GROUP(m_Socket->type) != NETCONN_TCP) {
    m_Metadata.send = 1;
  }

  N_NOTICE(" -> ok!");
  return 0;
}

ssize_t LwipSocketSyscalls::sendto_msg(const struct msghdr* msghdr) {
  err_t err;

  if (msghdr->msg_name) {
    /// \todo need to build this - but netconn_sendto() requires a netbuf
    SYSCALL_ERROR(Unimplemented);
    return -1;
  }

  // Can we send without blocking?
  if (!isBlocking() && !m_Metadata.send) {
    N_NOTICE(" -> send queue full, would block");
    SYSCALL_ERROR(NoMoreProcesses);
    return -1;
  }

  size_t bytesWritten = 0;
  bool ok = true;

  if (NETCONNTYPE_GROUP(m_Socket->type) == NETCONN_TCP) {
    for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
      void* buffer = msghdr->msg_iov[i].iov_base;
      size_t bufferlen = msghdr->msg_iov[i].iov_len;

      size_t thisBytesWritten = 0;
      err = netconn_write_partly(m_Socket, buffer, bufferlen, NETCONN_COPY | NETCONN_MORE,
                                 &thisBytesWritten);
      if (err != ERR_OK) {
        lwipToSyscallError(err);
        ok = false;
        break;
      }

      bytesWritten += thisBytesWritten;
    }
  } else {
    struct netbuf* buf = netbuf_new();
    for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
      netbuf_ref(buf, msghdr->msg_iov[i].iov_base, msghdr->msg_iov[i].iov_len);
    }

    /// \todo implement sendto
    err = netconn_send(m_Socket, buf);
    if (err != ERR_OK) {
      lwipToSyscallError(err);
      ok = false;
    } else {
      bytesWritten += netbuf_len(buf);
      netbuf_delete(buf);
    }
  }

  if (!bytesWritten) {
    if (!ok) {
      return -1;
    }
  }

  return bytesWritten;
}

ssize_t LwipSocketSyscalls::recvfrom_msg(struct msghdr* msghdr) {
  if (msghdr->msg_name) {
    /// \todo need to build this - extract from the pbuf
    SYSCALL_ERROR(Unimplemented);
    return -1;
  }

  // No data to read right now.
  if (!isBlocking()) {
    bool noData = false;
    {
      ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
      if (m_Metadata.closed) {
        return 0;
      }
      noData = !(m_Metadata.recv || m_Metadata.pb);
    }

    if (noData) {
      // If an app tightly calls recv() and keeps hitting here, it'll
      // burn a lot of cycles for no good reason. Instead, reschedule to
      // reduce that tight spin.
      Scheduler::instance().yield();

      N_NOTICE(" -> no more data available, would block");
      SYSCALL_ERROR(NoMoreProcesses);
      return -1;
    }
  }

  err_t err;
  if (!m_Metadata.pb) {
    struct pbuf* pb = nullptr;
    struct netbuf* buf = nullptr;

    // No partial data present from a previous read. Read new data from
    // the socket.
    if (NETCONNTYPE_GROUP(netconn_type(m_Socket)) == NETCONN_TCP) {
      err = netconn_recv_tcp_pbuf(m_Socket, &pb);
    } else {
      err = netconn_recv(m_Socket, &buf);
    }

    if (err != ERR_OK) {
      if (err == ERR_CLSD) {
        ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
        m_Metadata.closed = true;
        return 0;
      }

      N_NOTICE(" -> lwIP error");
      lwipToSyscallError(err);
      return -1;
    }

    if (pb == nullptr && buf != nullptr) {
      pb = buf->p;
    }
    if (!pb) {
      SYSCALL_ERROR(IoError);
      return -1;
    }

    m_Metadata.offset = 0;
    m_Metadata.pb = pb;
    m_Metadata.buf = buf;
  }

  size_t totalLen = 0;
  for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
    void* buffer = msghdr->msg_iov[i].iov_base;
    size_t bufferlen = msghdr->msg_iov[i].iov_len;

    // now we read some things.
    size_t finalPos = m_Metadata.offset + bufferlen;
    if (finalPos > m_Metadata.pb->tot_len) {
      bufferlen = m_Metadata.pb->tot_len - m_Metadata.offset;
      if (!bufferlen) {
        break;  // finished reading!
      }
    }

    pbuf_copy_partial(m_Metadata.pb, buffer, bufferlen, m_Metadata.offset);
    totalLen += bufferlen;
  }

  // partial read?
  if ((m_Metadata.offset + totalLen) < m_Metadata.pb->tot_len) {
    m_Metadata.offset += totalLen;
  } else {
    if (m_Metadata.buf == nullptr) {
      pbuf_free(m_Metadata.pb);
    } else {
      // will indirectly clean up m_Metadata.pb as it's a member of the
      // netbuf
      netbuf_free(m_Metadata.buf);
    }

    m_Metadata.pb = nullptr;
    m_Metadata.buf = nullptr;
    m_Metadata.offset = 0;
  }

  N_NOTICE(" -> " << totalLen);
  return totalLen;
}

int LwipSocketSyscalls::listen(int backlog) {
  err_t err = netconn_listen_with_backlog(m_Socket, backlog);
  if (err != ERR_OK) {
    N_NOTICE(" -> lwIP error");
    lwipToSyscallError(err);
    return -1;
  }

  return 0;
}

int LwipSocketSyscalls::bind(const struct sockaddr_storage* address, socklen_t addrlen) {
  uint16_t port = 0;
  ip_addr_t ipaddr;
  sockaddrToIpaddr(address, port, &ipaddr);

  err_t err = netconn_bind(m_Socket, &ipaddr, port);
  if (err != ERR_OK) {
    N_NOTICE(" -> lwIP error");
    lwipToSyscallError(err);
    return -1;
  }

  return 0;
}

int LwipSocketSyscalls::accept(struct sockaddr_storage* address, socklen_t* addrlen, int flags) {
  struct netconn* new_conn;
  err_t err = netconn_accept(m_Socket, &new_conn);
  if (err != ERR_OK) {
    N_NOTICE(" -> lwIP error");
    lwipToSyscallError(err);
    return -1;
  }

  // get the new peer
  ip_addr_t peer;
  uint16_t port;
  err = netconn_peer(new_conn, &peer, &port);
  if (err != ERR_OK) {
    netconn_delete(new_conn);
    lwipToSyscallError(err);
    return -1;
  }

  /// \todo handle other families
  struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(address);
  sin->sin_family = AF_INET;
  sin->sin_port = HOST_TO_BIG16(port);
  sin->sin_addr.s_addr = peer.u_addr.ip4.addr;
  *addrlen = sizeof(sockaddr_in);

  LwipSocketSyscalls* obj = new LwipSocketSyscalls(m_Domain, m_Type, m_Protocol);
  obj->m_Socket = new_conn;
  obj->m_Metadata.send = 1;
  obj->create();

  size_t fd = getAvailableDescriptor();
  FileDescriptor* desc = new FileDescriptor;
  desc->networkImpl = obj;
  desc->fd = fd;
  setSocketDescriptorFlags(desc, flags);

  addDescriptor(fd, desc);
  obj->associate(desc);

  return static_cast<int>(fd);
}

int LwipSocketSyscalls::shutdown(int how) {
  int rx = 0;
  int tx = 0;
  if (how == SHUT_RDWR) {
    rx = tx = 1;
  } else if (how == SHUT_RD) {
    rx = 1;
  } else {
    tx = 1;
  }

  err_t err = netconn_shutdown(m_Socket, rx, tx);
  if (err != ERR_OK) {
    lwipToSyscallError(err);
    return -1;
  }

  return 0;
}

int LwipSocketSyscalls::getpeername(struct sockaddr_storage* address, socklen_t* address_len) {
  ip_addr_t peer;
  uint16_t port;
  err_t err = netconn_peer(m_Socket, &peer, &port);
  if (err != ERR_OK) {
    N_NOTICE(" -> getpeername failed");
    lwipToSyscallError(err);
    return -1;
  }

  /// \todo handle other families
  struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(address);
  sin->sin_family = AF_INET;
  sin->sin_port = HOST_TO_BIG16(port);
  sin->sin_addr.s_addr = peer.u_addr.ip4.addr;
  *address_len = sizeof(sockaddr_in);

  return 0;
}

int LwipSocketSyscalls::getsockname(struct sockaddr_storage* address, socklen_t* address_len) {
  ip_addr_t self;
  uint16_t port;
  err_t err = netconn_addr(m_Socket, &self, &port);
  if (err != ERR_OK) {
    lwipToSyscallError(err);
    return -1;
  }

  /// \todo handle other families
  struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(address);
  sin->sin_family = AF_INET;
  sin->sin_port = HOST_TO_BIG16(port);
  sin->sin_addr.s_addr = self.u_addr.ip4.addr;
  *address_len = sizeof(sockaddr_in);

  return 0;
}

int LwipSocketSyscalls::setsockopt(int level, int optname, const void* optvalue, socklen_t optlen) {
  if (optlen < sizeof(int)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  const int value = *reinterpret_cast<const int*>(optvalue);
  if (level == SOL_SOCKET) {
    const uint8_t option = lwipSocketOption(optname);
    if (option) {
      LOCK_TCPIP_CORE();
      struct ip_pcb* pcb = m_Socket ? m_Socket->pcb.ip : nullptr;
      if (!pcb) {
        UNLOCK_TCPIP_CORE();
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }

      if (value) {
        ip_set_option(pcb, option);
      } else {
        ip_reset_option(pcb, option);
      }
      UNLOCK_TCPIP_CORE();
      return 0;
    }
  }

  if (m_Protocol == IPPROTO_TCP && level == IPPROTO_TCP) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    if (optname == linuxTcpNoDelay) {
      LOCK_TCPIP_CORE();
      struct tcp_pcb* pcb = m_Socket ? m_Socket->pcb.tcp : nullptr;
      if (!pcb) {
        UNLOCK_TCPIP_CORE();
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }

      N_NOTICE(" -> TCP_NODELAY");
      N_NOTICE("  --> val=" << value);

      // TCP_NODELAY controls Nagle's algorithm usage
      if (value) {
        tcp_nagle_disable(pcb);
      } else {
        tcp_nagle_enable(pcb);
      }

      UNLOCK_TCPIP_CORE();
      return 0;
    }
#pragma GCC diagnostic pop
  }

  SYSCALL_ERROR(ProtocolNotAvailable);
  return -1;
}

int LwipSocketSyscalls::getsockopt(int level, int optname, void* optvalue, socklen_t* optlen) {
  if (*optlen < sizeof(int)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  int value = 0;
  if (level == SOL_SOCKET) {
    if (optname == SO_TYPE) {
      value = m_Type;
    } else if (optname == SO_ERROR) {
      ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
      value = lwipErrorNumber(m_Metadata.error);
      m_Metadata.error = ERR_OK;
    } else {
      const uint8_t option = lwipSocketOption(optname);
      if (!option) {
        SYSCALL_ERROR(ProtocolNotAvailable);
        return -1;
      }
      LOCK_TCPIP_CORE();
      struct ip_pcb* pcb = m_Socket ? m_Socket->pcb.ip : nullptr;
      if (!pcb) {
        UNLOCK_TCPIP_CORE();
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      value = ip_get_option(pcb, option) ? 1 : 0;
      UNLOCK_TCPIP_CORE();
    }
  } else if (m_Protocol == IPPROTO_TCP && level == IPPROTO_TCP && optname == linuxTcpNoDelay) {
    LOCK_TCPIP_CORE();
    struct tcp_pcb* pcb = m_Socket ? m_Socket->pcb.tcp : nullptr;
    if (!pcb) {
      UNLOCK_TCPIP_CORE();
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    value = tcp_nagle_disabled(pcb) ? 1 : 0;
    UNLOCK_TCPIP_CORE();
  } else {
    SYSCALL_ERROR(ProtocolNotAvailable);
    return -1;
  }

  *reinterpret_cast<int*>(optvalue) = value;
  *optlen = sizeof(int);
  return 0;
}

bool LwipSocketSyscalls::canPoll() const {
  return true;
}

bool LwipSocketSyscalls::poll(bool& read, bool& write, bool& error, Semaphore* waiter) {
  bool ok = false;

  if (!(read || write || error)) {
    // not actually polling for anything
    return true;
  }

  ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);

  if (write) {
    write = m_Metadata.send != 0;
    ok = ok || write;
  }

  if (read) {
    read = m_Metadata.recv || m_Metadata.pb || m_Metadata.closed;
    ok = ok || read;
  }

  if (error) {
    error = m_Metadata.error != ERR_OK;
    ok = ok || error;
  }

  if (waiter && !ok) {
    // Need to wait for socket data.
    /// \todo this is buggy as it'll return for the wrong events!
    m_Metadata.semaphores.pushBack(waiter);
  }

  return ok;
}

void LwipSocketSyscalls::unPoll(Semaphore* waiter) {
  m_Metadata.lock.acquire();
  for (auto it = m_Metadata.semaphores.begin(); it != m_Metadata.semaphores.end();) {
    if ((*it) == waiter) {
      it = m_Metadata.semaphores.erase(it);
    } else {
      ++it;
    }
  }
  m_Metadata.lock.release();
}

void LwipSocketSyscalls::netconnCallback(struct netconn* conn, enum netconn_evt evt, u16_t len) {
  ConstexprLockGuard<Mutex, THREADS> objectsGuard(m_SyscallObjectsLock);
  LwipSocketSyscalls* obj = m_SyscallObjects.lookup(conn);
  if (!obj) {
    // Accepted netconns can receive data before accept() has associated a
    // Pedigree descriptor. lwIP initializes socket to -1 for this exact
    // handoff and invokes this callback while holding its core lock.
    if (conn && conn->socket < 0 && evt == NETCONN_EVT_RCVPLUS) {
      --conn->socket;
    }
    return;
  }

  ConstexprLockGuard<Mutex, THREADS> guard(obj->m_Metadata.lock);

  switch (evt) {
    case NETCONN_EVT_RCVPLUS:
      N_NOTICE("RCV+");
      ++(obj->m_Metadata.recv);
      break;
    case NETCONN_EVT_RCVMINUS:
      N_NOTICE("RCV-");
      if (obj->m_Metadata.recv) {
        --(obj->m_Metadata.recv);
      }
      break;
    case NETCONN_EVT_SENDPLUS:
      N_NOTICE("SND+");
      obj->m_Metadata.send = 1;
      break;
    case NETCONN_EVT_SENDMINUS:
      N_NOTICE("SND-");
      obj->m_Metadata.send = 0;
      break;
    case NETCONN_EVT_ERROR:
      N_NOTICE("ERR");
      obj->m_Metadata.error = netconn_err(conn);
      if (obj->m_Metadata.error == ERR_OK) {
        obj->m_Metadata.error = ERR_IF;
      }
      break;
    default:
      N_NOTICE("Unknown netconn callback error.");
  }

  /// \todo need a way to do this with lwip when threads are off
  EMIT_IF(THREADS) {
    for (auto& it : obj->m_Metadata.semaphores) {
      it->release();
    }
  }
}

void LwipSocketSyscalls::lwipToSyscallError(err_t err) {
  if (err != ERR_OK) {
    N_NOTICE(" -> lwip strerror gives '" << lwip_strerr(err) << "'");
    syscallError(lwipErrorNumber(err));
  }
}

LwipSocketSyscalls::LwipMetadata::LwipMetadata()
    : recv(0),
      send(0),
      error(ERR_OK),
      closed(false),
      lock(),
      semaphores(),
      offset(0),
      pb(nullptr),
      buf(nullptr) {}

UnixSocketSyscalls::UnixSocketSyscalls(int domain, int type, int protocol)
    : NetworkSyscalls(domain, type, protocol),
      m_Socket(nullptr),
      m_Remote(nullptr),
      m_RemoteTracked(false),
      m_LocalPath(),
      m_RemotePath() {}

UnixSocketSyscalls::~UnixSocketSyscalls() {
  N_NOTICE("UnixSocketSyscalls::~UnixSocketSyscalls");
  if (m_Socket) {
    UnixSocket* socket = m_Socket;
    m_Socket = nullptr;
    socket->unbind();
    if (m_LocalPath.length()) {
      LockGuard<Mutex> guard(UnixFilesystem::namespaceLock());
      if (socket->getName().length() && socket->getParent()) {
        Directory* parent = Directory::fromFile(socket->getParent());
        if (parent->lookup(socket->getName().view()) == socket) {
          parent->remove(socket->getName().view());
        }
      }
      VFS::instance().untrackFile(socket);
    } else {
      delete socket;
    }
  }

  if (m_RemoteTracked) {
    UnixSocket* remote = m_Remote;
    m_Remote = nullptr;
    m_RemoteTracked = false;
    releaseTrackedUnixSocket(remote);
  }
}

bool UnixSocketSyscalls::create() {
  if (m_Socket) {
    return true;
  }

  // Create an unnamed unix socket by default.
  m_Socket = new UnixSocket(String(), g_pUnixFilesystem, nullptr, nullptr, getSocketType());

  return true;
}

int UnixSocketSyscalls::connect(const struct sockaddr_storage* address, socklen_t addrlen) {
  String pathname;
  if (!unixSocketPath(address, addrlen, pathname, false)) {
    return -1;
  }

  N_NOTICE(" -> unix connect: '" << pathname << "'");

  File* file = findTrackedUnixSocket(pathname);
  if (!file) {
    SYSCALL_ERROR(DoesNotExist);
    N_NOTICE(" -> unix socket '" << pathname << "' doesn't exist");
    return -1;
  }

  if (!file->isSocket()) {
    /// \todo wrong error
    SYSCALL_ERROR(DoesNotExist);
    N_NOTICE(" -> target '" << pathname << "' is not a unix socket");
    releaseTrackedUnixSocket(file);
    return -1;
  }

  UnixSocket* target = static_cast<UnixSocket*>(file);

  if (getType() == SOCK_STREAM) {
    N_NOTICE(" -> stream");
    if (target->getType() != UnixSocket::Streaming || target->getState() != UnixSocket::Listening) {
      SYSCALL_ERROR(ConnectionRefused);
      releaseTrackedUnixSocket(target);
      return -1;
    }

    // Create the remote for accept() on the server side.
    UnixSocket* remote =
        new UnixSocket(String(), g_pUnixFilesystem, nullptr, nullptr, UnixSocket::Streaming);

    // Pair first so accept can never observe an endpoint before its peer
    // exists. addSocket activates and queues the connection atomically;
    // accept only transfers ownership of the queued endpoint.
    if (!m_Socket->bind(remote, false)) {
      delete remote;
      SYSCALL_ERROR(IsConnected);
      releaseTrackedUnixSocket(target);
      return -1;
    }
    if (!target->addSocket(remote)) {
      remote->failConnection();
      delete remote;
      SYSCALL_ERROR(ConnectionRefused);
      releaseTrackedUnixSocket(target);
      return -1;
    }
    N_NOTICE(" -> stream connected and queued");
  } else {
    if (target->getType() != UnixSocket::Datagram) {
      SYSCALL_ERROR(ProtocolWrongType);
      releaseTrackedUnixSocket(target);
      return -1;
    }
    N_NOTICE(" -> dgram");
  }

  if (m_RemoteTracked) {
    releaseTrackedUnixSocket(m_Remote);
  }
  m_Remote = target;
  m_RemoteTracked = true;
  m_RemotePath = pedigree_std::move(pathname);

  N_NOTICE(" -> remote is now " << m_RemotePath);

  if (getType() == SOCK_STREAM && !isBlocking()) {
    SYSCALL_ERROR(InProgress);
    return -1;
  }

  return 0;
}

ssize_t UnixSocketSyscalls::sendto_msg(const struct msghdr* msghdr) {
  N_NOTICE("UnixSocketSyscalls::sendto_msg");

  UnixSocket* remote = getRemote();
  UnixSocket* temporaryRemote = nullptr;
  if (getType() == SOCK_STREAM && !remote) {
    const bool closed = m_Socket && m_Socket->getState() == UnixSocket::Closed;
    N_NOTICE(" -> " << (closed ? "closed" : "not connected"));
    syscallError(closed ? Error::BrokenPipe : Error::NotConnected);
    return -1;
  }

  if (!m_Remote && getType() != SOCK_STREAM) {
    if (!msghdr->msg_name) {
      /// \todo needs some sort of errno here
      N_NOTICE(" -> sendto on unconnected socket with no address");
      return -1;
    }

    String pathname;
    if (!unixSocketPath(reinterpret_cast<const struct sockaddr_storage*>(msghdr->msg_name),
                        msghdr->msg_namelen, pathname, false)) {
      return -1;
    }

    N_NOTICE(" -> unix connect: '" << pathname << "'");

    File* file = findTrackedUnixSocket(pathname);
    if (!file) {
      SYSCALL_ERROR(DoesNotExist);
      N_NOTICE(" -> unix socket '" << pathname << "' doesn't exist");
      return -1;
    }

    if (!file->isSocket()) {
      /// \todo wrong error
      SYSCALL_ERROR(DoesNotExist);
      N_NOTICE(" -> target '" << pathname << "' is not a unix socket");
      releaseTrackedUnixSocket(file);
      return -1;
    }

    remote = static_cast<UnixSocket*>(file);
    temporaryRemote = remote;
  }

  if (getType() != SOCK_STREAM && (!remote || remote->getType() != UnixSocket::Datagram ||
                                   remote->getState() == UnixSocket::Closed)) {
    releaseTrackedUnixSocket(temporaryRemote);
    syscallError(remote && remote->getType() != UnixSocket::Datagram ? Error::ProtocolWrongType
                                                                     : Error::ConnectionRefused);
    return -1;
  }

  N_NOTICE(" -> transmitting!");

  uint64_t numWritten = 0;
  for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
    void* buffer = msghdr->msg_iov[i].iov_base;
    size_t bufferlen = msghdr->msg_iov[i].iov_len;

    uint64_t thisWrite =
        remote->write(reinterpret_cast<uintptr_t>(static_cast<const char*>(m_LocalPath)), bufferlen,
                      reinterpret_cast<uintptr_t>(buffer), isBlocking());

    if (!thisWrite) {
      // eof or some other similar condition
      break;
    }

    numWritten += thisWrite;
  }
  releaseTrackedUnixSocket(temporaryRemote);
  if (!numWritten) {
    if (getType() == SOCK_STREAM && m_Socket->getState() == UnixSocket::Closed) {
      SYSCALL_ERROR(BrokenPipe);
      N_NOTICE(" -> -1 (EPIPE)");
      return -1;
    }

    if (!isBlocking()) {
      SYSCALL_ERROR(NoMoreProcesses);
      N_NOTICE(" -> -1 (EAGAIN)");
      return -1;
    }
  }
  N_NOTICE(" -> " << numWritten);
  return numWritten;
}

ssize_t UnixSocketSyscalls::recvfrom_msg(struct msghdr* msghdr) {
  String remote;
  uint64_t numRead = 0;
  for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
    void* buffer = msghdr->msg_iov[i].iov_base;
    size_t bufferlen = msghdr->msg_iov[i].iov_len;

    uint64_t thisRead =
        m_Socket->recvfrom(bufferlen, reinterpret_cast<uintptr_t>(buffer), isBlocking(), remote);
    if (!thisRead) {
      // eof or some other similar condition
      break;
    }

    numRead += thisRead;
  }

  if (numRead && msghdr->msg_name) {
    struct sockaddr_un* un = reinterpret_cast<struct sockaddr_un*>(msghdr->msg_name);
    const size_t pathOffset = offsetof(struct sockaddr_un, sun_path);
    const size_t capacity = msghdr->msg_namelen;
    if (capacity >= sizeof(sa_family_t)) {
      un->sun_family = AF_UNIX;
    }
    if (capacity > pathOffset) {
      const size_t available = capacity - pathOffset;
      if (remote.length()) {
        StringCopyN(un->sun_path, remote.cstr(), available);
        un->sun_path[available - 1] = 0;
      } else {
        un->sun_path[0] = 0;
      }
    }
    msghdr->msg_namelen = sizeof(sa_family_t) + remote.length() + (remote.length() ? 1 : 0);
  }

  /// \todo get info from the socket about things like truncated buffer
  msghdr->msg_flags = 0;
  if (!numRead) {
    if (getType() == SOCK_STREAM && m_Socket->getState() == UnixSocket::Closed) {
      N_NOTICE(" -> 0 (EOF)");
      return 0;
    }

    if (!isBlocking()) {
      SYSCALL_ERROR(NoMoreProcesses);
      N_NOTICE(" -> -1 (EAGAIN)");
      return -1;
    }
  }
  N_NOTICE(" -> " << numRead);
  return numRead;
}

int UnixSocketSyscalls::listen(int backlog) {
  (void)backlog;

  if (m_Socket->getType() != UnixSocket::Streaming) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }

  /// \todo bind to an unnamed socket if we aren't already bound

  if (!m_Socket->markListening()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  return 0;
}

int UnixSocketSyscalls::bind(const struct sockaddr_storage* address, socklen_t addrlen) {
  /// \todo unbind existing socket if one exists.

  String adjusted_pathname;
  if (!unixSocketPath(address, addrlen, adjusted_pathname, true)) {
    return -1;
  }
  if (!adjusted_pathname.length()) {
    /// \todo re-bind an unnamed address if we are bound already
    return 0;
  }

  N_NOTICE(" -> unix bind: '" << adjusted_pathname << "'");

  File* cwd = VFS::instance().find(String("."));
  if (adjusted_pathname.endswith('/')) {
    // uh, that's a directory
    SYSCALL_ERROR(IsADirectory);
    return -1;
  }

  File* parentDirectory = cwd;

  const char* pDirname = DirectoryName(static_cast<const char*>(adjusted_pathname));
  const char* pBasename = BaseName(static_cast<const char*>(adjusted_pathname));

  String basename(pBasename);
  delete[] pBasename;

  if (pDirname) {
    // Reorder rfind result to be from beginning of string.
    String dirname(pDirname);
    delete[] pDirname;

    N_NOTICE(" -> dirname=" << dirname);

    parentDirectory = VFS::instance().find(dirname);
    if (!parentDirectory) {
      N_NOTICE(" -> parent directory '" << dirname << "' doesn't exist");
      SYSCALL_ERROR(DoesNotExist);
      return -1;
    }
  }

  if (!parentDirectory->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return -1;
  }

  Directory* pDir = Directory::fromFile(parentDirectory);

  /// \todo does this actually create a findable file?
  UnixSocket* socket = new UnixSocket(basename, parentDirectory->getFilesystem(), parentDirectory,
                                      nullptr, getSocketType());
  bool added = false;
  {
    LockGuard<Mutex> guard(UnixFilesystem::namespaceLock());
    added = pDir->addEphemeralFile(socket);
    if (added) {
      // The directory and this open socket each own one VFS reference.
      // Keeping both changes atomic prevents unlink from observing the
      // pathname before the descriptor has pinned its endpoint.
      VFS::instance().trackFile(socket);
    }
  }
  if (!added) {
    delete socket;
    SYSCALL_ERROR(AddressInUse);
    return -1;
  }
  N_NOTICE(" -> basename=" << basename);

  // bind() then connect().
  if (!m_LocalPath.length()) {
    // just an unnamed socket, safe to delete.
    delete m_Socket;
  }

  m_Socket = socket;
  m_LocalPath = pedigree_std::move(adjusted_pathname);

  return 0;
}

int UnixSocketSyscalls::accept(struct sockaddr_storage* address, socklen_t* addrlen, int flags) {
  N_NOTICE("unix accept");
  UnixSocket* remote = m_Socket->getSocket(isBlocking());
  if (!remote) {
    N_NOTICE("accept() failed");
    SYSCALL_ERROR(NoMoreProcesses);
    return -1;
  }

  if (remote) {
    N_NOTICE("accept() got a socket");

    struct sockaddr_un* sun = reinterpret_cast<struct sockaddr_un*>(address);

    if (remote->getName().length()) {
      // Named.
      String name;
      remote->getFullPath(name);

      StringCopy(sun->sun_path, name.cstr());
      *addrlen = sizeof(sa_family_t) + name.length();
    } else {
      *addrlen = sizeof(sa_family_t);
    }

    sun->sun_family = AF_UNIX;

    UnixSocketSyscalls* obj = new UnixSocketSyscalls(m_Domain, m_Type, m_Protocol);
    obj->m_Socket = remote;
    obj->m_Remote = nullptr;
    obj->m_LocalPath = m_LocalPath;
    obj->m_RemotePath = String();
    obj->create();

    size_t fd = getAvailableDescriptor();
    FileDescriptor* desc = new FileDescriptor;
    desc->networkImpl = obj;
    desc->fd = fd;
    setSocketDescriptorFlags(desc, flags);

    addDescriptor(fd, desc);
    obj->associate(desc);

    return static_cast<int>(fd);
  }

  return -1;
}

int UnixSocketSyscalls::shutdown(int how) {
  /// \todo
  N_NOTICE("UnixSocketSyscalls::shutdown");
  return 0;
}

int UnixSocketSyscalls::getpeername(struct sockaddr_storage* address, socklen_t* address_len) {
  N_NOTICE("UNIX getpeername");
  if (!m_Socket->wasConnected()) {
    SYSCALL_ERROR(NotConnected);
    return -1;
  }

  struct sockaddr_un* sun = reinterpret_cast<struct sockaddr_un*>(address);
  sun->sun_family = AF_UNIX;
  StringCopy(sun->sun_path, m_RemotePath.cstr());
  *address_len = sizeof(sa_family_t) + m_RemotePath.length() + (m_RemotePath.length() ? 1 : 0);

  N_NOTICE(" -> " << m_RemotePath);
  return 0;
}

int UnixSocketSyscalls::getsockname(struct sockaddr_storage* address, socklen_t* address_len) {
  N_NOTICE("UNIX getsockname");
  struct sockaddr_un* sun = reinterpret_cast<struct sockaddr_un*>(address);
  sun->sun_family = AF_UNIX;
  StringCopy(sun->sun_path, m_LocalPath.cstr());
  *address_len = sizeof(sa_family_t) + m_LocalPath.length() + (m_LocalPath.length() ? 1 : 0);

  N_NOTICE(" -> " << m_LocalPath);
  return 0;
}

int UnixSocketSyscalls::setsockopt(int level, int optname, const void* optvalue, socklen_t optlen) {
  if (level == SOL_SOCKET && optname == SO_REUSEADDR) {
    if (optlen < sizeof(int)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }

    // Local pathname sockets have no TIME_WAIT address state, but Go's
    // listener setup applies SO_REUSEADDR to every Linux socket.
    (void)*reinterpret_cast<const int*>(optvalue);
    return 0;
  }

  SYSCALL_ERROR(ProtocolNotAvailable);
  return -1;
}

int UnixSocketSyscalls::getsockopt(int level, int optname, void* optvalue, socklen_t* optlen) {
  if (level == SOL_SOCKET) {
    if (optname == SO_TYPE || optname == SO_ERROR) {
      if (*optlen < sizeof(int)) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }

      int value = getType();
      if (optname == SO_ERROR) {
        const UnixSocket::SocketState state = m_Socket->getState();
        if (m_Socket->wasConnected()) {
          value = 0;
        } else if (state == UnixSocket::Connecting) {
          value = Error::InProgress;
        } else if (state == UnixSocket::Closed) {
          value = Error::ConnectionRefused;
        } else {
          value = Error::NotConnected;
        }
      }

      *reinterpret_cast<int*>(optvalue) = value;
      *optlen = sizeof(value);
      return 0;
    } else if (optname == SO_PEERCRED) {
      N_NOTICE(" -> SO_PEERCRED");
      if (!m_Socket->wasConnected()) {
        SYSCALL_ERROR(NotConnected);
        return -1;
      }
      if (*optlen < sizeof(struct ucred)) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }

      // get credentials of other side of this socket
      struct ucred* targetCreds = reinterpret_cast<struct ucred*>(optvalue);
      struct ucred sourceCreds = m_Socket->getPeerCredentials();

      N_NOTICE(" --> pid=" << Dec << sourceCreds.pid);
      N_NOTICE(" --> uid=" << Dec << sourceCreds.uid);
      N_NOTICE(" --> gid=" << Dec << sourceCreds.gid);

      *targetCreds = sourceCreds;
      *optlen = sizeof(sourceCreds);

      return 0;
    }
  }

  SYSCALL_ERROR(ProtocolNotAvailable);
  return -1;
}

bool UnixSocketSyscalls::canPoll() const {
  return m_Socket != nullptr;
}

bool UnixSocketSyscalls::poll(bool& read, bool& write, bool& error, Semaphore* waiter) {
  UnixSocket* local = m_Socket;
  const bool checkRead = read;
  const bool checkWrite = write;
  read = false;
  write = false;
  error = false;

  if (!local) {
    error = true;
    return true;
  }

  const UnixSocket::SocketState state = local->getState();
  if (state == UnixSocket::Closed) {
    // The poll interface cannot express POLLHUP separately. POLLERR wakes
    // writers, while readable lets readers drain buffered data then see
    // persistent EOF.
    read = checkRead;
    error = true;
    return true;
  }

  bool ok = false;
  if (checkRead) {
    read = local->select(false, 0);
    ok = ok || read;
  }

  if (checkWrite) {
    write = local->select(true, 0);
    ok = ok || write;
  }

  if (waiter && !ok) {
    local->addWaiter(waiter, checkRead, checkWrite);
  }

  return ok;
}

void UnixSocketSyscalls::unPoll(Semaphore* waiter) {
  if (m_Socket) {
    m_Socket->removeWaiter(waiter);
  }
}

bool UnixSocketSyscalls::monitor(Thread* pThread, Event* pEvent) {
  if (!m_Socket) {
    return false;
  }

  m_Socket->addWaiter(pThread, pEvent);
  return true;
}

bool UnixSocketSyscalls::unmonitor(Event* pEvent) {
  if (!m_Socket) {
    return false;
  }

  m_Socket->removeWaiter(pEvent);
  return true;
}

bool UnixSocketSyscalls::pairWith(UnixSocketSyscalls* other) {
  if (!m_Socket->bind(other->m_Socket)) {
    return false;
  }

  // make sure both sides can use the socket
  other->m_Socket->acknowledgeBind();

  m_Remote = other->m_Socket;
  other->m_Remote = m_Socket;
  return true;
}

UnixSocket* UnixSocketSyscalls::getRemote() const {
  UnixSocket* remote = m_Remote;
  if (getType() == SOCK_STREAM) {
    remote = m_Socket && m_Socket->wasConnected() ? m_Socket : nullptr;
  }

  return remote;
}

UnixSocket::SocketType UnixSocketSyscalls::getSocketType() const {
  if (getType() == SOCK_STREAM) {
    return UnixSocket::Streaming;
  }

  return UnixSocket::Datagram;
}
