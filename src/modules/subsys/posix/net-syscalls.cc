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
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/HashTable.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/UniqueResource.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>

#include "eventfd-syscalls.h"
#include "file-syscalls.h"
#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/ResolvedPath.h"
#include "modules/subsys/posix/UnixFilesystem.h"
#include "modules/system/lwip/include/lwip/api.h"
#include "modules/system/lwip/include/lwip/ip.h"
#include "modules/system/lwip/include/lwip/ip_addr.h"
#include "modules/system/lwip/include/lwip/tcp.h"
#include "modules/system/lwip/include/lwip/tcpip.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MountView.h"
#include "modules/system/vfs/VFS.h"
#include "mqueue-netlink.h"
#include "net-syscalls.h"
#include "recvmmsg-syscalls.h"
#include "signalfd-syscalls.h"
#include "timerfd-syscalls.h"

#ifndef UTILITY_LINUX
#include <netdb.h>

#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <netinet/in.h>
#include <sys/un.h>

Tree<struct netconn*, LwipSocketSyscalls*> LwipSocketSyscalls::m_SyscallObjects;
Mutex LwipSocketSyscalls::m_SyscallObjectsLock;
Tree<UnixSocket*, UnixSocketSyscalls*> UnixSocketSyscalls::m_SyscallObjects;
Tree<UnixSocket*, UnixSocket*> UnixSocketSyscalls::m_Peers;
Tree<UnixSocket*, UnixSocket*> UnixSocketSyscalls::m_PendingListeners;
Mutex UnixSocketSyscalls::m_SyscallObjectsLock;

namespace {
struct NetbufReleaser {
  static void release(struct netbuf* buffer) {
    netbuf_delete(buffer);
  }
};

using NetbufOwner = UniqueResource<struct netbuf, NetbufReleaser>;

class SocketPayload {
 public:
  bool prepare(const struct msghdr& message, int type, int domain, bool sending,
               bool kernelBuffer = false) {
    size_t requested = 0;
    for (size_t i = 0; i < static_cast<size_t>(message.msg_iovlen); ++i) {
      if (message.msg_iov[i].iov_len > static_cast<size_t>(SSIZE_MAX) - requested) {
        SYSCALL_ERROR(InvalidArgument);
        return false;
      }
      requested += message.msg_iov[i].iov_len;
    }
    size_t capacity = requested;
    // Streams may make a short transfer. Datagrams must remain indivisible.
    if (type == SOCK_STREAM && capacity > 65536) {
      capacity = 65536;
    } else if (domain == 16 && !sending && capacity > 32) {
      capacity = 32;
    } else if (domain == AF_INET && type == SOCK_DGRAM && capacity > 65535) {
      if (sending) {
        syscallError(EMSGSIZE);
        return false;
      }
      capacity = 65535;
    }
    if (capacity) {
      m_Bytes = UniqueArray<uint8_t>::allocate(capacity);
      if (!m_Bytes) {
        SYSCALL_ERROR(OutOfMemory);
        return false;
      }
    }
    m_Vector = {m_Bytes.get(), capacity};
    if (!sending) {
      return true;
    }
    size_t copied = 0;
    for (size_t i = 0; i < static_cast<size_t>(message.msg_iovlen) && copied < capacity; ++i) {
      const size_t amount = message.msg_iov[i].iov_len < capacity - copied
                                ? message.msg_iov[i].iov_len
                                : capacity - copied;
      if (kernelBuffer) {
        MemoryCopy(m_Bytes.get() + copied, message.msg_iov[i].iov_base, amount);
      } else if (!PosixSubsystem::copyFromUser(m_Bytes.get() + copied, message.msg_iov[i].iov_base,
                                               amount)) {
        SYSCALL_ERROR(BadAddress);
        return false;
      }
      copied += amount;
    }
    return true;
  }

  void attach(struct msghdr& message) {
    message.msg_iov = &m_Vector;
    message.msg_iovlen = 1;
  }

  bool copyReceived(const struct msghdr& target, size_t received) {
    // MSG_TRUNC can report the packet length beyond the copied payload.
    const size_t length = received < m_Vector.iov_len ? received : m_Vector.iov_len;
    size_t copied = 0;
    for (size_t i = 0; i < static_cast<size_t>(target.msg_iovlen) && copied < length; ++i) {
      const size_t amount =
          target.msg_iov[i].iov_len < length - copied ? target.msg_iov[i].iov_len : length - copied;
      if (!PosixSubsystem::copyToUser(target.msg_iov[i].iov_base, m_Bytes.get() + copied, amount)) {
        SYSCALL_ERROR(BadAddress);
        return false;
      }
      copied += amount;
    }
    return true;
  }

 private:
  UniqueArray<uint8_t> m_Bytes;
  struct iovec m_Vector = {};
};

bool copySocketAddress(const struct sockaddr_storage* address, socklen_t length,
                       struct sockaddr_storage& result) {
  if (length < sizeof(sa_family_t) || length > sizeof(result)) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (!PosixSubsystem::copyFromUser(&result, address, length)) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  if ((result.ss_family == AF_INET && length < sizeof(struct sockaddr_in)) ||
      (result.ss_family == AF_INET6 && length < sizeof(struct sockaddr_in6))) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  return true;
}

bool validateSocketMessageFlags(int flags, bool sending, int domain = 0) {
  int supported = sending ? 0 : MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
  if (sending || domain == 16) {
    // Socket writes do not currently raise SIGPIPE, so suppression requires
    // no additional backend action.
    supported |= MSG_NOSIGNAL;
  }
#else
  (void)sending;
#endif
#ifdef MSG_WAITALL
  if (!sending && domain == 16) {
    // musl receives its fixed-size SIGEV_THREAD cookie with MSG_WAITALL.
    supported |= MSG_WAITALL;
  }
#endif
#ifdef MSG_TRUNC
  if (!sending) {
    supported |= MSG_TRUNC;
  }
#endif
#ifdef MSG_CMSG_CLOEXEC
  if (!sending) {
    supported |= MSG_CMSG_CLOEXEC;
  }
#endif

  if (flags & ~supported) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  return true;
}

constexpr size_t MaximumControlBytes = CMSG_SPACE(SocketRights::MaximumDescriptors * sizeof(int));

bool parseSocketRights(const struct msghdr& message, SharedPointer<SocketRights>& rights) {
  rights.reset();
  const size_t controlLength = static_cast<size_t>(message.msg_controllen);
  if (!controlLength) {
    return true;
  }
  if (!message.msg_control) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  if (controlLength > MaximumControlBytes) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  UniqueArray<uint8_t> control = UniqueArray<uint8_t>::allocate(controlLength);
  if (!PosixSubsystem::copyFromUser(control.get(), message.msg_control, controlLength)) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }

  constexpr size_t HeaderLength = CMSG_LEN(0);
  if (controlLength < HeaderLength) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  struct cmsghdr header = {};
  MemoryCopy(&header, control.get(), sizeof(header));
  const size_t recordLength = static_cast<size_t>(header.cmsg_len);
  if (recordLength < HeaderLength || recordLength > controlLength) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (header.cmsg_level != SOL_SOCKET || header.cmsg_type != SCM_RIGHTS) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }

  const size_t alignedRecordLength = CMSG_ALIGN(recordLength);
  if ((controlLength != recordLength && controlLength != alignedRecordLength) ||
      alignedRecordLength < recordLength) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  const size_t descriptorBytes = recordLength - HeaderLength;
  if (!descriptorBytes || (descriptorBytes % sizeof(int))) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  const size_t descriptorCount = descriptorBytes / sizeof(int);
  if (descriptorCount > SocketRights::MaximumDescriptors) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  if (!SocketRights::create(descriptorCount, rights)) {
    SYSCALL_ERROR(TooManyReferences);
    return false;
  }

  PosixSubsystem* subsystem = getSubsystem();
  if (!subsystem) {
    rights.reset();
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }

  const uint8_t* descriptorData = control.get() + HeaderLength;
  for (size_t i = 0; i < descriptorCount; ++i) {
    int fd = -1;
    MemoryCopy(&fd, descriptorData + (i * sizeof(fd)), sizeof(fd));
    DescriptorLease descriptor;
    if (fd < 0 || !subsystem->acquireFileDescriptor(static_cast<size_t>(fd), descriptor)) {
      rights.reset();
      SYSCALL_ERROR(BadFileDescriptor);
      return false;
    }
    if (descriptor->epollImpl ||
        (descriptor->networkImpl && descriptor->networkImpl->getDomain() == AF_UNIX)) {
      rights.reset();
      SYSCALL_ERROR(OperationNotSupported);
      return false;
    }

    FileDescriptor* transferred = new FileDescriptor(*descriptor);
    if ((descriptor->networkImpl && !transferred->networkPublished()) ||
        (descriptor->getEventFdImpl() && !transferred->eventFdPublished()) ||
        (descriptor->getTimerFdImpl() && !transferred->timerFdPublished()) ||
        (descriptor->getSignalFdImpl() && !transferred->signalFdPublished())) {
      delete transferred;
      rights.reset();
      SYSCALL_ERROR(BadFileDescriptor);
      return false;
    }
    transferred->fd = ~static_cast<size_t>(0);
    transferred->setFlags(0);
    rights->append(transferred);
  }

  return true;
}
}  // namespace

static File* findTrackedUnixSocket(const String& pathname) {
  ResolvedPath fileLease;
  File* file = findFilePath(pathname, fileLease, FilesystemPathRef(), true);
  if (file && !file->retainVfsReference()) {
    return nullptr;
  }
  return file;
}

static void releaseTrackedUnixSocket(File* file) {
  if (!file) {
    return;
  }

  file->releaseVfsReference();
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

static bool isSaneSocket(const DescriptorLease& f) {
  if (!f) {
    N_NOTICE(" -> isSaneSocket: descriptor is null");
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }

  if (!f->networkImpl) {
    N_NOTICE(" -> isSaneSocket: no network implementation found");
    syscallError(ENOTSOCK);
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

  if (!un->sun_path[0]) {
    static constexpr char digits[] = "0123456789abcdef";
    char encoded[1 + 2 * sizeof(un->sun_path) + 1];
    size_t encodedLength = 1;
    encoded[0] = '\1';
    for (size_t i = 1; i < pathLength; ++i) {
      const uint8_t value = static_cast<uint8_t>(un->sun_path[i]);
      encoded[encodedLength++] = digits[value >> 4];
      encoded[encodedLength++] = digits[value & 0xf];
    }
    encoded[encodedLength] = 0;
    path.assign(encoded, encodedLength);
    return true;
  }

  char boundedPath[sizeof(un->sun_path) + 1];
  ByteSet(boundedPath, 0, sizeof(boundedPath));
  MemoryCopy(boundedPath, un->sun_path, pathLength);
  normalisePath(path, boundedPath);
  if (path.length() >= sizeof(un->sun_path)) {
    SYSCALL_ERROR(NameTooLong);
    return false;
  }
  return true;
}

static bool isAbstractUnixSocket(const String& address) {
  return address.length() && address[0] == '\1';
}

static uint8_t decodeHexDigit(char value) {
  return value >= 'a' ? static_cast<uint8_t>(value - 'a' + 10) : static_cast<uint8_t>(value - '0');
}

static void writeUnixSocketAddress(const String& value, struct sockaddr_storage* address,
                                   socklen_t* addressLength) {
  const size_t pathOffset = offsetof(struct sockaddr_un, sun_path);
  const bool abstract = isAbstractUnixSocket(value);
  const size_t nameLength = abstract ? (value.length() - 1) / 2 : value.length();
  const size_t required = pathOffset + (abstract     ? 1 + nameLength
                                        : nameLength ? nameLength + 1
                                                     : 0);
  const size_t capacity = addressLength ? *addressLength : 0;

  if (address && capacity) {
    ByteSet(address, 0, capacity < sizeof(sockaddr_un) ? capacity : sizeof(sockaddr_un));
    auto* un = reinterpret_cast<struct sockaddr_un*>(address);
    if (capacity >= sizeof(sa_family_t)) {
      un->sun_family = AF_UNIX;
    }
    if (capacity > pathOffset) {
      const size_t available = capacity - pathOffset;
      if (abstract) {
        const size_t amount = nameLength < available - 1 ? nameLength : available - 1;
        for (size_t i = 0; i < amount; ++i) {
          un->sun_path[i + 1] = static_cast<char>((decodeHexDigit(value[1 + 2 * i]) << 4) |
                                                  decodeHexDigit(value[2 + 2 * i]));
        }
      } else if (nameLength) {
        const size_t amount = nameLength < available - 1 ? nameLength : available - 1;
        MemoryCopy(un->sun_path, value.cstr(), amount);
      }
    }
  }
  if (addressLength) {
    *addressLength = required;
  }
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
  NetworkSyscalls* syscalls;

  if (domain == AF_UNIX) {
    if (socketType != SOCK_STREAM && socketType != SOCK_DGRAM) {
      SYSCALL_ERROR(OperationNotSupported);
      return -1;
    }
    syscalls = new UnixSocketSyscalls(domain, socketType, protocol);
  } else if (domain == 16) {
    syscalls = new MqueueNetlinkSocket(socketType, protocol);
  } else {
    /// \todo handle non-lwIP domains
    syscalls = new LwipSocketSyscalls(domain, socketType, protocol);
  }

  if (!syscalls->create()) {
    delete syscalls;
    return -1;
  }

  FileDescriptor* f = new FileDescriptor;
  f->setNetworkImpl(SharedPointer<NetworkSyscalls>(syscalls));
  setSocketDescriptorFlags(f, flags);
  DescriptorLease installed;
  const size_t fd = installDescriptor(f, installed);
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
    N_NOTICE(" -> bad domain");
    syscallError(EAFNOSUPPORT);
    return -1;
  }

  int socketType = 0;
  int flags = 0;
  if (!splitSocketType(type, socketType, flags)) {
    return -1;
  }
  if (socketType != SOCK_STREAM) {
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

  fA->setNetworkImpl(SharedPointer<NetworkSyscalls>(syscallsA));
  fB->setNetworkImpl(SharedPointer<NetworkSyscalls>(syscallsB));

  setSocketDescriptorFlags(fA, flags);
  setSocketDescriptorFlags(fB, flags);

  DescriptorLease installedA;
  DescriptorLease installedB;
  const size_t fdA = installDescriptor(fA, installedA);
  const size_t fdB = installDescriptor(fB, installedB);

  syscallsA->associate(fA);
  syscallsB->associate(fB);

  const int result[2] = {static_cast<int>(fdA), static_cast<int>(fdB)};
  if (!PosixSubsystem::copyToUser(sv, result, sizeof(result))) {
    removeDescriptor(result[0], installedA);
    removeDescriptor(result[1], installedB);
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  N_NOTICE(" -> " << result[0] << ", " << result[1]);
  return 0;
}

int posix_connect(int sock, const struct sockaddr_storage* address, socklen_t addrlen) {
  N_NOTICE("connect");

  struct sockaddr_storage snapshot = {};
  if (!copySocketAddress(address, addrlen, snapshot)) {
    return -1;
  }

  N_NOTICE("connect(" << sock << ", " << reinterpret_cast<uintptr_t>(address) << ", " << addrlen
                      << ")");

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  if (snapshot.ss_family != f->networkImpl->getDomain()) {
    syscallError(EAFNOSUPPORT);
    N_NOTICE(" -> incorrect address family passed to connect()");
    return -1;
  }

  Thread* thread = beginInterruptibleSocketCall();
  const int result = f->networkImpl->connect(&snapshot, addrlen);
  return finishInterruptibleSocketCall(thread, static_cast<ssize_t>(result)) ? result : -1;
}

ssize_t posix_send(int sock, const void* buff, size_t bufflen, int flags) {
  N_NOTICE("send");

  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen,
                                    PosixSubsystem::SafeRead)) {
    N_NOTICE("send -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  N_NOTICE("send(" << sock << ", " << buff << ", " << bufflen << ", " << flags << ")");

  DescriptorLease f;
  acquireDescriptor(sock, f);
  return posix_send_descriptor(f, buff, bufflen, flags);
}

ssize_t posix_send_descriptor(const DescriptorLease& f, const void* buff, size_t bufflen, int flags,
                              bool kernelBuffer) {
  if (!validateSocketMessageFlags(flags, true)) {
    return -1;
  }
  if (!isSaneSocket(f)) {
    return -1;
  }

  struct iovec vector = {const_cast<void*>(buff), bufflen};
  struct msghdr message = {};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  message.msg_flags = flags;
  return posix_sendmsg_descriptor(f, &message, SharedPointer<SocketRights>(), kernelBuffer);
}

ssize_t posix_sendmsg_descriptor(const DescriptorLease& f, const struct msghdr* message,
                                 const SharedPointer<SocketRights>& rights, bool kernelBuffer) {
  if (!isSaneSocket(f)) {
    return -1;
  }

  SocketPayload payload;
  if (!payload.prepare(*message, f->networkImpl->getType(), f->networkImpl->getDomain(), true,
                       kernelBuffer)) {
    return -1;
  }
  struct msghdr snapshot = *message;
  payload.attach(snapshot);
  Thread* thread = beginInterruptibleSocketCall();
  const ssize_t result = f->networkImpl->sendto_msg(&snapshot, rights);
  return finishInterruptibleSocketCall(thread, result) ? result : -1;
}

ssize_t posix_sendto(int sock, const void* buff, size_t bufflen, int flags,
                     struct sockaddr_storage* address, socklen_t addrlen) {
  N_NOTICE("sendto");

  if (!validateSocketMessageFlags(flags, true)) {
    return -1;
  }

  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen,
                                    PosixSubsystem::SafeRead)) {
    N_NOTICE("sendto -> invalid address for transmission buffer");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  struct sockaddr_storage destination = {};
  const struct sockaddr_storage* destinationAddress = nullptr;
  if (address) {
    if (addrlen < sizeof(sa_family_t) || addrlen > sizeof(destination)) {
      N_NOTICE("sendto -> invalid destination address length");
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (!PosixSubsystem::copyFromUser(&destination, address, addrlen)) {
      N_NOTICE("sendto -> invalid destination address");
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    destinationAddress = &destination;
  }

  N_NOTICE("sendto(" << sock << ", " << buff << ", " << bufflen << ", " << flags << ", " << address
                     << ", " << addrlen << ")");

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  struct iovec vector = {const_cast<void*>(buff), bufflen};
  struct msghdr message = {};
  message.msg_name = const_cast<struct sockaddr_storage*>(destinationAddress);
  message.msg_namelen = addrlen;
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  message.msg_flags = flags;
  return posix_sendmsg_descriptor(f, &message);
}

ssize_t posix_recv(int sock, void* buff, size_t bufflen, int flags) {
  N_NOTICE("recv");

  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen,
                                    PosixSubsystem::SafeWrite)) {
    N_NOTICE("recv -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  N_NOTICE("recv(" << sock << ", " << buff << ", " << bufflen << ", " << flags << ")");

  DescriptorLease f;
  acquireDescriptor(sock, f);
  ssize_t n = posix_recv_descriptor(f, buff, bufflen, flags);

  N_NOTICE(" -> " << n);
  return n;
}

ssize_t posix_recv_descriptor(const DescriptorLease& f, void* buff, size_t bufflen, int flags) {
  if (!isSaneSocket(f)) {
    return -1;
  }
  if (!validateSocketMessageFlags(flags, false, f->networkImpl->getDomain())) {
    return -1;
  }

  struct iovec vector = {buff, bufflen};
  struct msghdr message = {};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  message.msg_flags = flags;
  return posix_recvmsg_descriptor(f, &message);
}

ssize_t posix_recvmsg_descriptor(const DescriptorLease& f, struct msghdr* message,
                                 SharedPointer<SocketRights>* rights) {
  if (!isSaneSocket(f)) {
    return -1;
  }

  const int pendingError = f->networkImpl->takeReceiveError();
  if (pendingError) {
    syscallError(pendingError);
    return -1;
  }
  SocketPayload payload;
  if (!payload.prepare(*message, f->networkImpl->getType(), f->networkImpl->getDomain(), false)) {
    return -1;
  }
  struct msghdr snapshot = *message;
  payload.attach(snapshot);
  Thread* thread = beginInterruptibleSocketCall();
  const ssize_t result = f->networkImpl->recvfrom_msg(&snapshot, rights);
  if (!finishInterruptibleSocketCall(thread, result) ||
      (result >= 0 && !payload.copyReceived(*message, static_cast<size_t>(result)))) {
    if (rights) {
      rights->reset();
    }
    return -1;
  }
  message->msg_namelen = snapshot.msg_namelen;
  message->msg_controllen = snapshot.msg_controllen;
  message->msg_flags = snapshot.msg_flags;
  return result;
}

ssize_t posix_recvfrom(int sock, void* buff, size_t bufflen, int flags,
                       struct sockaddr_storage* address, socklen_t* addrlen) {
  N_NOTICE("recvfrom");

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f) || !validateSocketMessageFlags(flags, false, f->networkImpl->getDomain())) {
    return -1;
  }

  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen,
                                    PosixSubsystem::SafeWrite)) {
    N_NOTICE("recvfrom -> invalid receive buffer");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  struct sockaddr_storage source = {};
  struct sockaddr_storage* sourceAddress = nullptr;
  socklen_t sourceCapacity = 0;
  if (address) {
    if (!PosixSubsystem::copyFromUser(&sourceCapacity, addrlen, sizeof(sourceCapacity))) {
      N_NOTICE("recvfrom -> invalid source address length");
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (sourceCapacity > static_cast<socklen_t>(INT_MAX)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }

    const size_t checkedCapacity =
        sourceCapacity < sizeof(source) ? sourceCapacity : sizeof(source);
    if (checkedCapacity &&
        !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address), checkedCapacity,
                                      PosixSubsystem::SafeWrite)) {
      N_NOTICE("recvfrom -> invalid source address buffer");
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    sourceAddress = &source;
  }

  N_NOTICE("recvfrom(" << sock << ", " << buff << ", " << bufflen << ", " << flags << ", "
                       << address << ", " << addrlen);

  struct iovec vector = {buff, bufflen};
  struct msghdr message = {};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  message.msg_flags = flags;
  message.msg_name = sourceAddress;
  message.msg_namelen = sourceAddress ? sizeof(source) : 0;
  ssize_t n = posix_recvmsg_descriptor(f, &message);
  const socklen_t sourceLength = message.msg_namelen;

  if (n >= 0 && sourceAddress) {
    size_t copyLength = sourceLength;
    if (copyLength > sourceCapacity) {
      copyLength = sourceCapacity;
    }
    if (copyLength > sizeof(source)) {
      copyLength = sizeof(source);
    }
    if ((copyLength && !PosixSubsystem::copyToUser(address, &source, copyLength)) ||
        !PosixSubsystem::copyToUser(addrlen, &sourceLength, sizeof(sourceLength))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  N_NOTICE(" -> " << n);
  return n;
}

int posix_bind(int sock, const struct sockaddr_storage* address, socklen_t addrlen) {
  N_NOTICE("bind");

  struct sockaddr_storage snapshot = {};
  if (!copySocketAddress(address, addrlen, snapshot)) {
    return -1;
  }

  N_NOTICE("bind(" << sock << ", " << address << ", " << addrlen << ")");

  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  if (f->networkImpl->getDomain() != snapshot.ss_family) {
    syscallError(EAFNOSUPPORT);
    return -1;
  }

  return f->networkImpl->bind(&snapshot, addrlen);
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
    if (!PosixSubsystem::copyFromUser(&addressCapacity, addrlen, sizeof(addressCapacity)) ||
        !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(addrlen), sizeof(socklen_t),
                                      PosixSubsystem::SafeWrite)) {
      N_NOTICE("accept4 -> invalid address length");
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (addressCapacity > static_cast<socklen_t>(INT_MAX)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }

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
  DescriptorLease accepted;
  int r = f->networkImpl->accept(&acceptedAddress, &acceptedLength, flags, &accepted);
  if (!finishInterruptibleSocketCall(thread, static_cast<ssize_t>(r))) {
    return -1;
  }
  if (r >= 0 && returnAddress) {
    const size_t copyLength = addressCapacity < acceptedLength
                                  ? static_cast<size_t>(addressCapacity)
                                  : static_cast<size_t>(acceptedLength);
    if (acceptedLength > sizeof(acceptedAddress) ||
        !PosixSubsystem::copyToUser(address, &acceptedAddress, copyLength) ||
        !PosixSubsystem::copyToUser(addrlen, &acceptedLength, sizeof(acceptedLength))) {
      removeDescriptor(r, accepted);
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
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

namespace {
int socketName(int socket, struct sockaddr_storage* address, socklen_t* addressLength, bool peer) {
  socklen_t capacity = 0;
  if (!PosixSubsystem::copyFromUser(&capacity, addressLength, sizeof(capacity))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (capacity > INT_MAX) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  DescriptorLease f;
  acquireDescriptor(socket, f);
  if (!isSaneSocket(f)) {
    return -1;
  }

  struct sockaddr_storage result = {};
  socklen_t length = sizeof(result);
  const int status = peer ? f->networkImpl->getpeername(&result, &length)
                          : f->networkImpl->getsockname(&result, &length);
  if (status < 0) {
    return -1;
  }
  if (length > sizeof(result)) {
    SYSCALL_ERROR(IoError);
    return -1;
  }

  const size_t copied = capacity < length ? capacity : length;
  if (!PosixSubsystem::copyToUser(address, &result, copied) ||
      !PosixSubsystem::copyToUser(addressLength, &length, sizeof(length))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}
}  // namespace

int posix_getpeername(int socket, struct sockaddr_storage* address, socklen_t* address_len) {
  return socketName(socket, address, address_len, true);
}

int posix_getsockname(int socket, struct sockaddr_storage* address, socklen_t* address_len) {
  return socketName(socket, address, address_len, false);
}

int posix_setsockopt(int sock, int level, int optname, const void* optvalue, socklen_t optlen) {
  if (optlen < sizeof(int)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  int value = 0;
  if (!PosixSubsystem::copyFromUser(&value, optvalue, sizeof(value))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f)) {
    return -1;
  }
  return f->networkImpl->setsockopt(level, optname, &value, sizeof(value));
}

int posix_getsockopt(int sock, int level, int optname, void* optvalue, socklen_t* optlen) {
  socklen_t capacity = 0;
  if (!PosixSubsystem::copyFromUser(&capacity, optlen, sizeof(capacity))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (capacity > INT_MAX) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  union {
    int scalar;
    struct ucred credentials;
  } value = {};
  socklen_t length = sizeof(value);
  DescriptorLease f;
  acquireDescriptor(sock, f);
  if (!isSaneSocket(f)) {
    return -1;
  }
  const int pendingError =
      level == SOL_SOCKET && optname == SO_ERROR ? f->networkImpl->takeReceiveError() : 0;
  if (pendingError) {
    value.scalar = pendingError;
    length = sizeof(value.scalar);
  } else if (f->networkImpl->getsockopt(level, optname, &value, &length) < 0) {
    return -1;
  }
  if (length > sizeof(value)) {
    SYSCALL_ERROR(IoError);
    return -1;
  }
  const socklen_t copied = capacity < length ? capacity : length;
  if (!PosixSubsystem::copyToUser(optvalue, &value, copied) ||
      !PosixSubsystem::copyToUser(optlen, &copied, sizeof(copied))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}

ssize_t posix_sendmsg(int sockfd, const struct msghdr* msg, int flags) {
  N_NOTICE("sendmsg(" << sockfd << ", " << msg << ", " << flags << ")");

  if (!validateSocketMessageFlags(flags, true)) {
    return -1;
  }

  struct msghdr message = {};
  if (!PosixSubsystem::copyFromUser(&message, msg, sizeof(message))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  SharedPointer<SocketRights> rights;
  if (!parseSocketRights(message, rights)) {
    return -1;
  }

  constexpr size_t MaximumIoVectors = 1024;
  const size_t vectorCount = static_cast<size_t>(message.msg_iovlen);
  if (vectorCount > MaximumIoVectors) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  UniqueArray<struct iovec> vectorOwner;
  if (vectorCount) {
    vectorOwner = UniqueArray<struct iovec>::allocate(vectorCount);
  }
  struct iovec* vectors = vectorOwner.get();
  if (vectorCount &&
      !PosixSubsystem::copyFromUser(vectors, message.msg_iov, vectorCount, sizeof(*vectors))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  constexpr size_t MaximumIoBytes = static_cast<size_t>(SSIZE_MAX);
  size_t totalLength = 0;
  for (size_t i = 0; i < vectorCount; ++i) {
    if (vectors[i].iov_len > MaximumIoBytes - totalLength) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (!PosixSubsystem::checkUserBuffer(reinterpret_cast<uintptr_t>(vectors[i].iov_base),
                                         vectors[i].iov_len, 1, PosixSubsystem::SafeRead)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    totalLength += vectors[i].iov_len;
  }

  struct sockaddr_storage address = {};
  if (message.msg_name) {
    if (message.msg_namelen > sizeof(address)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (!PosixSubsystem::copyFromUser(&address, message.msg_name, message.msg_namelen)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    message.msg_name = &address;
  }
  message.msg_iov = vectors;
  message.msg_control = nullptr;
  message.msg_controllen = 0;
  message.msg_flags = flags;

  DescriptorLease f;
  acquireDescriptor(sockfd, f);
  if (!isSaneSocket(f)) {
    return -1;
  }
  if (rights &&
      (f->networkImpl->getDomain() != AF_UNIX ||
       (f->networkImpl->getType() != SOCK_DGRAM && f->networkImpl->getType() != SOCK_STREAM))) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  if (rights && f->networkImpl->getType() == SOCK_STREAM && !totalLength) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  const ssize_t n = posix_sendmsg_descriptor(f, &message, rights);
  N_NOTICE(" -> " << n);
  return n;
}

ssize_t posix_recvmsg(int sockfd, struct msghdr* msg, int flags) {
  N_NOTICE("recvmsg(" << sockfd << ", " << msg << ", " << flags << ")");

  DescriptorLease f;
  acquireDescriptor(sockfd, f);
  if (!isSaneSocket(f) || !validateSocketMessageFlags(flags, false, f->networkImpl->getDomain())) {
    return -1;
  }

  return posix_recvmsg_user_descriptor(f, msg, flags);
}

ssize_t posix_recvmsg_user_descriptor(const DescriptorLease& f, struct msghdr* msg, int flags,
                                      unsigned int* receivedLength) {
  if (!isSaneSocket(f) || !validateSocketMessageFlags(flags, false, f->networkImpl->getDomain()))
    return -1;
  struct msghdr message = {};
  if (!PosixSubsystem::copyFromUser(&message, msg, sizeof(message)) ||
      !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(msg), sizeof(message),
                                    PosixSubsystem::SafeWrite)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (receivedLength &&
      !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(receivedLength),
                                    sizeof(*receivedLength), PosixSubsystem::SafeWrite)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  const struct msghdr originalMessage = message;

  constexpr size_t MaximumIoVectors = 1024;
  const size_t vectorCount = static_cast<size_t>(message.msg_iovlen);
  if (vectorCount > MaximumIoVectors) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  UniqueArray<struct iovec> vectorOwner;
  if (vectorCount) {
    vectorOwner = UniqueArray<struct iovec>::allocate(vectorCount);
  }
  struct iovec* vectors = vectorOwner.get();
  if (vectorCount &&
      !PosixSubsystem::copyFromUser(vectors, message.msg_iov, vectorCount, sizeof(*vectors))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  constexpr size_t MaximumIoBytes = static_cast<size_t>(SSIZE_MAX);
  size_t totalLength = 0;
  for (size_t i = 0; i < vectorCount; ++i) {
    if (vectors[i].iov_len > MaximumIoBytes - totalLength) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (!PosixSubsystem::checkUserBuffer(reinterpret_cast<uintptr_t>(vectors[i].iov_base),
                                         vectors[i].iov_len, 1, PosixSubsystem::SafeWrite)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    totalLength += vectors[i].iov_len;
  }

  void* userName = message.msg_name;
  const size_t userNameCapacity = message.msg_namelen;
  void* userControl = message.msg_control;
  const size_t userControlCapacity = static_cast<size_t>(message.msg_controllen);
  struct sockaddr_storage address = {};
  if (userName) {
    const size_t checkedCapacity =
        userNameCapacity < sizeof(address) ? userNameCapacity : sizeof(address);
    if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(userName), checkedCapacity,
                                      PosixSubsystem::SafeWrite)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    message.msg_name = &address;
    message.msg_namelen = checkedCapacity;
  }
  if (userControlCapacity) {
    if (!userControl) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    const size_t checkedCapacity =
        userControlCapacity < MaximumControlBytes ? userControlCapacity : MaximumControlBytes;
    if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(userControl), checkedCapacity,
                                      PosixSubsystem::SafeWrite)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }
  message.msg_iov = vectors;
  message.msg_control = nullptr;
  message.msg_controllen = 0;
  message.msg_flags = flags;

  SharedPointer<SocketRights> rights;
  const ssize_t n = posix_recvmsg_descriptor(f, &message, &rights);

  if (n >= 0) {
    struct msghdr result = originalMessage;
    const size_t rightsCount = rights ? rights->count() : 0;
    size_t disclosedCount = 0;
    if (rightsCount && userControl && userControlCapacity >= CMSG_LEN(sizeof(int))) {
      disclosedCount = (userControlCapacity - CMSG_LEN(0)) / sizeof(int);
      if (disclosedCount > rightsCount) {
        disclosedCount = rightsCount;
      }
    }

    UniqueArray<int> installedFds;
    UniqueArray<DescriptorLease> installedLeases;
    if (disclosedCount) {
      installedFds = UniqueArray<int>::allocate(disclosedCount);
      installedLeases = UniqueArray<DescriptorLease>::allocate(disclosedCount);
    }

    PosixSubsystem* subsystem = getSubsystem();
    size_t publishedCount = 0;
    auto rollback = [&]() {
      for (size_t i = 0; i < publishedCount; ++i) {
        subsystem->closeFileDescriptor(static_cast<size_t>(installedFds.get()[i]),
                                       installedLeases.get()[i]);
        installedLeases.get()[i].reset();
      }
    };

    for (; publishedCount < disclosedCount; ++publishedCount) {
      FileDescriptor* received = new FileDescriptor(*rights->descriptor(publishedCount));
      if ((rights->descriptor(publishedCount)->networkImpl && !received->networkPublished()) ||
          (rights->descriptor(publishedCount)->getEventFdImpl() && !received->eventFdPublished()) ||
          (rights->descriptor(publishedCount)->getTimerFdImpl() && !received->timerFdPublished()) ||
          (rights->descriptor(publishedCount)->getSignalFdImpl() &&
           !received->signalFdPublished())) {
        delete received;
        rollback();
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
      }
      int descriptorFlags = 0;
#ifdef MSG_CMSG_CLOEXEC
      descriptorFlags = (flags & MSG_CMSG_CLOEXEC) ? FD_CLOEXEC : 0;
#endif
      received->setFlags(descriptorFlags);
      const size_t fd =
          subsystem->installFileDescriptor(received, installedLeases.get()[publishedCount]);
      installedFds.get()[publishedCount] = static_cast<int>(fd);
    }

    size_t controlBytes = 0;
    UniqueArray<uint8_t> control;
    if (disclosedCount) {
      const size_t fullSpace = CMSG_SPACE(disclosedCount * sizeof(int));
      controlBytes = userControlCapacity < fullSpace ? userControlCapacity : fullSpace;
      control = UniqueArray<uint8_t>::allocate(controlBytes);
      ByteSet(control.get(), 0, controlBytes);

      struct cmsghdr header = {};
      header.cmsg_len = CMSG_LEN(disclosedCount * sizeof(int));
      header.cmsg_level = SOL_SOCKET;
      header.cmsg_type = SCM_RIGHTS;
      MemoryCopy(control.get(), &header, sizeof(header));
      MemoryCopy(control.get() + CMSG_LEN(0), installedFds.get(), disclosedCount * sizeof(int));
    }

    if (userName) {
      size_t nameBytes = message.msg_namelen;
      if (nameBytes > userNameCapacity) {
        nameBytes = userNameCapacity;
      }
      if (nameBytes > sizeof(address)) {
        nameBytes = sizeof(address);
      }
      if (nameBytes && !PosixSubsystem::copyToUser(userName, &address, nameBytes)) {
        rollback();
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
    }

    if (controlBytes && !PosixSubsystem::copyToUser(userControl, control.get(), controlBytes)) {
      rollback();
      SYSCALL_ERROR(BadAddress);
      return -1;
    }

    result.msg_namelen = message.msg_namelen;
    result.msg_controllen = controlBytes;
    result.msg_flags = message.msg_flags;
#ifdef MSG_CTRUNC
    if (disclosedCount < rightsCount) {
      result.msg_flags |= MSG_CTRUNC;
    }
#endif
    const unsigned int length = static_cast<unsigned int>(n);
    if ((receivedLength && !PosixSubsystem::copyToUser(receivedLength, &length, sizeof(length))) ||
        !PosixSubsystem::copyToUser(msg, &result, sizeof(result))) {
      rollback();
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }
  N_NOTICE(" -> " << n);
  return n;
}

NetworkSyscalls::NetworkSyscalls(int domain, int type, int protocol)
    : m_Domain(domain),
      m_Type(type),
      m_Protocol(protocol),
      m_Blocking(true),
      m_ReadinessNotifications(),
      m_LifecycleLock(),
      m_DescriptorOwners(0),
      m_DescriptorAdmissionOpen(true),
      m_LastDescriptorClosed(false),
      m_DescriptorLifetime() {}

int NetworkSyscalls::takeReceiveError() {
  int error;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_ReceiveErrorLock);
    error = m_ReceiveError;
    m_ReceiveError = 0;
  }
  if (error)
    notifyReadiness(ReadyError);
  return error;
}

void NetworkSyscalls::deferReceiveError(int error) {
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_ReceiveErrorLock);
    if (!m_ReceiveError && error)
      ++m_ReceiveErrorGeneration;
    m_ReceiveError = error;
  }
  notifyReadiness(ReadyError);
}

ReadyMask NetworkSyscalls::pendingReceiveReadiness() const {
  ConstexprLockGuard<Mutex, THREADS> guard(m_ReceiveErrorLock);
  return m_ReceiveError ? ReadyError : ReadyNone;
}

ReadinessGenerations NetworkSyscalls::withReceiveErrorGeneration(
    ReadinessGenerations generations) const {
  ConstexprLockGuard<Mutex, THREADS> guard(m_ReceiveErrorLock);
  // The independent error source must survive drain/refill callback reorder.
  generations.error += m_ReceiveErrorGeneration;
  return generations;
}

ReadinessGenerations NetworkSyscalls::readinessGenerations() {
  return withReceiveErrorGeneration(ReadinessGenerations());
}

NetworkSyscalls::~NetworkSyscalls() {
  lastDescriptorClosed();
}

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

  SharedPointer<SocketRights> rights;
  return sendto_msg(&msg, rights);
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

  ssize_t result = recvfrom_msg(&msg, nullptr);
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

ReadyMask NetworkSyscalls::queryReady(bool reading, bool writing) {
  (void)reading;
  (void)writing;
  return ReadyInvalid | pendingReceiveReadiness();
}

bool NetworkSyscalls::addDescriptorOwner() {
  ConstexprLockGuard<Mutex, THREADS> guard(m_LifecycleLock);
  if (!m_DescriptorAdmissionOpen) {
    return false;
  }

  ++m_DescriptorOwners;
  return true;
}

void NetworkSyscalls::removeDescriptorOwner() {
  bool closeEndpoint = false;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_LifecycleLock);
    assert(m_DescriptorOwners);
    --m_DescriptorOwners;
    if (!m_DescriptorOwners) {
      m_DescriptorAdmissionOpen = false;
      // AF_UNIX blocking operations use generation-held stream storage, so
      // retiring the table-visible endpoint wakes them safely. lwIP calls may
      // still be using the netconn through a syscall lease and retire when
      // that final object reference drains instead.
      closeEndpoint = m_Domain == AF_UNIX || m_Domain == 16;
    }
  }

  if (closeEndpoint) {
    lastDescriptorClosed();
  }
}

void NetworkSyscalls::retainDescriptorLifetime(const SharedPointer<NetworkSyscalls>& lifetime) {
  if (m_Domain != AF_UNIX || !lifetime) {
    return;
  }

  ConstexprLockGuard<Mutex, THREADS> guard(m_LifecycleLock);
  if (!m_DescriptorLifetime) {
    m_DescriptorLifetime = lifetime;
  }
}

SharedPointer<NetworkSyscalls> NetworkSyscalls::acquireDescriptorLifetime() const {
  ConstexprLockGuard<Mutex, THREADS> guard(m_LifecycleLock);
  return m_DescriptorLifetime;
}

SharedPointer<NetworkSyscalls> NetworkSyscalls::releaseDescriptorLifetime() {
  SharedPointer<NetworkSyscalls> lifetime;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_LifecycleLock);
    lifetime = pedigree_std::move(m_DescriptorLifetime);
  }
  return lifetime;
}

void NetworkSyscalls::lastDescriptorClosed() {
  if (!beginDescriptorClose()) {
    return;
  }

  m_ReadinessNotifications.closeAndWait();
  closeReadiness(ReadyInvalid | ReadyHangup);
}

bool NetworkSyscalls::monitor(Thread* pThread, Event* pEvent) {
  return false;
}

bool NetworkSyscalls::unmonitor(Event* pEvent) {
  return false;
}

void NetworkSyscalls::associate(FileDescriptor* fd) {
  m_Blocking = !fd || !(fd->getStatusFlags() & O_NONBLOCK);
}

bool NetworkSyscalls::isBlocking() const {
  return m_Blocking;
}

void NetworkSyscalls::setBlocking(bool blocking) {
  m_Blocking = blocking;
}

bool NetworkSyscalls::beginDescriptorClose() {
  ConstexprLockGuard<Mutex, THREADS> guard(m_LifecycleLock);
  if (m_LastDescriptorClosed) {
    return false;
  }

  m_DescriptorAdmissionOpen = false;
  m_LastDescriptorClosed = true;
  return true;
}

bool NetworkSyscalls::hasLastDescriptorClosed() const {
  ConstexprLockGuard<Mutex, THREADS> guard(m_LifecycleLock);
  return m_LastDescriptorClosed;
}

LwipSocketSyscalls::LwipSocketSyscalls(int domain, int type, int protocol)
    : NetworkSyscalls(domain, type, protocol), m_Socket(nullptr), m_ReceiveLock(), m_Metadata() {}

LwipSocketSyscalls::~LwipSocketSyscalls() {
  lastDescriptorClosed();
}

void LwipSocketSyscalls::lastDescriptorClosed() {
  if (!beginDescriptorClose()) {
    return;
  }

  struct netconn* socket = m_Socket;
  if (socket) {
    LOCK_TCPIP_CORE();
    {
      ConstexprLockGuard<Mutex, THREADS> guard(m_SyscallObjectsLock);
      m_SyscallObjects.remove(socket);
    }
    UNLOCK_TCPIP_CORE();
  }

  // A callback which acquired admission before the map removal may still be
  // finishing its metadata update. Drain it before publishing terminal state
  // or releasing the netconn.
  m_ReadinessNotifications.closeAndWait();

  struct pbuf* partialPacket = nullptr;
  struct netbuf* partialBuffer = nullptr;
  {
    ConstexprLockGuard<Mutex, THREADS> receiveGuard(m_ReceiveLock);
    {
      ConstexprLockGuard<Mutex, THREADS> metadataGuard(m_Metadata.lock);
      const ReadyMask previous = readinessLevelLocked();
      m_Metadata.closed = true;
      m_Metadata.peerClosed = true;
      m_Metadata.writeClosed = true;
      partialPacket = m_Metadata.pb;
      partialBuffer = m_Metadata.buf;
      m_Metadata.pb = nullptr;
      m_Metadata.buf = nullptr;
      m_Metadata.offset = 0;
      m_Metadata.partialRead = false;
      m_Metadata.receivingQueuedData = false;
      recordReadinessRisesLocked(previous);
    }
  }

  m_Socket = nullptr;
  if (partialBuffer) {
    netbuf_delete(partialBuffer);
  } else if (partialPacket) {
    pbuf_free(partialPacket);
  }
  if (socket) {
    netconn_delete(socket);
  }

  closeReadiness(ReadyInvalid | ReadyHangup);
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
        ConstexprLockGuard<Mutex, THREADS> metadataGuard(m_Metadata.lock);
        const ReadyMask previous = readinessLevelLocked();
        m_Metadata.recv += -1 - m_Socket->socket;
        m_Socket->socket = 0;
        recordReadinessRisesLocked(previous);
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
    ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
    const ReadyMask previous = readinessLevelLocked();
    m_Metadata.send = 1;
    recordReadinessRisesLocked(previous);
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
    ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
    const ReadyMask previous = readinessLevelLocked();
    m_Metadata.send = 1;
    recordReadinessRisesLocked(previous);
  }

  N_NOTICE(" -> ok!");
  return 0;
}

ssize_t LwipSocketSyscalls::sendto_msg(const struct msghdr* msghdr,
                                       const SharedPointer<SocketRights>& rights) {
  if (rights) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }

  err_t err;
  const bool tcp = NETCONNTYPE_GROUP(m_Socket->type) == NETCONN_TCP;
  ip_addr_t destination = {};
  uint16_t destinationPort = 0;
  bool hasDestination = false;

  if (msghdr->msg_name) {
    // Preserve the existing connected-stream behavior while enabling the
    // destination-bearing datagram path.
    if (tcp) {
      SYSCALL_ERROR(Unimplemented);
      return -1;
    }
    if (m_Domain != AF_INET) {
      SYSCALL_ERROR(OperationNotSupported);
      return -1;
    }
    if (msghdr->msg_namelen < sizeof(struct sockaddr_in)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }

    const struct sockaddr_storage* address =
        reinterpret_cast<const struct sockaddr_storage*>(msghdr->msg_name);
    err = sockaddrToIpaddr(address, destinationPort, &destination, false);
    if (err != ERR_OK) {
      lwipToSyscallError(err);
      return -1;
    }
    hasDestination = true;
  }

  if (tcp) {
    bool hasPayload = false;
    for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
      if (msghdr->msg_iov[i].iov_len) {
        hasPayload = true;
        break;
      }
    }
    if (!hasPayload) {
      return 0;
    }
  }

  // Can we send without blocking?
  bool sendAvailable = false;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
    sendAvailable = m_Metadata.send != 0;
  }
  if (!isBlocking() && !sendAvailable) {
    N_NOTICE(" -> send queue full, would block");
    SYSCALL_ERROR(NoMoreProcesses);
    return -1;
  }

  size_t bytesWritten = 0;
  bool ok = true;

  if (tcp) {
    for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
      void* buffer = msghdr->msg_iov[i].iov_base;
      size_t bufferlen = msghdr->msg_iov[i].iov_len;
      if (!bufferlen) {
        continue;
      }

      size_t thisBytesWritten = 0;
      err = netconn_write_partly(m_Socket, buffer, bufferlen, NETCONN_COPY | NETCONN_MORE,
                                 &thisBytesWritten);
      if (err != ERR_OK) {
        lwipToSyscallError(err);
        ok = false;
        break;
      }

      bytesWritten += thisBytesWritten;
      if (thisBytesWritten < bufferlen) {
        break;
      }
    }
  } else {
    NetbufOwner buffer = NetbufOwner::adopt(netbuf_new());
    if (!buffer) {
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }

    size_t datagramLength = 0;
    for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
      const size_t fragmentLength = msghdr->msg_iov[i].iov_len;
      if (fragmentLength > static_cast<size_t>(0xFFFF) - datagramLength) {
        SYSCALL_ERROR(TooBig);
        return -1;
      }
      datagramLength += fragmentLength;
    }

    char* payload =
        reinterpret_cast<char*>(netbuf_alloc(buffer.get(), static_cast<u16_t>(datagramLength)));
    if (!payload) {
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }

    size_t offset = 0;
    for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
      const size_t fragmentLength = msghdr->msg_iov[i].iov_len;
      if (fragmentLength) {
        MemoryCopy(payload + offset, msghdr->msg_iov[i].iov_base, fragmentLength);
        offset += fragmentLength;
      }
    }

    err = hasDestination ? netconn_sendto(m_Socket, buffer.get(), &destination, destinationPort)
                         : netconn_send(m_Socket, buffer.get());
    if (err != ERR_OK) {
      lwipToSyscallError(err);
      ok = false;
    } else {
      bytesWritten += netbuf_len(buffer.get());
    }
  }

  if (!bytesWritten) {
    if (!ok) {
      return -1;
    }
  } else {
    // A later vector failure cannot replace bytes already sent with an
    // error at the syscall boundary or make that progress restartable.
    syscallError(0);
  }

  return bytesWritten;
}

ssize_t LwipSocketSyscalls::recvfrom_msg(struct msghdr* msghdr,
                                         SharedPointer<SocketRights>* rights) {
  if (rights) {
    rights->reset();
  }

  // A duplicated descriptor shares the receive cursor and retained packet.
  // Serialize the whole receive operation, but never hold the metadata lock
  // across lwIP calls because its callback takes that lock.
  ConstexprLockGuard<Mutex, THREADS> receiveGuard(m_ReceiveLock);

  const bool tcp = NETCONNTYPE_GROUP(netconn_type(m_Socket)) == NETCONN_TCP;
  const int inputFlags = msghdr->msg_flags;
  const bool blocking = isBlocking() && !(inputFlags & MSG_DONTWAIT);
  if ((inputFlags & MSG_TRUNC) && (tcp || m_Type != SOCK_DGRAM)) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  if (msghdr->msg_name && tcp) {
    // Source address reporting for streams is unchanged by the UDP slice.
    SYSCALL_ERROR(Unimplemented);
    return -1;
  }
  if (msghdr->msg_name && m_Domain != AF_INET) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  msghdr->msg_flags = 0;
  if (!msghdr->msg_name) {
    msghdr->msg_namelen = 0;
  }

  // No data to read right now.
  if (!blocking) {
    bool noData = false;
    {
      ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
      if (m_Metadata.closed) {
        return 0;
      }
      noData = !(m_Metadata.recv || m_Metadata.partialRead);
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

    {
      ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
      // RCV- is delivered synchronously from netconn_recv. Preserve the
      // already-readable level until the dequeued packet is installed as the
      // partial buffer, so epoll cannot observe an internal handoff as a drain.
      m_Metadata.receivingQueuedData = m_Metadata.recv != 0;
    }

    // No partial data present from a previous read. Read new data from
    // the socket.
    if (tcp) {
      err = netconn_recv_tcp_pbuf(m_Socket, &pb);
    } else {
      err = netconn_recv(m_Socket, &buf);
    }

    if (err != ERR_OK) {
      if (err == ERR_CLSD) {
        ReadyMask changed = ReadyRead | ReadyReadHangup;
        {
          ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
          const ReadyMask previous = readinessLevelLocked();
          m_Metadata.receivingQueuedData = false;
          m_Metadata.closed = true;
          m_Metadata.peerClosed = true;
          if (m_Metadata.writeClosed) {
            changed |= ReadyHangup;
          }
          recordReadinessRisesLocked(previous);
        }
        notifyReadiness(changed);
        return 0;
      }

      {
        ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
        m_Metadata.receivingQueuedData = false;
      }
      notifyReadiness(ReadyRead);
      N_NOTICE(" -> lwIP error");
      lwipToSyscallError(err);
      return -1;
    }

    if (pb == nullptr && buf != nullptr) {
      pb = buf->p;
    }
    if (!pb) {
      {
        ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
        m_Metadata.receivingQueuedData = false;
      }
      notifyReadiness(ReadyRead);
      SYSCALL_ERROR(IoError);
      return -1;
    }

    {
      ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
      m_Metadata.offset = 0;
      m_Metadata.pb = pb;
      m_Metadata.buf = buf;
      m_Metadata.partialRead = true;
      m_Metadata.receivingQueuedData = false;
    }
  }

  const size_t packetLength = m_Metadata.pb->tot_len;
  if (!tcp && msghdr->msg_name) {
    const ip_addr_t* sourceAddress = netbuf_fromaddr(m_Metadata.buf);
    const uint16_t sourcePort = netbuf_fromport(m_Metadata.buf);
    struct sockaddr_in source = {};
    source.sin_family = AF_INET;
    source.sin_port = HOST_TO_BIG16(sourcePort);
    source.sin_addr.s_addr = ip_addr_get_ip4_u32(sourceAddress);

    const size_t addressCapacity = msghdr->msg_namelen;
    const size_t addressLength =
        addressCapacity < sizeof(source) ? addressCapacity : sizeof(source);
    if (addressLength) {
      MemoryCopy(msghdr->msg_name, &source, addressLength);
    }
    msghdr->msg_namelen = sizeof(source);
  }

  size_t totalLen = 0;
  size_t readOffset = m_Metadata.offset;
  for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
    void* buffer = msghdr->msg_iov[i].iov_base;
    size_t bufferlen = msghdr->msg_iov[i].iov_len;

    const size_t available = packetLength - readOffset;
    if (bufferlen > available) {
      bufferlen = available;
    }
    if (!bufferlen) {
      break;
    }

    pbuf_copy_partial(m_Metadata.pb, buffer, bufferlen, readOffset);
    totalLen += bufferlen;
    readOffset += bufferlen;
  }

  // TCP retains unread bytes as a stream cursor. Datagram reads consume one
  // whole packet and report that the caller's scatter buffer was too short.
  if (tcp && readOffset < packetLength) {
    ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
    m_Metadata.offset = readOffset;
  } else {
    struct pbuf* completedPacket = nullptr;
    struct netbuf* completedBuffer = nullptr;
    {
      ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
      completedPacket = m_Metadata.pb;
      completedBuffer = m_Metadata.buf;
      m_Metadata.pb = nullptr;
      m_Metadata.buf = nullptr;
      m_Metadata.offset = 0;
      m_Metadata.partialRead = false;
    }

    if (!tcp && readOffset < packetLength) {
      msghdr->msg_flags |= MSG_TRUNC;
    }

    if (completedBuffer) {
      netbuf_delete(completedBuffer);
    } else if (completedPacket) {
      pbuf_free(completedPacket);
    }
  }

  // Publish both sides of the readable predicate. Edge-triggered epoll must
  // observe a fully drained packet before a later arrival can raise another
  // edge; partial packets remain readable when the observer rechecks.
  notifyReadiness(ReadyRead);

  N_NOTICE(" -> " << totalLen);
  if (!tcp && (inputFlags & MSG_TRUNC) && totalLen < packetLength) {
    return packetLength;
  }
  return totalLen;
}

int LwipSocketSyscalls::listen(int backlog) {
  err_t err = netconn_listen_with_backlog(m_Socket, backlog);
  if (err != ERR_OK) {
    N_NOTICE(" -> lwIP error");
    lwipToSyscallError(err);
    return -1;
  }

  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
    m_Metadata.listening = true;
  }

  return 0;
}

int LwipSocketSyscalls::bind(const struct sockaddr_storage* address, socklen_t addrlen) {
  uint16_t port = 0;
  ip_addr_t ipaddr;
  err_t conversion = sockaddrToIpaddr(address, port, &ipaddr);
  if (conversion != ERR_OK) {
    lwipToSyscallError(conversion);
    return -1;
  }

  err_t err = netconn_bind(m_Socket, &ipaddr, port);
  if (err != ERR_OK) {
    N_NOTICE(" -> lwIP error");
    lwipToSyscallError(err);
    return -1;
  }

  return 0;
}

int LwipSocketSyscalls::accept(struct sockaddr_storage* address, socklen_t* addrlen, int flags,
                               DescriptorLease* accepted) {
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
  {
    ConstexprLockGuard<Mutex, THREADS> guard(obj->m_Metadata.lock);
    const ReadyMask previous = obj->readinessLevelLocked();
    obj->m_Metadata.send = 1;
    obj->recordReadinessRisesLocked(previous);
  }
  obj->create();

  FileDescriptor* desc = new FileDescriptor;
  desc->setNetworkImpl(SharedPointer<NetworkSyscalls>(obj));
  setSocketDescriptorFlags(desc, flags);

  DescriptorLease installed;

  const size_t fd = installDescriptor(desc, installed);

  if (accepted) {
    *accepted = pedigree_std::move(installed);
  }
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
  } else if (how == SHUT_WR) {
    tx = 1;
  } else {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  err_t err = netconn_shutdown(m_Socket, rx, tx);
  if (err != ERR_OK) {
    lwipToSyscallError(err);
    return -1;
  }

  ReadyMask changed = ReadyNone;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
    const ReadyMask previous = readinessLevelLocked();
    if (rx) {
      m_Metadata.closed = true;
      changed |= ReadyRead | ReadyReadHangup;
    }
    if (tx) {
      m_Metadata.writeClosed = true;
      changed |= ReadyWrite;
    }
    if ((m_Metadata.closed || m_Metadata.peerClosed) && m_Metadata.writeClosed) {
      changed |= ReadyHangup;
    }
    recordReadinessRisesLocked(previous);
  }
  notifyReadiness(changed);

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
  bool clearedError = false;
  if (level == SOL_SOCKET) {
    if (optname == SO_TYPE) {
      value = m_Type;
    } else if (optname == SO_ERROR) {
      {
        ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
        const ReadyMask previous = readinessLevelLocked();
        value = lwipErrorNumber(m_Metadata.error);
        clearedError = m_Metadata.error != ERR_OK;
        m_Metadata.error = ERR_OK;
        recordReadinessRisesLocked(previous);
      }
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
  if (clearedError) {
    notifyReadiness(ReadyError | ReadyWrite);
  }
  return 0;
}

bool LwipSocketSyscalls::canPoll() const {
  return true;
}

ReadyMask LwipSocketSyscalls::readinessLevelLocked() const {
  ReadyMask ready = ReadyNone;
  if (m_Metadata.recv || m_Metadata.partialRead || m_Metadata.receivingQueuedData ||
      m_Metadata.closed || m_Metadata.peerClosed) {
    ready |= ReadyRead;
  }
  if (m_Metadata.send && m_Metadata.error == ERR_OK) {
    ready |= ReadyWrite;
  }
  if (m_Metadata.closed || m_Metadata.peerClosed) {
    ready |= ReadyReadHangup;
  }
  if ((m_Metadata.closed || m_Metadata.peerClosed) && m_Metadata.writeClosed) {
    ready |= ReadyHangup;
  }
  if (m_Metadata.error != ERR_OK) {
    ready |= ReadyError;
  }

  return ready;
}

void LwipSocketSyscalls::recordReadinessRisesLocked(ReadyMask previous) {
  const ReadyMask current = readinessLevelLocked();
  if (!(previous & ReadyRead) && (current & ReadyRead)) {
    ++m_Metadata.generations.read;
  }
  if (!(previous & ReadyWrite) && (current & ReadyWrite)) {
    ++m_Metadata.generations.write;
  }
  if (!(previous & ReadyError) && (current & ReadyError)) {
    ++m_Metadata.generations.error;
  }
  if (!(previous & ReadyHangup) && (current & ReadyHangup)) {
    ++m_Metadata.generations.hangup;
  }
  if (!(previous & ReadyReadHangup) && (current & ReadyReadHangup)) {
    ++m_Metadata.generations.readHangup;
  }
}

ReadyMask LwipSocketSyscalls::queryReady(bool reading, bool writing) {
  // An epoll watch can outlive the last descriptor alias. Admit the complete
  // snapshot before touching transport state so close waits for this query.
  OperationBarrier::Lease query;
  if (!m_ReadinessNotifications.tryAcquire(query)) {
    return ReadyInvalid | ReadyHangup;
  }

  if (hasLastDescriptorClosed()) {
    return ReadyInvalid | ReadyHangup;
  }

  ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
  ReadyMask ready = readinessLevelLocked();
  if (!reading) {
    ready &= ~ReadyRead;
  }
  if (!writing) {
    ready &= ~ReadyWrite;
  }
  return ready | pendingReceiveReadiness();
}

ReadinessGenerations LwipSocketSyscalls::readinessGenerations() {
  OperationBarrier::Lease query;
  if (!m_ReadinessNotifications.tryAcquire(query) || hasLastDescriptorClosed()) {
    return ReadinessGenerations();
  }

  ConstexprLockGuard<Mutex, THREADS> guard(m_Metadata.lock);
  return withReceiveErrorGeneration(m_Metadata.generations);
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
    read = m_Metadata.recv || m_Metadata.partialRead || m_Metadata.closed || m_Metadata.peerClosed;
    ok = ok || read;
  }

  if (error) {
    error = m_Metadata.error != ERR_OK || pendingReceiveReadiness();
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
  LwipSocketSyscalls* obj = nullptr;
  OperationBarrier::Lease notification;
  {
    ConstexprLockGuard<Mutex, THREADS> objectsGuard(m_SyscallObjectsLock);
    obj = m_SyscallObjects.lookup(conn);
    if (!obj) {
      // Accepted netconns can receive data before accept() has associated a
      // Pedigree descriptor. lwIP initializes socket to -1 for this exact
      // handoff and invokes this callback while holding its core lock.
      if (conn && conn->socket < 0 && evt == NETCONN_EVT_RCVPLUS) {
        --conn->socket;
      }
      return;
    }

    if (!obj->m_ReadinessNotifications.tryAcquire(notification)) {
      return;
    }
  }

  ReadyMask changed = ReadyNone;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(obj->m_Metadata.lock);
    const ReadyMask previous = obj->readinessLevelLocked();

    switch (evt) {
      case NETCONN_EVT_RCVPLUS:
        N_NOTICE("RCV+");
        ++(obj->m_Metadata.recv);
        changed |= ReadyRead;
        if (NETCONNTYPE_GROUP(conn->type) == NETCONN_TCP && !len && !obj->m_Metadata.listening) {
          obj->m_Metadata.peerClosed = true;
          changed |= ReadyReadHangup;
          if (obj->m_Metadata.writeClosed) {
            changed |= ReadyHangup;
          }
        }
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
        changed |= ReadyWrite;
        break;
      case NETCONN_EVT_SENDMINUS:
        N_NOTICE("SND-");
        obj->m_Metadata.send = 0;
        changed |= ReadyWrite;
        break;
      case NETCONN_EVT_ERROR:
        N_NOTICE("ERR");
        obj->m_Metadata.error = netconn_err(conn);
        if (obj->m_Metadata.error == ERR_OK) {
          obj->m_Metadata.error = ERR_IF;
        }
        changed |= ReadyError;
        break;
      default:
        N_NOTICE("Unknown netconn callback error.");
    }

    obj->recordReadinessRisesLocked(previous);

    /// \todo need a way to do this with lwip when threads are off
    EMIT_IF(THREADS) {
      for (auto& it : obj->m_Metadata.semaphores) {
        it->release();
      }
    }
  }

  // Observers can re-enter queryReady(), which takes m_Metadata.lock.
  obj->notifyReadiness(changed);
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
      peerClosed(false),
      writeClosed(false),
      listening(false),
      partialRead(false),
      receivingQueuedData(false),
      lock(),
      semaphores(),
      offset(0),
      pb(nullptr),
      buf(nullptr),
      generations() {}

enum class UnixSocketReferenceOwnership { Heap, Vfs };

class UnixSocketReference {
 public:
  UnixSocketReference(UnixSocket* socket, UnixSocketReferenceOwnership ownership)
      : m_Socket(socket), m_Ownership(ownership) {}

  ~UnixSocketReference() {
    if (!m_Socket) {
      return;
    }

    if (m_Ownership == UnixSocketReferenceOwnership::Vfs) {
      releaseTrackedUnixSocket(m_Socket);
    } else {
      delete m_Socket;
    }
  }

  UnixSocket* get() const {
    return m_Socket;
  }

 private:
  UnixSocket* m_Socket;
  UnixSocketReferenceOwnership m_Ownership;
};

static HashTable<String, SharedPointer<UnixSocketReference>> g_AbstractUnixSockets;
static Mutex g_AbstractUnixSocketsLock;

class UnixSocketGeneration {
 public:
  explicit UnixSocketGeneration(const SharedPointer<UnixSocketReference>& reference)
      : m_Reference(reference), m_Retired(false) {}

  ~UnixSocketGeneration() {
    retire();

    UnixSocket* socket = get();
    List<UnixSocket*> peers;
    UnixSocketSyscalls::unregisterSocket(socket, peers);
    for (auto peer : peers) {
      UnixSocketSyscalls::notifySocket(
          peer, ReadyRead | ReadyWrite | ReadyError | ReadyReadHangup | ReadyHangup);
    }

    // The directory owns a bound pathname until unlink, independently of the
    // descriptor's endpoint lifetime. A closed endpoint remains unconnectable.
  }

  UnixSocket* get() const {
    return m_Reference ? m_Reference->get() : nullptr;
  }

  SharedPointer<UnixSocketReference> reference() const {
    return m_Reference;
  }

  void retire() {
    if (m_Retired.compareAndSwap(false, true)) {
      UnixSocket* socket = get();
      if (socket) {
        socket->unbind();
      }
    }
  }

 private:
  SharedPointer<UnixSocketReference> m_Reference;
  Atomic<bool> m_Retired;
};

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
using UnixEndpointReceiveLeaseHook = void (*)();
static UnixEndpointReceiveLeaseHook g_UnixEndpointReceiveLeaseHook = nullptr;
static UnixEndpointMutationLockHook g_UnixEndpointMutationLockHook = nullptr;
static UnixEndpointReadinessLeaseHook g_UnixEndpointReadinessLeaseHook = nullptr;

void invokeUnixEndpointMutationLockHook() {
  UnixEndpointMutationLockHook hook =
      __atomic_exchange_n(&g_UnixEndpointMutationLockHook,
                          static_cast<UnixEndpointMutationLockHook>(nullptr), __ATOMIC_ACQ_REL);
  if (hook) {
    hook();
  }
}

void invokeUnixEndpointReadinessLeaseHook() {
  UnixEndpointReadinessLeaseHook hook =
      __atomic_exchange_n(&g_UnixEndpointReadinessLeaseHook,
                          static_cast<UnixEndpointReadinessLeaseHook>(nullptr), __ATOMIC_ACQ_REL);
  if (hook) {
    hook();
  }
}
#endif

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void setUnixEndpointMutationLockHookForTest(UnixEndpointMutationLockHook hook) {
  __atomic_store_n(&g_UnixEndpointMutationLockHook, hook, __ATOMIC_RELEASE);
}

void setUnixEndpointReadinessLeaseHookForTest(UnixEndpointReadinessLeaseHook hook) {
  __atomic_store_n(&g_UnixEndpointReadinessLeaseHook, hook, __ATOMIC_RELEASE);
}
#endif

UnixSocketSyscalls::EndpointMutationGuard::EndpointMutationGuard(UnixSocketSyscalls& socket)
    : m_Socket(socket), m_Guard(socket.m_EndpointMutationLock) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  invokeUnixEndpointMutationLockHook();
#endif
}

UnixSocketSyscalls::EndpointMutationGuard::~EndpointMutationGuard() {
  m_Socket.m_EndpointMutationReleaseInProgress = true;
  m_Socket.m_EndpointMutationLock.release();
  m_Guard.disown();
  m_Socket.m_EndpointMutationReleaseInProgress = false;
  m_Socket.tryCompleteEndpointClose();
}

UnixSocketSyscalls::EndpointMutationPairGuard::EndpointMutationPairGuard(UnixSocketSyscalls& first,
                                                                         UnixSocketSyscalls& second)
    : m_First(first),
      m_Second(second),
      m_FirstGuard(first.m_EndpointMutationLock),
      m_SecondGuard(second.m_EndpointMutationLock) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  invokeUnixEndpointMutationLockHook();
#endif
}

UnixSocketSyscalls::EndpointMutationPairGuard::~EndpointMutationPairGuard() {
  m_First.m_EndpointMutationReleaseInProgress = true;
  m_Second.m_EndpointMutationReleaseInProgress = true;

  m_Second.m_EndpointMutationLock.release();
  m_SecondGuard.disown();
  m_First.m_EndpointMutationLock.release();
  m_FirstGuard.disown();

  m_First.m_EndpointMutationReleaseInProgress = false;
  m_Second.m_EndpointMutationReleaseInProgress = false;
  m_First.tryCompleteEndpointClose();
  m_Second.tryCompleteEndpointClose();
}

UnixSocketSyscalls::EndpointReadinessGuard::EndpointReadinessGuard()
    : m_Socket(nullptr), m_Lifetime(), m_Lease(), m_Acquired(false) {}

UnixSocketSyscalls::EndpointReadinessGuard::EndpointReadinessGuard(UnixSocketSyscalls& socket)
    : EndpointReadinessGuard() {
  const bool acquired = acquire(socket);
  (void)acquired;
}

bool UnixSocketSyscalls::EndpointReadinessGuard::acquire(UnixSocketSyscalls& socket) {
  assert(!m_Acquired);
  m_Socket = &socket;
  m_Lifetime = socket.acquireDescriptorLifetime();
  m_Acquired = socket.m_ReadinessNotifications.tryAcquire(m_Lease);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (m_Acquired) {
    invokeUnixEndpointReadinessLeaseHook();
  }
#endif
  if (!m_Acquired) {
    m_Lifetime.reset();
    m_Socket = nullptr;
  }
  return m_Acquired;
}

UnixSocketSyscalls::EndpointReadinessGuard::~EndpointReadinessGuard() {
  if (!m_Acquired) {
    return;
  }

  m_Acquired = false;
  m_Lease = OperationBarrier::Lease();
  m_Socket->tryCompleteEndpointClose();
}

UnixSocketSyscalls::UnixSocketSyscalls(int domain, int type, int protocol)
    : NetworkSyscalls(domain, type, protocol),
      m_EndpointStateLock(),
      m_EndpointMutationLock(),
      m_EndpointMutationReleaseInProgress(false),
      m_EndpointClosePending(false),
      m_EndpointRetired(false),
      m_EndpointCloseFinalized(false),
      m_LocalEndpoint(),
      m_RemoteEndpoint(),
      m_ClosingLocalEndpoint(),
      m_ClosingRemoteEndpoint(),
      m_LocalPath(),
      m_RemotePath(),
      m_OwnsAbstractName(false) {}

UnixSocketSyscalls::~UnixSocketSyscalls() {
  lastDescriptorClosed();
}

void UnixSocketSyscalls::registerSocket(UnixSocket* socket) {
  if (!socket) {
    return;
  }

  ConstexprLockGuard<Mutex, THREADS> guard(m_SyscallObjectsLock);
  UnixSocketSyscalls* current = m_SyscallObjects.lookup(socket);
  if (!current) {
    m_SyscallObjects.insert(socket, this);
  } else if (current != this) {
    FATAL("A Unix socket has multiple NetworkSyscalls owners.");
  }

  // Accepted sockets are registered after leaving the listener queue.
  m_PendingListeners.remove(socket);
}

void UnixSocketSyscalls::registerPeer(UnixSocket* socket, UnixSocket* peer, UnixSocket* listener) {
  if (!socket || !peer) {
    return;
  }

  ConstexprLockGuard<Mutex, THREADS> guard(m_SyscallObjectsLock);
  m_Peers.insert(socket, peer);
  m_Peers.insert(peer, socket);
  if (listener) {
    m_PendingListeners.insert(peer, listener);
  }
}

void UnixSocketSyscalls::unregisterPeer(UnixSocket* socket, UnixSocket* peer) {
  if (!socket || !peer) {
    return;
  }

  ConstexprLockGuard<Mutex, THREADS> guard(m_SyscallObjectsLock);
  if (m_Peers.lookup(socket) == peer) {
    m_Peers.remove(socket);
  }
  if (m_Peers.lookup(peer) == socket) {
    m_Peers.remove(peer);
  }
  m_PendingListeners.remove(peer);
}

void UnixSocketSyscalls::unregisterSocket(UnixSocket* socket, List<UnixSocket*>& peers) {
  if (!socket) {
    return;
  }

  List<UnixSocket*> pendingEndpoints;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_SyscallObjectsLock);
    m_SyscallObjects.remove(socket);

    UnixSocket* peer = m_Peers.lookup(socket);
    if (peer) {
      peers.pushBack(peer);
      m_Peers.remove(socket);
      if (m_Peers.lookup(peer) == socket) {
        m_Peers.remove(peer);
      }
      m_PendingListeners.remove(peer);
    }
    m_PendingListeners.remove(socket);

    // A closing listener owns queued endpoints which do not have syscall
    // wrappers yet. Preserve their client endpoint keys until after unbind()
    // has changed the shared connection state, then notify those clients.
    for (Tree<UnixSocket*, UnixSocket*>::Iterator it = m_PendingListeners.begin();
         it != m_PendingListeners.end(); ++it) {
      if (it.value() == socket) {
        pendingEndpoints.pushBack(it.key());
      }
    }

    for (auto pending : pendingEndpoints) {
      UnixSocket* pendingPeer = m_Peers.lookup(pending);
      if (pendingPeer) {
        peers.pushBack(pendingPeer);
        m_Peers.remove(pending);
        if (m_Peers.lookup(pendingPeer) == pending) {
          m_Peers.remove(pendingPeer);
        }
      }
      m_PendingListeners.remove(pending);
    }
  }
}

void UnixSocketSyscalls::notifySocket(UnixSocket* socket, ReadyMask mask) {
  if (!socket || !mask) {
    return;
  }

  UnixSocketSyscalls* target = nullptr;
  EndpointReadinessGuard notification;
  {
    ConstexprLockGuard<Mutex, THREADS> registryGuard(m_SyscallObjectsLock);
    target = m_SyscallObjects.lookup(socket);
    if (!target || !notification.acquire(*target)) {
      return;
    }
  }

  // queryReady() may take UnixSocket's connection or buffer locks.
  target->notifyReadiness(mask);
}

void UnixSocketSyscalls::notifyPeer(UnixSocket* socket, ReadyMask mask) {
  UnixSocket* peer = nullptr;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_SyscallObjectsLock);
    if (socket) {
      peer = m_Peers.lookup(socket);
    }
  }

  notifySocket(peer, mask);
}

bool UnixSocketSyscalls::publishAbstractSocket(
    const String& address, const SharedPointer<UnixSocketReference>& reference) {
  LockGuard<Mutex> guard(g_AbstractUnixSocketsLock);
  if (g_AbstractUnixSockets.contains(address)) {
    SYSCALL_ERROR(AddressInUse);
    return false;
  }
  if (!g_AbstractUnixSockets.insert(address, reference)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  return true;
}

SharedPointer<UnixSocketReference> UnixSocketSyscalls::acquireSocket(const String& address) {
  if (isAbstractUnixSocket(address)) {
    LockGuard<Mutex> guard(g_AbstractUnixSocketsLock);
    auto result = g_AbstractUnixSockets.lookup(address);
    if (!result.hasValue()) {
      SYSCALL_ERROR(DoesNotExist);
      return SharedPointer<UnixSocketReference>();
    }
    return result.value();
  }

  File* file = findTrackedUnixSocket(address);
  if (!file) {
    SYSCALL_ERROR(DoesNotExist);
    return SharedPointer<UnixSocketReference>();
  }
  if (!file->isSocket()) {
    releaseTrackedUnixSocket(file);
    SYSCALL_ERROR(DoesNotExist);
    return SharedPointer<UnixSocketReference>();
  }
  return SharedPointer<UnixSocketReference>(
      new UnixSocketReference(static_cast<UnixSocket*>(file), UnixSocketReferenceOwnership::Vfs));
}

void UnixSocketSyscalls::removeAbstractSocket(const String& address, UnixSocket* socket) {
  LockGuard<Mutex> guard(g_AbstractUnixSocketsLock);
  auto current = g_AbstractUnixSockets.lookup(address);
  if (current.hasValue() && current.value()->get() == socket) {
    g_AbstractUnixSockets.remove(address);
  }
}

SharedPointer<UnixSocketGeneration> UnixSocketSyscalls::acquireLocalEndpoint() const {
  ConstexprLockGuard<Mutex, THREADS> guard(m_EndpointStateLock);
  return m_LocalEndpoint;
}

void UnixSocketSyscalls::replaceLocalEndpoint(UnixSocket* socket, bool tracked,
                                              const String* localPath) {
  SharedPointer<UnixSocketReference> reference(new UnixSocketReference(
      socket, tracked ? UnixSocketReferenceOwnership::Vfs : UnixSocketReferenceOwnership::Heap));
  replaceLocalEndpoint(reference, localPath, false);
}

void UnixSocketSyscalls::replaceLocalEndpoint(const SharedPointer<UnixSocketReference>& reference,
                                              const String* localPath, bool ownsAbstractName) {
  UnixSocket* socket = reference ? reference->get() : nullptr;
  SharedPointer<UnixSocketGeneration> replacement(new UnixSocketGeneration(reference));
  SharedPointer<UnixSocketGeneration> previous;

  registerSocket(socket);
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_EndpointStateLock);
    previous = pedigree_std::move(m_LocalEndpoint);
    m_LocalEndpoint = pedigree_std::move(replacement);
    if (localPath) {
      m_LocalPath = *localPath;
    }
    m_OwnsAbstractName = ownsAbstractName;
  }

  if (previous) {
    previous->retire();
  }
}

void UnixSocketSyscalls::lastDescriptorClosed() {
  const bool firstClose = beginDescriptorClose();
  if (firstClose) {
    // Admission and notification delivery close immediately. Endpoint
    // retirement may need to wait for a mutation scope or for the current
    // readiness callback/query to release its own activity lease.
    m_EndpointClosePending = true;
    m_ReadinessNotifications.close();
  } else if (!m_EndpointClosePending) {
    return;
  }

#if THREADS
  if (m_EndpointMutationLock.isOwnedByCurrentThread()) {
    return;
  }
#endif
  tryCompleteEndpointClose();
}

void UnixSocketSyscalls::tryCompleteEndpointClose() {
  if (!m_EndpointClosePending || m_EndpointCloseFinalized || m_EndpointMutationReleaseInProgress) {
    return;
  }

  // A published descriptor keeps one cycle solely for this deferred path.
  // The local copy prevents the last readiness lease from deleting this
  // wrapper while it completes the close after returning from a handler.
  SharedPointer<NetworkSyscalls> completionLifetime = acquireDescriptorLifetime();

  if (!m_EndpointRetired) {
    TerminationDeferral terminationDeferral;
    if (!m_EndpointMutationLock.tryAcquire()) {
      return;
    }

    // Another completion attempt can retire the endpoint after the first
    // unlocked check and before this nonblocking acquisition.
    if (!m_EndpointRetired) {
      m_EndpointMutationReleaseInProgress = true;
      String abstractName;
      UnixSocket* abstractSocket = nullptr;
      {
        ConstexprLockGuard<Mutex, THREADS> guard(m_EndpointStateLock);
        if (m_OwnsAbstractName && isAbstractUnixSocket(m_LocalPath) && m_LocalEndpoint) {
          abstractName = m_LocalPath;
          abstractSocket = m_LocalEndpoint->get();
        }
        m_ClosingLocalEndpoint = pedigree_std::move(m_LocalEndpoint);
        m_ClosingRemoteEndpoint = pedigree_std::move(m_RemoteEndpoint);
        m_LocalPath.clear();
        m_RemotePath.clear();
        m_OwnsAbstractName = false;
      }

      if (abstractSocket) {
        removeAbstractSocket(abstractName, abstractSocket);
      }

      if (m_ClosingLocalEndpoint) {
        // Wake blocked reads and accepts. Their generation references keep the
        // retired object alive until those operations have observed closure.
        m_ClosingLocalEndpoint->retire();
      }

      m_EndpointRetired = true;
    }
    m_EndpointMutationLock.release();
    m_EndpointMutationReleaseInProgress = false;
  }

  if (!m_EndpointRetired || !m_ReadinessNotifications.isClosedAndDrained() ||
      !m_EndpointCloseFinalized.compareAndSwap(false, true)) {
    return;
  }

  N_NOTICE("UnixSocketSyscalls::~UnixSocketSyscalls");
  m_ClosingLocalEndpoint.reset();
  m_ClosingRemoteEndpoint.reset();
  closeReadiness(ReadyInvalid | ReadyHangup);
  m_EndpointClosePending = false;

  SharedPointer<NetworkSyscalls> publishedLifetime = releaseDescriptorLifetime();
}

bool UnixSocketSyscalls::create() {
  EndpointMutationGuard mutationGuard(*this);
  if (hasLastDescriptorClosed()) {
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }

  SharedPointer<UnixSocketGeneration> local = acquireLocalEndpoint();
  if (local) {
    registerSocket(local->get());
    return true;
  }

  // Create an unnamed unix socket by default.
  replaceLocalEndpoint(
      new UnixSocket(String(), g_pUnixSocketBacking, nullptr, nullptr, getSocketType()), false);

  return true;
}

int UnixSocketSyscalls::connect(const struct sockaddr_storage* address, socklen_t addrlen) {
  String pathname;
  if (!unixSocketPath(address, addrlen, pathname, false)) {
    return -1;
  }

  EndpointMutationGuard mutationGuard(*this);
  if (hasLastDescriptorClosed()) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  SharedPointer<UnixSocketGeneration> local = acquireLocalEndpoint();
  if (!local) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  UnixSocket* localSocket = local->get();

  N_NOTICE(" -> unix connect: '" << pathname << "'");

  SharedPointer<UnixSocketReference> targetReference = acquireSocket(pathname);
  if (!targetReference) {
    N_NOTICE(" -> unix socket '" << pathname << "' doesn't exist");
    return -1;
  }
  UnixSocket* target = targetReference->get();

  if (getType() == SOCK_STREAM) {
    N_NOTICE(" -> stream");
    if (target->getType() != UnixSocket::Streaming || target->getState() != UnixSocket::Listening) {
      SYSCALL_ERROR(ConnectionRefused);
      return -1;
    }

    // Create the remote for accept() on the server side.
    UnixSocket* remote =
        new UnixSocket(String(), g_pUnixSocketBacking, nullptr, nullptr, UnixSocket::Streaming);

    // Pair first so accept can never observe an endpoint before its peer
    // exists. addSocket activates and queues the connection atomically;
    // accept only transfers ownership of the queued endpoint.
    if (!localSocket->bind(remote, false)) {
      delete remote;
      SYSCALL_ERROR(IsConnected);
      return -1;
    }
    registerPeer(localSocket, remote, target);
    if (!target->addSocket(remote)) {
      unregisterPeer(localSocket, remote);
      remote->failConnection();
      delete remote;
      SYSCALL_ERROR(ConnectionRefused);
      return -1;
    }
    notifySocket(target, ReadyRead);
    notifyReadiness(ReadyWrite);
    N_NOTICE(" -> stream connected and queued");
  } else {
    if (target->getType() != UnixSocket::Datagram) {
      SYSCALL_ERROR(ProtocolWrongType);
      return -1;
    }
    if (target->getState() == UnixSocket::Closed) {
      SYSCALL_ERROR(ConnectionRefused);
      return -1;
    }
    N_NOTICE(" -> dgram");
  }

  SharedPointer<UnixSocketReference> previousRemote;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_EndpointStateLock);
    previousRemote = pedigree_std::move(m_RemoteEndpoint);
    m_RemoteEndpoint = pedigree_std::move(targetReference);
    m_RemotePath = pathname;
  }

  if (getType() != SOCK_STREAM) {
    notifyReadiness(ReadyWrite);
  }

  N_NOTICE(" -> remote is now " << pathname);

  if (getType() == SOCK_STREAM && !isBlocking()) {
    SYSCALL_ERROR(InProgress);
    return -1;
  }

  return 0;
}

ssize_t UnixSocketSyscalls::sendto_msg(const struct msghdr* msghdr,
                                       const SharedPointer<SocketRights>& rights) {
  N_NOTICE("UnixSocketSyscalls::sendto_msg");

  SharedPointer<UnixSocketGeneration> local;
  SharedPointer<UnixSocketReference> remoteReference;
  String localPath;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_EndpointStateLock);
    local = m_LocalEndpoint;
    remoteReference = m_RemoteEndpoint;
    localPath = m_LocalPath;
  }
  if (!local) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  UnixSocket* localSocket = local->get();
  if (getType() == SOCK_STREAM) {
    if (localSocket->wasConnected()) {
      remoteReference = local->reference();
    } else {
      remoteReference.reset();
    }
  }

  UnixSocket* remote = remoteReference ? remoteReference->get() : nullptr;
  if (getType() == SOCK_STREAM && !remote) {
    const bool closed = localSocket->getState() == UnixSocket::Closed;
    N_NOTICE(" -> " << (closed ? "closed" : "not connected"));
    syscallError(closed ? Error::BrokenPipe : Error::NotConnected);
    return -1;
  }
  if (getType() == SOCK_STREAM && localSocket->writeShutdown()) {
    SYSCALL_ERROR(BrokenPipe);
    return -1;
  }

  if (getType() != SOCK_STREAM && (msghdr->msg_name || !remote)) {
    if (!msghdr->msg_name) {
      syscallError(EDESTADDRREQ);
      N_NOTICE(" -> sendto on unconnected socket with no address");
      return -1;
    }

    String pathname;
    if (!unixSocketPath(reinterpret_cast<const struct sockaddr_storage*>(msghdr->msg_name),
                        msghdr->msg_namelen, pathname, false)) {
      return -1;
    }

    N_NOTICE(" -> unix connect: '" << pathname << "'");

    remoteReference = acquireSocket(pathname);
    if (!remoteReference) {
      N_NOTICE(" -> unix socket '" << pathname << "' doesn't exist");
      return -1;
    }
    remote = remoteReference->get();
  }

  if (getType() != SOCK_STREAM && (!remote || remote->getType() != UnixSocket::Datagram ||
                                   remote->getState() == UnixSocket::Closed)) {
    syscallError(remote && remote->getType() != UnixSocket::Datagram ? Error::ProtocolWrongType
                                                                     : Error::ConnectionRefused);
    return -1;
  }

  if (getType() == SOCK_STREAM) {
    bool hasPayload = false;
    for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
      if (msghdr->msg_iov[i].iov_len) {
        hasPayload = true;
        break;
      }
    }
    if (!hasPayload) {
      return 0;
    }
  }

  N_NOTICE(" -> transmitting!");

  uint64_t numWritten = 0;
  bool completedWrite = false;
  bool interrupted = false;
  int datagramError = 0;
  if (getType() == SOCK_DGRAM) {
    size_t datagramLength = 0;
    for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
      if (msghdr->msg_iov[i].iov_len > static_cast<size_t>(SSIZE_MAX) - datagramLength) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      datagramLength += msghdr->msg_iov[i].iov_len;
    }

    UniqueArray<uint8_t> datagram;
    const void* buffer = nullptr;
    if (datagramLength) {
      datagram = UniqueArray<uint8_t>::allocate(datagramLength);
      if (!datagram) {
        SYSCALL_ERROR(OutOfMemory);
        return -1;
      }
      size_t offset = 0;
      for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
        MemoryCopy(datagram.get() + offset, msghdr->msg_iov[i].iov_base,
                   msghdr->msg_iov[i].iov_len);
        offset += msghdr->msg_iov[i].iov_len;
      }
      buffer = datagram.get();
    }

    completedWrite =
        remote->sendDatagram(datagramLength, reinterpret_cast<uintptr_t>(buffer), isBlocking(),
                             reinterpret_cast<uintptr_t>(localPath.cstr()), rights, &datagramError);
    numWritten = completedWrite ? datagramLength : 0;
  } else {
    numWritten = localSocket->sendStream(msghdr->msg_iov, static_cast<size_t>(msghdr->msg_iovlen),
                                         isBlocking(), rights, &interrupted);
    completedWrite = numWritten;
  }
  if (completedWrite) {
    if (getType() == SOCK_STREAM) {
      notifyPeer(localSocket, ReadyRead);
    } else {
      notifySocket(remote, ReadyRead);
    }
  }
  if (!completedWrite) {
    if (datagramError) {
      syscallError(datagramError);
      return -1;
    }
    if (interrupted) {
      SYSCALL_ERROR(Interrupted);
      N_NOTICE(" -> -1 (EINTR)");
      return -1;
    }

    if (getType() == SOCK_STREAM && localSocket->getState() == UnixSocket::Closed) {
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

ssize_t UnixSocketSyscalls::recvfrom_msg(struct msghdr* msghdr,
                                         SharedPointer<SocketRights>* rights) {
  if (rights) {
    rights->reset();
  }

  const int inputFlags = msghdr->msg_flags;
  const bool blocking = isBlocking() && !(inputFlags & MSG_DONTWAIT);
#ifdef MSG_TRUNC
  if ((inputFlags & MSG_TRUNC) && getType() != SOCK_DGRAM) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
#endif

  SharedPointer<UnixSocketGeneration> local = acquireLocalEndpoint();
  if (!local) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  UnixSocket* localSocket = local->get();

  String remote;
  uint64_t numRead = 0;
  uint64_t datagramLength = 0;
  bool consumedDatagram = false;
  bool interrupted = false;
  if (getType() == SOCK_DGRAM) {
    size_t datagramCapacity = 0;
    for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen); ++i) {
      if (msghdr->msg_iov[i].iov_len > static_cast<size_t>(SSIZE_MAX) - datagramCapacity) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      datagramCapacity += msghdr->msg_iov[i].iov_len;
    }

    UniqueArray<uint8_t> datagram;
    void* buffer = nullptr;
    if (datagramCapacity) {
      datagram = UniqueArray<uint8_t>::allocate(datagramCapacity);
      if (!datagram) {
        SYSCALL_ERROR(OutOfMemory);
        return -1;
      }
      buffer = datagram.get();
    }

    SharedPointer<SocketRights> receivedRights;
    consumedDatagram =
        localSocket->receiveDatagram(datagramCapacity, reinterpret_cast<uintptr_t>(buffer),
                                     blocking, remote, receivedRights, numRead, datagramLength);
    if (rights) {
      *rights = receivedRights;
    }
    if (consumedDatagram && numRead) {
      size_t offset = 0;
      for (size_t i = 0; i < static_cast<size_t>(msghdr->msg_iovlen) && offset < numRead; ++i) {
        const size_t remaining = static_cast<size_t>(numRead) - offset;
        const size_t amount =
            msghdr->msg_iov[i].iov_len < remaining ? msghdr->msg_iov[i].iov_len : remaining;
        MemoryCopy(msghdr->msg_iov[i].iov_base, datagram.get() + offset, amount);
        offset += amount;
      }
    }
  } else {
    numRead = localSocket->receiveStream(msghdr->msg_iov, static_cast<size_t>(msghdr->msg_iovlen),
                                         blocking, rights, &interrupted);
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  UnixEndpointReceiveLeaseHook leaseHook =
      __atomic_load_n(&g_UnixEndpointReceiveLeaseHook, __ATOMIC_ACQUIRE);
  if (leaseHook) {
    leaseHook();
  }
#endif

  // The receive attempt has established the socket's new readable level,
  // including a successful zero-length datagram and a drain to empty.
  notifyReadiness(ReadyRead);

  if (numRead && getType() == SOCK_STREAM) {
    // Consuming the incoming stream frees capacity in the peer's outgoing
    // stream. The peer rechecks the precise level before reporting POLLOUT.
    notifyPeer(localSocket, ReadyWrite);
  }

  if ((numRead || consumedDatagram) && msghdr->msg_name) {
    writeUnixSocketAddress(remote, reinterpret_cast<struct sockaddr_storage*>(msghdr->msg_name),
                           &msghdr->msg_namelen);
  }

  msghdr->msg_flags = 0;
#ifdef MSG_TRUNC
  if (consumedDatagram && numRead < datagramLength) {
    msghdr->msg_flags |= MSG_TRUNC;
  }
#endif
  if (!numRead && !consumedDatagram) {
    if (interrupted) {
      SYSCALL_ERROR(Interrupted);
      N_NOTICE(" -> -1 (EINTR)");
      return -1;
    }

    if (getType() == SOCK_STREAM && localSocket->getState() == UnixSocket::Closed) {
      N_NOTICE(" -> 0 (EOF)");
      return 0;
    }

    if (!blocking) {
      SYSCALL_ERROR(NoMoreProcesses);
      N_NOTICE(" -> -1 (EAGAIN)");
      return -1;
    }
  }

#ifdef MSG_TRUNC
  if (consumedDatagram && (inputFlags & MSG_TRUNC)) {
    N_NOTICE(" -> " << datagramLength);
    return datagramLength;
  }
#endif
  N_NOTICE(" -> " << numRead);
  return numRead;
}

int UnixSocketSyscalls::listen(int backlog) {
  (void)backlog;

  SharedPointer<UnixSocketGeneration> local = acquireLocalEndpoint();
  if (!local) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  UnixSocket* localSocket = local->get();

  if (localSocket->getType() != UnixSocket::Streaming) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }

  /// \todo bind to an unnamed socket if we aren't already bound

  if (!localSocket->markListening()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  return 0;
}

int UnixSocketSyscalls::bind(const struct sockaddr_storage* address, socklen_t addrlen) {
  ResolvedPath parentLease;
  String adjusted_pathname;
  if (!unixSocketPath(address, addrlen, adjusted_pathname, true)) {
    return -1;
  }
  if (!adjusted_pathname.length()) {
    /// \todo re-bind an unnamed address if we are bound already
    return 0;
  }

  EndpointMutationGuard mutationGuard(*this);
  if (hasLastDescriptorClosed()) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_EndpointStateLock);
    if (!m_LocalEndpoint) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    if (m_LocalPath.length()) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
  }

  N_NOTICE(" -> unix bind: '" << adjusted_pathname << "'");

  if (isAbstractUnixSocket(adjusted_pathname)) {
    UnixSocket* socket =
        new UnixSocket(String(), g_pUnixSocketBacking, nullptr, nullptr, getSocketType());
    if (!socket) {
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }

    SharedPointer<UnixSocketReference> reference(
        new UnixSocketReference(socket, UnixSocketReferenceOwnership::Heap));
    if (!publishAbstractSocket(adjusted_pathname, reference)) {
      return -1;
    }

    replaceLocalEndpoint(reference, &adjusted_pathname, true);
    notifyReadiness(ReadyWrite);
    return 0;
  }

  if (adjusted_pathname.endswith('/')) {
    // uh, that's a directory
    SYSCALL_ERROR(IsADirectory);
    return -1;
  }

  Process* process = Processor::information().getCurrentThread()->getParent();
  auto context = process->acquireFilesystemContext();
  auto* view = VFS::instance().mountView();
  if (!context || !view) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  FilesystemPathRef parent;
  String basename;
  if (!view->resolveParent(context, FilesystemPathRef(), adjusted_pathname, parent, basename))
    return -1;
  parentLease.retain(parent);
  if (!parent || parent->provider() != view || !parent->node()->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return -1;
  }
  if (!basename.length() || basename == "." || basename == "..") {
    SYSCALL_ERROR(AddressInUse);
    return -1;
  }
  if (basename.length() > NAME_MAX) {
    SYSCALL_ERROR(NameTooLong);
    return -1;
  }
  File* parentDirectory = parent->node();
  Directory* pDir = Directory::fromFile(parentDirectory);
  if (parentDirectory->getFilesystem()->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return -1;
  }
  if (!VFS::checkAccess(parentDirectory, false, true, true))
    return -1;
  UnixSocket* socket = new UnixSocket(basename, parentDirectory->getFilesystem(), parentDirectory,
                                      nullptr, getSocketType());
  if (!socket) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  // Establish the descriptor's ownership before publishing the pathname.
  // addEphemeralFile adds the directory's separate ownership on success.
  VFS::instance().trackFile(socket);
  Directory::AddStatus addStatus = pDir->addEphemeralFile(socket);
  if (addStatus != Directory::AddStatus::Added) {
    socket->releaseVfsReference();
    if (addStatus == Directory::AddStatus::IoError) {
      SYSCALL_ERROR(IoError);
    } else if (addStatus == Directory::AddStatus::Detached) {
      SYSCALL_ERROR(DoesNotExist);
    } else {
      SYSCALL_ERROR(AddressInUse);
    }
    return -1;
  }
  N_NOTICE(" -> basename=" << basename);

  // Readers and acceptors retain the old generation outside the state lock.
  // Retiring it wakes those operations; destruction waits for their local
  // SharedPointer copies to leave scope.
  replaceLocalEndpoint(socket, true, &adjusted_pathname);
  notifyReadiness(ReadyWrite);

  return 0;
}

int UnixSocketSyscalls::accept(struct sockaddr_storage* address, socklen_t* addrlen, int flags,
                               DescriptorLease* accepted) {
  N_NOTICE("unix accept");
  SharedPointer<UnixSocketGeneration> local;
  String localPath;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_EndpointStateLock);
    local = m_LocalEndpoint;
    localPath = m_LocalPath;
  }
  if (!local) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  UnixSocket* remote = local->get()->getSocket(isBlocking());
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
    obj->replaceLocalEndpoint(remote, false, &localPath);
    obj->create();

    FileDescriptor* desc = new FileDescriptor;
    desc->setNetworkImpl(SharedPointer<NetworkSyscalls>(obj));
    setSocketDescriptorFlags(desc, flags);

    DescriptorLease installed;

    const size_t fd = installDescriptor(desc, installed);

    if (accepted) {
      *accepted = pedigree_std::move(installed);
    }
    obj->associate(desc);

    return static_cast<int>(fd);
  }

  return -1;
}

int UnixSocketSyscalls::shutdown(int how) {
  N_NOTICE("UnixSocketSyscalls::shutdown");
  if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  SharedPointer<UnixSocketGeneration> local = acquireLocalEndpoint();
  if (!local) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  UnixSocket* socket = local->get();
  if (!socket->shutdown(how)) {
    return -1;
  }

  notifySocket(socket, ReadyRead | ReadyWrite);
  notifyPeer(socket, ReadyRead | ReadyWrite);
  return 0;
}

int UnixSocketSyscalls::getpeername(struct sockaddr_storage* address, socklen_t* address_len) {
  N_NOTICE("UNIX getpeername");
  SharedPointer<UnixSocketGeneration> local;
  String remotePath;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_EndpointStateLock);
    local = m_LocalEndpoint;
    remotePath = m_RemotePath;
  }
  if (!local) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (!local->get()->wasConnected()) {
    SYSCALL_ERROR(NotConnected);
    return -1;
  }

  writeUnixSocketAddress(remotePath, address, address_len);

  N_NOTICE(" -> " << remotePath);
  return 0;
}

int UnixSocketSyscalls::getsockname(struct sockaddr_storage* address, socklen_t* address_len) {
  N_NOTICE("UNIX getsockname");
  String localPath;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_EndpointStateLock);
    if (!m_LocalEndpoint) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    localPath = m_LocalPath;
  }
  writeUnixSocketAddress(localPath, address, address_len);

  N_NOTICE(" -> " << localPath);
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
        SharedPointer<UnixSocketGeneration> local = acquireLocalEndpoint();
        if (!local) {
          SYSCALL_ERROR(BadFileDescriptor);
          return -1;
        }
        UnixSocket* localSocket = local->get();
        const UnixSocket::SocketState state = localSocket->getState();
        if (localSocket->wasConnected()) {
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
      SharedPointer<UnixSocketGeneration> local = acquireLocalEndpoint();
      if (!local) {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
      }
      UnixSocket* localSocket = local->get();
      if (!localSocket->wasConnected()) {
        SYSCALL_ERROR(NotConnected);
        return -1;
      }
      if (*optlen < sizeof(struct ucred)) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }

      // get credentials of other side of this socket
      struct ucred* targetCreds = reinterpret_cast<struct ucred*>(optvalue);
      struct ucred sourceCreds = localSocket->getPeerCredentials();

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
  return static_cast<bool>(acquireLocalEndpoint());
}

ReadyMask UnixSocketSyscalls::queryReady(bool reading, bool writing) {
  // Endpoint generations are released at final descriptor close, while the
  // NetworkSyscalls wrapper can remain retained by an epoll watch.
  EndpointReadinessGuard query(*this);
  if (!query) {
    return ReadyInvalid | ReadyHangup;
  }

  if (hasLastDescriptorClosed()) {
    return ReadyInvalid | ReadyHangup;
  }

  SharedPointer<UnixSocketGeneration> endpoint = acquireLocalEndpoint();
  if (!endpoint) {
    return ReadyInvalid;
  }
  UnixSocket* local = endpoint->get();

  const UnixSocket::SocketState state = local->getState();
  if (state == UnixSocket::Closed) {
    ReadyMask ready = ReadyHangup;
    if (reading) {
      ready |= ReadyRead | ReadyReadHangup;
    }
    if (!local->wasConnected()) {
      ready |= ReadyError;
    }
    return ready | pendingReceiveReadiness();
  }

  ReadyMask ready = ReadyNone;
  if (reading && local->select(false, 0)) {
    ready |= ReadyRead;
    if (local->readShutdown()) {
      ready |= ReadyReadHangup;
    }
  }

  if (writing) {
    if (getType() == SOCK_DGRAM) {
      // Datagram POLLOUT describes the local send path. A later sendto may
      // still race a particular destination becoming full.
      ready |= ReadyWrite;
    } else if (local->select(true, 0)) {
      ready |= ReadyWrite;
    }
  }

  return ready | pendingReceiveReadiness();
}

ReadinessGenerations UnixSocketSyscalls::readinessGenerations() {
  EndpointReadinessGuard query(*this);
  if (!query || hasLastDescriptorClosed()) {
    return ReadinessGenerations();
  }

  SharedPointer<UnixSocketGeneration> endpoint = acquireLocalEndpoint();
  return withReceiveErrorGeneration(endpoint ? endpoint->get()->readinessGenerations()
                                             : ReadinessGenerations());
}

bool UnixSocketSyscalls::poll(bool& read, bool& write, bool& error, Semaphore* waiter) {
  SharedPointer<UnixSocketGeneration> endpoint = acquireLocalEndpoint();
  UnixSocket* local = endpoint ? endpoint->get() : nullptr;
  const bool checkRead = read;
  const bool checkWrite = write;
  read = false;
  write = false;
  error = pendingReceiveReadiness();

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

  bool ok = error;
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
  SharedPointer<UnixSocketGeneration> local = acquireLocalEndpoint();
  if (local) {
    local->get()->removeWaiter(waiter);
  }
}

bool UnixSocketSyscalls::monitor(Thread* pThread, Event* pEvent) {
  SharedPointer<UnixSocketGeneration> local = acquireLocalEndpoint();
  if (!local) {
    return false;
  }

  local->get()->addWaiter(pThread, pEvent);
  return true;
}

bool UnixSocketSyscalls::unmonitor(Event* pEvent) {
  SharedPointer<UnixSocketGeneration> local = acquireLocalEndpoint();
  if (!local) {
    return false;
  }

  local->get()->removeWaiter(pEvent);
  return true;
}

bool UnixSocketSyscalls::pairWith(UnixSocketSyscalls* other) {
  if (!other || other == this) {
    return false;
  }

  UnixSocketSyscalls* firstMutation = this;
  UnixSocketSyscalls* secondMutation = other;
  if (reinterpret_cast<uintptr_t>(&firstMutation->m_EndpointMutationLock) >
      reinterpret_cast<uintptr_t>(&secondMutation->m_EndpointMutationLock)) {
    UnixSocketSyscalls* temporary = firstMutation;
    firstMutation = secondMutation;
    secondMutation = temporary;
  }
  EndpointMutationPairGuard mutationGuard(*firstMutation, *secondMutation);

  if (hasLastDescriptorClosed() || other->hasLastDescriptorClosed()) {
    return false;
  }

  SharedPointer<UnixSocketGeneration> local = acquireLocalEndpoint();
  SharedPointer<UnixSocketGeneration> otherLocal = other->acquireLocalEndpoint();
  if (!local || !otherLocal) {
    return false;
  }

  UnixSocket* localSocket = local->get();
  UnixSocket* otherSocket = otherLocal->get();
  if (!localSocket->bind(otherSocket)) {
    return false;
  }

  registerPeer(localSocket, otherSocket);

  // make sure both sides can use the socket
  otherSocket->acknowledgeBind();

  SharedPointer<UnixSocketReference> previousRemote;
  SharedPointer<UnixSocketReference> otherPreviousRemote;
  {
    ConstexprLockGuard<Mutex, THREADS> guard(m_EndpointStateLock);
    previousRemote = pedigree_std::move(m_RemoteEndpoint);
    m_RemoteEndpoint = otherLocal->reference();
  }
  {
    ConstexprLockGuard<Mutex, THREADS> guard(other->m_EndpointStateLock);
    otherPreviousRemote = pedigree_std::move(other->m_RemoteEndpoint);
    other->m_RemoteEndpoint = local->reference();
  }
  notifyReadiness(ReadyWrite);
  other->notifyReadiness(ReadyWrite);
  return true;
}

UnixSocket::SocketType UnixSocketSyscalls::getSocketType() const {
  if (getType() == SOCK_STREAM) {
    return UnixSocket::Streaming;
  }

  return UnixSocket::Datagram;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
class UnixEndpointLifetimeProbe : public UnixSocket {
 public:
  explicit UnixEndpointLifetimeProbe(Atomic<size_t>& destructions)
      : UnixSocket(String(), nullptr, nullptr, nullptr, UnixSocket::Datagram),
        m_Destructions(destructions) {}

  ~UnixEndpointLifetimeProbe() override {
    m_Destructions += 1;
  }

 private:
  Atomic<size_t>& m_Destructions;
};

struct UnixEndpointReplacementContext {
  explicit UnixEndpointReplacementContext(UnixSocketSyscalls* socket)
      : socket(socket),
        receiver(nullptr),
        continueReceive(0, false),
        entered(0),
        leaseHeld(0),
        returned(0),
        result(-2) {}

  UnixSocketSyscalls* socket;
  Thread* receiver;
  Semaphore continueReceive;
  Atomic<size_t> entered;
  Atomic<size_t> leaseHeld;
  Atomic<size_t> returned;
  Atomic<ssize_t> result;
};

UnixEndpointReplacementContext* g_UnixEndpointReplacementContext = nullptr;

void holdRetiredUnixEndpointLease() {
  UnixEndpointReplacementContext* context = g_UnixEndpointReplacementContext;
  if (!context || Processor::information().getCurrentThread() != context->receiver) {
    return;
  }

  context->leaseHeld += 1;
  context->continueReceive.acquire();
}

int blockedUnixEndpointReceive(void* parameter) {
  UnixEndpointReplacementContext* context =
      reinterpret_cast<UnixEndpointReplacementContext*>(parameter);
  char byte = 0;
  struct iovec vector = {&byte, sizeof(byte)};
  struct msghdr message = {};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;

  context->entered += 1;
  context->result = context->socket->recvfrom_msg(&message, nullptr);
  context->returned += 1;
  return 0;
}
}  // namespace

bool runHostedUnixEndpointLifetimeRegression(Process* process) {
  constexpr size_t Attempts = 10000;
  Atomic<size_t> destructions(0);
  UnixSocketSyscalls socket(AF_UNIX, SOCK_DGRAM, 0);
  socket.replaceLocalEndpoint(new UnixEndpointLifetimeProbe(destructions), false);

  UnixEndpointReplacementContext context(&socket);
  Thread* receiver =
      new Thread(process, blockedUnixEndpointReceive, &context, nullptr, false, true, true);
  receiver->setName("hosted Unix endpoint generation receive");
  context.receiver = receiver;
  g_UnixEndpointReplacementContext = &context;
  __atomic_store_n(&g_UnixEndpointReceiveLeaseHook, &holdRetiredUnixEndpointLease,
                   __ATOMIC_RELEASE);
  const bool started = receiver->start();

  bool blocked = false;
  for (size_t attempt = 0; attempt < Attempts && started; ++attempt) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (context.entered == 1 && !context.returned && receiver->getWaitDebugInfo(info) &&
        info.queue && info.queued && receiver->getDebugState(debugAddress) == Thread::SemWait) {
      blocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  String replacementPath("hosted-replacement");
  socket.replaceLocalEndpoint(new UnixEndpointLifetimeProbe(destructions), false, &replacementPath);
  bool leaseHeld = false;
  for (size_t attempt = 0; attempt < Attempts && started; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (context.leaseHeld == 1 && receiver->getWaitDebugInfo(info) && info.queue && info.queued &&
        info.channelOwner == &context.continueReceive) {
      leaseHeld = true;
      break;
    }
    Scheduler::instance().yield();
  }
  const bool retainedWhileInUse = leaseHeld && destructions == 0;
  context.continueReceive.release();
  const bool joined = started && receiver->join();
  __atomic_store_n(&g_UnixEndpointReceiveLeaseHook, nullptr, __ATOMIC_RELEASE);
  g_UnixEndpointReplacementContext = nullptr;

  bool passed = started && blocked && retainedWhileInUse && joined && context.returned == 1 &&
                context.result == 0 && destructions == 1 &&
                (socket.queryReady(true, true) & ReadyWrite);
  socket.lastDescriptorClosed();
  passed = passed && destructions == 2;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL unix-bind-replacement-lifetime: "
        "endpoint replacement freed a blocked receive generation or failed to wake it");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS unix-bind-replacement-lifetime");
  return true;
}
#endif
