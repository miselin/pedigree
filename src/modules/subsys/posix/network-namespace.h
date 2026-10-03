/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_NETWORK_NAMESPACE_H
#define POSIX_NETWORK_NAMESPACE_H

#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/Tree.h"

#include "user-namespace.h"

class NetworkSyscalls;
class PosixNetworkNamespace;
using NetworkNamespaceRef = SharedPointer<PosixNetworkNamespace>;

EXPORTED_PUBLIC NetworkNamespaceRef posix_sandbox_network(Thread& task);
EXPORTED_PUBLIC bool posix_sandbox_set_network(Thread& task, const NetworkNamespaceRef& space);

class EXPORTED_PUBLIC PosixNetworkNamespace {
 public:
  explicit PosixNetworkNamespace(const UserNamespaceRef& owner);
  const UserNamespaceRef& owner() const {
    return m_Owner;
  }
  uint64_t identity() const {
    return m_Identity;
  }
  bool usable() const;
  int configureAddress(uint32_t address, unsigned prefix, bool exclusive);
  int configureLink(uint32_t flags, uint32_t changed);
  uint32_t flags() const;
  uint32_t address() const;
  uint64_t reserve(int type, uint32_t address, uint16_t& port);
  void release(uint64_t token);
  uint32_t reserveRoutePort(uint32_t preferred, bool fixed);
  void releaseRoutePort(uint32_t port);
  bool find(int type, uint32_t address, uint16_t port, uint32_t& boundAddress, uint64_t& token);

 private:
  const UserNamespaceRef m_Owner;
  const uint64_t m_Identity;
  mutable Mutex m_Lock;
  bool m_Up = false;
  uint32_t m_Address = 0;
  uint16_t m_NextPort = 32768;
  uint64_t m_NextToken = 0;
  Tree<uint64_t, uint64_t> m_Bindings;
  Tree<uint32_t, bool> m_RoutePorts;
  uint32_t m_NextRoutePort = 0x80000000;
};

EXPORTED_PUBLIC bool posix_network_namespace_prepare(const UserNamespaceRef& owner,
                                                     NetworkNamespaceRef& result);
EXPORTED_PUBLIC NetworkSyscalls* posix_network_socket(int domain, int type, int protocol,
                                                      const NetworkNamespaceRef& space);
NetworkSyscalls* posix_route_netlink_socket(int type, int protocol,
                                            const NetworkNamespaceRef& space);
EXPORTED_PUBLIC int posix_network_ioctl(NetworkSyscalls& socket, unsigned long request,
                                        uintptr_t argument);

#endif
