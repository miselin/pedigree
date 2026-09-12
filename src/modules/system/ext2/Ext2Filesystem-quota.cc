/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include "Ext2File.h"
#include "Ext2Filesystem.h"
#include "ext2.h"

#ifdef EXT2_STANDALONE
#include <errno.h>
#else
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#endif

int Ext2Filesystem::currentIoError() {
#ifdef EXT2_STANDALONE
  const int error = errno;
#else
  auto* thread = Processor::information().getCurrentThread();
  const int error = thread ? thread->getErrno() : 0;
#endif
  return error ? error : Error::IoError;
}

QuotaStatus Ext2Filesystem::quotaIoStatus() {
  switch (currentIoError()) {
    case Error::OutOfMemory:
      return QuotaStatus::NoMemory;
    case Error::ValueTooLarge:
      return QuotaStatus::Overflow;
    case Error::FileTooLarge:
      return QuotaStatus::TooLarge;
    case Error::NoSpaceLeftOnDevice:
      return QuotaStatus::NoSpace;
    case Error::QuotaExceeded:
      return QuotaStatus::Limit;
    default:
      return QuotaStatus::IoError;
  }
}

bool Ext2Filesystem::quotaSucceeded(QuotaStatus status) {
  if (status == QuotaStatus::Success)
    return true;
  syscallError(quotaError(status));
  return false;
}

bool Ext2Filesystem::isQuotaFile(uint32_t inode) {
  LockGuard<Mutex> allocation(m_WriteLock);
  const auto* charge = m_Quota.find(inode);
  return charge && charge->exempt;
}

QuotaStatus Ext2Filesystem::prepareQuotaInodeLocked(uint32_t inode) {
  if (m_Quota.find(inode))
    return QuotaStatus::Success;
  const uint32_t total = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_count);
  const uint32_t perGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  if (!inode || !perGroup || (total && inode > total) ||
      (inode - 1) / perGroup >= m_nGroupDescriptors)
    return QuotaStatus::Invalid;
  syscallError(0);
  const Inode* metadata = getInode(inode);
  if (!metadata)
    return quotaIoStatus();
  return m_Quota.track(inode, Ext2Owner::uid(*metadata), Ext2Owner::gid(*metadata),
                       static_cast<uint64_t>(LITTLE_TO_HOST32(metadata->i_blocks)) * 512);
}

QuotaStatus Ext2Filesystem::scanQuotaInodesLocked() {
  const uint32_t perGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  const uint32_t total = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_count);
  uint32_t first = LITTLE_TO_HOST32(m_pSuperblock->s_first_ino);
  if (!LITTLE_TO_HOST32(m_pSuperblock->s_rev_level))
    first = 11;
  if (!first)
    first = 2;
  if (!perGroup || !total || !m_BlockSize ||
      (static_cast<uint64_t>(total) + perGroup - 1) / perGroup > m_nGroupDescriptors)
    return QuotaStatus::IoError;
  for (uint32_t group = 0; group < m_nGroupDescriptors; ++group) {
    syscallError(0);
    if (!ensureFreeInodeBitmapLoaded(group))
      return quotaIoStatus();
    for (uint32_t index = 0; index < perGroup; ++index) {
      const uint64_t inode = static_cast<uint64_t>(group) * perGroup + index + 1;
      if (inode > total)
        break;
      if (inode < first && inode != 2)
        continue;
      const size_t field = (index / 8) / m_BlockSize;
      if (field >= m_pInodeBitmaps[group].count())
        return QuotaStatus::IoError;
      const auto* byte = reinterpret_cast<const uint8_t*>(m_pInodeBitmaps[group][field] +
                                                          (index / 8) % m_BlockSize);
      if (!(*byte & (1U << (index % 8))))
        continue;
      // Existing overlays include allocations not attached to i_blocks yet.
      const auto status = prepareQuotaInodeLocked(static_cast<uint32_t>(inode));
      if (status != QuotaStatus::Success)
        return status;
    }
  }
  return QuotaStatus::Success;
}

QuotaStatus Ext2Filesystem::flushQuotaLocked(QuotaType type) {
  auto* file = m_QuotaFiles[static_cast<size_t>(type)];
  if (!file)
    return QuotaStatus::Success;
  Vector<QuotaTable::Entry> snapshot;
  bool prepared = false;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    size_t required;
    {
      LockGuard<Mutex> allocation(m_WriteLock);
      const auto& entries = m_Quota.table(type).entries();
      required = entries.count();
      if (required <= snapshot.size()) {
        for (const auto& entry : entries) {
          if (entry.dirty)
            snapshot.pushBack(entry);
        }
        prepared = true;
        break;
      }
    }
    if (!snapshot.tryReserve(required, false))
      return QuotaStatus::NoMemory;
  }

  if (!prepared)
    return QuotaStatus::Busy;
  QuotaStatus result = QuotaStatus::Success;
  for (const auto& entry : snapshot) {
    const auto status = file->writeQuotaRecord(entry.id, entry.record);
    if (result == QuotaStatus::Success)
      result = status;
  }
  // A successful snapshot is durable before its dirty bits can be cleared.
  // Even an empty snapshot reaches the device barrier.
  if (!file->sync())
    return QuotaStatus::IoError;
  {
    LockGuard<Mutex> allocation(m_WriteLock);
    if (!flushAttributeWritesLocked() || !m_pDisk->syncAll())
      return QuotaStatus::IoError;
  }
  if (result != QuotaStatus::Success)
    return result;
  {
    LockGuard<Mutex> allocation(m_WriteLock);
    for (const auto& saved : snapshot) {
      auto* entry = m_Quota.table(type).find(saved.id);
      if (entry && QuotaTable::sameRecord(entry->record, saved.record))
        entry->dirty = false;
    }
  }
  return QuotaStatus::Success;
}

QuotaStatus Ext2Filesystem::flushQuotas() {
  LockGuard<Mutex> control(m_QuotaControlLock);
  QuotaStatus result = QuotaStatus::Success;
  const QuotaType types[] = {QuotaType::User, QuotaType::Group};
  for (auto type : types) {
    const auto status = flushQuotaLocked(type);
    if (result == QuotaStatus::Success)
      result = status;
  }
  return result;
}

QuotaStatus Ext2Filesystem::quotaControl(const QuotaRequest& request, QuotaResponse& response,
                                         File* quotaFile) {
  if (request.type != QuotaType::User && request.type != QuotaType::Group)
    return QuotaStatus::Invalid;
  if (!m_pDisk || !m_pSuperblock || !m_BlockSize)
    return QuotaStatus::IoError;
  LockGuard<Mutex> control(m_QuotaControlLock);
  const size_t index = static_cast<size_t>(request.type);
  if (request.operation == QuotaOperation::Enable) {
    if (m_bReadOnly)
      return QuotaStatus::ReadOnly;
    if (request.format != Quota::OldFormat)
      return QuotaStatus::Unsupported;
    if (!quotaFile || quotaFile->getFilesystem() != this || quotaFile->isDirectory() ||
        quotaFile->isSymlink() || quotaFile->isPipe() || quotaFile->isSocket())
      return QuotaStatus::Invalid;
    if (m_QuotaFiles[index])
      return QuotaStatus::Busy;
    LockGuard<Mutex> names(m_QuotaNamespaceLock);
    Inode* metadata;
    {
      LockGuard<Mutex> allocation(m_WriteLock);
      metadata = getInode(quotaFile->getInode());
    }
    if (!metadata)
      return QuotaStatus::IoError;
    if ((LITTLE_TO_HOST16(metadata->i_mode) & 0xf000) != EXT2_S_IFREG)
      return QuotaStatus::Invalid;
    auto* file = new Ext2File(quotaFile->getName(), quotaFile->getInode(), metadata, this);
    if (!file)
      return QuotaStatus::NoMemory;
    if (!file->valid()) {
      delete file;
      return QuotaStatus::NoMemory;
    }
    QuotaTable loaded;
    auto status = file->beginQuota(loaded);
    if (status == QuotaStatus::Success) {
      LockGuard<Mutex> allocation(m_WriteLock);
      status = scanQuotaInodesLocked();
      if (status == QuotaStatus::Success) {
        const uint64_t slots = file->maximumFileSize() / QuotaOld::RecordSize;
        const uint32_t maximumId = slots > 0x100000000ULL ? 0xffffffffU : slots - 1;
        status = m_Quota.enable(request.type, loaded, maximumId);
      }
    }
    if (status == QuotaStatus::Success) {
      m_QuotaFiles[index] = file;
      status = flushQuotaLocked(request.type);
    }
    if (status != QuotaStatus::Success) {
      file->endQuota(request.type, false);
      m_QuotaFiles[index] = nullptr;
      delete file;
    }
    return status;
  }

  if (!m_QuotaFiles[index])
    return request.operation == QuotaOperation::Sync ? QuotaStatus::Success
                                                     : QuotaStatus::NotEnabled;
  if (request.operation == QuotaOperation::Disable) {
    LockGuard<Mutex> names(m_QuotaNamespaceLock);
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
      const auto status = flushQuotaLocked(request.type);
      if (status != QuotaStatus::Success)
        return status;
#if defined(PEDIGREE_BUILDUTILS)
      if (m_QuotaOffTestHook)
        m_QuotaOffTestHook(m_QuotaOffTestContext);
#endif
      const auto finished = m_QuotaFiles[index]->endQuota(request.type, true);
      if (finished == QuotaStatus::Busy)
        continue;
      if (finished != QuotaStatus::Success)
        return finished;
      delete m_QuotaFiles[index];
      m_QuotaFiles[index] = nullptr;
      return QuotaStatus::Success;
    }
    return QuotaStatus::Busy;
  }
  if (request.operation == QuotaOperation::Sync)
    return flushQuotaLocked(request.type);
  if (request.operation == QuotaOperation::GetFormat) {
    response.format = Quota::OldFormat;
    return QuotaStatus::Success;
  }
  if (request.operation == QuotaOperation::Get) {
    LockGuard<Mutex> allocation(m_WriteLock);
    const auto* entry = m_Quota.table(request.type).find(request.id);
    response.record = entry ? entry->record : QuotaRecord();
    response.record.valid = Quota::Supported;
    return QuotaStatus::Success;
  }
  if (request.operation == QuotaOperation::Set) {
    if (m_bReadOnly)
      return QuotaStatus::ReadOnly;
    if ((static_cast<uint64_t>(request.id) + 1) * QuotaOld::RecordSize >
        m_QuotaFiles[index]->maximumFileSize())
      return QuotaStatus::Overflow;
    QuotaStatus status;
    {
      LockGuard<Mutex> allocation(m_WriteLock);
      status = m_Quota.table(request.type).set(request.id, request.record);
    }
    return status == QuotaStatus::Success ? flushQuotaLocked(request.type) : status;
  }
  return QuotaStatus::Unsupported;
}

bool Ext2Filesystem::closeQuotaFiles(bool discardOnFailure) {
  LockGuard<Mutex> control(m_QuotaControlLock);
  bool succeeded = true;
  const QuotaType types[] = {QuotaType::User, QuotaType::Group};
  for (auto type : types) {
    const size_t index = static_cast<size_t>(type);
    if (!m_QuotaFiles[index])
      continue;
    if (flushQuotaLocked(type) != QuotaStatus::Success) {
      succeeded = false;
      ERROR("Ext2: quota writeback failed at filesystem teardown");
      if (!discardOnFailure)
        continue;
    }
    m_QuotaFiles[index]->endQuota(type, false);
    delete m_QuotaFiles[index];
    m_QuotaFiles[index] = nullptr;
  }
  return succeeded;
}
