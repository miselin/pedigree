/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/time/Time.h"

#include <cstdint>
#include <vector>

#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/File.h"
#include <gtest/gtest.h>

namespace {
constexpr size_t kPageSize = TargetInfo::getPageSize();

class MetadataWriteFile final : public File {
 public:
  explicit MetadataWriteFile(size_t readableBlocks = 3)
      : File(String("metadata"), 11, 22, 33, 1, nullptr, 2 * kPageSize, nullptr),
        m_Data(readableBlocks * kPageSize) {}

  bool rejectPreparation = false;

 protected:
  bool prepareWrite(uint64_t location, uint64_t size) override {
    return !rejectPreparation && File::prepareWrite(location, size);
  }

  uintptr_t readBlock(uint64_t location) override {
    return location < m_Data.size() ? reinterpret_cast<uintptr_t>(m_Data.data() + location) : 0;
  }

  bool pinBlock(uint64_t location) override {
    return location < m_Data.size();
  }

 private:
  std::vector<uint8_t> m_Data;
};

void expectHistoricalTimes(const File& file) {
  const File::Attributes attributes = file.getAttributes();
  EXPECT_EQ(attributes.accessed, 11U);
  EXPECT_EQ(attributes.modified, 22U);
  EXPECT_EQ(attributes.changed, 33U);
}

void expectWriteTimes(const File& file, Time::Timestamp before) {
  const File::Attributes attributes = file.getAttributes();
  EXPECT_EQ(attributes.accessed, 11U);
  EXPECT_GE(attributes.modified, before);
  EXPECT_LE(attributes.modified, Time::getTime());
  EXPECT_EQ(attributes.changed, attributes.modified);
  EXPECT_NE(attributes.modified, 22U);
  EXPECT_NE(attributes.changed, 33U);
}
}  // namespace

TEST(FileMetadata, SuccessfulOverwriteAndAppendUpdateTimes) {
  for (bool append : {false, true}) {
    MetadataWriteFile file;
    const uint8_t value = 0x6b;
    uint64_t offset = 17;
    const Time::Timestamp before = Time::getTime();
    const uint64_t written = append ? file.append(1, reinterpret_cast<uintptr_t>(&value), offset)
                                    : file.write(offset, 1, reinterpret_cast<uintptr_t>(&value));
    ASSERT_EQ(written, 1U);
    expectWriteTimes(file, before);
    EXPECT_EQ(offset, append ? 2 * kPageSize : 17U);
    EXPECT_EQ(file.getSize(), 2 * kPageSize + (append ? 1 : 0));
    uint8_t observed = 0;
    ASSERT_EQ(file.read(offset, 1, reinterpret_cast<uintptr_t>(&observed)), 1U);
    EXPECT_EQ(observed, value);
  }
}

TEST(FileMetadata, EmptyAndRejectedWritesPreserveTimes) {
  MetadataWriteFile file;
  const uint8_t value = 0x6b;
  uint64_t offset = 0;
  EXPECT_EQ(file.write(0, 0, reinterpret_cast<uintptr_t>(&value)), 0U);
  EXPECT_EQ(file.append(0, reinterpret_cast<uintptr_t>(&value), offset), 0U);
  EXPECT_EQ(file.write(~uint64_t(0), 2, reinterpret_cast<uintptr_t>(&value)), 0U);
  file.rejectPreparation = true;
  EXPECT_EQ(file.write(0, 1, reinterpret_cast<uintptr_t>(&value)), 0U);
  expectHistoricalTimes(file);

  MetadataWriteFile unreadable(0);
  EXPECT_EQ(unreadable.write(0, 1, reinterpret_cast<uintptr_t>(&value)), 0U);
  expectHistoricalTimes(unreadable);
}

TEST(FileMetadata, PartialWriteUpdatesTimesAfterAcceptedPrefix) {
  MetadataWriteFile file(1);
  std::vector<uint8_t> source(2 * kPageSize, 0x6b);
  const Time::Timestamp before = Time::getTime();
  EXPECT_EQ(file.write(0, source.size(), reinterpret_cast<uintptr_t>(source.data())), kPageSize);
  expectWriteTimes(file, before);
  std::vector<uint8_t> observed(kPageSize);
  ASSERT_EQ(file.read(0, observed.size(), reinterpret_cast<uintptr_t>(observed.data())),
            observed.size());
  EXPECT_EQ(observed, std::vector<uint8_t>(kPageSize, 0x6b));
}

TEST(RamFileMetadata, AllocationTracksTouchedPagesAndTruncation) {
  RamFs filesystem;
  RamFile file(String("sparse"), 1, &filesystem, nullptr);
  const uint64_t sectorsPerPage = kPageSize / 512;
  EXPECT_EQ(file.getAttributes().blocks, 0U);
  ASSERT_TRUE(file.resize(3 * kPageSize));
  EXPECT_EQ(file.getAttributes().blocks, 0U);

  const uint8_t value = 0x6b;
  ASSERT_EQ(file.write(2 * kPageSize + 17, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(file.getAttributes().blocks, sectorsPerPage);
  ASSERT_EQ(file.write(2 * kPageSize + 19, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(file.getAttributes().blocks, sectorsPerPage);
  ASSERT_EQ(file.write(17, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(file.getAttributes().blocks, 2 * sectorsPerPage);

  uint8_t hole = 0xff;
  ASSERT_EQ(file.read(kPageSize, 1, reinterpret_cast<uintptr_t>(&hole)), 1U);
  EXPECT_EQ(hole, 0U);
  EXPECT_EQ(file.getAttributes().blocks, 3 * sectorsPerPage);
  ASSERT_TRUE(file.resize(kPageSize + 1));
  EXPECT_EQ(file.getAttributes().blocks, 2 * sectorsPerPage);
  ASSERT_TRUE(file.resize(kPageSize));
  EXPECT_EQ(file.getAttributes().blocks, sectorsPerPage);
  ASSERT_TRUE(file.resize(0));
  EXPECT_EQ(file.getAttributes().blocks, 0U);
  ASSERT_TRUE(file.resize(3 * kPageSize));
  EXPECT_EQ(file.getAttributes().blocks, 0U);
}
