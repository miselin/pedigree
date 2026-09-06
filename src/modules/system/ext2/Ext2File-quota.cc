/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/syscallError.h"

#include "Ext2File.h"
#include "Ext2Filesystem.h"
#include "ext2.h"

QuotaStatus Ext2File::beginQuota(QuotaTable& loaded) {
  auto writes = lockWrites();
  {
    LockGuard<Mutex> data(m_State->dataLock);
    LockGuard<Mutex> inode(m_State->writebackLock);
    LockGuard<Mutex> allocation(m_pExt2Fs->m_WriteLock);
    if (m_State->quotaFile || __atomic_load_n(&m_State->pageLoans, __ATOMIC_ACQUIRE))
      return QuotaStatus::Busy;
    if (!m_State->allocationValid || !LITTLE_TO_HOST16(m_pInode->i_links_count))
      return QuotaStatus::Invalid;
    if (!m_nSize || m_nSize % QuotaOld::RecordSize)
      return QuotaStatus::Invalid;
    auto status = m_pExt2Fs->prepareQuotaInodeLocked(m_InodeNumber);
    if (status != QuotaStatus::Success)
      return status;
    status = m_pExt2Fs->m_Quota.exempt(m_InodeNumber, true);
    if (status != QuotaStatus::Success)
      return status;
    __atomic_store_n(&m_State->quotaFile, true, __ATOMIC_RELEASE);
    m_OwnsQuotaProtection = true;
  }

  // The shared write lock stabilizes this snapshot. The active flag rejects
  // aliases, future mapping loans and namespace changes until quota-off.
  uint8_t bytes[QuotaOld::RecordSize];
  for (uint64_t offset = 0; offset < getSize(); offset += sizeof(bytes)) {
    syscallError(0);
    if (read(offset, sizeof(bytes), reinterpret_cast<uintptr_t>(bytes)) != sizeof(bytes))
      return m_pExt2Fs->quotaIoStatus();
    if (!offset && QuotaOld::looksLikeNewFormat(bytes, sizeof(bytes)))
      return QuotaStatus::Invalid;
    QuotaRecord record;
    auto status = QuotaOld::decode(bytes, sizeof(bytes), record);
    if (status != QuotaStatus::Success)
      return status;
    if (record.blockSoftLimit || record.inodeSoftLimit ||
        (offset && (record.blockTime || record.inodeTime)))
      return QuotaStatus::Unsupported;
    if (offset && !record.blockHardLimit && !record.inodeHardLimit)
      continue;
    auto* entry = loaded.prepare(static_cast<uint32_t>(offset / sizeof(bytes)));
    if (!entry)
      return QuotaStatus::NoMemory;
    entry->record = record;
  }
  return QuotaStatus::Success;
}

QuotaStatus Ext2File::endQuota(QuotaType type, bool requireClean) {
  auto writes = lockWrites();
  LockGuard<Mutex> data(m_State->dataLock);
  LockGuard<Mutex> inode(m_State->writebackLock);
  LockGuard<Mutex> allocation(m_pExt2Fs->m_WriteLock);
  if (!m_OwnsQuotaProtection)
    return QuotaStatus::Success;
  if (requireClean) {
    for (const auto& entry : m_pExt2Fs->m_Quota.table(type).entries()) {
      if (entry.dirty)
        return QuotaStatus::Busy;
    }
  }
  const auto status = m_pExt2Fs->m_Quota.exempt(m_InodeNumber, false);
  if (status != QuotaStatus::Success)
    return status;
  m_pExt2Fs->m_Quota.disable(type);
  __atomic_store_n(&m_State->quotaFile, false, __ATOMIC_RELEASE);
  m_OwnsQuotaProtection = false;
  return QuotaStatus::Success;
}

QuotaStatus Ext2File::writeQuotaRecord(uint32_t id, const QuotaRecord& record) {
  uint8_t bytes[QuotaOld::RecordSize];
  const auto status = QuotaOld::encode(record, bytes, sizeof(bytes));
  if (status != QuotaStatus::Success)
    return status;
  const uint64_t offset = static_cast<uint64_t>(id) * sizeof(bytes);
  if (offset > maximumFileSize() || sizeof(bytes) > maximumFileSize() - offset) {
    return QuotaStatus::TooLarge;
  }
  auto writes = lockWrites();
  // Only prepareWrite examines this flag, under this same shared write lock.
  // Other mutation entrypoints reject an active quota file unconditionally.
  m_State->quotaInternalWrite = true;
  syscallError(0);
  const uint64_t written = writes.write(offset, sizeof(bytes), reinterpret_cast<uintptr_t>(bytes));
  m_State->quotaInternalWrite = false;
  return written == sizeof(bytes) ? QuotaStatus::Success : m_pExt2Fs->quotaIoStatus();
}
