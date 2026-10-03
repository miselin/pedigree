/* Copyright (c) 2026, Pedigree Developers. */
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1
#include "network-namespace.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include <errno.h>

#include "PosixSubsystem.h"
#include "net-syscalls.h"
#include <netinet/in.h>

namespace {
uint64_t nextNamespace = 0;
constexpr uint32_t Loopback = 0x7f000001;
uint64_t endpointKey(int type, uint32_t address, uint16_t port) {
  return (uint64_t(type) << 48) | (uint64_t(address) << 16) | port;
}
}  // namespace

PosixNetworkNamespace::PosixNetworkNamespace(const UserNamespaceRef& owner)
    : m_Owner(owner), m_Identity(__atomic_add_fetch(&nextNamespace, 1, __ATOMIC_RELAXED)) {}

bool posix_network_namespace_prepare(const UserNamespaceRef& owner, NetworkNamespaceRef& result) {
  result = NetworkNamespaceRef::tryAllocate(owner);
  if (!result) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  return true;
}

bool PosixNetworkNamespace::usable() const {
  LockGuard<Mutex> guard(m_Lock);
  return m_Up && m_Address;
}
uint32_t PosixNetworkNamespace::flags() const {
  LockGuard<Mutex> guard(m_Lock);
  return 8 | (m_Up ? 1 | 0x40 : 0);
}
uint32_t PosixNetworkNamespace::address() const {
  LockGuard<Mutex> guard(m_Lock);
  return m_Address;
}
int PosixNetworkNamespace::configureAddress(uint32_t address, unsigned prefix, bool exclusive) {
  if (!posix_namespace_capable(m_Owner, PosixCapabilities::NetAdmin)) {
    return EPERM;
  }
  if (address != Loopback || prefix != 8) {
    return EINVAL;
  }
  LockGuard<Mutex> guard(m_Lock);
  if (exclusive && m_Address) {
    return EEXIST;
  }
  m_Address = address;
  return 0;
}
int PosixNetworkNamespace::configureLink(uint32_t flags, uint32_t changed) {
  if (!posix_namespace_capable(m_Owner, PosixCapabilities::NetAdmin)) {
    return EPERM;
  }
  if (changed & ~1U) {
    return EOPNOTSUPP;
  }
  LockGuard<Mutex> guard(m_Lock);
  if (changed & 1) {
    m_Up = flags & 1;
  }
  return 0;
}

uint64_t PosixNetworkNamespace::reserve(int type, uint32_t address, uint16_t& port) {
  if (port && port < 1024 && !posix_namespace_capable(m_Owner, PosixCapabilities::NetBindService)) {
    syscallError(EACCES);
    return 0;
  }
  LockGuard<Mutex> guard(m_Lock);
  if (address && address != m_Address) {
    syscallError(EADDRNOTAVAIL);
    return 0;
  }
  auto available = [&](uint16_t candidate) {
    for (auto it = m_Bindings.begin(); it != m_Bindings.end(); ++it) {
      if ((it.key() >> 48) == static_cast<unsigned>(type) && uint16_t(it.key()) == candidate) {
        const uint32_t existing = it.key() >> 16;
        if (!existing || !address || existing == address) {
          return false;
        }
      }
    }
    return true;
  };
  if (port) {
    if (!available(port)) {
      syscallError(EADDRINUSE);
      return 0;
    }
  } else {
    unsigned attempts = 0;
    do {
      if (++attempts > 28232) {
        syscallError(EADDRINUSE);
        return 0;
      }
      port = m_NextPort++;
      if (m_NextPort == 61000) {
        m_NextPort = 32768;
      }
    } while (!available(port));
  }
  const uint64_t token = ++m_NextToken;
  if (!m_Bindings.tryInsert(endpointKey(type, address, port), token)) {
    syscallError(ENOMEM);
    return 0;
  }
  return token;
}
void PosixNetworkNamespace::release(uint64_t token) {
  LockGuard<Mutex> guard(m_Lock);
  for (auto it = m_Bindings.begin(); it != m_Bindings.end(); ++it) {
    if (it.value() == token) {
      m_Bindings.remove(it.key());
      return;
    }
  }
}
uint32_t PosixNetworkNamespace::reserveRoutePort(uint32_t preferred, bool fixed) {
  LockGuard<Mutex> guard(m_Lock);
  if (m_RoutePorts.lookup(preferred)) {
    if (fixed) {
      syscallError(EADDRINUSE);
      return 0;
    }
    do {
      preferred = m_NextRoutePort++;
    } while (!preferred || m_RoutePorts.lookup(preferred));
  }
  if (!m_RoutePorts.tryInsert(preferred, true)) {
    syscallError(ENOMEM);
    return 0;
  }
  return preferred;
}
void PosixNetworkNamespace::releaseRoutePort(uint32_t port) {
  LockGuard<Mutex> guard(m_Lock);
  m_RoutePorts.remove(port);
}
bool PosixNetworkNamespace::find(int type, uint32_t address, uint16_t port, uint32_t& boundAddress,
                                 uint64_t& token) {
  LockGuard<Mutex> guard(m_Lock);
  if (!m_Up || !m_Address) {
    syscallError(ENETUNREACH);
    return false;
  }
  if (address != m_Address) {
    syscallError(ENETUNREACH);
    return false;
  }
  boundAddress = address;
  token = m_Bindings.lookup(endpointKey(type, address, port));
  if (!token) {
    boundAddress = 0;
    token = m_Bindings.lookup(endpointKey(type, 0, port));
  }
  if (!token) {
    syscallError(ECONNREFUSED);
  }
  return token != 0;
}

int posix_network_ioctl(NetworkSyscalls& socket, unsigned long request, uintptr_t argument) {
  const auto& space = socket.networkNamespace();
  if (!space) {
    syscallError(ENOTTY);
    return -1;
  }
  if (request != 0x8910 && request != 0x8913 && request != 0x8914 && request != 0x8915 &&
      request != 0x8916 && request != 0x891b && request != 0x8921 && request != 0x8933) {
    syscallError(ENOTTY);
    return -1;
  }
  struct IfRequest {
    char name[16];
    union {
      int index;
      int mtu;
      uint16_t flags;
      sockaddr address;
      char padding[sizeof(uintptr_t) == 8 ? 24 : 16];
    } value;
  } snapshot = {};
  if (!PosixSubsystem::copyFromUser(&snapshot, reinterpret_cast<void*>(argument),
                                    sizeof(snapshot))) {
    syscallError(EFAULT);
    return -1;
  }
  if (request == 0x8910) {
    if (snapshot.value.index != 1) {
      syscallError(ENODEV);
      return -1;
    }
    ByteSet(snapshot.name, 0, sizeof(snapshot.name));
    MemoryCopy(snapshot.name, "lo", 2);
  } else {
    if (snapshot.name[0] != 'l' || snapshot.name[1] != 'o' || snapshot.name[2]) {
      syscallError(ENODEV);
      return -1;
    }
    switch (request) {
      case 0x8933:
        snapshot.value.index = 1;
        break;
      case 0x8913:
        snapshot.value.flags = space->flags();
        break;
      case 0x8921:
        snapshot.value.mtu = 65536;
        break;
      case 0x8915:
      case 0x891b: {
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = HOST_TO_BIG32(request == 0x8915 ? space->address() : 0xff000000);
        MemoryCopy(&snapshot.value.address, &address, sizeof(address));
        break;
      }
      case 0x8914: {
        const int error = space->configureLink(snapshot.value.flags, 1);
        if (error) {
          syscallError(error);
          return -1;
        }
        break;
      }
      case 0x8916: {
        sockaddr_in address;
        MemoryCopy(&address, &snapshot.value.address, sizeof(address));
        const int error =
            address.sin_family != AF_INET
                ? EINVAL
                : space->configureAddress(BIG_TO_HOST32(address.sin_addr.s_addr), 8, false);
        if (error) {
          syscallError(error);
          return -1;
        }
        break;
      }
    }
  }
  if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(argument), &snapshot, sizeof(snapshot))) {
    syscallError(EFAULT);
    return -1;
  }
  return 0;
}
