/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_ADVISORY_LOCK_TABLE_H
#define POSIX_ADVISORY_LOCK_TABLE_H

#include <stddef.h>
#include <stdint.h>

namespace PosixAdvisory {
constexpr size_t MaximumGrants = 4096;
constexpr size_t MaximumWaiters = 256;
constexpr int64_t LastOffset = INT64_MAX;

enum class OwnerKind { Process, OpenDescription };
enum class Namespace { Record, Flock };
enum class Type { Read, Write, Unlock };
enum class Result { Success, Conflict, Full };
enum class RangeResult { Success, Invalid, Overflow };

struct Range {
  int64_t first = 0;
  int64_t last = LastOffset;
};

struct Grant {
  uintptr_t inode = 0;
  uint64_t owner = 0;
  int32_t pid = 0;
  OwnerKind kind = OwnerKind::Process;
  Namespace name = Namespace::Record;
  Type type = Type::Unlock;
  Range range;
};

RangeResult normalise(int whence, int64_t start, int64_t length, uint64_t position,
                      uint64_t fileSize, Range& result);

/** Caller supplies two equal-capacity buffers and serializes every operation. */
class Table {
 public:
  Table(Grant* current, Grant* scratch, size_t capacity);
  bool conflict(const Grant& request, Grant& result) const;
  bool query(const Grant& request, Grant& result) const;
  Result apply(const Grant& request);
  bool removeOwner(uint64_t owner, uintptr_t inode = 0);
  bool wouldDeadlock(const Grant& request, const Grant* const* waiters, size_t waiterCount) const;
  size_t count() const {
    return m_Count;
  }
  const Grant& at(size_t index) const {
    return m_Current[index];
  }

 private:
  bool append(const Grant& grant, size_t& count);
  Grant* m_Current;
  Grant* m_Scratch;
  const size_t m_Capacity;
  size_t m_Count = 0;
};
}  // namespace PosixAdvisory
#endif
