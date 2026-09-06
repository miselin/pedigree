/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "uts-namespace.h"

namespace {
Mutex creationLock;
UtsRef initialNamespace;
size_t namespaceCount = 1;
uint64_t nextIdentity = 1;
constexpr size_t MaximumNamespaces = 256;
size_t localId(const Thread& thread) {
  return const_cast<Thread&>(thread).getId();
}
}  // namespace

class PosixUtsTaskBinding {
 public:
  explicit PosixUtsTaskBinding(const UtsRef& value) : space(value) {}
  UtsRef space;
  size_t localId = 0, taskId = 0;
  bool live = false;
};

class PosixUtsProcessView {
 public:
  Mutex lock;
  size_t processId = 0;
  bool attached = false, closed = false;
  PreparedUtsThread* first = nullptr;
  SharedPointer<PosixUtsTaskBinding> leader;
};

PosixUtsNamespace::PosixUtsNamespace(uint64_t identity, const Snapshot& names, bool charged)
    : m_Identity(identity), m_Charged(charged), m_Names(names) {}
PosixUtsNamespace::~PosixUtsNamespace() {
  if (m_Charged)
    __atomic_sub_fetch(&namespaceCount, size_t(1), __ATOMIC_RELEASE);
}
PosixUtsNamespace::Snapshot PosixUtsNamespace::snapshot() const {
  LockGuard<Mutex> guard(m_Lock);
  return m_Names;
}
void PosixUtsNamespace::setName(bool domain, const char* bytes, size_t length) {
  assert(length <= 64);
  LockGuard<Mutex> guard(m_Lock);
  char* output = domain ? m_Names.domain : m_Names.node;
  if (length)
    MemoryCopy(output, bytes, length);
  ByteSet(output + length, 0, 65 - length);
}

UtsStatus posix_uts_initial(UtsRef& result) {
  UtsRef acquired;
  {
    LockGuard<Mutex> guard(creationLock);
    if (!initialNamespace) {
      PosixUtsNamespace::Snapshot names;
      MemoryCopy(names.node, "pedigree", 8);
      MemoryCopy(names.domain, "(none)", 6);
      initialNamespace = SharedPointer<PosixUtsNamespace>::tryAllocate(uint64_t(1), names, false);
      if (!initialNamespace)
        return UtsStatus::NoMemory;
    }
    acquired = initialNamespace;
  }
  result = pedigree_std::move(acquired);
  return UtsStatus::Success;
}

UtsStatus posix_uts_copy(const UtsRef& source, UtsRef& result) {
  if (!source)
    return UtsStatus::Missing;
  const auto names = source->snapshot();
  uint64_t identity;
  {
    LockGuard<Mutex> guard(creationLock);
    if (__atomic_load_n(&namespaceCount, __ATOMIC_ACQUIRE) >= MaximumNamespaces ||
        nextIdentity == ~uint64_t(0))
      return UtsStatus::NoSpace;
    __atomic_add_fetch(&namespaceCount, size_t(1), __ATOMIC_ACQ_REL);
    identity = ++nextIdentity;
  }
  UtsRef copy = SharedPointer<PosixUtsNamespace>::tryAllocate(identity, names, true);
  if (!copy) {
    __atomic_sub_fetch(&namespaceCount, size_t(1), __ATOMIC_RELEASE);
    return UtsStatus::NoMemory;
  }
  result = pedigree_std::move(copy);
  return UtsStatus::Success;
}

PreparedUtsThread::PreparedUtsThread() = default;
PreparedUtsThread::~PreparedUtsThread() = default;
UtsStatus posix_uts_prepare_thread(const UtsRef& source, bool copy,
                                   UniquePointer<PreparedUtsThread>& result) {
  if (!source)
    return UtsStatus::Missing;
  UtsRef space = source;
  if (copy) {
    const auto status = posix_uts_copy(source, space);
    if (status != UtsStatus::Success)
      return status;
  }
  auto prepared = UniquePointer<PreparedUtsThread>::allocate();
  if (!prepared)
    return UtsStatus::NoMemory;
  prepared.get()->m_Binding = SharedPointer<PosixUtsTaskBinding>::tryAllocate(space);
  if (!prepared.get()->m_Binding)
    return UtsStatus::NoMemory;
  result = pedigree_std::move(prepared);
  return UtsStatus::Success;
}

PosixUtsTarget::PosixUtsTarget() = default;
PosixUtsTarget::PosixUtsTarget(const PosixUtsTarget&) = default;
PosixUtsTarget& PosixUtsTarget::operator=(const PosixUtsTarget&) = default;
PosixUtsTarget::~PosixUtsTarget() = default;
PosixUtsTarget::operator bool() const {
  return bool(m_View);
}

PosixNamespaceContext::PosixNamespaceContext()
    : m_View(SharedPointer<PosixUtsProcessView>::tryAllocate()) {}
PosixNamespaceContext::~PosixNamespaceContext() {
  close();
}
bool PosixNamespaceContext::valid() const {
  return bool(m_View);
}
void PosixNamespaceContext::attach(Process& process) {
  if (!m_View)
    return;
  LockGuard<Mutex> guard(m_View->lock);
  assert(!m_View->closed && (!m_View->attached || m_View->processId == process.getId()));
  m_View->processId = process.getId();
  m_View->attached = true;
}

bool PosixNamespaceContext::acquireThread(const Thread& thread, UtsRef& result) const {
  if (!m_View)
    return false;
  UtsRef acquired;
  {
    LockGuard<Mutex> guard(m_View->lock);
    if (m_View->closed || !m_View->attached || thread.getParent()->getId() != m_View->processId)
      return false;
    for (auto* node = m_View->first; node; node = node->m_Next) {
      if (node->m_Binding->localId == localId(thread) && node->m_Binding->live) {
        acquired = node->m_Binding->space;
        break;
      }
    }
  }
  if (!acquired)
    return false;
  result = pedigree_std::move(acquired);
  return true;
}

void PosixNamespaceContext::publishThread(UniquePointer<PreparedUtsThread>& prepared,
                                          Thread& thread, bool leader) {
  assert(m_View && prepared && prepared.get()->m_Binding);
  SharedPointer<PosixUtsTaskBinding> oldLeader;
  {
    LockGuard<Mutex> guard(m_View->lock);
    assert(m_View->attached && !m_View->closed && thread.getParent()->getId() == m_View->processId);
    // An unstarted task can finish its exit hook before this publication.
    // Its terminal marker precedes that hook and prevents binding resurrection.
    if (thread.getUnwindState() == Thread::TerminateThread)
      return;
    for (auto* node = m_View->first; node; node = node->m_Next)
      assert(node->m_Binding->localId != localId(thread));
    prepared.get()->m_Binding->localId = localId(thread);
    prepared.get()->m_Binding->taskId = thread.getTaskId();
    prepared.get()->m_Binding->live = true;
    prepared.get()->m_Next = m_View->first;
    if (leader) {
      oldLeader = pedigree_std::move(m_View->leader);
      m_View->leader = prepared.get()->m_Binding;
    }
    m_View->first = prepared.releaseOwnership();
  }
}

void PosixNamespaceContext::promoteExec(const Thread& thread) {
  assert(bool(m_View));
  SharedPointer<PosixUtsTaskBinding> oldLeader;
  LockGuard<Mutex> guard(m_View->lock);
  for (auto* node = m_View->first; node; node = node->m_Next) {
    if (node->m_Binding->localId == localId(thread) && node->m_Binding->live) {
      node->m_Binding->taskId = thread.getTaskId();
      oldLeader = pedigree_std::move(m_View->leader);
      m_View->leader = node->m_Binding;
      return;
    }
  }
  assert(false);
}

UtsStatus PosixNamespaceContext::replaceThread(const Thread& thread, const UtsRef& replacement) {
  if (!m_View || !replacement)
    return UtsStatus::Missing;
  UtsRef previous;
  LockGuard<Mutex> guard(m_View->lock);
  if (m_View->closed)
    return UtsStatus::Missing;
  for (auto* node = m_View->first; node; node = node->m_Next) {
    if (node->m_Binding->localId == localId(thread) && node->m_Binding->live) {
      previous = pedigree_std::move(node->m_Binding->space);
      node->m_Binding->space = replacement;
      return UtsStatus::Success;
    }
  }
  return UtsStatus::Missing;
}

void PosixNamespaceContext::retireThread(const Thread& thread) {
  if (!m_View || __atomic_load_n(&m_View->closed, __ATOMIC_ACQUIRE))
    return;
  PreparedUtsThread* retired = nullptr;
  UtsRef previous;
  SharedPointer<PosixUtsTaskBinding> oldLeader;
  {
    LockGuard<Mutex> guard(m_View->lock);
    auto** link = &m_View->first;
    while (*link) {
      if ((*link)->m_Binding->localId == localId(thread)) {
        retired = *link;
        *link = retired->m_Next;
        retired->m_Next = nullptr;
        retired->m_Binding->live = false;
        previous = pedigree_std::move(retired->m_Binding->space);
        if (m_View->leader == retired->m_Binding)
          oldLeader = pedigree_std::move(m_View->leader);
        break;
      }
      link = &(*link)->m_Next;
    }
  }
  delete retired;
}

void PosixNamespaceContext::close() {
  if (!m_View || __atomic_load_n(&m_View->closed, __ATOMIC_ACQUIRE))
    return;
  PreparedUtsThread* retired;
  SharedPointer<PosixUtsTaskBinding> oldLeader;
  {
    LockGuard<Mutex> guard(m_View->lock);
    __atomic_store_n(&m_View->closed, true, __ATOMIC_RELEASE);
    retired = m_View->first;
    m_View->first = nullptr;
    oldLeader = pedigree_std::move(m_View->leader);
    for (auto* node = retired; node; node = node->m_Next)
      node->m_Binding->live = false;
  }
  while (retired) {
    auto* next = retired->m_Next;
    retired->m_Binding->space.reset();
    delete retired;
    retired = next;
  }
}

bool PosixNamespaceContext::leaderTarget(PosixUtsTarget& result) const {
  PosixUtsTarget target;
  if (!m_View)
    return false;
  {
    LockGuard<Mutex> guard(m_View->lock);
    if (m_View->closed || !m_View->attached)
      return false;
    target.m_View = m_View;
  }
  result = target;
  return true;
}

bool PosixNamespaceContext::taskTarget(size_t taskId, PosixUtsTarget& result) const {
  PosixUtsTarget target;
  if (!m_View)
    return false;
  {
    LockGuard<Mutex> guard(m_View->lock);
    if (m_View->closed || !m_View->attached)
      return false;
    for (auto* node = m_View->first; node; node = node->m_Next) {
      if (node->m_Binding->live && node->m_Binding->taskId == taskId) {
        target.m_View = m_View;
        target.m_Task = node->m_Binding;
        target.m_ExpectedTaskId = taskId;
        break;
      }
    }
  }
  if (!target)
    return false;
  result = target;
  return true;
}

bool PosixNamespaceContext::threadTarget(const Thread& thread, PosixUtsTarget& result) const {
  return taskTarget(thread.getTaskId(), result);
}

bool PosixNamespaceContext::nextTaskTarget(size_t afterTaskId, size_t& taskId,
                                           PosixUtsTarget& result) const {
  PosixUtsTarget target;
  if (!m_View)
    return false;
  {
    LockGuard<Mutex> guard(m_View->lock);
    if (m_View->closed || !m_View->attached)
      return false;
    for (auto* node = m_View->first; node; node = node->m_Next) {
      const size_t id = node->m_Binding->taskId;
      if (node->m_Binding->live && id > afterTaskId &&
          (!target.m_Task || id < target.m_ExpectedTaskId)) {
        target.m_View = m_View;
        target.m_Task = node->m_Binding;
        target.m_ExpectedTaskId = id;
      }
    }
  }
  if (!target)
    return false;
  taskId = target.m_ExpectedTaskId;
  result = target;
  return true;
}

UtsStatus posix_uts_acquire_target(const PosixUtsTarget& target, UtsRef& result) {
  if (!target.m_View)
    return UtsStatus::Missing;
  Thread* current = Processor::information().getCurrentThread();
  Scheduler::ProcessLease lease;
  if (!Scheduler::instance().acquireProcessById(lease, target.m_View->processId))
    return UtsStatus::Missing;
  if (lease->getType() != Process::Posix || current->getParent()->getType() != Process::Posix)
    return UtsStatus::Denied;
  UtsRef acquired;
  {
    // Credential mutation and exec share this gate; no target address-space
    // switch or image ownership is needed to read its namespace metadata.
    MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
    auto* subsystem = static_cast<PosixSubsystem*>(lease->getSubsystem());
    auto context =
        subsystem ? subsystem->namespaceContext() : SharedPointer<PosixNamespaceContext>();
    if (!context || context->m_View != target.m_View)
      return UtsStatus::Missing;
    if (current->getParent() != lease.get()) {
      FilesystemCredentials source;
      const auto destination = static_cast<PosixProcess*>(lease.get())->snapshotCredentials();
      if (!Process::currentFilesystemCredentials(source) || !destination.dumpable ||
          source.uid != destination.ruid || source.uid != destination.euid ||
          source.uid != destination.suid || source.gid != destination.rgid ||
          source.gid != destination.egid || source.gid != destination.sgid)
        return UtsStatus::Denied;
    }
    LockGuard<Mutex> guard(target.m_View->lock);
    auto binding = target.m_Task ? target.m_Task : target.m_View->leader;
    if (target.m_View->closed || !binding || !binding->live ||
        (target.m_Task && binding->taskId != target.m_ExpectedTaskId))
      return UtsStatus::Missing;
    acquired = binding->space;
  }
  if (!acquired)
    return UtsStatus::Missing;
  result = pedigree_std::move(acquired);
  return UtsStatus::Success;
}

int posix_uts_error(UtsStatus status) {
  switch (status) {
    case UtsStatus::Success:
      Processor::information().getCurrentThread()->setErrno(0);
      return 0;
    case UtsStatus::NoMemory:
      SYSCALL_ERROR(OutOfMemory);
      break;
    case UtsStatus::NoSpace:
      SYSCALL_ERROR(NoSpaceLeftOnDevice);
      break;
    case UtsStatus::Missing:
      SYSCALL_ERROR(DoesNotExist);
      break;
    case UtsStatus::Denied:
      SYSCALL_ERROR(PermissionDenied);
      break;
    case UtsStatus::Invalid:
      SYSCALL_ERROR(InvalidArgument);
      break;
  }
  return -1;
}
