#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/Processor.h"

#include "modules/system/users/Group.h"
#include "modules/system/users/User.h"

bool Process::loadFilesystemIds(const Thread& task, uint32_t& uid, uint32_t& gid) {
  if (!task.m_FilesystemIdsValid)
    return false;
  uid = task.m_FilesystemUid;
  gid = task.m_FilesystemGid;
  return true;
}

void Process::publishFilesystemIds(Thread& task, uint32_t uid, uint32_t gid) {
  task.m_FilesystemUid = uid;
  task.m_FilesystemGid = gid;
  task.m_FilesystemIdsValid = true;
}

bool Process::snapshotFilesystemCredentials(const Thread* task, FilesystemCredentials& out) const {
  LockGuard<Spinlock> guard(m_CredentialLock);
  out = m_NativeFilesystemCredentials;
  if (task && task->getParent() != this)
    return false;
  // Early filesystem discovery precedes loading the account database.
  if (!out.valid && this == Scheduler::instance().getKernelProcess()) {
    out.uid = out.gid = 0;
    out.valid = true;
  }
  if (!out.valid) {
    User* user = m_pEffectiveUser ? m_pEffectiveUser : m_pUser;
    Group* group = m_pEffectiveGroup ? m_pEffectiveGroup : m_pGroup;
    if (!user || !group || user->getId() >= UINT32_MAX || group->getId() >= UINT32_MAX)
      return false;
    out.uid = user->getId();
    out.gid = group->getId();
    out.valid = true;
  }
  if (task)
    loadFilesystemIds(*task, out.uid, out.gid);
  return true;
}

bool Process::currentFilesystemCredentials(FilesystemCredentials& out) {
  Thread* task = Processor::information().getCurrentThread();
  if (!task)
    return false;
  if (task->m_FilesystemOverride) {
    out = *task->m_FilesystemOverride;
    return out.valid;
  }
  return task->getParent()->snapshotFilesystemCredentials(task, out);
}

void Process::inheritFilesystemIds(Thread& child, const Thread* creator) const {
  FilesystemCredentials credentials;
  const Process* source = creator ? creator->getParent() : this;
  if (!source->snapshotFilesystemCredentials(creator, credentials))
    return;
  LockGuard<Spinlock> guard(m_CredentialLock);
  publishFilesystemIds(child, credentials.uid, credentials.gid);
}

void Process::publishAccountIdentity(User* user, Group* group) {
  m_pUser = m_pEffectiveUser = user;
  m_pGroup = m_pEffectiveGroup = group;
}

bool Process::installUserIdentity(User* user, Group* group, const uint32_t* groups, size_t count) {
  if (!user || !group || user->getId() >= UINT32_MAX || group->getId() >= UINT32_MAX ||
      count > FilesystemCredentials::MaximumGroups || (count && !groups))
    return false;
  for (size_t i = 0; i < count; ++i)
    if (groups[i] == UINT32_MAX)
      return false;
  LockGuard<Spinlock> guard(m_CredentialLock);
  publishAccountIdentity(user, group);
  auto& credentials = m_NativeFilesystemCredentials;
  credentials.uid = user->getId();
  credentials.gid = group->getId();
  credentials.groupCount = count;
  for (size_t i = 0; i < count; ++i)
    credentials.groups[i] = groups[i];
  credentials.valid = true;
  Thread* current = Processor::information().getCurrentThread();
  if (current && current->getParent() == this)
    publishFilesystemIds(*current, credentials.uid, credentials.gid);
  return true;
}

Process::FilesystemAccessScope::FilesystemAccessScope(const FilesystemCredentials& credentials)
    : m_Credentials(credentials),
      m_Thread(Processor::information().getCurrentThread()),
      m_Previous(m_Thread ? m_Thread->m_FilesystemOverride : nullptr) {
  if (m_Thread)
    m_Thread->m_FilesystemOverride = &m_Credentials;
}

Process::FilesystemAccessScope::~FilesystemAccessScope() {
  if (m_Thread)
    m_Thread->m_FilesystemOverride = m_Previous;
}

User* Process::getUser() const {
  LockGuard<Spinlock> guard(m_CredentialLock);
  return m_pUser;
}

User* Process::getEffectiveUser() const {
  LockGuard<Spinlock> guard(m_CredentialLock);
  return m_pEffectiveUser;
}

Group* Process::getGroup() const {
  LockGuard<Spinlock> guard(m_CredentialLock);
  return m_pGroup;
}

Group* Process::getEffectiveGroup() const {
  LockGuard<Spinlock> guard(m_CredentialLock);
  return m_pEffectiveGroup;
}

void Process::setUser(User* value) {
  LockGuard<Spinlock> guard(m_CredentialLock);
  m_pUser = value;
  m_NativeFilesystemCredentials.valid = false;
}

void Process::setEffectiveUser(User* value) {
  LockGuard<Spinlock> guard(m_CredentialLock);
  m_pEffectiveUser = value;
  m_NativeFilesystemCredentials.valid = false;
}

void Process::setGroup(Group* value) {
  LockGuard<Spinlock> guard(m_CredentialLock);
  m_pGroup = value;
  m_NativeFilesystemCredentials.valid = false;
}

void Process::setEffectiveGroup(Group* value) {
  LockGuard<Spinlock> guard(m_CredentialLock);
  m_pEffectiveGroup = value;
  m_NativeFilesystemCredentials.valid = false;
}
