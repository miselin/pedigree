/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/syscallError.h"

#include <array>
#include <cerrno>
#include <cstring>

#include "modules/system/ext2/Ext2Quota.h"
#include <gtest/gtest.h>

namespace {
void limit(QuotaTable& table, uint32_t id, uint64_t blocks, uint64_t inodes) {
  QuotaRecord record;
  record.valid = Quota::Limits;
  record.blockHardLimit = blocks;
  record.inodeHardLimit = inodes;
  ASSERT_EQ(table.set(id, record), QuotaStatus::Success);
}
}  // namespace

TEST(QuotaAccounting, OldFormatPreservesUnitsAndChecksNarrowing) {
  const uint32_t disk[] = {12, 0, 3, 19, 0, 7, 604800, 604800};
  QuotaRecord record;
  ASSERT_EQ(QuotaOld::decode(disk, sizeof(disk), record), QuotaStatus::Success);
  EXPECT_EQ(record.blockHardLimit, 12U);
  EXPECT_EQ(record.currentSpace, 3072U);
  EXPECT_EQ(record.inodeHardLimit, 19U);
  EXPECT_EQ(record.currentInodes, 7U);
  uint32_t encoded[8] = {};
  ASSERT_EQ(QuotaOld::encode(record, encoded, sizeof(encoded)), QuotaStatus::Success);
  EXPECT_EQ(std::memcmp(disk, encoded, sizeof(disk)), 0);
  record.currentSpace = 1025;
  ASSERT_EQ(QuotaOld::encode(record, encoded, sizeof(encoded)), QuotaStatus::Success);
  EXPECT_EQ(encoded[2], 2U);
  record.blockHardLimit = 0x100000000ULL;
  EXPECT_EQ(QuotaOld::encode(record, encoded, sizeof(encoded)), QuotaStatus::Overflow);
  EXPECT_EQ(encoded[0], 12U);
  EXPECT_EQ(QuotaOld::decode(disk, sizeof(disk) - 1, record), QuotaStatus::Invalid);
}

TEST(QuotaAccounting, EnableIncludesReservationsBeforeInodeAttachment) {
  Ext2QuotaLedger ledger;
  ASSERT_EQ(ledger.track(11, 1000, 42, 1024), QuotaStatus::Success);
  ASSERT_EQ(ledger.reserve(11, 2048), QuotaStatus::Success);
  QuotaTable loaded;
  limit(loaded, 1000, 4, 2);
  loaded.find(1000)->record.currentSpace = 999999;
  ASSERT_EQ(ledger.enable(QuotaType::User, loaded), QuotaStatus::Success);
  auto* entry = ledger.table(QuotaType::User).find(1000);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->record.currentSpace, 3072U);
  EXPECT_EQ(entry->record.currentInodes, 1U);
  ASSERT_EQ(ledger.reserve(11, 1024), QuotaStatus::Success);
  EXPECT_EQ(ledger.reserve(11, 1), QuotaStatus::Limit);
  ledger.refund(11, 2048);
  EXPECT_EQ(entry->record.currentSpace, 2048U);
  EXPECT_EQ(ledger.find(11)->bytes, 2048U);
}

TEST(QuotaAccounting, GroupFailureDoesNotChargeUserOrInode) {
  Ext2QuotaLedger ledger;
  QuotaTable user, group;
  limit(user, 1000, 10, 10);
  limit(group, 42, 1, 1);
  ASSERT_EQ(ledger.enable(QuotaType::User, user), QuotaStatus::Success);
  ASSERT_EQ(ledger.enable(QuotaType::Group, group), QuotaStatus::Success);
  ASSERT_EQ(ledger.create(11, 1000, 42), QuotaStatus::Success);
  ASSERT_EQ(ledger.reserve(11, 1024), QuotaStatus::Success);
  EXPECT_EQ(ledger.reserve(11, 1024), QuotaStatus::Limit);
  EXPECT_EQ(ledger.create(12, 1000, 42), QuotaStatus::Limit);
  EXPECT_EQ(ledger.find(12), nullptr);
  EXPECT_EQ(ledger.table(QuotaType::User).find(1000)->record.currentSpace, 1024U);
  EXPECT_EQ(ledger.table(QuotaType::User).find(1000)->record.currentInodes, 1U);
}

TEST(QuotaAccounting, TransferChecksBothDimensionsBeforeChangingOwners) {
  Ext2QuotaLedger ledger;
  ASSERT_EQ(ledger.track(11, 1000, 42, 2048), QuotaStatus::Success);
  QuotaTable user, group;
  limit(user, 1001, 10, 10);
  limit(group, 43, 1, 10);
  ASSERT_EQ(ledger.enable(QuotaType::User, user), QuotaStatus::Success);
  ASSERT_EQ(ledger.enable(QuotaType::Group, group), QuotaStatus::Success);
  EXPECT_EQ(ledger.transfer(11, 1001, 43), QuotaStatus::Limit);
  EXPECT_EQ(ledger.find(11)->uid, 1000U);
  EXPECT_EQ(ledger.find(11)->gid, 42U);
  EXPECT_EQ(ledger.table(QuotaType::User).find(1000)->record.currentSpace, 2048U);
  EXPECT_EQ(ledger.table(QuotaType::User).find(1001)->record.currentSpace, 0U);
  limit(ledger.table(QuotaType::Group), 43, 2, 1);
  ASSERT_EQ(ledger.transfer(11, 1001, 43), QuotaStatus::Success);
  EXPECT_EQ(ledger.table(QuotaType::User).find(1000)->record.currentInodes, 0U);
  EXPECT_EQ(ledger.table(QuotaType::Group).find(43)->record.currentSpace, 2048U);
  ledger.forget(11);
  EXPECT_EQ(ledger.table(QuotaType::Group).find(43)->record.currentSpace, 0U);
  EXPECT_EQ(ledger.table(QuotaType::Group).find(43)->record.currentInodes, 0U);
}

TEST(QuotaAccounting, QuotaFileExemptionAndDisableRetainOtherType) {
  Ext2QuotaLedger ledger;
  ASSERT_EQ(ledger.track(11, 1000, 42, 1024), QuotaStatus::Success);
  ASSERT_EQ(ledger.track(12, 1000, 42, 2048), QuotaStatus::Success);
  QuotaTable user, group;
  ASSERT_EQ(ledger.enable(QuotaType::User, user), QuotaStatus::Success);
  ASSERT_EQ(ledger.enable(QuotaType::Group, group), QuotaStatus::Success);
  ASSERT_EQ(ledger.exempt(12, true), QuotaStatus::Success);
  EXPECT_EQ(ledger.table(QuotaType::User).find(1000)->record.currentSpace, 1024U);
  EXPECT_EQ(ledger.table(QuotaType::Group).find(42)->record.currentInodes, 1U);
  ASSERT_EQ(ledger.reserve(12, 1024), QuotaStatus::Success);
  ledger.disable(QuotaType::User);
  ASSERT_EQ(ledger.exempt(12, false), QuotaStatus::Success);
  EXPECT_EQ(ledger.table(QuotaType::Group).find(42)->record.currentSpace, 4096U);
  EXPECT_FALSE(ledger.enabled(QuotaType::User));
  EXPECT_TRUE(ledger.enabled(QuotaType::Group));
}

TEST(QuotaAccounting, ReenableRecountsChangesMadeWhileDisabled) {
  Ext2QuotaLedger ledger;
  QuotaTable user;
  limit(user, 1000, 2, 1);
  ASSERT_EQ(ledger.enable(QuotaType::User, user), QuotaStatus::Success);
  ASSERT_EQ(ledger.create(11, 1000, 42), QuotaStatus::Success);
  ASSERT_EQ(ledger.reserve(11, 2048), QuotaStatus::Success);
  ledger.disable(QuotaType::User);
  ASSERT_EQ(ledger.reserve(11, 1024), QuotaStatus::Success);
  QuotaTable reloaded;
  limit(reloaded, 1000, 2, 1);
  ASSERT_EQ(ledger.enable(QuotaType::User, reloaded), QuotaStatus::Success);
  EXPECT_EQ(ledger.table(QuotaType::User).find(1000)->record.currentSpace, 3072U);
  EXPECT_EQ(ledger.reserve(11, 1024), QuotaStatus::Limit);
  ledger.refund(11, 2048);
  EXPECT_EQ(ledger.reserve(11, 1024), QuotaStatus::Success);
}

TEST(QuotaAccounting, UnsupportedLimitControlsLeaveRecordUnchanged) {
  QuotaTable table;
  limit(table, 1000, 9, 3);
  QuotaRecord change;
  change.valid = Quota::Space;
  change.currentSpace = 1024;
  EXPECT_EQ(table.set(1000, change), QuotaStatus::Unsupported);
  change.valid = Quota::BlockLimits;
  change.blockHardLimit = 8;
  change.blockSoftLimit = 4;
  EXPECT_EQ(table.set(1000, change), QuotaStatus::Unsupported);
  EXPECT_EQ(table.find(1000)->record.blockHardLimit, 9U);
  EXPECT_EQ(table.find(1000)->record.currentSpace, 0U);
}

TEST(QuotaAccounting, NativeMutexCleanupPreservesSelectedSyscallError) {
  Mutex mutex;
  syscallError(122);
  ASSERT_TRUE(mutex.acquire());
  EXPECT_EQ(errno, 122);
  EXPECT_FALSE(mutex.tryAcquire());
  EXPECT_EQ(errno, 122);
  mutex.release();
  EXPECT_EQ(errno, 122);
  ASSERT_TRUE(mutex.tryAcquire());
  EXPECT_EQ(errno, 122);
  mutex.release();
  EXPECT_EQ(errno, 122);
}
