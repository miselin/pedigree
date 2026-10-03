/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/StaticString.h"

#include "PosixProcess.h"
#include "sandbox-state.h"
#include "user-namespace.h"

namespace {
uint64_t nextIdentity = 0xefffffff;
Thread& current() {
  return *Processor::information().getCurrentThread();
}
uint32_t effectiveUid(Thread& task) {
  return static_cast<uint32_t>(task.getParent()->getEffectiveUserId());
}
bool number(const char*& cursor, const char* end, uint32_t& value) {
  while (cursor != end && (*cursor == ' ' || *cursor == '\t')) {
    ++cursor;
  }
  if (cursor == end || *cursor < '0' || *cursor > '9') {
    return false;
  }
  uint64_t result = 0;
  do {
    result = result * 10 + (*cursor++ - '0');
    if (result > UINT32_MAX) {
      return false;
    }
  } while (cursor != end && *cursor >= '0' && *cursor <= '9');
  value = static_cast<uint32_t>(result);
  return true;
}
bool overlaps(uint32_t first, uint32_t count, uint32_t other, uint32_t otherCount) {
  return uint64_t(first) < uint64_t(other) + otherCount &&
         uint64_t(other) < uint64_t(first) + count;
}
int denied() {
  SYSCALL_ERROR(NotEnoughPermissions);
  return -1;
}
int invalid() {
  SYSCALL_ERROR(InvalidArgument);
  return -1;
}
}  // namespace

PosixUserNamespace::PosixUserNamespace(const UserNamespaceRef& parent, uint32_t owner,
                                       uint32_t group, bool creatorSetfcap)
    : m_Parent(parent),
      m_Owner(owner),
      m_Group(group),
      m_Depth(parent ? parent->depth() + 1 : 1),
      m_Identity(__atomic_add_fetch(&nextIdentity, 1, __ATOMIC_RELAXED)),
      m_CreatorSetfcap(creatorSetfcap),
      m_GroupsAllowed(!parent || parent->groupsAllowed()) {}

bool PosixUserNamespace::toGlobal(bool group, uint32_t id, uint32_t& global, uint32_t count) const {
  LockGuard<Mutex> guard(m_Lock);
  const Map& map = group ? m_Gids : m_Uids;
  for (size_t i = 0; i < map.count; ++i) {
    const auto& range = map.ranges[i];
    if (id >= range.inside && uint64_t(id) + count <= uint64_t(range.inside) + range.count) {
      global = range.global + (id - range.inside);
      return true;
    }
  }
  return false;
}

bool PosixUserNamespace::fromGlobal(bool group, uint32_t global, uint32_t& id) const {
  LockGuard<Mutex> guard(m_Lock);
  const Map& map = group ? m_Gids : m_Uids;
  for (size_t i = 0; i < map.count; ++i) {
    const auto& range = map.ranges[i];
    if (global >= range.global && uint64_t(global) < uint64_t(range.global) + range.count) {
      id = range.inside + (global - range.global);
      return true;
    }
  }
  return false;
}

bool PosixUserNamespace::groupsAllowed() const {
  LockGuard<Mutex> guard(m_Lock);
  return m_GroupsAllowed;
}

int PosixUserNamespace::writeMap(bool group, const char* bytes, size_t length) {
  TerminationDeferral lifetime;
  if (!length || length >= 4096) {
    return invalid();
  }
  const auto authority = posix_task_credentials(current());
  if (authority.userNamespace.get() != this && authority.userNamespace != m_Parent) {
    return denied();
  }
  const unsigned capability = group ? PosixCapabilities::Setgid : PosixCapabilities::Setuid;
  // A creator in the parent has authority over the child, but never vice versa.
  if (authority.userNamespace.get() == this) {
    if (!(authority.effective & (uint64_t(1) << capability))) {
      return denied();
    }
  } else if (effectiveUid(current()) != m_Owner && !posix_namespace_capable(m_Parent, capability)) {
    return denied();
  }

  auto storage = UniquePointer<Map>::allocate();
  Map* parsed = storage.get();
  if (!parsed) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  const char* cursor = bytes;
  const char* end = bytes + length;
  while (cursor != end) {
    if (parsed->count == MaximumRanges) {
      return invalid();
    }
    auto& range = parsed->ranges[parsed->count];
    if (!number(cursor, end, range.inside) || cursor == end ||
        (*cursor != ' ' && *cursor != '\t') || !number(cursor, end, range.outside) ||
        cursor == end || (*cursor != ' ' && *cursor != '\t') || !number(cursor, end, range.count) ||
        !range.count || uint64_t(range.inside) + range.count > UINT32_MAX ||
        uint64_t(range.outside) + range.count > UINT32_MAX) {
      return invalid();
    }
    while (cursor != end && (*cursor == ' ' || *cursor == '\t')) {
      ++cursor;
    }
    if (cursor != end && *cursor++ != '\n') {
      return invalid();
    }
    for (size_t i = 0; i < parsed->count; ++i) {
      const auto& previous = parsed->ranges[i];
      if (overlaps(range.inside, range.count, previous.inside, previous.count) ||
          overlaps(range.outside, range.count, previous.outside, previous.count)) {
        return invalid();
      }
    }
    range.global = range.outside;
    if (m_Parent && !m_Parent->toGlobal(group, range.outside, range.global, range.count)) {
      return denied();
    }
    if (!group && range.outside == 0 &&
        !(authority.userNamespace.get() == this
              ? m_CreatorSetfcap
              : posix_namespace_capable(m_Parent, PosixCapabilities::Setfcap))) {
      return denied();
    }
    ++parsed->count;
  }
  const bool parentPrivilege = posix_namespace_capable(m_Parent, capability);
  const uint32_t real =
      group ? current().getParent()->getGroupId() : current().getParent()->getUserId();
  LockGuard<Mutex> guard(m_Lock);
  Map& destination = group ? m_Gids : m_Uids;
  if (destination.count) {
    return denied();
  }
  if (!parentPrivilege &&
      (parsed->count != 1 || parsed->ranges[0].count != 1 || parsed->ranges[0].global != real ||
       effectiveUid(current()) != m_Owner || (group && m_GroupsAllowed))) {
    return denied();
  }
  destination = *parsed;
  current().setErrno(0);
  return static_cast<int>(length);
}

size_t PosixUserNamespace::readMap(bool group, const UserNamespaceRef& viewer, char* bytes,
                                   size_t capacity) const {
  auto storage = UniquePointer<Map>::allocate();
  Map* snapshot = storage.get();
  if (!snapshot) {
    SYSCALL_ERROR(OutOfMemory);
    return 0;
  }
  {
    LockGuard<Mutex> guard(m_Lock);
    *snapshot = group ? m_Gids : m_Uids;
  }
  size_t used = 0;
  for (size_t i = 0; i < snapshot->count; ++i) {
    const auto& range = snapshot->ranges[i];
    uint32_t outside = range.global;
    if (viewer.get() == this) {
      outside = range.outside;
    } else if (viewer && !viewer->fromGlobal(group, range.global, outside)) {
      outside = 65534;
    }
    NormalStaticString line;
    line.append(range.inside);
    line.append(' ');
    line.append(outside);
    line.append(' ');
    line.append(range.count);
    line.append('\n');
    const size_t copied = line.length() < capacity - used ? line.length() : capacity - used;
    MemoryCopy(bytes + used, static_cast<const char*>(line), copied);
    used += copied;
    if (used == capacity) {
      break;
    }
  }
  return used;
}

int PosixUserNamespace::writeSetgroups(const char* bytes, size_t length) {
  TerminationDeferral lifetime;
  bool allow;
  if ((length == 4 || (length == 5 && bytes[4] == '\n')) && !MemoryCompare(bytes, "deny", 4)) {
    allow = false;
  } else if ((length == 5 || (length == 6 && bytes[5] == '\n')) &&
             !MemoryCompare(bytes, "allow", 5)) {
    allow = true;
  } else {
    return invalid();
  }
  const auto authority = posix_task_credentials(current());
  if (authority.userNamespace.get() != this && authority.userNamespace != m_Parent) {
    return denied();
  }
  if (authority.userNamespace.get() == this) {
    if (!(authority.effective & (uint64_t(1) << PosixCapabilities::SysAdmin))) {
      return denied();
    }
  } else if (effectiveUid(current()) != m_Owner &&
             !posix_namespace_capable(m_Parent, PosixCapabilities::SysAdmin)) {
    return denied();
  }
  LockGuard<Mutex> guard(m_Lock);
  if (m_Gids.count || (allow && !m_GroupsAllowed)) {
    return denied();
  }
  m_GroupsAllowed = allow;
  current().setErrno(0);
  return static_cast<int>(length);
}

PosixTaskCredentials posix_task_credentials(Thread& task) {
  auto stored = posix_sandbox_credentials(task);
  if (stored) {
    return *stored;
  }
  PosixTaskCredentials result;
  if (!effectiveUid(task)) {
    result.permitted = result.effective = PosixCapabilities::All;
  }
  return result;
}
UserNamespaceRef posix_user_namespace(Thread& task) {
  auto stored = posix_sandbox_credentials(task);
  return stored ? stored->userNamespace : UserNamespaceRef();
}
bool posix_namespace_capable(Thread& task, const UserNamespaceRef& target, unsigned capability) {
  if (capability > PosixCapabilities::Last) {
    return false;
  }
  const auto authority = posix_task_credentials(task);
  auto space = target;
  for (;;) {
    if (space == authority.userNamespace) {
      return authority.effective & (uint64_t(1) << capability);
    }
    if (!space) {
      return false;
    }
    if (space->parent() == authority.userNamespace && space->owner() == effectiveUid(task)) {
      return true;
    }
    space = space->parent();
  }
}
bool posix_namespace_capable(const UserNamespaceRef& space, unsigned capability) {
  return posix_namespace_capable(current(), space, capability);
}
bool posix_capable(unsigned capability) {
  return posix_namespace_capable(posix_user_namespace(current()), capability);
}
bool posix_global_capable(unsigned capability) {
  return posix_namespace_capable(UserNamespaceRef(), capability);
}
bool posix_user_namespace_prepare(Thread& creator, TaskCredentialsRef& result) {
  auto parent = posix_user_namespace(creator);
  if (parent && parent->depth() >= 32) {
    SYSCALL_ERROR(NoSpaceLeftOnDevice);
    return false;
  }
  uint32_t uid = effectiveUid(creator);
  uint32_t gid = creator.getParent()->getEffectiveGroupId();
  uint32_t ignored;
  if (parent &&
      (!parent->fromGlobal(false, uid, ignored) || !parent->fromGlobal(true, gid, ignored))) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  auto space = UserNamespaceRef::tryAllocate(
      parent, uid, gid, posix_namespace_capable(creator, parent, PosixCapabilities::Setfcap));
  auto replacement = TaskCredentialsRef::tryAllocate();
  if (!space || !replacement) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  replacement->userNamespace = space;
  replacement->effective = replacement->permitted = PosixCapabilities::All;
  result = replacement;
  return true;
}
uint32_t posix_visible_id(bool group, uint32_t global) {
  auto space = posix_user_namespace(current());
  uint32_t visible = global;
  if (space && !space->fromGlobal(group, global, visible)) {
    return 65534;
  }
  return visible;
}
bool posix_global_id(bool group, uint32_t visible, uint32_t& global) {
  if (visible == UINT32_MAX) {
    global = visible;
    return true;
  }
  auto space = posix_user_namespace(current());
  global = visible;
  return !space || space->toGlobal(group, visible, global);
}
