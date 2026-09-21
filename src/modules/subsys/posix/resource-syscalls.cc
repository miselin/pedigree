/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/syscallError.h"

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "linux-resource-abi.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "system-syscalls.h"
#include <sys/resource.h>

namespace {
bool getRlimitValue(int resource, struct rlimit& result) {
  result = {};
  switch (resource) {
    case RLIMIT_CPU:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_FSIZE:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_DATA:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_STACK:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_CORE:
      result.rlim_cur = 0;
      result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_RSS:
      result.rlim_cur = result.rlim_max = 1ULL << 48ULL;
      break;
    case RLIMIT_NPROC:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_NOFILE:
      result.rlim_cur = result.rlim_max = 16384;
      break;
    case RLIMIT_MEMLOCK: {
      GRAB_POSIX_SUBSYSTEM(false);
      MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
      const auto limit = pSubsystem->memoryLockAccount().limit();
      result.rlim_cur = limit.current;
      result.rlim_max = limit.maximum;
      break;
    }
    case RLIMIT_AS:
      result.rlim_cur = result.rlim_max = 1ULL << 48ULL;
      break;
    case RLIMIT_LOCKS:
      result.rlim_cur = result.rlim_max = 1024;
      break;
    case RLIMIT_SIGPENDING:
      result.rlim_cur = result.rlim_max = 16;
      break;
    case RLIMIT_MSGQUEUE:
      result.rlim_cur = result.rlim_max = 0x100000;
      break;
    case RLIMIT_NICE:
      result.rlim_cur = result.rlim_max = 1;
      break;
    case RLIMIT_RTPRIO:
      result.rlim_cur = result.rlim_max = 0;
      break;
#ifdef RLIMIT_RTTIME
    case RLIMIT_RTTIME:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
#endif
    default:
      SYSCALL_ERROR(InvalidArgument);
      return false;
  }

  return true;
}

int changeMemoryLimit(const LinuxRlimit64& limit, LinuxRlimit64* previous) {
  GRAB_POSIX_SUBSYSTEM(-1);
  auto* thread = Processor::information().getCurrentThread();
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  auto& account = pSubsystem->memoryLockAccount();
  const auto old = account.limit();
  const auto status = account.setLimit(limit, thread->getParent()->getEffectiveUserId() == 0);
  if (status == PosixMemoryLockAccount::LimitStatus::Invalid) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (status == PosixMemoryLockAccount::LimitStatus::PermissionDenied) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  if (previous)
    *previous = old;
  return 0;
}
}  // namespace

int posix_getrlimit(int resource, struct rlimit* rlim) {
  SC_NOTICE("getrlimit(" << Dec << resource << ")");

  struct rlimit result = {};
  if (!getRlimitValue(resource, result)) {
    SC_NOTICE(" -> unsupported resource");
    return -1;
  }

  if (!PosixSubsystem::copyToUser(rlim, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  SC_NOTICE(" -> cur = " << result.rlim_cur);
  SC_NOTICE(" -> max = " << result.rlim_max);
  return 0;
}

int posix_setrlimit(int resource, const struct rlimit* rlim) {
  SC_NOTICE("setrlimit(" << Dec << resource << ")");

  if (resource == RLIMIT_MEMLOCK) {
    struct rlimit limit;
    if (!PosixSubsystem::copyFromUser(&limit, rlim, sizeof(limit))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return changeMemoryLimit(
        {static_cast<uint64_t>(limit.rlim_cur), static_cast<uint64_t>(limit.rlim_max)}, nullptr);
  }

  struct rlimit current = {};
  if (!getRlimitValue(resource, current)) {
    return -1;
  }

  (void)rlim;
  SYSCALL_ERROR(Unimplemented);
  return -1;
}

int posix_prlimit64(int pid, int resource, const LinuxRlimit64* newLimit, LinuxRlimit64* oldLimit) {
  SC_NOTICE("prlimit64(" << Dec << pid << ", " << resource << ")");

  Process* current = Processor::information().getCurrentThread()->getParent();
  Scheduler::ProcessLease targetLease;
  if (pid < 0) {
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }
  if (pid && static_cast<size_t>(pid) != current->getUserspaceId()) {
    if (!Scheduler::instance().acquireProcessByUserspaceId(targetLease, static_cast<size_t>(pid)) ||
        targetLease->getType() != Process::Posix) {
      SYSCALL_ERROR(NoSuchProcess);
      return -1;
    }
    // Cross-process access needs a coherent credential snapshot shared with
    // credential mutation. Until then, only the caller's limits are exposed.
    SYSCALL_ERROR(Unimplemented);
    return -1;
  }

  struct rlimit currentLimit = {};
  if (!getRlimitValue(resource, currentLimit)) {
    return -1;
  }

  LinuxRlimit64 previous = {static_cast<uint64_t>(currentLimit.rlim_cur),
                            static_cast<uint64_t>(currentLimit.rlim_max)};
  if (newLimit) {
    if (resource != RLIMIT_MEMLOCK) {
      SYSCALL_ERROR(Unimplemented);
      return -1;
    }
    LinuxRlimit64 limit;
    if (!PosixSubsystem::copyFromUser(&limit, newLimit, sizeof(limit))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (changeMemoryLimit(limit, &previous))
      return -1;
  }

  if (oldLimit) {
    // Linux commits the new limit before copying the old one to userspace.
    if (!PosixSubsystem::copyToUser(oldLimit, &previous, sizeof(previous))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  return 0;
}
