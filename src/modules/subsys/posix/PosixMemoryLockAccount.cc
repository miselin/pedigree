/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "PosixMemoryLockAccount.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"

#include "modules/system/vfs/MemoryMappedFile.h"

PosixMemoryLockAccount::PosixMemoryLockAccount(const PosixMemoryLockAccount& parent)
    : MemoryLockAccount() {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  m_Limit = parent.m_Limit;
}

bool PosixMemoryLockAccount::permitsTotalPages(size_t total, bool privileged) const {
  return privileged || total <= m_Limit.current / PhysicalMemoryManager::getPageSize();
}

PosixMemoryLockAccount::LimitStatus PosixMemoryLockAccount::setLimit(const LinuxRlimit64& limit,
                                                                     bool privileged) {
  if (limit.current > limit.maximum)
    return LimitStatus::Invalid;
  if (limit.maximum > m_Limit.maximum && !privileged)
    return LimitStatus::PermissionDenied;
  m_Limit = limit;
  return LimitStatus::Success;
}
