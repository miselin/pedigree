/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"

#include "Ext2Directory.h"
#include "Ext2File.h"
#include "Ext2Filesystem.h"
#include "Ext2Node.h"
#include "ext2.h"

bool Ext2Filesystem::beginWritableMount() {
  if (checkOptionalFeature(0x4) || checkRequiredFeature(~size_t(0x2)) ||
      checkReadOnlyFeature(~size_t(0x3))) {
    ERROR("Ext2: unsupported filesystem features prevent a writable mount");
    return false;
  }
  m_MountState = LITTLE_TO_HOST16(m_pSuperblock->s_state);
  m_pSuperblock->s_state = HOST_TO_LITTLE16(m_MountState & ~EXT2_STATE_CLEAN);
  m_pDisk->write(1024ULL);
#if !CRIPPLE_HDD || defined(EXT2_STANDALONE)
  if (!m_pDisk->sync(1024ULL, false)) {
    ERROR("Ext2: could not persist the writable mount state");
    return false;
  }
#endif
  return true;
}

Filesystem::SyncStatus Ext2Filesystem::shutdown() {
  TerminationDeferral lifetime;
  if (m_ShutdownComplete)
    return SyncStatus::Success;
  if (m_bReadOnly)
    return m_MountState == EXT2_STATE_CLEAN ? sync() : SyncStatus::IoError;
  if (!m_pDisk || !m_pSuperblock || !m_BlockSize)
    return SyncStatus::IoError;

  auto status = sync();
  if (status != SyncStatus::Success)
    return status;
  if (!closeQuotaFiles(false))
    return SyncStatus::IoError;
  delete m_pRoot;
  m_pRoot = nullptr;
  // Releasing directory aliases can retire orphan allocations and quota owners.
  // All of those writes, and callback retirement, precede the clean marker.
  status = sync();
  if (status != SyncStatus::Success)
    return status;
  for (auto it = m_InodeStates.begin(); it != m_InodeStates.end(); ++it) {
    auto* state = it.value();
    if (state->references || state->pageLoans || state->syncReferences || state->orphan)
      return SyncStatus::IoError;
    if (state->cache && !state->cache->fill.shutdown())
      return SyncStatus::IoError;
  }
  for (auto it = m_InodeStates.begin(); it != m_InodeStates.end(); ++it)
    delete it.value();
  m_InodeStates.clear();
  if (!m_pDisk->syncAll() || m_TeardownFailed || m_MountState != EXT2_STATE_CLEAN)
    return SyncStatus::IoError;
  // This driver cannot certify journal recovery or unknown incompatible formats.
  if (checkOptionalFeature(0x4) || checkRequiredFeature(~size_t(0x2)) ||
      checkReadOnlyFeature(~size_t(0x3)))
    return SyncStatus::Unsupported;

  m_pSuperblock->s_state = HOST_TO_LITTLE16(m_MountState);
  m_pDisk->write(1024ULL);
  if (!m_pDisk->sync(1024ULL, false) || !m_pDisk->syncAll()) {
    m_pSuperblock->s_state = HOST_TO_LITTLE16(m_MountState & ~EXT2_STATE_CLEAN);
    m_pDisk->write(1024ULL);
    if (!m_pDisk->sync(1024ULL, false))
      ERROR("Ext2: could not restore the unchecked state after shutdown I/O failure");
    return SyncStatus::IoError;
  }
  m_ShutdownComplete = true;
  return SyncStatus::Success;
}

class Ext2Filesystem::SyncSnapshot {
 public:
  explicit SyncSnapshot(Ext2Filesystem& filesystem) : filesystem(filesystem) {}
  ~SyncSnapshot() {
    for (const auto& entry : entries)
      filesystem.releaseSyncState(entry.inode, entry.state);
  }

  struct Entry {
    uint32_t inode;
    Ext2InodeState* state;
  };
  Ext2Filesystem& filesystem;
  Vector<Entry> entries;
};

void Ext2Filesystem::releaseSyncState(uint32_t inode, Ext2InodeState* state) {
  assert(__atomic_load_n(&state->syncReferences, __ATOMIC_RELAXED));
  __atomic_sub_fetch(&state->syncReferences, size_t(1), __ATOMIC_RELEASE);
  // The last admission must provide wipe() a node when final unlink raced sync.
  Ext2Node admission(inode, state->metadata, this, *state);
}

Filesystem::SyncStatus Ext2Filesystem::sync() {
  TerminationDeferral lifetime;
  if (m_bReadOnly)
    return SyncStatus::Success;
  if (!m_pDisk || !m_pSuperblock || !m_BlockSize)
    return SyncStatus::IoError;

  const auto quotaStatus = flushQuotas();

  SyncSnapshot snapshot(*this);
  for (;;) {
    size_t required;
    {
      LockGuard<Mutex> registry(m_InodeStateLock);
      required = m_InodeStates.count();
      if (required <= snapshot.entries.size()) {
        for (auto it = m_InodeStates.begin(); it != m_InodeStates.end(); ++it) {
          Ext2InodeState* state = it.value();
          // Dormant identities without a cache have discarded their block maps.
          // Pinning those would suppress the next opener's mapping reload.
          if (!state->references && !state->cache)
            continue;
          if (state->references == ~size_t(0))
            return SyncStatus::NoMemory;
          ++state->references;
          __atomic_add_fetch(&state->syncReferences, size_t(1), __ATOMIC_RELEASE);
          snapshot.entries.pushBack({it.key(), state});
        }
        break;
      }
    }
    // Allocation can re-enter filesystem caches under memory pressure.
    if (!snapshot.entries.tryReserve(required, false))
      return SyncStatus::NoMemory;
  }

  bool succeeded = quotaStatus == QuotaStatus::Success;
  for (const auto& entry : snapshot.entries) {
    LockGuard<Mutex> data(entry.state->dataLock);
    if (!entry.state->allocationValid)
      succeeded = false;
    if (entry.state->cache)
      succeeded =
          entry.state->cache->fill.syncAll(Ext2File::sharedFillBatchCallback, entry.state) &&
          succeeded;
  }

  {
#if THREADS || defined(STANDALONE_MUTEXES)
    LockGuard<Mutex> allocation(m_WriteLock);
#endif
    // A whole-disk drain must not bypass failed xattr dependency ordering.
    if (!flushAttributeWritesLocked())
      return SyncStatus::IoError;
    succeeded = m_pDisk->syncAll() && succeeded;
  }
  return succeeded                              ? SyncStatus::Success
         : quotaStatus == QuotaStatus::NoMemory ? SyncStatus::NoMemory
                                                : SyncStatus::IoError;
}
