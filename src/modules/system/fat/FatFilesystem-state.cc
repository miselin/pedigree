/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/TerminationDeferral.h"

#include "FatFilesystem.h"
#include "FatSymlink.h"

namespace {
uint64_t slotKey(uint32_t cluster, uint32_t offset) {
  return (uint64_t(cluster) << 32) | offset;
}
bool publishedSlot(uint32_t cluster, uint32_t offset) {
  return cluster != 0xdeadbeef && offset != 0xbeefdead;
}
}  // namespace

FatFile::State::State(FatFilesystem* owner) : filesystem(owner) {
  cache.fill.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.fill.setCallback(checkedWriteCallback, this);
  cache.fill.setBackgroundWriteback(checkedBatchCallback);
}

FatFile::State::~State() {
  if (!cache.fill.shutdown())
    ERROR("FAT: unable to drain file cache during unmount");
}

uintptr_t FatFilesystem::fileIdentifier(uint32_t cluster, uint32_t offset) {
  LockGuard<Mutex> registry(m_StateLock);
  return fileIdentifierLocked(slotKey(cluster, offset));
}

uintptr_t FatFilesystem::fileIdentifierLocked(uint64_t slot) {
  uintptr_t identifier = m_FileIdentifiers.lookup(slot);
  if (!identifier) {
    // Public IDs occupy the range above every valid FAT32 cluster number.
    assert(m_NextFileIdentifier >= 0x10000000);
    identifier = m_NextFileIdentifier++;
    m_FileIdentifiers.insert(slot, identifier);
  }
  return identifier;
}

FatFile::State* FatFilesystem::acquireFileState(FatFile* file, uintptr_t inode, size_t size,
                                                uint32_t cluster, uint32_t offset,
                                                Time::Timestamp accessed, Time::Timestamp modified,
                                                Time::Timestamp changed) {
  LockGuard<Mutex> registry(m_StateLock);
  const bool published = publishedSlot(cluster, offset);
  FatFile::State* state = published ? m_FileStates.lookup(slotKey(cluster, offset)) : nullptr;
  if (!state) {
    state = new FatFile::State(this);
    state->inode = inode;
    state->size = size;
    state->directoryCluster = cluster;
    state->directoryOffset = offset;
    state->accessed = accessed;
    state->modified = modified;
    state->changed = changed;
    state->next = m_StateList;
    m_StateList = state;
    if (published) {
      state->identifier = fileIdentifierLocked(slotKey(cluster, offset));
      m_FileStates.insert(slotKey(cluster, offset), state);
      state->registered = true;
    }
  }
  return state;
}

void FatFilesystem::releaseFileState(FatFile* file) {
  auto* state = file->m_State;
  LockGuard<Mutex> data(state->dataLock);
  // Keep failed writeback retryable even when the last descriptor closes.
  const bool written = state->cache.fill.syncAll(FatFile::checkedBatchCallback, state);
  if (m_pDisk && !syncFileMetadata(file))
    WARNING("FAT: retaining pending metadata after close");
  if (!written)
    WARNING("FAT: retaining dirty file pages after close");
  bool retire = false;
  {
    LockGuard<Mutex> guard(m_FileMutationLock);
    LockGuard<Mutex> registry(m_StateLock);
    FatFile** alias = &state->aliases;
    while (*alias && *alias != file)
      alias = &(*alias)->m_NextAlias;
    if (*alias)
      *alias = file->m_NextAlias;
    retire = state->unlinked && !state->aliases;
    if (retire)
      state->retiring = true;
  }
  if (retire) {
    // Callback storage survives the terminal drain; the old allocation cannot
    // be reused while a callback can still write its pages.
    state->cache.fill.shutdown();
    retireNode(file);
    state->inode = 0;
  }
}

void FatFilesystem::publishSize(File* file, size_t size) {
  if (file->isDirectory() || file->isSymlink()) {
    file->setSize(size);
    return;
  }
  auto* regular = static_cast<FatFile*>(file);
  LockGuard<Mutex> registry(m_StateLock);
  regular->m_State->size = size;
  regular->File::setSize(size);
  for (FatFile* alias = regular->m_State->aliases; alias; alias = alias->m_NextAlias)
    alias->File::setSize(size);
}

bool FatFilesystem::isNodeUnlinked(File* file) const {
  if (file->isDirectory())
    return static_cast<FatDirectory*>(file)->m_Unlinked;
  if (file->isSymlink())
    return static_cast<FatSymlink*>(file)->m_Unlinked;
  return static_cast<FatFile*>(file)->m_State->unlinked;
}

void FatFilesystem::unlinkNode(File* file) {
  if (file->isDirectory() || file->isSymlink()) {
    if (file->isDirectory()) {
      auto* node = static_cast<FatDirectory*>(file);
      discardPendingAttributes(node->getDirCluster(), node->getDirOffset());
    } else {
      auto* node = static_cast<FatSymlink*>(file);
      discardPendingAttributes(node->getDirCluster(), node->getDirOffset());
    }
    unlinkNonFileNode(file);
    return;
  }
  LockGuard<Mutex> registry(m_StateLock);
  auto* state = static_cast<FatFile*>(file)->m_State;
  state->unlinked = true;
  if (state->registered) {
    m_FileStates.remove(slotKey(state->directoryCluster, state->directoryOffset));
    m_FileIdentifiers.remove(slotKey(state->directoryCluster, state->directoryOffset));
    state->registered = false;
  }
}

void FatFilesystem::moveNode(File* file, uint32_t cluster, uint32_t offset) {
  if (file->isDirectory() || file->isSymlink()) {
    if (file->isDirectory()) {
      auto* node = static_cast<FatDirectory*>(file);
      movePendingAttributes(node->getDirCluster(), node->getDirOffset(), cluster, offset);
    } else {
      auto* node = static_cast<FatSymlink*>(file);
      movePendingAttributes(node->getDirCluster(), node->getDirOffset(), cluster, offset);
    }
    moveNonFileNode(file, cluster, offset);
  } else {
    LockGuard<Mutex> registry(m_StateLock);
    auto* state = static_cast<FatFile*>(file)->m_State;
    if (state->registered) {
      m_FileStates.remove(slotKey(state->directoryCluster, state->directoryOffset));
      m_FileIdentifiers.remove(slotKey(state->directoryCluster, state->directoryOffset));
    }
    state->directoryCluster = cluster;
    state->directoryOffset = offset;
    if (!state->identifier)
      state->identifier = fileIdentifierLocked(slotKey(cluster, offset));
    else
      m_FileIdentifiers.insert(slotKey(cluster, offset), state->identifier);
    m_FileStates.insert(slotKey(cluster, offset), state);
    state->registered = true;
  }
}

void FatFilesystem::retireNode(File* file) {
  if (m_bReadOnly)
    return;
  LockGuard<Mutex> guard(m_FileMutationLock);
  if (!file->isDirectory() && !file->isSymlink()) {
    auto* regular = static_cast<FatFile*>(file);
    for (uint32_t cluster : regular->m_RetiredClusters) {
      if (!setClusterEntry(cluster, 0))
        m_IoFailed = true;
    }
    regular->m_RetiredClusters.clear();
  }
  if (!releaseClusterChain(file->getInode(), false)) {
    m_IoFailed = true;
    ERROR("FAT: orphan allocation reclamation needs a FAT retry");
  }
}

Filesystem::SyncStatus FatFilesystem::sync() {
  TerminationDeferral lifetime;
  if (!m_pDisk)
    return SyncStatus::IoError;
  if (m_bReadOnly)
    return m_IoFailed ? SyncStatus::IoError : SyncStatus::Success;
  Vector<FatFile::State*> states;
  {
    LockGuard<Mutex> registry(m_StateLock);
    for (auto* state = m_StateList; state; state = state->next) {
      if (!states.tryReserve(states.count() + 1))
        return SyncStatus::NoMemory;
      states.pushBack(state);
    }
  }
  bool succeeded = true;
  for (auto* state : states) {
    LockGuard<Mutex> data(state->dataLock);
    if (state->retiring)
      continue;
    FatFile file(*state);
    succeeded = state->cache.fill.syncAll(FatFile::checkedBatchCallback, state) && succeeded;
    succeeded = syncFileMetadata(&file) && succeeded;
  }
  LockGuard<Mutex> guard(m_FileMutationLock);
  succeeded = syncPendingAttributes() && succeeded;
  succeeded = syncFat() && succeeded;
  succeeded = m_pDisk->syncAll() && succeeded;
  return succeeded ? SyncStatus::Success : SyncStatus::IoError;
}

Filesystem::SyncStatus FatFilesystem::shutdown() {
  TerminationDeferral lifetime;
  if (m_ShutdownComplete)
    return SyncStatus::Success;
  auto status = sync();
  if (status != SyncStatus::Success)
    return status;
  delete m_pRoot;
  m_pRoot = nullptr;
  if (!drainFileStates(true))
    return SyncStatus::IoError;
  // Closing aliases may release orphan clusters or queue directory attributes.
  status = sync();
  if (status != SyncStatus::Success || m_IoFailed || !m_MountedClean)
    return status == SyncStatus::Success ? SyncStatus::IoError : status;
  m_ShutdownComplete = true;
  return SyncStatus::Success;
}

bool FatFilesystem::drainFileStates(bool checked) {
  bool succeeded = true;
  while (m_StateList) {
    auto* state = m_StateList;
    if (checked && state->aliases)
      return false;
    if (!state->retiring && m_pDisk) {
      FatFile file(*state);
      if (!state->cache.fill.syncAll(FatFile::checkedBatchCallback, state)) {
        succeeded = false;
        ERROR("FAT: dirty pages remain during unmount");
      }
      succeeded = syncFileMetadata(&file) && succeeded;
    }
    if (checked && (!succeeded || !state->cache.fill.shutdown()))
      return false;
    m_StateList = state->next;
    delete state;
  }
  m_FileStates.clear();
  m_FileIdentifiers.clear();
  return succeeded;
}
