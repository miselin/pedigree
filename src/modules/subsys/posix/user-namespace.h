/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_USER_NAMESPACE_H
#define POSIX_USER_NAMESPACE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

class Thread;
class PosixUserNamespace;
using UserNamespaceRef = SharedPointer<PosixUserNamespace>;

namespace PosixCapabilities {
enum : unsigned {
  Chown = 0,
  DacOverride = 1,
  DacReadSearch = 2,
  Fowner = 3,
  Fsetid = 4,
  Kill = 5,
  Setgid = 6,
  Setuid = 7,
  Setpcap = 8,
  NetBindService = 10,
  NetAdmin = 12,
  NetRaw = 13,
  SysChroot = 18,
  SysPtrace = 19,
  SysAdmin = 21,
  SysBoot = 22,
  SysNice = 23,
  SysResource = 24,
  SysTime = 25,
  Mknod = 27,
  Setfcap = 31,
  Last = 40
};
constexpr uint64_t All = (uint64_t(1) << (Last + 1)) - 1;
}  // namespace PosixCapabilities

// Published through Thread::SecurityState. Updates replace the entire snapshot.
class EXPORTED_PUBLIC PosixTaskCredentials {
 public:
  UserNamespaceRef userNamespace;
  uint64_t permitted = 0, effective = 0, inheritable = 0;
  uint64_t bounding = PosixCapabilities::All, ambient = 0;
  uint32_t secureBits = 0;
};
using TaskCredentialsRef = SharedPointer<PosixTaskCredentials>;

class EXPORTED_PUBLIC PosixUserNamespace {
 public:
  static constexpr size_t MaximumRanges = 340;
  struct Range {
    uint32_t inside, outside, count, global;
  };

  PosixUserNamespace(const UserNamespaceRef& parent, uint32_t owner, uint32_t group,
                     bool creatorSetfcap);
  const UserNamespaceRef& parent() const {
    return m_Parent;
  }
  uint32_t owner() const {
    return m_Owner;
  }
  unsigned depth() const {
    return m_Depth;
  }
  uint64_t identity() const {
    return m_Identity;
  }
  bool toGlobal(bool group, uint32_t id, uint32_t& global, uint32_t count = 1) const;
  bool fromGlobal(bool group, uint32_t global, uint32_t& id) const;
  bool groupsAllowed() const;
  int writeMap(bool group, const char* bytes, size_t length);
  size_t readMap(bool group, const UserNamespaceRef& viewer, char* bytes, size_t capacity) const;
  int writeSetgroups(const char* bytes, size_t length);

 private:
  struct Map {
    Range ranges[MaximumRanges] = {};
    size_t count = 0;
  };
  const UserNamespaceRef m_Parent;
  const uint32_t m_Owner, m_Group;
  const unsigned m_Depth;
  const uint64_t m_Identity;
  const bool m_CreatorSetfcap;
  mutable Mutex m_Lock;
  Map m_Uids, m_Gids;
  bool m_GroupsAllowed;
};

EXPORTED_PUBLIC PosixTaskCredentials posix_task_credentials(Thread& task);
EXPORTED_PUBLIC UserNamespaceRef posix_user_namespace(Thread& task);
EXPORTED_PUBLIC bool posix_user_namespace_prepare(Thread& creator, TaskCredentialsRef& result);
EXPORTED_PUBLIC bool posix_capable(unsigned capability);
EXPORTED_PUBLIC bool posix_global_capable(unsigned capability);
EXPORTED_PUBLIC bool posix_namespace_capable(const UserNamespaceRef& space, unsigned capability);
EXPORTED_PUBLIC bool posix_namespace_capable(Thread& task, const UserNamespaceRef& space,
                                             unsigned capability);
EXPORTED_PUBLIC uint32_t posix_visible_id(bool group, uint32_t global);
EXPORTED_PUBLIC bool posix_global_id(bool group, uint32_t visible, uint32_t& global);
EXPORTED_PUBLIC int posix_capability_prctl(int option, unsigned long arg2, unsigned long arg3,
                                           unsigned long arg4, unsigned long arg5);
EXPORTED_PUBLIC bool posix_capabilities_exec(Thread& task, uint32_t globalUid);
EXPORTED_PUBLIC bool posix_capabilities_uid_change(Thread& task, uint32_t oldReal,
                                                   uint32_t oldEffective, uint32_t oldSaved,
                                                   uint32_t newReal, uint32_t newEffective,
                                                   uint32_t newSaved, uint32_t namespaceRoot);
EXPORTED_PUBLIC bool posix_capabilities_fsuid_change(Thread& task, uint32_t oldUid, uint32_t newUid,
                                                     uint32_t namespaceRoot);

#endif
