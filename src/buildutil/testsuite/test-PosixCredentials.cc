#define PEDIGREE_EXTERNAL_SOURCE 1
#include "modules/subsys/posix/credential-state.h"
#include <gtest/gtest.h>

using namespace PosixCredentials;
namespace {
Snapshot ordinary() {
  Snapshot state;
  state.ruid = 100;
  state.euid = 101;
  state.suid = 102;
  state.rgid = 200;
  state.egid = 201;
  state.sgid = 202;
  return state;
}
void sameIdentity(const Snapshot& a, const Snapshot& b) {
  EXPECT_EQ(a.ruid, b.ruid);
  EXPECT_EQ(a.euid, b.euid);
  EXPECT_EQ(a.suid, b.suid);
  EXPECT_EQ(a.rgid, b.rgid);
  EXPECT_EQ(a.egid, b.egid);
  EXPECT_EQ(a.sgid, b.sgid);
  EXPECT_EQ(a.dumpable, b.dumpable);
  EXPECT_EQ(a.generation, b.generation);
}
}  // namespace

TEST(PosixCredentials, RejectedTupleHasNoPartialPublication) {
  const auto old = ordinary();
  Snapshot next;
  uint32_t fs;
  EXPECT_EQ(prepare(old, Change::SetResUid, 101, 102, 900, 101, next, fs), Status::Denied);
  sameIdentity(old, next);
  EXPECT_EQ(fs, 101U);
  EXPECT_EQ(prepare(old, Change::SetReGid, 202, 200, UINT32_MAX, 201, next, fs), Status::Denied);
  sameIdentity(old, next);
}
TEST(PosixCredentials, PrivilegedSetUidDropsAllIdsAndRepeatedMuslCallSucceeds) {
  Snapshot root, first, repeated;
  uint32_t fs, repeatedFs;
  ASSERT_EQ(prepare(root, Change::SetUid, 70001, UINT32_MAX, UINT32_MAX, 0, first, fs),
            Status::Success);
  EXPECT_EQ(first.ruid, 70001U);
  EXPECT_EQ(first.euid, 70001U);
  EXPECT_EQ(first.suid, 70001U);
  EXPECT_EQ(fs, 70001U);
  EXPECT_FALSE(first.dumpable);
  ASSERT_EQ(prepare(first, Change::SetUid, 70001, UINT32_MAX, UINT32_MAX, fs, repeated, repeatedFs),
            Status::Success);
  sameIdentity(first, repeated);
}
TEST(PosixCredentials, RepeatedThreeDistinctIdsRemainAdmissibleAfterDrop) {
  Snapshot root, first, repeated;
  uint32_t fs;
  ASSERT_EQ(prepare(root, Change::SetResUid, 100, 101, 102, 0, first, fs), Status::Success);
  ASSERT_EQ(prepare(first, Change::SetResUid, 100, 101, 102, 0, repeated, fs), Status::Success);
  EXPECT_EQ(repeated.ruid, 100U);
  EXPECT_EQ(repeated.euid, 101U);
  EXPECT_EQ(repeated.suid, 102U);
  EXPECT_EQ(fs, 101U);
}
TEST(PosixCredentials, UnprivilegedSingleSetterPreservesRealAndSaved) {
  auto old = ordinary();
  Snapshot next;
  uint32_t fs;
  ASSERT_EQ(prepare(old, Change::SetUid, 102, UINT32_MAX, UINT32_MAX, 101, next, fs),
            Status::Success);
  EXPECT_EQ(next.ruid, 100U);
  EXPECT_EQ(next.euid, 102U);
  EXPECT_EQ(next.suid, 102U);
  EXPECT_EQ(prepare(old, Change::SetUid, 101, UINT32_MAX, UINT32_MAX, 101, next, fs),
            Status::Denied);
}
TEST(PosixCredentials, PairSetterUsesOldRealForSavedRule) {
  const auto old = ordinary();
  Snapshot next;
  uint32_t fs;
  ASSERT_EQ(prepare(old, Change::SetReUid, UINT32_MAX, 100, UINT32_MAX, 101, next, fs),
            Status::Success);
  EXPECT_EQ(next.suid, 102U);
  ASSERT_EQ(prepare(old, Change::SetReUid, 100, 101, UINT32_MAX, 101, next, fs), Status::Success);
  EXPECT_EQ(next.suid, 101U);
}
TEST(PosixCredentials, AllSentinelsPreserveFsButExplicitEffectiveRepairsIt) {
  const auto old = ordinary();
  Snapshot next;
  uint32_t fs;
  ASSERT_EQ(prepare(old, Change::SetResUid, UINT32_MAX, UINT32_MAX, UINT32_MAX, 102, next, fs),
            Status::Success);
  sameIdentity(old, next);
  EXPECT_EQ(fs, 102U);
  ASSERT_EQ(prepare(old, Change::SetResUid, UINT32_MAX, 101, UINT32_MAX, 102, next, fs),
            Status::Success);
  EXPECT_EQ(fs, 101U);
  EXPECT_FALSE(next.dumpable);
  EXPECT_EQ(next.generation, 1U);
}
TEST(PosixCredentials, RealOnlyChangeDoesNotResetDumpability) {
  const auto old = ordinary();
  Snapshot next;
  uint32_t fs;
  ASSERT_EQ(prepare(old, Change::SetResUid, 102, UINT32_MAX, UINT32_MAX, 101, next, fs),
            Status::Success);
  EXPECT_TRUE(next.dumpable);
  EXPECT_EQ(next.generation, 1U);
}
TEST(PosixCredentials, MaximumNumericIdIsNotSignedOrAccountLimited) {
  Snapshot root, next;
  uint32_t fs;
  ASSERT_EQ(prepare(root, Change::SetGid, UINT32_MAX - 1, UINT32_MAX, UINT32_MAX, 0, next, fs),
            Status::Success);
  EXPECT_EQ(next.egid, UINT32_MAX - 1);
  EXPECT_EQ(fs, UINT32_MAX - 1);
  EXPECT_EQ(prepare(root, Change::SetUid, UINT32_MAX, 0, 0, 0, next, fs), Status::Invalid);
}
