/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

#include "PosixSubsystem.h"
#include "ipc-common.h"
#include "sysv-semaphore-syscalls.h"

namespace {
constexpr size_t MaximumSets = 128;
constexpr size_t MaximumSemaphores = 256;
constexpr size_t MaximumOperations = 128;
constexpr size_t MaximumUndoRecords = 4096;
constexpr size_t MaximumUndoOwners = 4096;
constexpr int MaximumValue = 32767;
constexpr int Create = 01000, Exclusive = 02000, NoWait = 04000, Undo = 0x1000;
constexpr int Remove = 0, SetMetadata = 1, Stat = 2, Info = 3;
constexpr int GetPid = 11, GetValue = 12, GetAll = 13, GetNegativeCount = 14;
constexpr int GetZeroCount = 15, SetValue = 16, SetAll = 17;
constexpr int SemStat = 18, SemInfo = 19, SemStatAny = 20;

struct Operation {
  uint16_t number;
  int16_t value;
  int16_t flags;
};

struct Metadata {
  PosixIpc::Permission permission;
  int64_t operationTime;
  uint64_t unused1;
  int64_t changeTime;
  uint64_t unused2;
  uint64_t count;
  uint64_t unused3;
  uint64_t unused4;
};

struct Information {
  int map, identifiers, semaphores, undoStructures, perSet, operations;
  int undoEntries, undoSize, maximumValue, maximumAdjustment;
};

static_assert(sizeof(Operation) == 6, "Linux sembuf ABI");
static_assert(sizeof(PosixIpc::Permission) == 48, "Linux ipc_perm ABI");
static_assert(sizeof(Metadata) == 104, "Linux amd64 semid_ds ABI");

struct SemaphoreValue {
  unsigned short value = 0;
  int pid = 0;
  unsigned negativeWaiters = 0;
  unsigned zeroWaiters = 0;
};

struct Set {
  Metadata metadata = {};
  SemaphoreValue semaphores[MaximumSemaphores];
  ConditionVariable changed;
  int id = 0;
  bool removed = false;
};

struct UndoGroup {
  size_t owners = 0;
};

struct UndoOwner {
  Thread* thread;
  UndoGroup* group;
  UndoOwner* next;
};

struct UndoRecord {
  UndoGroup* group;
  int id;
  unsigned number;
  int adjustment;
  UndoRecord* next;
};

// One lock also orders undo retirement against SETVAL, SETALL and IPC_RMID.
Mutex registryLock;
SharedPointer<Set> registry[MaximumSets];
unsigned sequences[MaximumSets] = {};
UndoRecord* undoRecords = nullptr;
size_t undoRecordCount = 0;
UndoOwner* undoOwners = nullptr;
size_t undoOwnerCount = 0;

SharedPointer<Set> findSet(int id) {
  if (id < 0)
    return {};
  const SharedPointer<Set>& set = registry[static_cast<unsigned>(id) % MaximumSets];
  return set && set->id == id ? set : SharedPointer<Set>();
}

UndoOwner* findOwner(Thread* thread) {
  for (UndoOwner* owner = undoOwners; owner; owner = owner->next)
    if (owner->thread == thread)
      return owner;
  return nullptr;
}

UndoGroup* createUndoGroup(Thread* thread) {
  UndoOwner* owner = findOwner(thread);
  if (owner)
    return owner->group;
  if (undoOwnerCount == MaximumUndoOwners) {
    SYSCALL_ERROR(OutOfMemory);
    return nullptr;
  }
  UndoGroup* group = new UndoGroup;
  group->owners = 1;
  undoOwners = new UndoOwner{thread, group, undoOwners};
  ++undoOwnerCount;
  return group;
}

UndoRecord* findUndo(UndoGroup* group, int id, unsigned number) {
  for (UndoRecord* record = undoRecords; record; record = record->next)
    if (record->group == group && record->id == id && record->number == number)
      return record;
  return nullptr;
}

void clearUndo(int id, int number = -1) {
  UndoRecord** link = &undoRecords;
  while (*link) {
    UndoRecord* record = *link;
    if (record->id == id && (number < 0 || record->number == static_cast<unsigned>(number))) {
      *link = record->next;
      delete record;
      --undoRecordCount;
    } else {
      link = &record->next;
    }
  }
}

// No values or undo entries are published until the entire vector can commit.
int perform(Set& set, Process* process, UndoGroup* group, const Operation* operations, size_t count,
            size_t& blocked) {
  unsigned short values[MaximumSemaphores];
  int adjustments[MaximumSemaphores] = {};
  bool adjusted[MaximumSemaphores] = {};
  for (size_t i = 0; i < set.metadata.count; ++i)
    values[i] = set.semaphores[i].value;
  for (size_t i = 0; i < count; ++i) {
    const Operation& operation = operations[i];
    int next = values[operation.number] + operation.value;
    if ((!operation.value && next) || next < 0) {
      blocked = i;
      return 1;
    }
    if (next > MaximumValue) {
      SYSCALL_ERROR(BadRange);
      return -1;
    }
    values[operation.number] = next;
    if ((operation.flags & Undo) && operation.value) {
      if (!adjusted[operation.number]) {
        UndoRecord* record = findUndo(group, set.id, operation.number);
        adjustments[operation.number] = record ? record->adjustment : 0;
        adjusted[operation.number] = true;
      }
      const int adjustment = adjustments[operation.number] - operation.value;
      if (adjustment < -MaximumValue - 1 || adjustment > MaximumValue) {
        SYSCALL_ERROR(BadRange);
        return -1;
      }
      adjustments[operation.number] = adjustment;
    }
  }
  size_t needed = 0;
  for (size_t i = 0; i < set.metadata.count; ++i)
    if (adjusted[i] && adjustments[i] && !findUndo(group, set.id, i))
      ++needed;
  if (needed > MaximumUndoRecords - undoRecordCount) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  for (size_t i = 0; i < set.metadata.count; ++i) {
    if (adjusted[i]) {
      UndoRecord* record = findUndo(group, set.id, i);
      if (record) {
        record->adjustment = adjustments[i];
      } else if (adjustments[i]) {
        undoRecords =
            new UndoRecord{group, set.id, static_cast<unsigned>(i), adjustments[i], undoRecords};
        ++undoRecordCount;
      }
    }
    set.semaphores[i].value = values[i];
  }
  for (size_t i = 0; i < count; ++i)
    set.semaphores[operations[i].number].pid = process->getId();
  UndoRecord** link = &undoRecords;
  while (*link) {
    UndoRecord* record = *link;
    if (!record->adjustment) {
      *link = record->next;
      delete record;
      --undoRecordCount;
    } else {
      link = &record->next;
    }
  }
  set.metadata.operationTime = Time::getTime();
  set.changed.broadcast();
  return 0;
}
}  // namespace

int posix_semget(int key, int count, int flags) {
  if (count < 0 || count > static_cast<int>(MaximumSemaphores)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  LockGuard<Mutex> guard(registryLock);
  size_t freeSlot = MaximumSets;
  for (size_t i = 0; i < MaximumSets; ++i) {
    if (!registry[i]) {
      if (freeSlot == MaximumSets)
        freeSlot = i;
      continue;
    }
    Set& set = *registry[i];
    if (!key || set.metadata.permission.key != key)
      continue;
    if ((flags & (Create | Exclusive)) == (Create | Exclusive)) {
      SYSCALL_ERROR(FileExists);
      return -1;
    }
    if (static_cast<unsigned>(count) > set.metadata.count) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    const unsigned requested = ((flags >> 6) | (flags >> 3) | flags) & 7;
    if (!PosixIpc::allowed(set.metadata.permission, requested)) {
      SYSCALL_ERROR(PermissionDenied);
      return -1;
    }
    return set.id;
  }
  if (key && !(flags & Create)) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  if (!count) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (freeSlot == MaximumSets) {
    SYSCALL_ERROR(NoSpaceLeftOnDevice);
    return -1;
  }
  SharedPointer<Set> set(new Set);
  auto& permission = set->metadata.permission;
  PosixIpc::initialize(permission, key, flags & 0777, sequences[freeSlot]++ & 0xffff);
  set->id = permission.sequence * MaximumSets + freeSlot;
  set->metadata.count = count;
  set->metadata.changeTime = Time::getTime();
  registry[freeSlot] = set;
  return set->id;
}

int posix_semop(int id, const void* operations, size_t count) {
  return posix_semtimedop(id, operations, count, nullptr);
}

int posix_semtimedop(int id, const void* operations, size_t count, const void* timeout) {
  if (id < 0 || !count) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (count > MaximumOperations) {
    SYSCALL_ERROR(TooBig);
    return -1;
  }
  Operation requested[MaximumOperations];
  if (!PosixSubsystem::copyFromUser(requested, operations, count * sizeof(Operation))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  Time::Timestamp remaining = Time::Infinity;
  Time::Timestamp deadline = Time::Infinity;
  if (timeout) {
    struct {
      int64_t seconds, nanoseconds;
    } duration;
    if (!PosixSubsystem::copyFromUser(&duration, timeout, sizeof(duration))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (duration.seconds < 0 || duration.nanoseconds < 0 ||
        duration.nanoseconds >= static_cast<int64_t>(Time::Multiplier::Second)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    const Time::Timestamp seconds = duration.seconds;
    remaining = seconds > (Time::Infinity - 1 - duration.nanoseconds) / Time::Multiplier::Second
                    ? Time::Infinity - 1
                    : seconds * Time::Multiplier::Second + duration.nanoseconds;
    const Time::Timestamp now = Time::getTicks();
    deadline = remaining >= Time::Infinity - now ? Time::Infinity - 1 : now + remaining;
  }
  LockGuard<Mutex> guard(registryLock);
  SharedPointer<Set> set = findSet(id);
  if (!set) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  unsigned access = 4;
  bool needsUndo = false;
  for (size_t i = 0; i < count; ++i) {
    if (requested[i].number >= set->metadata.count) {
      SYSCALL_ERROR(FileTooLarge);
      return -1;
    }
    if (requested[i].value)
      access = 2;
    needsUndo |= (requested[i].flags & Undo) && requested[i].value;
  }
  Process* process = PosixIpc::process();
  if (!PosixIpc::allowed(set->metadata.permission, access)) {
    SYSCALL_ERROR(PermissionDenied);
    return -1;
  }
  UndoGroup* group = nullptr;
  if (needsUndo) {
    group = createUndoGroup(Processor::information().getCurrentThread());
    if (!group)
      return -1;
  }
  for (;;) {
    if (set->removed) {
      SYSCALL_ERROR(IdentifierRemoved);
      return -1;
    }
    size_t blocked = 0;
    const int result = perform(*set, process, group, requested, count, blocked);
    if (result <= 0)
      return result;
    if (deadline != Time::Infinity) {
      const Time::Timestamp now = Time::getTicks();
      remaining = now >= deadline ? 0 : deadline - now;
    }
    if ((requested[blocked].flags & NoWait) || !remaining) {
      SYSCALL_ERROR(NoMoreProcesses);
      return -1;
    }
    SemaphoreValue& semaphore = set->semaphores[requested[blocked].number];
    unsigned& waiters =
        requested[blocked].value ? semaphore.negativeWaiters : semaphore.zeroWaiters;
    ++waiters;
    ConditionVariable::Error error = ConditionVariable::NoError;
    const bool awakened = set->changed.wait(registryLock, remaining, error);
    --waiters;
    if (set->removed) {
      SYSCALL_ERROR(IdentifierRemoved);
      return -1;
    }
    if (!awakened) {
      if (error == ConditionVariable::TimedOut)
        SYSCALL_ERROR(NoMoreProcesses);
      else
        SYSCALL_ERROR(Interrupted);
      return -1;
    }
  }
}

int posix_semctl(int id, int number, int command, uintptr_t argument) {
  if (id < 0 || command < Remove || (command > Info && command < GetPid) || command > SemStatAny) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (command == SetValue &&
      (static_cast<int>(argument) < 0 || static_cast<int>(argument) > MaximumValue)) {
    SYSCALL_ERROR(BadRange);
    return -1;
  }
  LockGuard<Mutex> guard(registryLock);
  if (command == Info || command == SemInfo) {
    int highest = 0, usedSets = 0, usedSemaphores = 0;
    for (size_t i = 0; i < MaximumSets; ++i) {
      if (registry[i]) {
        highest = i;
        ++usedSets;
        usedSemaphores += registry[i]->metadata.count;
      }
    }
    const Information information = {0,
                                     MaximumSets,
                                     MaximumSets * MaximumSemaphores,
                                     MaximumUndoRecords,
                                     MaximumSemaphores,
                                     MaximumOperations,
                                     MaximumUndoRecords,
                                     command == SemInfo ? usedSets : 20,
                                     MaximumValue,
                                     command == SemInfo ? usedSemaphores : MaximumValue};
    if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(argument), &information,
                                    sizeof(information))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return highest;
  }
  const bool byIndex = command == SemStat || command == SemStatAny;
  SharedPointer<Set> set = byIndex && id >= 0 && id < static_cast<int>(MaximumSets) ? registry[id]
                           : byIndex ? SharedPointer<Set>()
                                     : findSet(id);
  if (!set) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  Process* process = PosixIpc::process();
  auto& permission = set->metadata.permission;
  if (command == Remove || command == SetMetadata) {
    if (!PosixIpc::owner(permission)) {
      SYSCALL_ERROR(NotEnoughPermissions);
      return -1;
    }
  } else if (command != SemStatAny &&
             !PosixIpc::allowed(permission, command == SetValue || command == SetAll ? 2 : 4)) {
    SYSCALL_ERROR(PermissionDenied);
    return -1;
  }
  switch (command) {
    case Remove:
      set->removed = true;
      clearUndo(set->id);
      registry[static_cast<unsigned>(set->id) % MaximumSets].reset();
      set->changed.broadcast();
      return 0;
    case SetMetadata: {
      Metadata requested;
      if (!PosixSubsystem::copyFromUser(&requested, reinterpret_cast<void*>(argument),
                                        sizeof(requested))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      if (requested.permission.uid == 0xffffffffU || requested.permission.gid == 0xffffffffU) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      permission.uid = requested.permission.uid;
      permission.gid = requested.permission.gid;
      permission.mode = requested.permission.mode & 0777;
      set->metadata.changeTime = Time::getTime();
      return 0;
    }
    case Stat:
    case SemStat:
    case SemStatAny:
      if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(argument), &set->metadata,
                                      sizeof(set->metadata))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      return byIndex ? set->id : 0;
    case GetAll:
    case SetAll: {
      unsigned short values[MaximumSemaphores];
      const size_t bytes = set->metadata.count * sizeof(values[0]);
      if (command == GetAll) {
        for (size_t i = 0; i < set->metadata.count; ++i)
          values[i] = set->semaphores[i].value;
        if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(argument), values, bytes)) {
          SYSCALL_ERROR(BadAddress);
          return -1;
        }
      } else {
        if (!PosixSubsystem::copyFromUser(values, reinterpret_cast<void*>(argument), bytes)) {
          SYSCALL_ERROR(BadAddress);
          return -1;
        }
        for (size_t i = 0; i < set->metadata.count; ++i) {
          if (values[i] > MaximumValue) {
            SYSCALL_ERROR(BadRange);
            return -1;
          }
        }
        clearUndo(set->id);
        for (size_t i = 0; i < set->metadata.count; ++i) {
          set->semaphores[i].value = values[i];
          set->semaphores[i].pid = process->getId();
        }
        set->metadata.changeTime = Time::getTime();
        set->changed.broadcast();
      }
      return 0;
    }
    case GetPid:
    case GetValue:
    case GetNegativeCount:
    case GetZeroCount:
    case SetValue:
      break;
    default:
      SYSCALL_ERROR(InvalidArgument);
      return -1;
  }
  if (number < 0 || static_cast<unsigned>(number) >= set->metadata.count) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  SemaphoreValue& semaphore = set->semaphores[number];
  switch (command) {
    case GetPid:
      return semaphore.pid;
    case GetValue:
      return semaphore.value;
    case GetNegativeCount:
      return semaphore.negativeWaiters;
    case GetZeroCount:
      return semaphore.zeroWaiters;
    default:
      const int value = static_cast<int>(argument);
      clearUndo(set->id, number);
      semaphore.value = value;
      semaphore.pid = process->getId();
      set->metadata.changeTime = Time::getTime();
      set->changed.broadcast();
      return 0;
  }
}

bool posix_sem_clone(Thread* parent, Thread* child, bool shareUndo) {
  if (!shareUndo)
    return true;
  LockGuard<Mutex> guard(registryLock);
  if (findOwner(child))
    return true;
  const size_t needed = findOwner(parent) ? 1 : 2;
  if (needed > MaximumUndoOwners - undoOwnerCount) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  UndoGroup* group = createUndoGroup(parent);
  undoOwners = new UndoOwner{child, group, undoOwners};
  ++group->owners;
  ++undoOwnerCount;
  return true;
}

void posix_sem_thread_exit(Thread* thread) {
  LockGuard<Mutex> guard(registryLock);
  UndoOwner** ownerLink = &undoOwners;
  while (*ownerLink && (*ownerLink)->thread != thread)
    ownerLink = &(*ownerLink)->next;
  if (!*ownerLink)
    return;
  UndoOwner* owner = *ownerLink;
  UndoGroup* group = owner->group;
  *ownerLink = owner->next;
  delete owner;
  --undoOwnerCount;
  if (--group->owners)
    return;
  UndoRecord** link = &undoRecords;
  while (*link) {
    UndoRecord* record = *link;
    if (record->group != group) {
      link = &record->next;
      continue;
    }
    SharedPointer<Set> set = findSet(record->id);
    if (set && record->adjustment) {
      SemaphoreValue& semaphore = set->semaphores[record->number];
      const int value = semaphore.value + record->adjustment;
      // Exit must not block; Linux clips an adjustment which can no longer fit.
      semaphore.value = value < 0 ? 0 : value > MaximumValue ? MaximumValue : value;
      semaphore.pid = thread->getParent()->getId();
      set->metadata.operationTime = Time::getTime();
      set->changed.broadcast();
    }
    *link = record->next;
    delete record;
    --undoRecordCount;
  }
  delete group;
}
