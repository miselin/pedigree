/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"

#include "Ext2File.h"
#include "Ext2Filesystem.h"
#include "Ext2Node.h"

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
