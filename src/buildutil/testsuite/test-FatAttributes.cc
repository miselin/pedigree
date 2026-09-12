/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "modules/system/fat/FatFilesystem.h"
#include <gtest/gtest.h>

namespace {
class FatAttributeHarness : public FatFilesystem {
 public:
  using FatFilesystem::getFatDate;
  using FatFilesystem::getFatTime;
  using FatFilesystem::getUnixTimestamp;
  using FatFilesystem::writeEntryAttributes;

  void setVolumeId(FatType type, uint32_t id, uint8_t signature) {
    m_Type = type;
    if (type == FAT32) {
      m_Superblock32.BS_VolID = HOST_TO_LITTLE32(id);
      m_Superblock32.BS_BootSig = signature;
    } else {
      m_Superblock16.BS_VolID = HOST_TO_LITTLE32(id);
      m_Superblock16.BS_BootSig = signature;
    }
  }
};
}  // namespace

TEST(FatAttributes, CalendarHandlesLeapDayAndNonLeapCentury) {
  FatAttributeHarness filesystem;
  EXPECT_EQ(filesystem.getFatDate(951868799), (20U << 9) | (2U << 5) | 29U);
  EXPECT_EQ(filesystem.getFatTime(951868799), (23U << 11) | (59U << 5) | 29U);
  EXPECT_EQ(filesystem.getUnixTimestamp(HOST_TO_LITTLE16((23U << 11) | (59U << 5) | 29U),
                                        HOST_TO_LITTLE16((20U << 9) | (2U << 5) | 29U)),
            951868798U);
  EXPECT_EQ(filesystem.getUnixTimestamp(HOST_TO_LITTLE16((12U << 11) | (34U << 5) | 28U),
                                        HOST_TO_LITTLE16((120U << 9) | (3U << 5) | 1U)),
            4107587696U);
  EXPECT_EQ(filesystem.getFatDate(4107587696U), (120U << 9) | (3U << 5) | 1U);
}

TEST(FatAttributes, CalendarClampsUnrepresentableTimesAndRejectsMalformedDates) {
  FatAttributeHarness filesystem;
  EXPECT_EQ(filesystem.getFatDate(0), 33U);
  EXPECT_EQ(filesystem.getFatTime(0), 0U);
  EXPECT_EQ(filesystem.getFatDate(UINT64_MAX), (127U << 9) | (12U << 5) | 31U);
  EXPECT_EQ(filesystem.getFatTime(UINT64_MAX), (23U << 11) | (59U << 5) | 29U);
  EXPECT_EQ(filesystem.getUnixTimestamp(0, 0), 0U);
  EXPECT_EQ(filesystem.getUnixTimestamp(0, HOST_TO_LITTLE16((120U << 9) | (2U << 5) | 29U)), 0U);
  EXPECT_EQ(filesystem.getUnixTimestamp(HOST_TO_LITTLE16(24U << 11), HOST_TO_LITTLE16(33)), 0U);
  EXPECT_EQ(filesystem.getUnixTimestamp(0, HOST_TO_LITTLE16(33)), 315532800U);
}

TEST(FatAttributes, UpdatesTimesWithoutOverwritingBirthTimeOrOtherDirectoryFields) {
  FatAttributeHarness filesystem;
  File file(String("timestamps"), 1789182245, 951868799, 4107587696U, 0, nullptr, 0, nullptr);
  Dir entry = {};
  entry.DIR_CrtDate = HOST_TO_LITTLE16((19U << 9) | (12U << 5) | 31U);
  entry.DIR_CrtTime = HOST_TO_LITTLE16((23U << 11) | (59U << 5) | 29U);
  entry.DIR_CrtTimeTenth = 99;
  entry.DIR_Attr = ATTR_HIDDEN | ATTR_READONLY;
  entry.DIR_FileSize = HOST_TO_LITTLE32(12345);
  entry.DIR_FstClusLO = HOST_TO_LITTLE16(321);
  filesystem.writeEntryAttributes(&file, &entry);

  EXPECT_EQ(filesystem.getUnixTimestamp(entry.DIR_WrtTime, entry.DIR_WrtDate), 951868798U);
  EXPECT_EQ(filesystem.getUnixTimestamp(0, entry.DIR_LstAccDate), 1789171200U);
  EXPECT_EQ(filesystem.getUnixTimestamp(entry.DIR_CrtTime, entry.DIR_CrtDate), 946684798U);
  EXPECT_EQ(entry.DIR_CrtTimeTenth, 99U);
  EXPECT_EQ(entry.DIR_Attr, ATTR_HIDDEN | ATTR_READONLY);
  EXPECT_EQ(LITTLE_TO_HOST32(entry.DIR_FileSize), 12345U);
  EXPECT_EQ(LITTLE_TO_HOST16(entry.DIR_FstClusLO), 321U);
}

TEST(FatAttributes, InitialCreationRecordsOddSecondWithoutChangingSubsequentBirthTime) {
  FatAttributeHarness filesystem;
  File file(String("new"), 1789182245, 1789182245, 1789182245, 0, nullptr, 0, nullptr);
  Dir entry = {};
  filesystem.writeEntryAttributes(&file, &entry, true);
  EXPECT_EQ(filesystem.getUnixTimestamp(entry.DIR_CrtTime, entry.DIR_CrtDate), 1789182244U);
  EXPECT_EQ(entry.DIR_CrtTimeTenth, 100U);
  file.setCreationTime(4107587696U);
  filesystem.writeEntryAttributes(&file, &entry);
  EXPECT_EQ(filesystem.getUnixTimestamp(entry.DIR_CrtTime, entry.DIR_CrtDate), 1789182244U);
  EXPECT_EQ(entry.DIR_CrtTimeTenth, 100U);
}

TEST(FatAttributes, VolumeIdUsesTheSelectedExtendedBpbAndRequiresItsSignature) {
  FatAttributeHarness filesystem;
  String uuid;
  filesystem.setVolumeId(FAT16, 0x1234ABCD, 0x29);
  ASSERT_TRUE(filesystem.getUuid(uuid));
  EXPECT_STREQ(uuid.cstr(), "1234-ABCD");
  filesystem.setVolumeId(FAT32, 0x98760012, 0x28);
  ASSERT_TRUE(filesystem.getUuid(uuid));
  EXPECT_STREQ(uuid.cstr(), "9876-0012");
  filesystem.setVolumeId(FAT12, 0xFFFFFFFF, 0);
  EXPECT_FALSE(filesystem.getUuid(uuid));
  EXPECT_EQ(uuid.length(), 0U);
}
