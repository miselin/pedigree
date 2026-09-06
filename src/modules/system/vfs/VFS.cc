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

#include "VFS.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Directory.h"
#include "File.h"

#ifndef VFS_STANDALONE
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#include "modules/Module.h"
#endif

class Disk;

class VfsMountState {
 public:
  VfsMountState(Filesystem* filesystem, uint32_t id) : filesystem(filesystem), id(id) {}
  ~VfsMountState() {
    storagePins.closeAndWait();
    operations.closeAndWait();
  }

  static uint32_t reserveId() {
    static uint32_t next = 1;
    uint32_t candidate = __atomic_load_n(&next, __ATOMIC_RELAXED);
    while (candidate <= 0x7fffffffU) {
      if (__atomic_compare_exchange_n(&next, &candidate, candidate + 1, false, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED))
        return candidate;
    }
    return 0;
  }

  void retire() {
    storagePins.wait();
    operations.wait();
    filesystem = nullptr;
    retirement.beginRetirement();
    retirement.finishRetirement();
  }

  Filesystem* filesystem;
  const uint32_t id;
  OperationBarrier operations;
  OperationBarrier storagePins;
  InodeEventSource retirement;
};

class VfsFilesystemPin {
 public:
  // Release admission before the state that owns its barrier. Copies of the
  // outer pin may survive registry shutdown and do not need fresh admission.
  SharedPointer<VfsMountState> state;
  OperationBarrier::Lease admission;
};

VFS::FilesystemPin::FilesystemPin() = default;
VFS::FilesystemPin::FilesystemPin(const FilesystemPin& other) = default;
VFS::FilesystemPin::FilesystemPin(FilesystemPin&& other) noexcept = default;
VFS::FilesystemPin::~FilesystemPin() = default;
VFS::FilesystemPin& VFS::FilesystemPin::operator=(const FilesystemPin& other) = default;
VFS::FilesystemPin& VFS::FilesystemPin::operator=(FilesystemPin&& other) noexcept = default;

Filesystem* VFS::FilesystemPin::filesystem() const {
  return m_Pin ? m_Pin->state->filesystem : nullptr;
}

VFS::MountIdentity VFS::FilesystemPin::identity() const {
  MountIdentity identity;
  if (m_Pin)
    identity.m_State = m_Pin->state;
  return identity;
}

VFS::FilesystemPin::operator bool() const {
  return static_cast<bool>(m_Pin);
}

void VFS::FilesystemPin::reset() {
  m_Pin.reset();
}

bool VFS::MountIdentity::pin(FilesystemPin& pin) const {
  pin.reset();
  auto retained = SharedPointer<VfsFilesystemPin>::tryAllocate();
  if (!retained || !m_State || !m_State->storagePins.tryAcquire(retained->admission))
    return false;
  retained->state = m_State;
  pin.m_Pin = pedigree_std::move(retained);
  return true;
}

bool VFS::pinFilesystem(Filesystem* key, FilesystemPin& pin) const {
  pin.reset();
  auto retained = SharedPointer<VfsFilesystemPin>::tryAllocate();
  if (!retained)
    return false;
  {
    LockGuard<Mutex> guard(m_MountTableLock);
    MountInfo* info = m_Mounts.lookup(key);
    if (!info || !info->state->storagePins.tryAcquire(retained->admission))
      return false;
    retained->state = info->state;
  }
  pin.m_Pin = pedigree_std::move(retained);
  return true;
}

VFS::MountIdentity::MountIdentity() = default;
VFS::MountIdentity::MountIdentity(const MountIdentity& other) = default;
VFS::MountIdentity::MountIdentity(MountIdentity&& other) noexcept = default;
VFS::MountIdentity::~MountIdentity() = default;
VFS::MountIdentity& VFS::MountIdentity::operator=(const MountIdentity& other) = default;
VFS::MountIdentity& VFS::MountIdentity::operator=(MountIdentity&& other) noexcept = default;

VFS::MountInfo::MountInfo(const String& stableName, const String& path,
                          const SharedPointer<VfsMountState>& state)
    : stableName(stableName), path(path), state(state) {}
VFS::MountInfo::~MountInfo() = default;

uint32_t VFS::MountIdentity::id() const {
  return m_State ? m_State->id : 0;
}

VFS::MountIdentity::operator bool() const {
  return static_cast<bool>(m_State);
}

bool VFS::MountIdentity::acquire(MountOperation& operation) const {
  operation.reset();
  if (!m_State || !m_State->operations.tryAcquire(operation.m_Admission))
    return false;
  operation.m_State = m_State;
  return true;
}

bool VFS::MountIdentity::subscribeRetirement(const SharedPointer<FileEventObserver>& observer,
                                             FileEventSubscription& subscription) const {
  if (!m_State) {
    subscription.reset();
    return false;
  }
  return m_State->retirement.subscribeFileEvents(FileEvents::SourceRetired, observer, subscription);
}

VFS::MountOperation::MountOperation() = default;

VFS::MountOperation::MountOperation(MountOperation&& other) noexcept
    : m_State(pedigree_std::move(other.m_State)),
      m_Admission(pedigree_std::move(other.m_Admission)) {}

VFS::MountOperation::~MountOperation() {
  reset();
}

VFS::MountOperation& VFS::MountOperation::operator=(MountOperation&& other) noexcept {
  if (this != &other) {
    reset();
    m_State = pedigree_std::move(other.m_State);
    m_Admission = pedigree_std::move(other.m_Admission);
  }
  return *this;
}

Filesystem* VFS::MountOperation::filesystem() const {
  return m_State ? m_State->filesystem : nullptr;
}

uint32_t VFS::MountOperation::id() const {
  return m_State ? m_State->id : 0;
}

VFS::MountIdentity VFS::MountOperation::identity() const {
  MountIdentity identity;
  identity.m_State = m_State;
  return identity;
}

VFS::MountOperation::operator bool() const {
  return static_cast<bool>(m_Admission);
}

void VFS::MountOperation::reset() {
  m_Admission = OperationBarrier::Lease();
  m_State.reset();
}

bool VFS::acquireMount(Filesystem* key, MountOperation& operation) const {
  operation.reset();
  LockGuard<Mutex> guard(m_MountTableLock);
  MountInfo* info = m_Mounts.lookup(key);
  if (!info || !info->state->operations.tryAcquire(operation.m_Admission))
    return false;
  operation.m_State = info->state;
  return true;
}

/// \todo Figure out a way to clean up files after deletion. Directory::remove()
///       is not the right place to do this. There needs to be a way to add a
///       File to some sort of queue that cleans it up once it hits refcount
///       zero or something like that.

VFS VFS::m_Instance;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
VFS::RetainTrackedFileHook VFS::m_RetainTrackedFileHook = nullptr;
#endif

VFS& VFS::instance() {
  return m_Instance;
}

VFS::VFS()
    : m_MountMutationLock(),
      m_MountTableLock(),
      m_pRootFilesystem(nullptr),
      m_Mounts(),
      m_ProbeCallbacks(),
      m_MountCallbacks()
#if THREADS
      ,
      m_CallbackLock(),
      m_NextCallbackSequence(1),
      m_pActiveCallbacks(nullptr),
      m_CallbacksClosing(false)
#endif
{
}

VFS::~VFS() {
#if THREADS
  TerminationDeferral teardownDeferral;
#endif
#if THREADS
  m_CallbackLock.acquire();
  m_CallbacksClosing = true;
  if (m_pActiveCallbacks) {
    m_CallbackLock.release();
    FATAL("VFS destroyed with an active callback invocation");
  }
  for (auto item : m_ProbeCallbacks) {
    if (item->state.inFlight || item->state.removers) {
      m_CallbackLock.release();
      FATAL("VFS destroyed before probe callback retirement completed");
    }
  }
  for (auto item : m_MountCallbacks) {
    if (item->state.inFlight || item->state.removers) {
      m_CallbackLock.release();
      FATAL("VFS destroyed before mount callback retirement completed");
    }
  }
  m_CallbackLock.release();
#endif

  // Wipe out probe callbacks we know about.
  for (auto it = m_ProbeCallbacks.begin(); it != m_ProbeCallbacks.end(); ++it) {
    delete *it;
  }
  for (auto it = m_MountCallbacks.begin(); it != m_MountCallbacks.end(); ++it) {
    delete *it;
  }

  Vector<MountInfo*> mountInfo;
  Vector<Filesystem*> filesystems;
  {
    LockGuard<Mutex> mutationGuard(m_MountMutationLock);
    LockGuard<Mutex> tableGuard(m_MountTableLock);
    for (auto it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
      it.value()->state->storagePins.close();
      it.value()->state->operations.close();
      mountInfo.pushBack(it.value());
      filesystems.pushBack(it.key());
    }
    m_Mounts.clear();
    m_pRootFilesystem = nullptr;
  }

  // Filesystem destructors can re-enter VFS, so publication locks must no
  // longer be held when ownership is released.
  for (auto info : mountInfo) {
    info->state->retire();
    delete info;
  }
  for (auto filesystem : filesystems) {
    delete filesystem;
  }
}

bool VFS::mount(Disk* pDisk, String& stableName, Filesystem** pMountedFs) {
#if THREADS
  TerminationDeferral dispatchDeferral;
#endif
  DiskUse diskUse;
  if (pDisk && !pDisk->acquireUse(diskUse))
    return false;
#if THREADS
  Thread* current = Processor::information().getCurrentThread();
  void* owner =
      current ? static_cast<void*>(current) : static_cast<void*>(&Processor::information());
  size_t boundary = 0;
  m_CallbackLock.acquire();
  boundary = m_NextCallbackSequence;
  m_CallbackLock.release();

  size_t afterSequence = 0;
  while (true) {
    ActiveInvocation invocation = {nullptr, owner, nullptr};
    ProbeCallbackItem* item = acquireProbeCallback(afterSequence, boundary, invocation);
    if (!item) {
      break;
    }

    Filesystem* pFs = item->callback(pDisk);
    if (pFs) {
      m_CallbackLock.acquire();
      // This is the publication commit point. Retirement which closes the
      // entry first rejects the result while the provider remains pinned;
      // commit which wins keeps the pin through every use below.
      const bool committed = item->state.enabled && !m_CallbacksClosing;
      m_CallbackLock.release();
      if (!committed) {
        // The provider must still be pinned while its rejected result is
        // destroyed; removal can only return after finishCallback below.
        delete pFs;
        finishCallback(&item->state, invocation);
        continue;
      }

      pFs->m_DiskUse = pedigree_std::move(diskUse);

      if (stableName.length() == 0) {
        stableName = pFs->getVolumeLabel();
      }
      stableName = registerFilesystem(pFs, stableName);
      if (!stableName.length()) {
        delete pFs;
        finishCallback(&item->state, invocation);
        return false;
      }
      dispatchMountCallbacks(owner);

      if (pMountedFs) {
        *pMountedFs = pFs;
      }

      NOTICE("mounted filesystem '" << stableName << "'");

      finishCallback(&item->state, invocation);
      return true;
    }

    finishCallback(&item->state, invocation);
  }
#else
  for (List<ProbeCallbackItem*>::Iterator it = m_ProbeCallbacks.begin();
       it != m_ProbeCallbacks.end(); it++) {
    Filesystem* pFs = (*it)->callback(pDisk);
    if (pFs) {
      pFs->m_DiskUse = pedigree_std::move(diskUse);

      if (stableName.length() == 0) {
        stableName = pFs->getVolumeLabel();
      }
      stableName = registerFilesystem(pFs, stableName);

      if (!stableName.length()) {
        delete pFs;
        return false;
      }

      for (List<MountCallbackItem*>::Iterator it2 = m_MountCallbacks.begin();
           it2 != m_MountCallbacks.end(); it2++) {
        (*it2)->callback();
      }

      if (pMountedFs) {
        *pMountedFs = pFs;
      }

      NOTICE("mounted filesystem '" << stableName << "'");

      return true;
    }
  }
#endif
  return false;
}

String VFS::registerFilesystem(Filesystem* pFs, const String& preferredStableName) {
  LockGuard<Mutex> mutationGuard(m_MountMutationLock);
  return registerFilesystemLocked(pFs, preferredStableName);
}

String VFS::registerFilesystemLocked(Filesystem* pFs, const String& preferredStableName) {
  if (!pFs) {
    return String();
  }
  if (pFs->m_pDisk && !pFs->m_DiskUse && !pFs->m_pDisk->acquireUse(pFs->m_DiskUse))
    return String();

  MountInfo* info = nullptr;
  Filesystem* root = nullptr;
  {
    LockGuard<Mutex> tableGuard(m_MountTableLock);
    MountInfo* existing = m_Mounts.lookup(pFs);
    if (existing) {
      return existing->stableName;
    }

    String stableName = getUniqueStableNameLocked(preferredStableName);
    NormalStaticString path;
    path += "/media/";
    path += stableName;
    const uint32_t id = VfsMountState::reserveId();
    if (!id)
      return String();
    SharedPointer<VfsMountState> state(new VfsMountState(pFs, id));
    if (!state)
      return String();
    info = new MountInfo(stableName, String(path), state);
    if (!info || !m_Mounts.tryInsert(pFs, info)) {
      delete info;
      return String();
    }
    root = m_pRootFilesystem;
  }

  if (root) {
    attachFilesystem(root, pFs, info->path);
  }

  return info->stableName;
}

bool VFS::unregisterFilesystem(Filesystem* pFs, bool canDelete) {
#if THREADS
  TerminationDeferral teardownDeferral;
#endif
  if (!pFs) {
    return false;
  }

  MountInfo* info = nullptr;
  bool detach = false;
  {
    LockGuard<Mutex> mutationGuard(m_MountMutationLock);
    {
      LockGuard<Mutex> tableGuard(m_MountTableLock);
      info = m_Mounts.lookup(pFs);
      if (!info || !info->state->storagePins.tryCloseIfIdle()) {
        return false;
      }
      m_Mounts.take(pFs, info);
      info->state->operations.close();

      if (pFs == m_pRootFilesystem) {
        m_pRootFilesystem = nullptr;
      } else {
        detach = m_pRootFilesystem != nullptr;
      }
    }

    if (detach) {
      Directory::ChildLease pointLease;
      File* point = findRetained(info->path, pointLease);
      if (point && point->isDirectory()) {
        Directory::fromFile(point)->setReparsePoint(nullptr);
      }
    }
  }

  info->state->retire();
  delete info;
  if (canDelete) {
    delete pFs;
  }
  return true;
}

bool VFS::setRootFilesystem(Filesystem* pFs) {
  LockGuard<Mutex> mutationGuard(m_MountMutationLock);
  if (pFs) {
    bool registered = false;
    {
      LockGuard<Mutex> tableGuard(m_MountTableLock);
      registered = m_Mounts.lookup(pFs) != nullptr;
    }
    if (!registered) {
      if (!registerFilesystemLocked(pFs, pFs->getVolumeLabel()).length())
        return false;
    }
  }

  {
    LockGuard<Mutex> tableGuard(m_MountTableLock);
    m_pRootFilesystem = pFs;
  }
  attachRegisteredFilesystemsLocked();
  return true;
}

Filesystem* VFS::getRootFilesystem() const {
  LockGuard<Mutex> tableGuard(m_MountTableLock);
  return m_pRootFilesystem;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
Filesystem* VFS::swapRootFilesystemForHostedTest(Filesystem* pFs) {
  LockGuard<Mutex> mutationGuard(m_MountMutationLock);
  LockGuard<Mutex> tableGuard(m_MountTableLock);
  Filesystem* previous = m_pRootFilesystem;
  m_pRootFilesystem = pFs;
  return previous;
}
#endif

bool VFS::getMountPath(Filesystem* pFs, String& path) const {
  LockGuard<Mutex> tableGuard(m_MountTableLock);
  MountInfo* info = m_Mounts.lookup(pFs);
  if (!info) {
    return false;
  }

  path = info->path;
  return true;
}

Filesystem* VFS::getFilesystemAt(const String& path) const {
  LockGuard<Mutex> tableGuard(m_MountTableLock);
  for (MountTable::Iterator it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
    if (it.value()->path == path) {
      return it.key();
    }
  }

  return nullptr;
}

void VFS::getMounts(Vector<MountSnapshot>& mounts) const {
  struct MountSnapshotSource {
    MountSnapshotSource() : filesystem(nullptr) {}
    MountSnapshotSource(Filesystem* filesystem, const String& stableName, const String& path)
        : filesystem(filesystem), stableName(stableName), path(path) {}

    Filesystem* filesystem;
    String stableName;
    String path;
  };

  mounts.clear();
  Vector<MountSnapshotSource> sources;
  LockGuard<Mutex> mutationGuard(m_MountMutationLock);
  {
    LockGuard<Mutex> tableGuard(m_MountTableLock);
    for (MountTable::Iterator it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
      sources.createBack(it.key(), it.value()->stableName, it.value()->path);
    }
  }

  for (const auto& source : sources) {
    bool hasDisk = false;
    String diskParentName;
    String diskName;
    Disk* disk = source.filesystem->getDisk();
    if (disk) {
      hasDisk = true;
      disk->getName(diskName);
      if (disk->getParent()) {
        disk->getParent()->getName(diskParentName);
      }
    }
    mounts.createBack(source.stableName, source.path, hasDisk, diskParentName, diskName);
  }
}

namespace {
Filesystem::SyncStatus syncPinnedFilesystem(Filesystem* filesystem) {
#if !defined(VFS_STANDALONE) && THREADS
  Thread* thread = Processor::information().getCurrentThread();
  const int previousError = thread ? thread->getErrno() : 0;
  if (thread)
    thread->setErrno(0);
#endif
  const auto result = filesystem->sync();
#if !defined(VFS_STANDALONE) && THREADS
  if (thread)
    thread->setErrno(previousError);
#endif
  return result;
}
}  // namespace

Filesystem::SyncStatus VFS::syncFilesystem(Filesystem* key) {
#if !defined(VFS_STANDALONE) && THREADS
  TerminationDeferral lifetime;
#endif
  if (!key)
    return Filesystem::SyncStatus::Unsupported;
  auto pin = SharedPointer<VfsFilesystemPin>::tryAllocate();
  if (!pin)
    return Filesystem::SyncStatus::NoMemory;
  {
    LockGuard<Mutex> guard(m_MountTableLock);
    MountInfo* info = m_Mounts.lookup(key);
    if (!info)
      return Filesystem::SyncStatus::Unsupported;
    if (!info->state->storagePins.tryAcquire(pin->admission))
      return Filesystem::SyncStatus::IoError;
    pin->state = info->state;
  }
  return syncPinnedFilesystem(pin->state->filesystem);
}

Filesystem::SyncStatus VFS::syncAll() {
#if !defined(VFS_STANDALONE) && THREADS
  TerminationDeferral lifetime;
#endif
  struct Snapshot {
    ~Snapshot() {
      delete[] pins;
    }
    VfsFilesystemPin* pins = nullptr;
    size_t count = 0;
  } snapshot;
  for (;;) {
    size_t capacity;
    {
      LockGuard<Mutex> guard(m_MountTableLock);
      capacity = m_Mounts.count();
    }
    if (capacity > ~size_t(0) / sizeof(VfsFilesystemPin)) {
      ERROR("VFS::syncAll: filesystem snapshot is too large");
      return Filesystem::SyncStatus::NoMemory;
    }
    delete[] snapshot.pins;
    snapshot.pins = capacity ? new VfsFilesystemPin[capacity] : nullptr;
    if (capacity && !snapshot.pins) {
      ERROR("VFS::syncAll: cannot allocate filesystem snapshot");
      return Filesystem::SyncStatus::NoMemory;
    }
    {
      LockGuard<Mutex> guard(m_MountTableLock);
      // Registration may have grown the table while allocation was unlocked.
      if (m_Mounts.count() > capacity)
        continue;
      for (auto it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
        auto& pin = snapshot.pins[snapshot.count++];
        pin.state = it.value()->state;
        const bool admitted = pin.state->storagePins.tryAcquire(pin.admission);
        (void)admitted;
      }
    }
    break;
  }

  auto firstError = Filesystem::SyncStatus::Success;
  for (size_t n = 0; n < snapshot.count; ++n) {
    auto& pin = snapshot.pins[n];
    const auto status = pin.admission ? syncPinnedFilesystem(pin.state->filesystem)
                                      : Filesystem::SyncStatus::IoError;
    if (status != Filesystem::SyncStatus::Success) {
      const char* reason = status == Filesystem::SyncStatus::Unsupported ? "unsupported"
                           : status == Filesystem::SyncStatus::NoMemory  ? "out of memory"
                                                                         : "I/O error";
      ERROR("VFS::syncAll: filesystem " << Dec << pin.state->id << " sync failed: " << reason);
      if (firstError == Filesystem::SyncStatus::Success)
        firstError = status;
    }
  }
  return firstError;
}

File* VFS::find(const String& path, File* pStartNode) {
  // NOTICE("find: " << path);

  File* pResult = 0;

  pStartNode = resolveStartNode(path, pStartNode);
  if (pStartNode) {
    pResult = pStartNode->getFilesystem()->find(path.view(), pStartNode);
  }

  // NOTICE("find: " << path << " -> " << pResult);
  return pResult;
}

File* VFS::findRetained(const String& path, Directory::ChildLease& result, File* pStartNode) {
  pStartNode = resolveStartNode(path, pStartNode);
  if (!pStartNode) {
    return nullptr;
  }
  return pStartNode->getFilesystem()->findRetained(path.view(), result, pStartNode);
}

void VFS::addProbeCallback(Filesystem::ProbeCallback callback) {
  if (!callback) {
    FATAL("VFS cannot register a null probe callback");
  }

  ProbeCallbackItem* item = new ProbeCallbackItem(callback);
#if THREADS
  m_CallbackLock.acquire();
  if (m_CallbacksClosing) {
    m_CallbackLock.release();
    delete item;
    FATAL("VFS probe callback registered during teardown");
  }
  for (auto registered : m_ProbeCallbacks) {
    if (registered->callback == callback) {
      if (!registered->state.draining) {
        registered->state.enabled = true;
      }
      m_CallbackLock.release();
      delete item;
      return;
    }
  }
  if (m_NextCallbackSequence == static_cast<size_t>(-1)) {
    m_CallbackLock.release();
    delete item;
    FATAL("VFS callback sequence exhausted");
  }
  item->state.sequence = m_NextCallbackSequence++;
  item->state.debugAddress = reinterpret_cast<uintptr_t>(callback);
  m_ProbeCallbacks.pushBack(item);
  m_CallbackLock.release();
#else
  for (auto registered : m_ProbeCallbacks) {
    if (registered->callback == callback) {
      delete item;
      return;
    }
  }
  m_ProbeCallbacks.pushBack(item);
#endif
}

bool VFS::removeProbeCallback(Filesystem::ProbeCallback callback) {
#if THREADS
  if (!callback) {
    return false;
  }

  TerminationDeferral removalDeferral;
  Thread* current = Processor::information().getCurrentThread();
  const bool canYield =
      current && Processor::executionContext() == ExecutionContext::WaitableThread;
  void* owner =
      current ? static_cast<void*>(current) : static_cast<void*>(&Processor::information());
  ProbeCallbackItem* item = nullptr;
  bool callbackContext = false;

  m_CallbackLock.acquire();
  if (m_CallbacksClosing) {
    m_CallbackLock.release();
    return false;
  }
  for (auto it = m_ProbeCallbacks.begin(); it != m_ProbeCallbacks.end(); ++it) {
    if ((*it)->callback != callback) {
      continue;
    }

    item = *it;
    item->state.enabled = false;
    callbackContext = isCallbackInvocation(owner);
    if (!callbackContext && canYield) {
      item->state.draining = true;
      ++item->state.removers;
    }
    break;
  }
  m_CallbackLock.release();

  if (!item) {
    return false;
  }
  if (callbackContext || !canYield) {
    return false;
  }

  drainProbeCallback(item);
  return true;
#else
  for (auto it = m_ProbeCallbacks.begin(); it != m_ProbeCallbacks.end(); ++it) {
    ProbeCallbackItem* item = *it;
    if (item->callback == callback) {
      m_ProbeCallbacks.erase(it);
      delete item;
      return true;
    }
  }
  return false;
#endif
}

void VFS::addMountCallback(MountCallback callback) {
  if (!callback) {
    FATAL("VFS cannot register a null mount callback");
  }

  MountCallbackItem* item = new MountCallbackItem(callback);
#if THREADS
  m_CallbackLock.acquire();
  if (m_CallbacksClosing) {
    m_CallbackLock.release();
    delete item;
    FATAL("VFS mount callback registered during teardown");
  }
  for (auto registered : m_MountCallbacks) {
    if (registered->callback == callback) {
      if (!registered->state.draining) {
        registered->state.enabled = true;
      }
      m_CallbackLock.release();
      delete item;
      return;
    }
  }
  if (m_NextCallbackSequence == static_cast<size_t>(-1)) {
    m_CallbackLock.release();
    delete item;
    FATAL("VFS callback sequence exhausted");
  }
  item->state.sequence = m_NextCallbackSequence++;
  item->state.debugAddress = reinterpret_cast<uintptr_t>(callback);
  m_MountCallbacks.pushBack(item);
  m_CallbackLock.release();
#else
  for (auto registered : m_MountCallbacks) {
    if (registered->callback == callback) {
      delete item;
      return;
    }
  }
  m_MountCallbacks.pushBack(item);
#endif
}

bool VFS::removeMountCallback(MountCallback callback) {
#if THREADS
  if (!callback) {
    return false;
  }

  TerminationDeferral removalDeferral;
  Thread* current = Processor::information().getCurrentThread();
  const bool canYield =
      current && Processor::executionContext() == ExecutionContext::WaitableThread;
  void* owner =
      current ? static_cast<void*>(current) : static_cast<void*>(&Processor::information());
  MountCallbackItem* item = nullptr;
  bool callbackContext = false;

  m_CallbackLock.acquire();
  if (m_CallbacksClosing) {
    m_CallbackLock.release();
    return false;
  }
  for (auto it = m_MountCallbacks.begin(); it != m_MountCallbacks.end(); ++it) {
    if ((*it)->callback != callback) {
      continue;
    }

    item = *it;
    item->state.enabled = false;
    callbackContext = isCallbackInvocation(owner);
    if (!callbackContext && canYield) {
      item->state.draining = true;
      ++item->state.removers;
    }
    break;
  }
  m_CallbackLock.release();

  if (!item) {
    return false;
  }
  if (callbackContext || !canYield) {
    return false;
  }

  drainMountCallback(item);
  return true;
#else
  for (auto it = m_MountCallbacks.begin(); it != m_MountCallbacks.end(); ++it) {
    MountCallbackItem* item = *it;
    if (item->callback == callback) {
      m_MountCallbacks.erase(it);
      delete item;
      return true;
    }
  }
  return false;
#endif
}

#if THREADS
VFS::ProbeCallbackItem* VFS::acquireProbeCallback(size_t& afterSequence, size_t boundary,
                                                  ActiveInvocation& invocation) {
  ProbeCallbackItem* item = nullptr;
  m_CallbackLock.acquire();
  if (!m_CallbacksClosing) {
    for (auto candidate : m_ProbeCallbacks) {
      if (candidate->state.sequence <= afterSequence) {
        continue;
      }
      if (candidate->state.sequence >= boundary) {
        break;
      }

      afterSequence = candidate->state.sequence;
      if (!candidate->state.enabled) {
        continue;
      }

      item = candidate;
      invocation.state = &item->state;
      ++item->state.inFlight;
      invocation.next = m_pActiveCallbacks;
      m_pActiveCallbacks = &invocation;
      break;
    }
  }
  m_CallbackLock.release();
  return item;
}

VFS::MountCallbackItem* VFS::acquireMountCallback(size_t& afterSequence, size_t boundary,
                                                  ActiveInvocation& invocation) {
  MountCallbackItem* item = nullptr;
  m_CallbackLock.acquire();
  if (!m_CallbacksClosing) {
    for (auto candidate : m_MountCallbacks) {
      if (candidate->state.sequence <= afterSequence) {
        continue;
      }
      if (candidate->state.sequence >= boundary) {
        break;
      }

      afterSequence = candidate->state.sequence;
      if (!candidate->state.enabled) {
        continue;
      }

      item = candidate;
      invocation.state = &item->state;
      ++item->state.inFlight;
      invocation.next = m_pActiveCallbacks;
      m_pActiveCallbacks = &invocation;
      break;
    }
  }
  m_CallbackLock.release();
  return item;
}

void VFS::finishCallback(CallbackState* state, ActiveInvocation& invocation) {
  auto completionGuard = state->drainWaiters.acquire();
  bool wakeDrainers = false;

  m_CallbackLock.acquire();
  ActiveInvocation** link = &m_pActiveCallbacks;
  while (*link && *link != &invocation) {
    link = &((*link)->next);
  }
  if (!*link) {
    m_CallbackLock.release();
    FATAL("VFS lost an active callback invocation");
  }
  *link = invocation.next;

  if (!state->inFlight) {
    m_CallbackLock.release();
    FATAL("VFS callback pin underflow");
  }
  --state->inFlight;
  wakeDrainers = !state->inFlight && state->draining;
  m_CallbackLock.release();

  if (wakeDrainers) {
    completionGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(state));
  }
}

void VFS::drainProbeCallback(ProbeCallbackItem* item) {
  while (true) {
    bool complete = false;
    {
      auto waitGuard = item->state.drainWaiters.acquire();
      m_CallbackLock.acquire();
      if (!item->state.inFlight) {
        complete = true;
        m_CallbackLock.release();
      } else {
        m_CallbackLock.release();
        const WaitQueue::WakeReason reason = waitGuard.waitForCompletion(
            WaitQueue::Channel(&item->state), Thread::CallbackDrain, item->state.debugAddress);
        (void)reason;
      }
    }
    if (complete) {
      break;
    }
  }

  bool deleteItem = false;
  m_CallbackLock.acquire();
  if (!item->state.removers) {
    m_CallbackLock.release();
    FATAL("VFS probe callback remover underflow");
  }
  --item->state.removers;
  if (!item->state.removers) {
    for (auto it = m_ProbeCallbacks.begin(); it != m_ProbeCallbacks.end(); ++it) {
      if (*it == item) {
        m_ProbeCallbacks.erase(it);
        deleteItem = true;
        break;
      }
    }
  }
  m_CallbackLock.release();

  if (deleteItem) {
    delete item;
  }
}

void VFS::drainMountCallback(MountCallbackItem* item) {
  while (true) {
    bool complete = false;
    {
      auto waitGuard = item->state.drainWaiters.acquire();
      m_CallbackLock.acquire();
      if (!item->state.inFlight) {
        complete = true;
        m_CallbackLock.release();
      } else {
        m_CallbackLock.release();
        const WaitQueue::WakeReason reason = waitGuard.waitForCompletion(
            WaitQueue::Channel(&item->state), Thread::CallbackDrain, item->state.debugAddress);
        (void)reason;
      }
    }
    if (complete) {
      break;
    }
  }

  bool deleteItem = false;
  m_CallbackLock.acquire();
  if (!item->state.removers) {
    m_CallbackLock.release();
    FATAL("VFS mount callback remover underflow");
  }
  --item->state.removers;
  if (!item->state.removers) {
    for (auto it = m_MountCallbacks.begin(); it != m_MountCallbacks.end(); ++it) {
      if (*it == item) {
        m_MountCallbacks.erase(it);
        deleteItem = true;
        break;
      }
    }
  }
  m_CallbackLock.release();

  if (deleteItem) {
    delete item;
  }
}

void VFS::dispatchMountCallbacks(void* owner) {
  size_t boundary = 0;
  m_CallbackLock.acquire();
  boundary = m_NextCallbackSequence;
  m_CallbackLock.release();

  size_t afterSequence = 0;
  while (true) {
    ActiveInvocation invocation = {nullptr, owner, nullptr};
    MountCallbackItem* item = acquireMountCallback(afterSequence, boundary, invocation);
    if (!item) {
      break;
    }

    item->callback();
    finishCallback(&item->state, invocation);
  }
}

bool VFS::isCallbackInvocation(void* owner) const {
  for (ActiveInvocation* invocation = m_pActiveCallbacks; invocation;
       invocation = invocation->next) {
    if (invocation->owner == owner) {
      return true;
    }
  }
  return false;
}
#endif

bool VFS::createFile(const String& path, uint32_t mask, File* pStartNode) {
  pStartNode = resolveStartNode(path, pStartNode);
  return pStartNode && pStartNode->getFilesystem()->createFile(path, mask, pStartNode);
}

bool VFS::createDirectory(const String& path, uint32_t mask, File* pStartNode) {
  pStartNode = resolveStartNode(path, pStartNode);
  if (!pStartNode) {
    NOTICE("no start node found");
    return false;
  }

  return pStartNode->getFilesystem()->createDirectory(path, mask, pStartNode);
}

bool VFS::createSymlink(const String& path, const String& value, File* pStartNode) {
  pStartNode = resolveStartNode(path, pStartNode);
  return pStartNode && pStartNode->getFilesystem()->createSymlink(path, value, pStartNode);
}

bool VFS::createLink(const String& path, File* target, File* pStartNode) {
  pStartNode = resolveStartNode(path, pStartNode);
  return pStartNode && pStartNode->getFilesystem()->createLink(path, target, pStartNode);
}

bool VFS::remove(const String& path, File* pStartNode) {
  return remove(path, pStartNode, nullptr);
}

bool VFS::remove(const String& path, File* pStartNode, File* expected) {
  pStartNode = resolveStartNode(path, pStartNode);
  return pStartNode && pStartNode->getFilesystem()->remove(path, pStartNode, expected);
}

bool VFS::rename(const String& oldPath, File* oldStart, const String& newPath, File* newStart,
                 bool noReplace) {
  oldStart = resolveStartNode(oldPath, oldStart);
  newStart = resolveStartNode(newPath, newStart);
  return oldStart && newStart &&
         oldStart->getFilesystem()->rename(oldPath.view(), oldStart, newPath.view(), newStart,
                                           noReplace);
}

bool VFS::checkAccess(File* pFile, bool bRead, bool bWrite, bool bExecute) {
#ifdef VFS_STANDALONE
  // We don't check permissions on standalone builds of the VFS.
  return true;
#else
  if (!pFile) {
    // The error for a null file is not EPERM or EACCESS.
    return true;
  }

  FilesystemCredentials credentials;
  if (!Process::currentFilesystemCredentials(credentials)) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  return checkAccess(pFile, bRead, bWrite, bExecute, credentials);
#endif
}

bool VFS::checkAccess(File* file, bool read, bool write, bool execute, int64_t uid, int64_t gid,
                      const Vector<int64_t>& groups) {
  FilesystemCredentials credentials;
  if (uid >= 0 && static_cast<uint64_t>(uid) < UINT32_MAX && gid >= 0 &&
      static_cast<uint64_t>(gid) < UINT32_MAX && groups.count() <= credentials.MaximumGroups) {
    credentials.uid = uid;
    credentials.gid = gid;
    credentials.groupCount = groups.count();
    credentials.valid = true;
    for (size_t i = 0; i < groups.count(); ++i) {
      if (groups[i] < 0 || static_cast<uint64_t>(groups[i]) >= UINT32_MAX)
        credentials.valid = false;
      credentials.groups[i] = groups[i];
    }
  }
  return checkAccess(file, read, write, execute, credentials);
}

bool VFS::checkAccess(File* pFile, bool bRead, bool bWrite, bool bExecute,
                      const FilesystemCredentials& credentials) {
#ifdef VFS_STANDALONE
  return true;
#else
  if (!pFile)
    return true;
  if (!credentials.valid) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  const uint32_t processUid = credentials.uid;
  const uint32_t processGid = credentials.gid;
  uint32_t check = 0;
  const auto attributes = pFile->getAttributes();
  const int64_t fuid = attributes.uid;
  const int64_t fgid = attributes.gid;
  uint32_t permissions = attributes.permissions;
  uint32_t needed = (bRead ? FILE_UR : 0) | (bWrite ? FILE_UW : 0) | (bExecute ? FILE_UX : 0);

  if (processUid == 0) {
    if (!bExecute || (permissions & (FILE_UX | FILE_GX | FILE_OX))) {
      return true;
    }
  } else if (fuid == processUid) {
    check = (permissions >> FILE_UBITS) & 0x7;
  } else {
    bool inFileGroup = fgid == processGid;

    if (!inFileGroup) {
      for (size_t i = 0; i < credentials.groupCount; ++i) {
        if (credentials.groups[i] == fgid) {
          inFileGroup = true;
          break;
        }
      }
    }

    check = (permissions >> (inFileGroup ? FILE_GBITS : FILE_OBITS)) & 0x7;
  }

  if ((check & needed) != needed) {
    NOTICE("VFS::checkAccess: needed " << Oct << needed << ", check was " << check);
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }

  return true;
#endif
}

void VFS::trackFile(File* pFile) {
  LockGuard<Mutex> guard(m_TrackedFilesLock);
  size_t n = m_TrackedFiles.lookup(pFile);
  ++n;
  m_TrackedFiles.insert(pFile, n);
}

bool VFS::tryTrackFile(File* file) {
  if (!file)
    return false;
  LockGuard<Mutex> guard(m_TrackedFilesLock);
  const size_t count = m_TrackedFiles.lookup(file);
  return count != ~size_t(0) && m_TrackedFiles.tryInsert(file, count + 1);
}

bool VFS::retainTrackedFile(File* pFile) {
  if (!pFile) {
    return false;
  }

  LockGuard<Mutex> guard(m_TrackedFilesLock);
  size_t n = m_TrackedFiles.lookup(pFile);
  if (!n) {
    return false;
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  RetainTrackedFileHook hook = __atomic_load_n(&m_RetainTrackedFileHook, __ATOMIC_ACQUIRE);
  if (hook) {
    hook(pFile);
  }
#endif

  m_TrackedFiles.insert(pFile, n + 1);
  return true;
}

bool VFS::untrackFile(File* pFile, bool destroy) {
  bool finalOwner = false;
  {
    LockGuard<Mutex> guard(m_TrackedFilesLock);
    size_t n = m_TrackedFiles.lookup(pFile);
    if (!n) {
      return false;
    }

    if (n == 1) {
      m_TrackedFiles.remove(pFile);
      finalOwner = true;
    } else {
      m_TrackedFiles.insert(pFile, n - 1);
    }
  }

  if (finalOwner && destroy) {
    delete pFile;
  }
  return finalOwner;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void VFS::setRetainTrackedFileHookForHostedTest(RetainTrackedFileHook hook) {
  __atomic_store_n(&m_RetainTrackedFileHook, hook, __ATOMIC_RELEASE);
}
#endif

String VFS::getUniqueStableNameLocked(const String& preferredName) const {
  NormalStaticString safeName;
  for (size_t i = 0; i < preferredName.length(); ++i) {
    char c = preferredName[i];
    bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-';
    safeName.append(allowed ? c : '-');
  }

  String base(safeName, safeName.length());
  if (!base.length() || base == "." || base == "..") {
    base.assign("filesystem");
  }

  size_t suffix = 1;
  while (true) {
    NormalStaticString candidateBuffer;
    candidateBuffer += base;
    if (suffix > 1) {
      candidateBuffer += "-";
      candidateBuffer.append(suffix);
    }
    String candidate(candidateBuffer, candidateBuffer.length());

    bool exists = false;
    for (MountTable::Iterator it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
      if (it.value()->stableName == candidate) {
        exists = true;
        break;
      }
    }

    if (!exists) {
      return candidate;
    }

    ++suffix;
  }
}

File* VFS::resolveStartNode(const String& path, File* pStartNode) {
  if (!path.length() || path[0] != '/') {
    return pStartNode;
  }

  Filesystem* root = nullptr;
  {
    LockGuard<Mutex> tableGuard(m_MountTableLock);
    root = m_pRootFilesystem;
  }
  return root ? root->getRoot() : nullptr;
}

bool VFS::attachFilesystem(Filesystem* pRootFs, Filesystem* pFs, const String& path) {
  if (!pRootFs || !pFs || pFs == pRootFs) {
    return pFs == pRootFs;
  }

  Directory::ChildLease mediaLease;
  if (!findRetained(String("/media"), mediaLease)) {
    createDirectory(String("/media"), 0755);
  }

  Directory::ChildLease pointLease;
  File* point = findRetained(path, pointLease);
  if (!point) {
    createDirectory(path, 0755);
    point = findRetained(path, pointLease);
  }

  if (!point || !point->isDirectory()) {
    ERROR("VFS: cannot attach filesystem at " << path);
    return false;
  }

  Directory::fromFile(point)->setReparsePoint(Directory::fromFile(pFs->getRoot()));
  NOTICE("VFS: attached filesystem at " << path);
  return true;
}

void VFS::attachRegisteredFilesystemsLocked() {
  struct AttachWork {
    AttachWork() : filesystem(nullptr) {}
    AttachWork(Filesystem* filesystem, const String& path) : filesystem(filesystem), path(path) {}

    Filesystem* filesystem;
    String path;
  };

  Filesystem* root = nullptr;
  Vector<AttachWork> work;
  {
    LockGuard<Mutex> tableGuard(m_MountTableLock);
    root = m_pRootFilesystem;
    if (!root) {
      return;
    }

    for (MountTable::Iterator it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
      MountInfo* info = it.value();
      if (it.key() == root) {
        info->path.assign("/");
        continue;
      }

      NormalStaticString path;
      path += "/media/";
      path += info->stableName;
      info->path.assign(path, path.length());
      work.createBack(it.key(), info->path);
    }
  }

  for (const auto& item : work) {
    attachFilesystem(root, item.filesystem, item.path);
  }
}

#ifndef VFS_STANDALONE
static bool initVFS() {
  return true;
}

static void destroyVFS() {}

MODULE_INFO("vfs", &initVFS, &destroyVFS);
#endif

bool VFS::diskMount(uint32_t id, MountIdentity& identity) const {
  identity = MountIdentity();
  if (!id || id > 0xfffff)
    return false;
  LockGuard<Mutex> guard(m_MountTableLock);
  for (auto it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
    const auto& state = it.value()->state;
    if (state->id == id && state->filesystem->getDisk() && state->operations.isOpen()) {
      identity.m_State = state;
      return true;
    }
  }
  return false;
}
bool VFS::snapshotDiskMounts(Vector<MountIdentity>& mounts) const {
  mounts.clear();
  LockGuard<Mutex> guard(m_MountTableLock);
  if (!mounts.tryReserve(m_Mounts.count()))
    return false;
  for (auto it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
    const auto& state = it.value()->state;
    if (state->id > 0xfffff || !state->filesystem->getDisk() || !state->operations.isOpen())
      continue;
    MountIdentity identity;
    identity.m_State = state;
    mounts.pushBack(identity);
  }
  return true;
}
