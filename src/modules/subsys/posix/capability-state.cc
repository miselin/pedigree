/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include "sandbox-state.h"
#include "user-namespace.h"

namespace {
constexpr uint32_t NoRoot = 1, NoSetuidFixup = 4, KeepCaps = 16, KeepCapsLocked = 32;
constexpr uint32_t NoAmbientRaise = 64;
Thread& current() {
  return *Processor::information().getCurrentThread();
}
int invalid() {
  SYSCALL_ERROR(InvalidArgument);
  return -1;
}
int denied() {
  SYSCALL_ERROR(NotEnoughPermissions);
  return -1;
}
bool publish(Thread& task, const PosixTaskCredentials& value) {
  auto next = TaskCredentialsRef::tryAllocate(value);
  if (!next) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  return posix_sandbox_set_credentials(task, next);
}
}  // namespace

int posix_capability_prctl(int option, unsigned long arg2, unsigned long arg3, unsigned long arg4,
                           unsigned long arg5) {
  TerminationDeferral lifetime;
  auto next = posix_task_credentials(current());
  if (arg4 || arg5 || (option != 47 && arg3)) {
    return invalid();
  }
  int result = 0;
  bool changed = false;
  switch (option) {
    case 7:  // PR_GET_KEEPCAPS
      if (arg2) {
        return invalid();
      }
      result = bool(next.secureBits & KeepCaps);
      break;
    case 8:  // PR_SET_KEEPCAPS
      if (arg2 > 1) {
        return invalid();
      }
      if (next.secureBits & KeepCapsLocked) {
        return denied();
      }
      next.secureBits = (next.secureBits & ~KeepCaps) | (arg2 ? KeepCaps : 0);
      changed = true;
      break;
    case 23:  // PR_CAPBSET_READ
      if (arg2 > PosixCapabilities::Last) {
        return invalid();
      }
      result = bool(next.bounding & (uint64_t(1) << arg2));
      break;
    case 24:  // PR_CAPBSET_DROP
      if (arg2 > PosixCapabilities::Last) {
        return invalid();
      }
      if (!(next.effective & (uint64_t(1) << PosixCapabilities::Setpcap))) {
        return denied();
      }
      next.bounding &= ~(uint64_t(1) << arg2);
      changed = true;
      break;
    case 27:  // PR_GET_SECUREBITS
      if (arg2) {
        return invalid();
      }
      result = next.secureBits;
      break;
    case 28:  // PR_SET_SECUREBITS
      if (arg2 & ~0xffUL) {
        return invalid();
      }
      if (!(next.effective & (uint64_t(1) << PosixCapabilities::Setpcap))) {
        return denied();
      }
      for (unsigned bit = 1; bit < 8; bit += 2) {
        if ((next.secureBits & (1U << bit)) && ((next.secureBits ^ arg2) & (3U << (bit - 1)))) {
          return denied();
        }
      }
      next.secureBits = arg2;
      changed = true;
      break;
    case 47:  // PR_CAP_AMBIENT
      if (arg2 == 4) {
        if (arg3) {
          return invalid();
        }
        next.ambient = 0;
        changed = true;
      } else {
        if (arg3 > PosixCapabilities::Last) {
          return invalid();
        }
        const uint64_t bit = uint64_t(1) << arg3;
        switch (arg2) {
          case 1:
            result = bool(next.ambient & bit);
            break;
          case 2:
            if ((next.secureBits & NoAmbientRaise) || !(next.permitted & bit) ||
                !(next.inheritable & bit)) {
              return denied();
            }
            next.ambient |= bit;
            changed = true;
            break;
          case 3:
            next.ambient &= ~bit;
            changed = true;
            break;
          default:
            return invalid();
        }
      }
      break;
    default:
      return invalid();
  }
  if (changed && !publish(current(), next)) {
    return -1;
  }
  current().setErrno(0);
  return result;
}

bool posix_capabilities_exec(Thread& task, uint32_t globalUid) {
  TerminationDeferral lifetime;
  auto next = posix_task_credentials(task);
  const uint64_t oldPermitted = next.permitted;
  uint32_t root = 0;
  const bool rootMapped = !next.userNamespace || next.userNamespace->toGlobal(false, 0, root);
  if (!(next.secureBits & NoRoot) && rootMapped && globalUid == root) {
    next.permitted = next.inheritable | next.bounding;
    if (posix_no_new_privs()) {
      next.permitted &= oldPermitted;
    }
    next.effective = next.permitted;
  } else {
    // This filesystem does not support executable file capabilities or set-ID transitions.
    next.permitted = next.effective = next.ambient;
  }
  next.secureBits &= ~KeepCaps;
  return publish(task, next);
}

bool posix_capabilities_uid_change(Thread& task, uint32_t oldReal, uint32_t oldEffective,
                                   uint32_t oldSaved, uint32_t newReal, uint32_t newEffective,
                                   uint32_t newSaved, uint32_t namespaceRoot) {
  if (oldReal == newReal && oldEffective == newEffective && oldSaved == newSaved) {
    return true;
  }
  auto stored = posix_sandbox_credentials(task);
  PosixTaskCredentials next;
  if (stored) {
    next = *stored;
  } else if (!oldEffective) {
    next.permitted = next.effective = PosixCapabilities::All;
  }
  if (next.secureBits & NoSetuidFixup) {
    return true;
  }
  if ((oldReal == namespaceRoot || oldEffective == namespaceRoot || oldSaved == namespaceRoot) &&
      newReal != namespaceRoot && newEffective != namespaceRoot && newSaved != namespaceRoot) {
    if (!(next.secureBits & KeepCaps)) {
      next.permitted = 0;
    }
    next.ambient = 0;
  }
  if (oldEffective == namespaceRoot && newEffective != namespaceRoot) {
    next.effective = 0;
  } else if (oldEffective != namespaceRoot && newEffective == namespaceRoot) {
    next.effective = next.permitted;
  }
  return publish(task, next);
}

bool posix_capabilities_fsuid_change(Thread& task, uint32_t oldUid, uint32_t newUid,
                                     uint32_t namespaceRoot) {
  if (oldUid == newUid) {
    return true;
  }
  auto stored = posix_sandbox_credentials(task);
  PosixTaskCredentials next;
  if (stored) {
    next = *stored;
  } else if (!oldUid) {
    next.permitted = next.effective = PosixCapabilities::All;
  }
  if (next.secureBits & NoSetuidFixup) {
    return true;
  }
  constexpr uint64_t filesystemCaps = (uint64_t(1) << 0) | (uint64_t(1) << 1) | (uint64_t(1) << 2) |
                                      (uint64_t(1) << 3) | (uint64_t(1) << 4) | (uint64_t(1) << 9) |
                                      (uint64_t(1) << 27);
  if (oldUid == namespaceRoot && newUid != namespaceRoot) {
    next.effective &= ~filesystemCaps;
  } else if (oldUid != namespaceRoot && newUid == namespaceRoot) {
    next.effective |= next.permitted & filesystemCaps;
  } else {
    return true;
  }
  return publish(task, next);
}
