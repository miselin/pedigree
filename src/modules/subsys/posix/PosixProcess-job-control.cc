/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/assert.h"

#include "PosixProcess.h"
#include "TerminalControl.h"

ProcessGroup::~ProcessGroup() {
  RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
  assert(!firstMember && !memberCount);
  if (registered)
    ProcessGroupManager::instance().unregisterGroup(processGroupId, this);
}

void PosixProcess::initializeJobControl(Process* parent) {
  LockGuard<Mutex> terminalGuard(TerminalControl::lock());
  if (parent && parent->getType() == Posix) {
    inheritProcessGroup(static_cast<PosixProcess*>(parent));
    // Core construction precedes POSIX session staging. Re-snapshot both
    // authorities under the same policy lock used by setsid and terminal claims.
    setCttyContext(parent->acquireCttyContext());
    return;
  }
  auto* group = new ProcessGroup;
  if (!group)
    return;
  group->processGroupId = getUserspaceId();
  group->sessionId = getUserspaceId();
  group->Leader = this;
  setProcessGroup(group);
}

void PosixProcess::setProcessGroup(ProcessGroup* group) {
  RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
  ProcessGroup* old = m_pProcessGroup;
  if (old == group)
    return;
  if (old) {
    if (m_GroupPrevious)
      m_GroupPrevious->m_GroupNext = m_GroupNext;
    else
      old->firstMember = m_GroupNext;
    if (m_GroupNext)
      m_GroupNext->m_GroupPrevious = m_GroupPrevious;
    --old->memberCount;
    if (old->Leader == this)
      old->Leader = nullptr;
  }
  m_pProcessGroup = group;
  m_GroupPrevious = m_GroupNext = nullptr;
  m_GroupMembership = NoGroup;
  if (group) {
    if (!group->sessionId)
      group->sessionId = m_SessionId;
    m_SessionId = group->sessionId;
    m_GroupNext = group->firstMember;
    if (m_GroupNext)
      m_GroupNext->m_GroupPrevious = this;
    group->firstMember = this;
    ++group->memberCount;
    m_GroupMembership =
        static_cast<size_t>(group->processGroupId) == getUserspaceId() ? Leader : Member;
    if (!group->registered)
      ProcessGroupManager::instance().registerGroup(group->processGroupId, group);
  }
  if (old && !old->memberCount)
    delete old;
}

void PosixProcess::inheritProcessGroup(PosixProcess* parent) {
  if (!parent)
    return;
  RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
  setProcessGroup(parent->m_pProcessGroup);
}

ProcessGroup* PosixProcess::getProcessGroup() const {
  return m_pProcessGroup;
}

bool PosixProcess::getProcessGroupId(size_t& id) const {
  RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
  if (!m_pProcessGroup)
    return false;
  id = m_pProcessGroup->processGroupId;
  return true;
}

void PosixProcess::leaveProcessGroup() {
  setProcessGroup(nullptr);
}

void PosixProcess::setGroupMembership(Membership type) {
  RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
  assert(type == m_GroupMembership);
}

PosixProcess::Membership PosixProcess::getGroupMembership() const {
  RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
  return m_GroupMembership;
}

size_t PosixProcess::getSessionId() const {
  RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
  return m_SessionId;
}

bool PosixProcess::sharesSession(const PosixProcess& other) const {
  RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
  return m_SessionId && m_SessionId == other.m_SessionId;
}

bool PosixProcess::jobControlReady() const {
  RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
  return m_SessionId && m_pProcessGroup;
}

void PosixProcess::markExecCommitted() {
  RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
  m_ExecCommitted = true;
}

bool PosixProcess::hasExecCommitted() const {
  RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
  return m_ExecCommitted;
}

int PosixProcess::createSession() {
  LockGuard<Mutex> terminalGuard(TerminalControl::lock());
  auto prepared = UniquePointer<ProcessGroup>::allocate();
  if (!prepared) {
    ERROR("PosixProcess::createSession - out of memory!");
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  int error = 0;
  {
    RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
    if (ProcessGroupManager::instance().findGroup(getUserspaceId())) {
      error = Error::NotEnoughPermissions;
    } else {
      prepared.get()->processGroupId = getUserspaceId();
      prepared.get()->sessionId = getUserspaceId();
      prepared.get()->Leader = this;
      setProcessGroup(prepared.releaseOwnership());
    }
  }
  if (!error)
    setCttyContext(SharedPointer<Process::ControllingTerminal>());
  syscallError(error);
  return error ? -1 : static_cast<int>(getUserspaceId());
}

int PosixProcess::changeProcessGroup(PosixProcess& caller, int id) {
  // Both the registry and memberships are intrusive: commit cannot allocate
  // while the group spinlock is held, or partially leave the original group.
  UniquePointer<ProcessGroup> prepared;
  if (static_cast<size_t>(id) == getUserspaceId())
    prepared = UniquePointer<ProcessGroup>::allocate();
  int error = 0;
  {
    RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
    ProcessGroup* existing = ProcessGroupManager::instance().findGroup(id);
    if (this != &caller && getParent() != &caller)
      error = Error::NoSuchProcess;
    else if (m_SessionId != caller.m_SessionId)
      error = Error::NotEnoughPermissions;
    else if (this != &caller && m_ExecCommitted)
      error = Error::PermissionDenied;
    else if (m_SessionId == getUserspaceId())
      error = Error::NotEnoughPermissions;
    else if (existing && existing->sessionId != m_SessionId)
      error = Error::NotEnoughPermissions;
    else if (existing)
      setProcessGroup(existing);
    else if (static_cast<size_t>(id) != getUserspaceId())
      error = Error::NotEnoughPermissions;
    else if (!prepared)
      error = Error::OutOfMemory;
    else {
      prepared.get()->processGroupId = id;
      prepared.get()->sessionId = m_SessionId;
      prepared.get()->Leader = this;
      setProcessGroup(prepared.releaseOwnership());
    }
  }
  syscallError(error);
  return error ? -1 : 0;
}
