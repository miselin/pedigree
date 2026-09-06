/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_POSIX_MEMORY_LOCK_ACCOUNT_H
#define PEDIGREE_POSIX_MEMORY_LOCK_ACCOUNT_H

#include "pedigree/kernel/processor/UserMemoryPolicy.h"

#include "linux-resource-abi.h"

class EXPORTED_PUBLIC PosixMemoryLockAccount final : public MemoryLockAccount {
 public:
  enum class LimitStatus { Success, Invalid, PermissionDenied };

  PosixMemoryLockAccount() = default;
  PosixMemoryLockAccount(const PosixMemoryLockAccount& parent);

  bool permitsTotalPages(size_t total, bool privileged) const override;

  // Limit mutation and mapping admission share the manager operation gate.
  LinuxRlimit64 limit() const {
    return m_Limit;
  }
  LimitStatus setLimit(const LinuxRlimit64& limit, bool privileged);

 private:
  PosixMemoryLockAccount& operator=(const PosixMemoryLockAccount&) = delete;
  LinuxRlimit64 m_Limit{1ULL << 24, 1ULL << 24};
};

#endif
