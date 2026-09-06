#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/processor/Processor.h"

#include "PosixProcess.h"
#include "modules/system/users/Group.h"
#include "modules/system/users/User.h"
#include "modules/system/vfs/MemoryMappedFile.h"

PosixProcess::CredentialSnapshot PosixProcess::snapshotCredentials() const {
  LockGuard<Spinlock> guard(m_CredentialLock);
  return m_Credentials;
}

FilesystemCredentials PosixProcess::realFilesystemCredentials() const {
  LockGuard<Spinlock> guard(m_CredentialLock);
  FilesystemCredentials out;
  out.uid = m_Credentials.ruid;
  out.gid = m_Credentials.rgid;
  out.groupCount = m_Credentials.groupCount;
  for (size_t i = 0; i < out.groupCount; ++i)
    out.groups[i] = m_Credentials.groups[i];
  out.valid = true;
  return out;
}

bool PosixProcess::snapshotFilesystemCredentials(const Thread* task,
                                                 FilesystemCredentials& out) const {
  LockGuard<Spinlock> guard(m_CredentialLock);
  out = FilesystemCredentials();
  if (task && task->getParent() != this)
    return false;
  out.uid = m_Credentials.euid;
  out.gid = m_Credentials.egid;
  if (task)
    loadFilesystemIds(*task, out.uid, out.gid);
  out.groupCount = m_Credentials.groupCount;
  for (size_t i = 0; i < out.groupCount; ++i)
    out.groups[i] = m_Credentials.groups[i];
  out.valid = true;
  return true;
}

PosixProcess::CredentialStatus PosixProcess::changeCredentials(Thread& task,
                                                               CredentialChange change,
                                                               uint32_t first, uint32_t second,
                                                               uint32_t third) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  LockGuard<Spinlock> guard(m_CredentialLock);
  if (task.getParent() != this)
    return CredentialStatus::Invalid;
  uint32_t fsuid = m_Credentials.euid, fsgid = m_Credentials.egid;
  loadFilesystemIds(task, fsuid, fsgid);
  const bool group = change == CredentialChange::SetGid || change == CredentialChange::SetReGid ||
                     change == CredentialChange::SetResGid;
  CredentialSnapshot next;
  uint32_t nextFs;
  const auto status = PosixCredentials::prepare(m_Credentials, change, first, second, third,
                                                group ? fsgid : fsuid, next, nextFs);
  if (status != CredentialStatus::Success)
    return status;
  m_Credentials = next;
  publishFilesystemIds(task, group ? fsuid : nextFs, group ? nextFs : fsgid);
  return status;
}

PosixProcess::CredentialStatus PosixProcess::replaceGroups(Thread& task, const uint32_t* groups,
                                                           size_t count) {
  if (count > FilesystemCredentials::MaximumGroups || (count && !groups))
    return CredentialStatus::Invalid;
  uint32_t ordered[FilesystemCredentials::MaximumGroups] = {};
  for (size_t i = 0; i < count; ++i) {
    if (groups[i] == UINT32_MAX)
      return CredentialStatus::Invalid;
    size_t position = i;
    while (position && ordered[position - 1] > groups[i]) {
      ordered[position] = ordered[position - 1];
      --position;
    }
    ordered[position] = groups[i];
  }
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  LockGuard<Spinlock> guard(m_CredentialLock);
  if (task.getParent() != this)
    return CredentialStatus::Invalid;
  if (m_Credentials.euid)
    return CredentialStatus::Denied;
  bool changed = count != m_Credentials.groupCount;
  for (size_t i = 0; i < count; ++i) {
    changed |= m_Credentials.groups[i] != ordered[i];
    m_Credentials.groups[i] = ordered[i];
  }
  m_Credentials.groupCount = count;
  if (changed)
    ++m_Credentials.generation;
  return CredentialStatus::Success;
}

uint32_t PosixProcess::changeFilesystemId(Thread& task, bool group, uint32_t requested) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  LockGuard<Spinlock> guard(m_CredentialLock);
  uint32_t uid = m_Credentials.euid, gid = m_Credentials.egid;
  if (task.getParent() != this)
    return group ? gid : uid;
  loadFilesystemIds(task, uid, gid);
  const uint32_t old = group ? gid : uid;
  const uint32_t real = group ? m_Credentials.rgid : m_Credentials.ruid;
  const uint32_t effective = group ? m_Credentials.egid : m_Credentials.euid;
  const uint32_t saved = group ? m_Credentials.sgid : m_Credentials.suid;
  if (requested != UINT32_MAX && requested != old &&
      (!m_Credentials.euid || requested == real || requested == effective || requested == saved)) {
    publishFilesystemIds(task, group ? uid : requested, group ? requested : gid);
    m_Credentials.dumpable = false;
    ++m_Credentials.generation;
  }
  return old;
}

void PosixProcess::setDumpable(bool dumpable) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  LockGuard<Spinlock> guard(m_CredentialLock);
  if (m_Credentials.dumpable != dumpable) {
    m_Credentials.dumpable = dumpable;
    ++m_Credentials.generation;
  }
}

void PosixProcess::commitExecCredentials(Thread& task, bool readable) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  LockGuard<Spinlock> guard(m_CredentialLock);
  m_Credentials.suid = m_Credentials.euid;
  m_Credentials.sgid = m_Credentials.egid;
  publishFilesystemIds(task, m_Credentials.euid, m_Credentials.egid);
  m_Credentials.dumpable = readable && m_Credentials.ruid == m_Credentials.euid &&
                           m_Credentials.rgid == m_Credentials.egid;
  ++m_Credentials.generation;
}

bool PosixProcess::installUserIdentity(User* user, Group* group, const uint32_t* groups,
                                       size_t count) {
  if (!user || !group || user->getId() >= UINT32_MAX || group->getId() >= UINT32_MAX ||
      count > FilesystemCredentials::MaximumGroups || (count && !groups))
    return false;
  for (size_t i = 0; i < count; ++i)
    if (groups[i] == UINT32_MAX)
      return false;
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  LockGuard<Spinlock> guard(m_CredentialLock);
  CredentialSnapshot next;
  next.ruid = next.euid = next.suid = user->getId();
  next.rgid = next.egid = next.sgid = group->getId();
  next.groupCount = count;
  for (size_t i = 0; i < count; ++i)
    next.groups[i] = groups[i];
  next.generation = m_Credentials.generation + 1;
  next.dumpable = false;
  m_Credentials = next;
  publishAccountIdentity(user, group);
  Thread* current = Processor::information().getCurrentThread();
  if (current && current->getParent() == this)
    publishFilesystemIds(*current, next.euid, next.egid);
  return true;
}

int64_t PosixProcess::getUserId() const {
  return snapshotCredentials().ruid;
}
int64_t PosixProcess::getGroupId() const {
  return snapshotCredentials().rgid;
}
int64_t PosixProcess::getEffectiveUserId() const {
  return snapshotCredentials().euid;
}
int64_t PosixProcess::getEffectiveGroupId() const {
  return snapshotCredentials().egid;
}
int64_t PosixProcess::getSavedUserId() const {
  return snapshotCredentials().suid;
}
int64_t PosixProcess::getSavedGroupId() const {
  return snapshotCredentials().sgid;
}
void PosixProcess::getSupplementalGroupIds(Vector<int64_t>& groups) const {
  const auto snapshot = snapshotCredentials();
  for (size_t i = 0; i < snapshot.groupCount; ++i)
    groups.pushBack(snapshot.groups[i]);
}

// These legacy kernel setters remain for trusted setup and existing fixtures.
// User syscalls must use the validated tuple transition above.
void PosixProcess::setTrustedIdentity(uint32_t CredentialSnapshot::* field, int64_t id) {
  if (id < 0 || static_cast<uint64_t>(id) >= UINT32_MAX)
    return;
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  LockGuard<Spinlock> guard(m_CredentialLock);
  m_Credentials.*field = static_cast<uint32_t>(id);
  m_Credentials.dumpable = false;
  ++m_Credentials.generation;
  Thread* task = Processor::information().getCurrentThread();
  if (task && task->getParent() == this)
    publishFilesystemIds(*task, m_Credentials.euid, m_Credentials.egid);
}
void PosixProcess::setUserId(int64_t id) {
  setTrustedIdentity(&CredentialSnapshot::ruid, id);
}
void PosixProcess::setGroupId(int64_t id) {
  setTrustedIdentity(&CredentialSnapshot::rgid, id);
}
void PosixProcess::setEffectiveUserId(int64_t id) {
  setTrustedIdentity(&CredentialSnapshot::euid, id);
}
void PosixProcess::setEffectiveGroupId(int64_t id) {
  setTrustedIdentity(&CredentialSnapshot::egid, id);
}
void PosixProcess::setSavedUserId(int64_t id) {
  setTrustedIdentity(&CredentialSnapshot::suid, id);
}
void PosixProcess::setSavedGroupId(int64_t id) {
  setTrustedIdentity(&CredentialSnapshot::sgid, id);
}

void PosixProcess::setSupplementalGroupIds(const Vector<int64_t>& groups) {
  if (groups.count() > FilesystemCredentials::MaximumGroups)
    return;
  for (int64_t id : groups)
    if (id < 0 || static_cast<uint64_t>(id) >= UINT32_MAX)
      return;
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  LockGuard<Spinlock> guard(m_CredentialLock);
  m_Credentials.groupCount = groups.count();
  for (size_t i = 0; i < groups.count(); ++i)
    m_Credentials.groups[i] = groups[i];
  ++m_Credentials.generation;
}
