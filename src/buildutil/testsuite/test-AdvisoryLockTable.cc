/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "modules/subsys/posix/advisory-lock-table.h"
#include <gtest/gtest.h>

using namespace PosixAdvisory;

namespace {
Grant request(uint64_t owner, uintptr_t inode, Type type, int64_t first = 0,
              int64_t last = LastOffset, OwnerKind kind = OwnerKind::Process,
              Namespace name = Namespace::Record) {
  return {inode, owner, static_cast<int32_t>(owner), kind, name, type, {first, last}};
}

template <size_t Capacity>
struct Storage {
  Grant first[Capacity];
  Grant second[Capacity];
  Table table{first, second, Capacity};
};

void expectSame(const Grant& a, const Grant& b) {
  EXPECT_EQ(a.inode, b.inode);
  EXPECT_EQ(a.owner, b.owner);
  EXPECT_EQ(a.pid, b.pid);
  EXPECT_EQ(a.kind, b.kind);
  EXPECT_EQ(a.name, b.name);
  EXPECT_EQ(a.type, b.type);
  EXPECT_EQ(a.range.first, b.range.first);
  EXPECT_EQ(a.range.last, b.range.last);
}
}  // namespace

TEST(AdvisoryLockTable, NormalisesSignedOffsetsAndFutureEof) {
  Range range;
  ASSERT_EQ(normalise(1, -5, -4, 20, 99, range), RangeResult::Success);
  EXPECT_EQ(range.first, 11);
  EXPECT_EQ(range.last, 14);
  ASSERT_EQ(normalise(2, 8, 0, 0, 20, range), RangeResult::Success);
  EXPECT_EQ(range.first, 28);
  EXPECT_EQ(range.last, LastOffset);
  ASSERT_EQ(normalise(0, LastOffset, 1, 0, 0, range), RangeResult::Success);
  EXPECT_EQ(range.first, LastOffset);
  EXPECT_EQ(range.last, LastOffset);
  ASSERT_EQ(normalise(0, 1, LastOffset, 0, 0, range), RangeResult::Success);
  EXPECT_EQ(range.last, LastOffset);
}

TEST(AdvisoryLockTable, InvalidAndOverflowRangesPreserveOutput) {
  Range range{17, 29};
  EXPECT_EQ(normalise(9, 0, 0, 0, 0, range), RangeResult::Invalid);
  EXPECT_EQ(normalise(0, -1, 0, 0, 0, range), RangeResult::Invalid);
  EXPECT_EQ(normalise(0, 3, -4, 0, 0, range), RangeResult::Invalid);
  EXPECT_EQ(normalise(0, LastOffset, INT64_MIN, 0, 0, range), RangeResult::Invalid);
  EXPECT_EQ(normalise(1, 1, 0, LastOffset, 0, range), RangeResult::Overflow);
  EXPECT_EQ(normalise(0, LastOffset, 2, 0, 0, range), RangeResult::Overflow);
  EXPECT_EQ(normalise(2, 0, 0, 0, UINT64_MAX, range), RangeResult::Overflow);
  EXPECT_EQ(range.first, 17);
  EXPECT_EQ(range.last, 29);
}

TEST(AdvisoryLockTable, FullSplitAndConversionAreTransactional) {
  Storage<2> storage;
  auto& table = storage.table;
  ASSERT_EQ(table.apply(request(1, 10, Type::Write, 0, 99)), Result::Success);
  ASSERT_EQ(table.apply(request(2, 20, Type::Write)), Result::Success);
  Grant before[2] = {table.at(0), table.at(1)};
  EXPECT_EQ(table.apply(request(1, 10, Type::Unlock, 20, 39)), Result::Full);
  ASSERT_EQ(table.count(), 2U);
  expectSame(table.at(0), before[0]);
  expectSame(table.at(1), before[1]);
  EXPECT_EQ(table.apply(request(1, 10, Type::Read, 20, 39)), Result::Full);
  ASSERT_EQ(table.count(), 2U);
  expectSame(table.at(0), before[0]);
  expectSame(table.at(1), before[1]);
  EXPECT_TRUE(table.removeOwner(1, 10));
  ASSERT_EQ(table.count(), 1U);
  EXPECT_EQ(table.at(0).owner, 2U);
  EXPECT_TRUE(table.removeOwner(2));
  EXPECT_EQ(table.count(), 0U);
}

TEST(AdvisoryLockTable, MergesAtCapacityAndBridgesDifferentTypes) {
  Storage<3> storage;
  auto& table = storage.table;
  ASSERT_EQ(table.apply(request(1, 10, Type::Write, 0, 9)), Result::Success);
  ASSERT_EQ(table.apply(request(1, 10, Type::Read, 10, 19)), Result::Success);
  ASSERT_EQ(table.apply(request(1, 10, Type::Write, 20, 29)), Result::Success);
  ASSERT_EQ(table.count(), 3U);
  ASSERT_EQ(table.apply(request(1, 10, Type::Write, 10, 19)), Result::Success);
  ASSERT_EQ(table.count(), 1U);
  EXPECT_EQ(table.at(0).range.first, 0);
  EXPECT_EQ(table.at(0).range.last, 29);
  ASSERT_EQ(table.apply(request(1, 10, Type::Unlock, 10, 19)), Result::Success);
  ASSERT_EQ(table.count(), 2U);
  ASSERT_EQ(table.apply(request(1, 10, Type::Write, 10, LastOffset)), Result::Success);
  ASSERT_EQ(table.count(), 1U);
  EXPECT_EQ(table.at(0).range.last, LastOffset);
}

TEST(AdvisoryLockTable, OwnerAndNamespaceConflictRules) {
  Storage<8> storage;
  auto& table = storage.table;
  ASSERT_EQ(table.apply(request(1, 10, Type::Read, 0, 9)), Result::Success);
  ASSERT_EQ(table.apply(request(2, 10, Type::Read, 0, 9)), Result::Success);
  EXPECT_EQ(table.apply(request(1, 10, Type::Write, 0, 9)), Result::Conflict);
  EXPECT_EQ(table.count(), 2U);
  auto ofd = request(3, 10, Type::Write, 0, 9, OwnerKind::OpenDescription);
  EXPECT_EQ(table.apply(ofd), Result::Conflict);
  auto flock = ofd;
  flock.name = Namespace::Flock;
  ASSERT_EQ(table.apply(flock), Result::Success);
  Grant found;
  EXPECT_FALSE(table.conflict(flock, found));
  flock.owner = 4;
  ASSERT_TRUE(table.conflict(flock, found));
  EXPECT_EQ(found.owner, 3U);
  EXPECT_EQ(found.kind, OwnerKind::OpenDescription);
  EXPECT_EQ(found.name, Namespace::Flock);
  EXPECT_TRUE(table.removeOwner(1, 10));
  ASSERT_TRUE(table.conflict(ofd, found));
  EXPECT_EQ(found.owner, 2U);
}

TEST(AdvisoryLockTable, OfdUnlockQueryFindsOwnRecordsWithoutUnlocking) {
  Storage<8> storage;
  auto& table = storage.table;
  auto read = request(1, 10, Type::Read, 0, 9, OwnerKind::OpenDescription);
  auto write = request(1, 10, Type::Write, 20, 29, OwnerKind::OpenDescription);
  auto flock =
      request(1, 10, Type::Write, 0, LastOffset, OwnerKind::OpenDescription, Namespace::Flock);
  ASSERT_EQ(table.apply(read), Result::Success);
  ASSERT_EQ(table.apply(write), Result::Success);
  ASSERT_EQ(table.apply(flock), Result::Success);
  ASSERT_EQ(table.apply(request(2, 10, Type::Read, 0, 9)), Result::Success);
  Grant before[4] = {table.at(0), table.at(1), table.at(2), table.at(3)};
  auto query = request(1, 10, Type::Unlock, 5, 5, OwnerKind::OpenDescription);
  Grant found;
  EXPECT_FALSE(table.conflict(query, found));
  ASSERT_TRUE(table.query(query, found));
  expectSame(found, read);
  query.range = {10, 19};
  EXPECT_FALSE(table.query(query, found));
  query.range = {25, 25};
  ASSERT_TRUE(table.query(query, found));
  expectSame(found, write);
  query.owner = 3;
  EXPECT_FALSE(table.query(query, found));
  ASSERT_EQ(table.count(), 4U);
  for (size_t i = 0; i < 4; ++i)
    expectSame(table.at(i), before[i]);
  query.owner = 1;
  ASSERT_EQ(table.apply(query), Result::Success);
  EXPECT_FALSE(table.query(query, found));
  query.range = {26, 26};
  ASSERT_TRUE(table.query(query, found));
  EXPECT_EQ(found.range.first, 26);
  EXPECT_EQ(found.range.last, 29);
}

TEST(AdvisoryLockTable, DeadlockGraphTracksAllThreadsAndRemovedWaiters) {
  Storage<8> storage;
  auto& table = storage.table;
  ASSERT_EQ(table.apply(request(1, 10, Type::Write)), Result::Success);
  ASSERT_EQ(table.apply(request(2, 20, Type::Write)), Result::Success);
  ASSERT_EQ(table.apply(request(3, 30, Type::Write)), Result::Success);
  Grant unrelated = request(1, 40, Type::Write);
  Grant first = request(1, 20, Type::Write);
  Grant second = request(2, 30, Type::Write);
  const Grant* waiters[3] = {&unrelated, &first, &second};
  Grant candidate = request(3, 10, Type::Write);
  EXPECT_TRUE(table.wouldDeadlock(candidate, waiters, 3));
  waiters[1] = nullptr;
  EXPECT_FALSE(table.wouldDeadlock(candidate, waiters, 3));
  waiters[1] = &first;
  EXPECT_TRUE(table.removeOwner(1, 10));
  EXPECT_FALSE(table.wouldDeadlock(candidate, waiters, 3));
  ASSERT_EQ(table.apply(request(1, 10, Type::Write, 0, LastOffset, OwnerKind::OpenDescription)),
            Result::Success);
  EXPECT_FALSE(table.wouldDeadlock(candidate, waiters, 3));
}

TEST(AdvisoryLockTable, RangeTransactionsAgreeWithIndependentByteModel) {
  Storage<64> storage;
  auto& table = storage.table;
  // Each byte records unlocked/read/write for two independent process owners.
  unsigned model[2][16] = {};
  uint32_t random = 0x91bc08U;
  for (size_t step = 0; step < 1000; ++step) {
    random = random * 1664525U + 1013904223U;
    const unsigned owner = (random >> 24) & 1U;
    const unsigned first = (random >> 16) & 15U;
    const unsigned last = first + ((random >> 8) % (16 - first));
    const unsigned type = random % 3;
    bool blocked = false;
    for (unsigned i = first; i <= last; ++i) {
      blocked |= type && model[1 - owner][i] && (type == 2 || model[1 - owner][i] == 2);
    }
    const Type lockType = type == 0 ? Type::Unlock : (type == 1 ? Type::Read : Type::Write);
    ASSERT_EQ(table.apply(request(owner + 1, 10, lockType, first, last)),
              blocked ? Result::Conflict : Result::Success)
        << "step " << step;
    if (!blocked) {
      for (unsigned i = first; i <= last; ++i)
        model[owner][i] = type;
    }
    unsigned actual[2][16] = {};
    for (size_t g = 0; g < table.count(); ++g) {
      const Grant& grant = table.at(g);
      ASSERT_GE(grant.owner, 1U);
      ASSERT_LE(grant.owner, 2U);
      ASSERT_GE(grant.range.first, 0);
      ASSERT_LT(grant.range.last, 16);
      for (int64_t i = grant.range.first; i <= grant.range.last; ++i) {
        ASSERT_EQ(actual[grant.owner - 1][i], 0U);
        actual[grant.owner - 1][i] = grant.type == Type::Read ? 1 : 2;
      }
    }
    for (unsigned o = 0; o < 2; ++o) {
      for (unsigned i = 0; i < 16; ++i)
        EXPECT_EQ(actual[o][i], model[o][i]);
    }
  }
}
