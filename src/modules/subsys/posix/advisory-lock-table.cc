/* Copyright (c) 2026, Pedigree Developers. */
#include "advisory-lock-table.h"

namespace PosixAdvisory {
namespace {
bool sameOwner(const Grant& a, const Grant& b) {
  return a.owner == b.owner && a.kind == b.kind;
}

bool sameSet(const Grant& a, const Grant& b) {
  return a.inode == b.inode && a.name == b.name;
}

bool overlaps(const Range& a, const Range& b) {
  return a.first <= b.last && b.first <= a.last;
}

bool conflicts(const Grant& a, const Grant& b) {
  return a.type != Type::Unlock && b.type != Type::Unlock && sameSet(a, b) && !sameOwner(a, b) &&
         overlaps(a.range, b.range) && (a.type == Type::Write || b.type == Type::Write);
}
}  // namespace

RangeResult normalise(int whence, int64_t start, int64_t length, uint64_t position,
                      uint64_t fileSize, Range& result) {
  uint64_t base = 0;
  if (whence == 1) {
    base = position;
  } else if (whence == 2) {
    base = fileSize;
  } else if (whence != 0) {
    return RangeResult::Invalid;
  }
  int64_t first = 0;
  if (base > static_cast<uint64_t>(LastOffset) ||
      __builtin_add_overflow(static_cast<int64_t>(base), start, &first)) {
    return RangeResult::Overflow;
  }
  if (first < 0) {
    return RangeResult::Invalid;
  }
  int64_t last = LastOffset;
  if (length > 0) {
    if (__builtin_add_overflow(first, length - 1, &last)) {
      return RangeResult::Overflow;
    }
  } else if (length < 0) {
    last = first - 1;
    first += length;
    if (first < 0) {
      return RangeResult::Invalid;
    }
  }
  result = {first, last};
  return RangeResult::Success;
}

Table::Table(Grant* current, Grant* scratch, size_t capacity)
    : m_Current(current), m_Scratch(scratch), m_Capacity(capacity) {}

bool Table::conflict(const Grant& request, Grant& result) const {
  bool found = false;
  for (size_t i = 0; i < m_Count; ++i) {
    const Grant& grant = m_Current[i];
    if (conflicts(request, grant) && (!found || grant.range.first < result.range.first)) {
      result = grant;
      found = true;
    }
  }
  return found;
}

bool Table::query(const Grant& request, Grant& result) const {
  if (request.type != Type::Unlock)
    return conflict(request, result);
  // F_OFD_GETLK with F_UNLCK inspects this open description's own records.
  // Unlocking and ordinary conflict checks keep their separate semantics.
  if (request.kind != OwnerKind::OpenDescription || request.name != Namespace::Record)
    return false;
  bool found = false;
  for (size_t i = 0; i < m_Count; ++i) {
    const Grant& grant = m_Current[i];
    if (sameSet(request, grant) && sameOwner(request, grant) &&
        overlaps(request.range, grant.range) &&
        (!found || grant.range.first < result.range.first)) {
      result = grant;
      found = true;
    }
  }
  return found;
}

bool Table::append(const Grant& grant, size_t& count) {
  if (count == m_Capacity)
    return false;
  m_Scratch[count++] = grant;
  return true;
}

Result Table::apply(const Grant& request) {
  Grant blocked;
  if (conflict(request, blocked))
    return Result::Conflict;
  Grant replacement = request;
  if (replacement.type != Type::Unlock) {
    // Existing owner ranges are canonical; only the new range can connect
    // them. Expand it before staging so a merge also succeeds at capacity.
    for (size_t i = 0; i < m_Count;) {
      const Grant& old = m_Current[i];
      const bool touching =
          overlaps(replacement.range, old.range) ||
          (replacement.range.last != LastOffset && replacement.range.last + 1 == old.range.first) ||
          (old.range.last != LastOffset && old.range.last + 1 == replacement.range.first);
      if (sameSet(old, replacement) && sameOwner(old, replacement) &&
          old.type == replacement.type && touching &&
          (old.range.first < replacement.range.first || old.range.last > replacement.range.last)) {
        if (old.range.first < replacement.range.first)
          replacement.range.first = old.range.first;
        if (old.range.last > replacement.range.last)
          replacement.range.last = old.range.last;
        i = 0;
      } else {
        ++i;
      }
    }
  }
  size_t staged = 0;
  if (replacement.type != Type::Unlock && !append(replacement, staged))
    return Result::Full;
  for (size_t i = 0; i < m_Count; ++i) {
    const Grant& old = m_Current[i];
    if (!sameSet(old, replacement) || !sameOwner(old, replacement) ||
        !overlaps(old.range, replacement.range)) {
      if (!append(old, staged))
        return Result::Full;
      continue;
    }
    if (old.range.first < replacement.range.first) {
      Grant left = old;
      left.range.last = replacement.range.first - 1;
      if (!append(left, staged))
        return Result::Full;
    }
    if (old.range.last > replacement.range.last) {
      Grant right = old;
      right.range.first = replacement.range.last + 1;
      if (!append(right, staged))
        return Result::Full;
    }
  }
  Grant* previous = m_Current;
  m_Current = m_Scratch;
  m_Scratch = previous;
  m_Count = staged;
  return Result::Success;
}

bool Table::removeOwner(uint64_t owner, uintptr_t inode) {
  size_t kept = 0;
  for (size_t i = 0; i < m_Count; ++i) {
    const Grant& grant = m_Current[i];
    if (grant.owner != owner || (inode && grant.inode != inode)) {
      m_Current[kept++] = grant;
    }
  }
  const bool changed = kept != m_Count;
  m_Count = kept;
  return changed;
}

bool Table::wouldDeadlock(const Grant& request, const Grant* const* waiters,
                          size_t waiterCount) const {
  if (request.kind != OwnerKind::Process || request.name != Namespace::Record ||
      request.type == Type::Unlock || waiterCount > MaximumWaiters)
    return false;

  uint64_t reachable[MaximumWaiters + 1] = {request.owner};
  size_t reached = 1;
  for (size_t node = 0; node < reached; ++node) {
    for (size_t w = 0; w <= waiterCount; ++w) {
      const Grant* waiting = w == waiterCount ? &request : waiters[w];
      if (!waiting || waiting->owner != reachable[node] || waiting->kind != OwnerKind::Process ||
          waiting->name != Namespace::Record)
        continue;
      for (size_t g = 0; g < m_Count; ++g) {
        const Grant& blocker = m_Current[g];
        if (blocker.kind != OwnerKind::Process || !conflicts(*waiting, blocker))
          continue;
        if (blocker.owner == request.owner)
          return true;

        bool hasWait = false;
        for (size_t i = 0; i < waiterCount; ++i) {
          if (waiters[i] && waiters[i]->owner == blocker.owner &&
              waiters[i]->kind == OwnerKind::Process && waiters[i]->name == Namespace::Record) {
            hasWait = true;
            break;
          }
        }
        if (!hasWait)
          continue;
        bool seen = false;
        for (size_t i = 0; i < reached; ++i)
          seen |= reachable[i] == blocker.owner;
        if (!seen && reached < MaximumWaiters + 1)
          reachable[reached++] = blocker.owner;
      }
    }
  }
  return false;
}
}  // namespace PosixAdvisory
