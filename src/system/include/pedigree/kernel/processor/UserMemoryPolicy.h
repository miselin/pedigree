/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESSOR_USERMEMORYPOLICY_H
#define PEDIGREE_KERNEL_PROCESSOR_USERMEMORYPOLICY_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Pointers.h"

class VirtualAddressSpace;

enum class MemoryLockMode { None, Eager, OnFault };
enum class PopulationStatus { Success, Inaccessible, NoMemory, IoError };
enum class MemoryLockStatus {
  Success,
  InvalidRange,
  Unmapped,
  Unsupported,
  NoMemory,
  LockLimit,
  PopulationNoMemory,
  PopulationInaccessible,
  PopulationIoError,
};

struct MemoryLockCharge {
  size_t managedPages = 0;
  size_t rawPages = 0;
};

/** The address-space operation gate protects both policy and accounting. */
class EXPORTED_PUBLIC MemoryLockAccount {
 public:
  virtual ~MemoryLockAccount() = default;
  virtual bool permitsTotalPages(size_t total, bool privileged) const = 0;
  MemoryLockCharge charge() const {
    return m_Charge;
  }
  MemoryLockMode futureMode() const {
    return m_Future;
  }
  void publish(MemoryLockCharge charge, MemoryLockMode future) {
    m_Charge = charge;
    m_Future = future;
  }

 private:
  MemoryLockCharge m_Charge;
  MemoryLockMode m_Future = MemoryLockMode::None;
};

/** Every plan lives inside one operation. Commit has no recoverable failure;
 * population can fail after lock metadata has been published. */
struct MemoryLockRange {
  uintptr_t base;
  size_t length;
};

class EXPORTED_PUBLIC PreparedMemoryLock {
 public:
  virtual ~PreparedMemoryLock() = default;
  virtual size_t removedPages() const = 0;
  virtual size_t addedPages() const = 0;
  virtual size_t coveredPages() const = 0;
  virtual size_t eligiblePages() const = 0;
  virtual size_t removedRangeCount() const {
    return 0;
  }
  virtual const MemoryLockRange* removedRanges() const {
    return nullptr;
  }
  virtual void commit() = 0;
  virtual PopulationStatus populate() = 0;
};

/** Permanent VFS bridge; neither it nor an account owns a Process or Thread. */
class EXPORTED_PUBLIC UserMemoryPolicy {
 public:
  virtual ~UserMemoryPolicy() = default;
  virtual bool callerHasMemoryLockPrivilege() const = 0;
  virtual bool overlapsManagedMemory(VirtualAddressSpace& space, uintptr_t base,
                                     size_t length) const = 0;
  virtual void enterOperation() = 0;
  virtual void leaveOperation() = 0;
};

class EXPORTED_PUBLIC UserMemoryOperation {
 public:
  explicit UserMemoryOperation(VirtualAddressSpace& space);
  ~UserMemoryOperation();
  bool privileged() const {
    return m_Privileged;
  }

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(UserMemoryOperation);
  Uninterruptible m_EventDeferral;
  TerminationDeferral m_TerminationDeferral;
  UserMemoryPolicy* m_Policy;
  bool m_Privileged;
};

struct UserRegion {
  enum class Kind { Heap, Stack };
  uint64_t id;
  uintptr_t base;
  size_t length;
  Kind kind;
  bool privateWritable;
};

/** Nonowning raw allocation inventory. Core retains physical-page ownership. */
class EXPORTED_PUBLIC RawUserMemory {
 public:
  explicit RawUserMemory(VirtualAddressSpace& space);
  ~RawUserMemory();
  uint64_t nextRegionId();
  bool completeInventory() const;
  void setCompleteInventory(bool complete);
  bool hasLockedMemory(uintptr_t base, size_t length) const;
  MemoryLockStatus prepareChange(const UserRegion* previous, const UserRegion* replacement,
                                 UniquePointer<PreparedMemoryLock>& result);
  MemoryLockStatus prepareLocks(uintptr_t base, size_t length, MemoryLockMode mode,
                                UniquePointer<PreparedMemoryLock>& result);
  MemoryLockStatus prepareAllLocks(MemoryLockMode mode, UniquePointer<PreparedMemoryLock>& result);
  MemoryLockStatus prepareReplacement(uintptr_t base, size_t length,
                                      UniquePointer<PreparedMemoryLock>& result);
  MemoryLockStatus cloneInto(RawUserMemory& target);
  size_t retireRegion(uint64_t id);
  void clear();

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(RawUserMemory);
  struct State;
  class Plan;
  VirtualAddressSpace& m_Space;
  UniquePointer<State> m_State;
};

#endif
