/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include "fanotify-queue.h"
#include "fanotify-syscalls.h"
#include "file-handle-syscalls.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/system/vfs/File.h"

namespace {
constexpr size_t MaximumGroups = 64, MaximumMarks = 256, MaximumGlobalMarks = 4096;
size_t groups = 0, marks = 0;
constexpr FileEventMask Events = FileEvents::Modify | FileEvents::Attributes | FileEvents::Open |
                                 FileEvents::CloseWrite | FileEvents::CloseNoWrite;

bool charge(size_t& counter, size_t limit) {
  size_t value = __atomic_load_n(&counter, __ATOMIC_RELAXED);
  do {
    if (value >= limit)
      return false;
  } while (!__atomic_compare_exchange_n(&counter, &value, value + 1, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED));
  return true;
}

class FanotifyObserver final : public FileEventObserver {
 public:
  FanotifyObserver(const SharedPointer<FanotifyQueue>& queue, const FanotifyRecord& identity)
      : m_Queue(queue), m_Identity(identity) {}
  ~FanotifyObserver() override {
    LockGuard<Mutex> guard(m_Lock);
    releaseCharge();
  }
  bool arm(uint64_t mask) {
    LockGuard<Mutex> guard(m_Lock);
    if (m_Retired)
      return false;
    m_Mask = mask;
    m_Active = true;
    return true;
  }
  bool active() {
    LockGuard<Mutex> guard(m_Lock);
    return m_Active;
  }
  bool change(uint64_t mask, bool remove) {
    LockGuard<Mutex> guard(m_Lock);
    if (remove)
      m_Mask &= ~mask;
    else
      m_Mask |= mask;
    if (!m_Mask) {
      m_Active = false;
      releaseCharge();
    }
    return m_Active;
  }
  void deactivate() {
    LockGuard<Mutex> guard(m_Lock);
    m_Active = false;
    m_Retired = true;
    releaseCharge();
  }
  void fileEvent(const FileEvent& event) override {
    LockGuard<Mutex> guard(m_Lock);
    if (event.mask & FileEvents::SourceRetired) {
      m_Active = false;
      m_Retired = true;
      releaseCharge();
      return;
    }
    uint64_t selected = 0;
    if (event.mask & FileEvents::Modify)
      selected |= 0x02;
    if (event.mask & FileEvents::Attributes)
      selected |= 0x04;
    if (event.mask & FileEvents::CloseWrite)
      selected |= 0x08;
    if (event.mask & FileEvents::CloseNoWrite)
      selected |= 0x10;
    if (event.mask & FileEvents::Open)
      selected |= 0x20;
    selected &= m_Mask;
    if (!m_Active || !selected)
      return;
    FanotifyRecord record = m_Identity;
    record.mask = selected;
    record.producer = event.producerPid;
    m_Queue->enqueue(record);
  }

 private:
  void releaseCharge() {
    if (m_Charged) {
      m_Charged = false;
      __atomic_sub_fetch(&marks, 1, __ATOMIC_ACQ_REL);
    }
  }
  Mutex m_Lock;
  SharedPointer<FanotifyQueue> m_Queue;
  FanotifyRecord m_Identity;
  uint64_t m_Mask = 0;
  bool m_Active = false, m_Retired = false;
  bool m_Charged = true;
};

struct FanotifyMark {
  ~FanotifyMark() {
    inode.reset();
    mount.reset();
    if (charged)
      __atomic_sub_fetch(&marks, 1, __ATOMIC_ACQ_REL);
  }
  FanotifyRecord identity;
  VFS::MountIdentity mountIdentity;
  SharedPointer<FileEventObserver> observer;
  FanotifyObserver* concrete = nullptr;
  FileEventSubscription inode, mount;
  bool charged = false;
};
}  // namespace

class FanotifyState {
 public:
  Mutex lock;
  SharedPointer<FanotifyQueue> queue{new FanotifyQueue};
  UniquePointer<FanotifyMark> marks[MaximumMarks];
  size_t count = 0;
  bool closed = false;
};

FanotifyInstance::FanotifyInstance() : m_State(new FanotifyState) {}
FanotifyInstance::~FanotifyInstance() {
  lastDescriptorClosed();
  delete m_State;
}
SharedPointer<FanotifyInstance> FanotifyInstance::create() {
  if (!charge(groups, MaximumGroups)) {
    SYSCALL_ERROR(ProcessFileLimit);
    return {};
  }
  auto* instance = new FanotifyInstance;
  if (!instance || !instance->m_State || !instance->m_State->queue ||
      !instance->m_State->queue->valid()) {
    delete instance;
    __atomic_sub_fetch(&groups, 1, __ATOMIC_ACQ_REL);
    SYSCALL_ERROR(OutOfMemory);
    return {};
  }
  instance->m_Quota = true;
  return SharedPointer<FanotifyInstance>(instance);
}

void FanotifyInstance::reapMarks() {
  UniquePointer<FanotifyMark> retiring[MaximumMarks];
  size_t count = 0;
  LockGuard<Mutex> guard(m_State->lock);
  for (size_t index = 0; index < m_State->count;) {
    if (m_State->marks[index].get()->concrete->active()) {
      ++index;
      continue;
    }
    retiring[count++] = pedigree_std::move(m_State->marks[index]);
    --m_State->count;
    if (index != m_State->count)
      m_State->marks[index] = pedigree_std::move(m_State->marks[m_State->count]);
  }
}

int FanotifyInstance::changeMark(File& target, const VFS::MountOperation& mount,
                                 const FileHandle& handle, const FileSystemId& fsid, uint64_t mask,
                                 bool remove) {
  TerminationDeferral lifetime;
  reapMarks();
  FanotifyRecord identity;
  identity.mountId = mount.id();
  identity.handle = handle;
  identity.fsid = fsid;
  UniquePointer<FanotifyMark> retiring, candidate;
  LockGuard<Mutex> guard(m_State->lock);
  if (m_State->closed) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  for (size_t index = 0; index < m_State->count; ++index) {
    auto* mark = m_State->marks[index].get();
    if (!mark->identity.sameTarget(identity) || !mark->concrete->active())
      continue;
    if (!mark->concrete->change(mask, remove)) {
      retiring = pedigree_std::move(m_State->marks[index]);
      --m_State->count;
      if (index != m_State->count)
        m_State->marks[index] = pedigree_std::move(m_State->marks[m_State->count]);
    }
    return 0;
  }
  if (remove) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  if (m_State->count == MaximumMarks || !charge(marks, MaximumGlobalMarks)) {
    SYSCALL_ERROR(NoSpaceLeftOnDevice);
    return -1;
  }
  candidate = UniquePointer<FanotifyMark>::allocate();
  if (!candidate) {
    __atomic_sub_fetch(&marks, 1, __ATOMIC_ACQ_REL);
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  auto* mark = candidate.get();
  mark->charged = true;
  mark->identity = identity;
  mark->mountIdentity = mount.identity();
  mark->concrete = new FanotifyObserver(m_State->queue, identity);
  if (!mark->concrete) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  mark->observer.reset(mark->concrete);
  // Retirement releases quota immediately; only subscription drain and
  // storage reclamation need to wait for an ordinary group operation.
  mark->charged = false;
  const auto status =
      target.subscribeInodeEvents(Events | FileEvents::SourceRetired, mark->observer, mark->inode);
  if (status != FileHandleStatus::Success)
    return posix_handle_error(status);
  if (!mark->mountIdentity.subscribeRetirement(mark->observer, mark->mount)) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  if (!mark->concrete->arm(mask)) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  m_State->marks[m_State->count++] = pedigree_std::move(candidate);
  return 0;
}

int FanotifyInstance::flushMarks() {
  TerminationDeferral lifetime;
  UniquePointer<FanotifyMark> retiring[MaximumMarks];
  LockGuard<Mutex> guard(m_State->lock);
  if (m_State->closed) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  for (size_t index = 0; index < m_State->count; ++index) {
    m_State->marks[index].get()->concrete->deactivate();
    retiring[index] = pedigree_std::move(m_State->marks[index]);
  }
  m_State->count = 0;
  return 0;
}

void FanotifyInstance::lastDescriptorClosed() {
  TerminationDeferral lifetime;
  if (!m_State)
    return;
  UniquePointer<FanotifyMark> retiring[MaximumMarks];
  {
    LockGuard<Mutex> guard(m_State->lock);
    if (m_State->closed)
      return;
    m_State->closed = true;
    for (size_t index = 0; index < m_State->count; ++index) {
      m_State->marks[index].get()->concrete->deactivate();
      retiring[index] = pedigree_std::move(m_State->marks[index]);
    }
    m_State->count = 0;
    if (m_Quota) {
      m_Quota = false;
      __atomic_sub_fetch(&groups, 1, __ATOMIC_ACQ_REL);
    }
  }
  if (m_State->queue)
    m_State->queue->close();
}

ssize_t FanotifyInstance::readToUser(void* buffer, size_t count, bool canBlock) {
  struct Cursor {
    uintptr_t address;
  } cursor{reinterpret_cast<uintptr_t>(buffer)};
  auto copy = [](void* opaque, const void* bytes, size_t length) -> bool {
    auto* cursor = static_cast<Cursor*>(opaque);
    if (length > ~uintptr_t(0) - cursor->address ||
        !PosixSubsystem::copyToUser(reinterpret_cast<void*>(cursor->address), bytes, length))
      return false;
    cursor->address += length;
    return true;
  };
  return readWithCopy(count, canBlock, copy, &cursor);
}

ssize_t FanotifyInstance::readWithCopy(size_t count, bool canBlock, PosixDescriptorReadCopy copy,
                                       void* opaque) {
  TerminationDeferral lifetime;
  reapMarks();
  auto queue = m_State->queue;
  size_t copied = 0;
  for (;;) {
    FanotifyRecord record;
    const auto result = queue->take(record, count - copied, canBlock && !copied);
    if (result != FanotifyQueue::Take::Ready) {
      if (copied) {
        Processor::information().getCurrentThread()->setErrno(0);
        return copied;
      }
      syscallError(result == FanotifyQueue::Take::Empty         ? Error::NoMoreProcesses
                   : result == FanotifyQueue::Take::TooSmall    ? Error::InvalidArgument
                   : result == FanotifyQueue::Take::Interrupted ? Error::Interrupted
                                                                : Error::BadFileDescriptor);
      return -1;
    }
    uint8_t bytes[FanotifyQueue::MaximumRecordSize];
    record.encode(bytes);
    const size_t size = record.encodedSize();
    // Fanotify drops a dequeued notification after failed copyout. EFAULT
    // overrides even a previously copied prefix of this read/readv.
    if (!copy(opaque, bytes, size)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    copied += size;
  }
}

ReadyMask FanotifyInstance::queryReady() {
  return m_State->queue->queryReady();
}
ReadinessSource* FanotifyInstance::readinessSource() {
  return m_State->queue.get();
}
int FanotifyInstance::queuedMetadataBytes() {
  return m_State->queue->queuedMetadataBytes();
}
