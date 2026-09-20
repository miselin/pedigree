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

class CachedPinFile final : public File {
 public:
  CachedPinFile()
      : File(String("pin"), 0, 0, 0, 1, nullptr, kPageSize, nullptr),
        oldData(kPageSize, 0x19),
        newData(kPageSize, 0x72),
        current(oldData.data()) {}

  uintptr_t acquire(bool retryChanged) {
    return acquireCachedBlock(0, retryChanged);
  }

  bool replaceOnPin = false;
  bool rejectPin = false;
  bool rejectRead = false;
  size_t pins = 0;
  size_t reads = 0;

 protected:
  bool useFillCache() const override {
    return false;
  }

  uintptr_t readBlock(uint64_t) override {
    ++reads;
    if (rejectRead)
      return 0;
    ++pins;
    return reinterpret_cast<uintptr_t>(current);
  }

  bool pinBlock(uint64_t location) override {
    if (rejectPin)
      return false;
    if (replaceOnPin) {
      replaceOnPin = false;
      current = newData.data();
      evict(location);
    }
    ++pins;
    return true;
  }

  void unpinBlock(uint64_t) override {
    ASSERT_GT(pins, 0U);
    --pins;
  }

 private:
  std::vector<uint8_t> oldData;
  std::vector<uint8_t> newData;
  uint8_t* current;
};

class PinnedRamFile final : public RamFile {
 public:
  explicit PinnedRamFile(RamFs& filesystem)
      : RamFile(String("pinned-ram"), 1, &filesystem, nullptr) {}

  using RamFile::acquireCachedBlock;
  using RamFile::unpinBlock;
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

TEST(FileCachedPin, RejectsReplacedAddressAndBalancesReadFallback) {
  for (bool directAcquisition : {false, true}) {
    CachedPinFile file;
    uint8_t value = 0;
    ASSERT_EQ(file.read(0, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
    ASSERT_EQ(value, 0x19);
    ASSERT_EQ(file.pins, 0U);
    file.replaceOnPin = true;
    if (directAcquisition) {
      EXPECT_EQ(file.acquire(false), 0U);
      EXPECT_EQ(file.pins, 0U);
      EXPECT_EQ(file.reads, 1U);
    }
    EXPECT_EQ(file.read(0, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
    EXPECT_EQ(value, 0x72);
    EXPECT_EQ(file.pins, 0U);
    EXPECT_EQ(file.reads, 2U);
  }
}

TEST(FileCachedPin, RejectedPinAndBackendMissOwnNoReference) {
  CachedPinFile file;
  uint8_t value = 0;
  ASSERT_EQ(file.read(0, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  file.rejectPin = true;
  EXPECT_EQ(file.acquire(false), 0U);
  EXPECT_EQ(file.pins, 0U);
  file.rejectRead = true;
  value = 0xa5;
  EXPECT_EQ(file.read(0, 1, reinterpret_cast<uintptr_t>(&value)), 0U);
  EXPECT_EQ(value, 0xa5);
  EXPECT_EQ(file.pins, 0U);
}

TEST(RamFileCachedPin, MissDoesNotAllocateAndOneReleaseAllowsShrink) {
  RamFs filesystem;
  filesystem.setProcessOwnership(false);
  PinnedRamFile file(filesystem);
  ASSERT_TRUE(file.resize(2 * kPageSize));
  EXPECT_EQ(file.acquireCachedBlock(0, false), 0U);
  EXPECT_EQ(file.getAttributes().blocks, 0U);
  const uint8_t value = 0x6b;
  ASSERT_EQ(file.write(17, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  const uintptr_t address = file.acquireCachedBlock(0, false);
  ASSERT_NE(address, 0U);
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(address)[17], value);
  EXPECT_FALSE(file.resize(0));
  file.unpinBlock(0);
  EXPECT_TRUE(file.resize(0));
  EXPECT_EQ(file.acquireCachedBlock(0, false), 0U);
  EXPECT_EQ(file.getAttributes().blocks, 0U);
}

TEST(RamFileCachedPin, RepeatedReadsReleasePinsAcrossShrinkAndRewrite) {
  RamFs filesystem;
  filesystem.setProcessOwnership(false);
  RamFile file(String("rewrite"), 1, &filesystem, nullptr);
  std::vector<uint8_t> bytes(kPageSize + 2);
  std::vector<uint8_t> observed(bytes.size());
  for (uint8_t value : {0x19, 0x72, 0x3c}) {
    bytes.assign(bytes.size(), value);
    ASSERT_EQ(file.write(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())), bytes.size());
    ASSERT_EQ(file.read(0, observed.size(), reinterpret_cast<uintptr_t>(observed.data())),
              observed.size());
    EXPECT_EQ(observed, bytes);
  }
  ASSERT_TRUE(file.resize(1));
  ASSERT_TRUE(file.resize(bytes.size()));
  bytes.assign(bytes.size(), 0);
  bytes[0] = 0x3c;
  ASSERT_EQ(file.read(0, observed.size(), reinterpret_cast<uintptr_t>(observed.data())),
            observed.size());
  EXPECT_EQ(observed, bytes);
  ASSERT_TRUE(file.resize(0));
  bytes.assign(bytes.size(), 0x95);
  ASSERT_EQ(file.write(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())), bytes.size());
  ASSERT_EQ(file.read(0, observed.size(), reinterpret_cast<uintptr_t>(observed.data())),
            observed.size());
  EXPECT_EQ(observed, bytes);
  EXPECT_TRUE(file.resize(0));
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
