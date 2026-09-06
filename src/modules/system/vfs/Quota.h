/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_VFS_QUOTA_H
#define PEDIGREE_VFS_QUOTA_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

enum class QuotaType { User, Group };
enum class QuotaOperation { Enable, Disable, GetFormat, Get, Set, Sync };
enum class QuotaStatus {
  Success,
  Unsupported,
  Invalid,
  NotEnabled,
  Busy,
  ReadOnly,
  NoMemory,
  IoError,
  Limit,
  Overflow,
  Permission,
  NoSpace,
  TooLarge
};

int EXPORTED_PUBLIC quotaError(QuotaStatus status);

namespace Quota {
constexpr uint32_t OldFormat = 1;
constexpr uint32_t BlockLimits = 1, Space = 2, InodeLimits = 4, Inodes = 8;
constexpr uint32_t BlockTime = 16, InodeTime = 32;
constexpr uint32_t Limits = BlockLimits | InodeLimits;
constexpr uint32_t Usage = Space | Inodes;
constexpr uint32_t Supported = Limits | Usage;
}  // namespace Quota

struct QuotaRecord {
  // Linux quota limits are in 1024-byte units; currentSpace is in bytes.
  uint64_t blockHardLimit = 0;
  uint64_t blockSoftLimit = 0;
  uint64_t currentSpace = 0;
  uint64_t inodeHardLimit = 0;
  uint64_t inodeSoftLimit = 0;
  uint64_t currentInodes = 0;
  uint64_t blockTime = 0;
  uint64_t inodeTime = 0;
  uint32_t valid = 0;
  uint32_t reserved = 0;
};

struct QuotaRequest {
  QuotaOperation operation = QuotaOperation::Get;
  QuotaType type = QuotaType::User;
  uint32_t id = 0;
  uint32_t format = 0;
  QuotaRecord record;
};

struct QuotaResponse {
  QuotaRecord record;
  uint32_t format = 0;
};

#endif
