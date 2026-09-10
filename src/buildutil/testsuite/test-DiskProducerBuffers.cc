/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Disk.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "modules/drivers/common/partition/Partition.h"
#include <gtest/gtest.h>

namespace {
constexpr size_t PageBytes = TargetInfo::getPageSize();

class LegacyBufferDisk : public Disk {
 public:
  explicit LegacyBufferDisk(size_t size = 3 * PageBytes + 127)
      : cached(size), durable(size), loans((size + PageBytes - 1) / PageBytes) {
    for (size_t i = 0; i < size; ++i)
      cached[i] = static_cast<uint8_t>((i * 37 + 11) % 251);
    durable = cached;
  }

  size_t getSize() const override {
    return cached.size();
  }
  size_t getNativeBlockSize() const override {
    return PageBytes;
  }
  BufferView read(uint64_t location) override {
    reads.push_back(location);
    if (location >= getSize() || location % 512 || location / PageBytes == failedReadPage)
      return {};
    const size_t page = location / PageBytes;
    ++loans[page];
    const size_t available = std::min(PageBytes - location % PageBytes, getSize() - location);
    return BufferView(cached.data() + location, available);
  }
  void write(uint64_t location) override {
    writes.push_back(location);
  }
  bool sync(uint64_t location, bool async) override {
    syncLocations.push_back(location);
    EXPECT_FALSE(async);
    if (!supportsSync || syncLocations.size() == failedSyncCall)
      return false;
    const size_t page = location - location % PageBytes;
    EXPECT_GT(loans[page / PageBytes], 0U);
    const size_t length = std::min(PageBytes, getSize() - page);
    std::copy_n(cached.begin() + page, length, durable.begin() + page);
    return true;
  }
  bool pin(uint64_t location) override {
    if (location >= getSize())
      return false;
    ++loans[location / PageBytes];
    return true;
  }
  void unpin(uint64_t location) override {
    ASSERT_LT(location / PageBytes, loans.size());
    ASSERT_GT(loans[location / PageBytes], 0U);
    --loans[location / PageBytes];
  }
  void expectNoLoans() const {
    for (size_t count : loans)
      EXPECT_EQ(count, 0U);
  }

  std::vector<uint8_t> cached;
  std::vector<uint8_t> durable;
  std::vector<size_t> loans;
  std::vector<uint64_t> reads;
  std::vector<uint64_t> writes;
  std::vector<uint64_t> syncLocations;
  size_t failedReadPage = std::numeric_limits<size_t>::max();
  size_t failedSyncCall = std::numeric_limits<size_t>::max();
  bool supportsSync = true;
};

class ForwardingDisk final : public Disk {
 public:
  size_t getSize() const override {
    return size;
  }
  bool readInto(uint64_t location, void* buffer, size_t length) override {
    reads.push_back(location);
    lastBuffer = buffer;
    lastLength = length;
    return result;
  }
  bool readIntoBatch(ReadBuffer* buffers, size_t count) override {
    batches.emplace_back(buffers, buffers + count);
    bool success = true;
    for (size_t i = 0; i < count; ++i) {
      buffers[i].complete = result && i != failedBatchIndex;
      success &= buffers[i].complete;
      if (buffers[i].complete)
        std::fill_n(static_cast<uint8_t*>(buffers[i].buffer), buffers[i].length, 0xb6);
    }
    return success;
  }
  bool writeFrom(uint64_t location, const void* buffer, size_t length) override {
    writes.push_back(location);
    lastBuffer = buffer;
    lastLength = length;
    return result;
  }
  bool syncData() override {
    ++barriers;
    return result;
  }
  void align(uint64_t location) override {
    alignments.push_back(location);
  }
  bool pin(uint64_t) override {
    return false;
  }
  void unpin(uint64_t) override {}

  size_t size = 4 * PageBytes;
  bool result = true;
  const void* lastBuffer = nullptr;
  size_t lastLength = 0;
  size_t barriers = 0;
  std::vector<uint64_t> reads;
  std::vector<uint64_t> writes;
  std::vector<uint64_t> alignments;
  std::vector<std::vector<ReadBuffer>> batches;
  size_t failedBatchIndex = std::numeric_limits<size_t>::max();
};
}  // namespace

TEST(DiskProducerBuffers, ReadsExactUnalignedRangeAcrossPagesAndTerminalTail) {
  LegacyBufferDisk disk;
  const size_t start = PageBytes - 13;
  std::vector<uint8_t> output(disk.getSize() - start);
  ASSERT_TRUE(disk.readInto(start, output.data(), output.size()));
  EXPECT_TRUE(std::equal(output.begin(), output.end(), disk.cached.begin() + start));
  EXPECT_EQ(disk.reads,
            (std::vector<uint64_t>{PageBytes - 512, PageBytes, 2 * PageBytes, 3 * PageBytes}));
  disk.expectNoLoans();
}

TEST(DiskProducerBuffers, DefaultWritesPersistBeforeReturnAndPreserveNativeSectorNeighbours) {
  LegacyBufferDisk disk;
  auto expected = disk.durable;
  const size_t start = PageBytes - 17;
  std::vector<uint8_t> input(1024 + 39, 0xa7);
  std::copy(input.begin(), input.end(), expected.begin() + start);
  ASSERT_TRUE(disk.writeFrom(start, input.data(), input.size()));
  EXPECT_EQ(disk.durable, expected);
  EXPECT_EQ(disk.cached, expected);
  EXPECT_EQ(disk.writes, (std::vector<uint64_t>{PageBytes - 512, PageBytes}));
  EXPECT_EQ(disk.syncLocations, disk.writes);
  ASSERT_TRUE(disk.syncData());
  EXPECT_EQ(disk.syncLocations.size(), 2U);
  disk.expectNoLoans();
}

TEST(DiskProducerBuffers, FailedWriteKeepsPartialProgressRetryableAndReleasesEveryLoan) {
  LegacyBufferDisk disk;
  const auto original = disk.durable;
  std::vector<uint8_t> input(31, 0xc3);
  const size_t start = PageBytes - 8;
  disk.failedSyncCall = 2;
  ASSERT_FALSE(disk.writeFrom(start, input.data(), input.size()));
  auto partial = original;
  std::copy_n(input.begin(), 8, partial.begin() + start);
  EXPECT_EQ(disk.durable, partial);
  disk.expectNoLoans();

  auto expected = original;
  std::copy(input.begin(), input.end(), expected.begin() + start);
  EXPECT_EQ(disk.cached, expected);
  disk.failedSyncCall = std::numeric_limits<size_t>::max();
  ASSERT_TRUE(disk.writeFrom(start, input.data(), input.size()));
  EXPECT_EQ(disk.durable, expected);
  disk.expectNoLoans();
}

TEST(DiskProducerBuffers, ReadFailureAndUnsupportedPersistenceDoNotLeakLoans) {
  LegacyBufferDisk disk;
  disk.failedReadPage = 1;
  std::vector<uint8_t> output(31, 0xfd);
  ASSERT_FALSE(disk.readInto(PageBytes - 8, output.data(), output.size()));
  EXPECT_TRUE(std::equal(output.begin(), output.begin() + 8, disk.cached.begin() + PageBytes - 8));
  EXPECT_TRUE(std::all_of(output.begin() + 8, output.end(), [](uint8_t v) { return v == 0xfd; }));
  disk.expectNoLoans();

  const auto original = disk.durable;
  disk.supportsSync = false;
  ASSERT_FALSE(disk.writeFrom(1, output.data(), 3));
  EXPECT_EQ(disk.durable, original);
  disk.expectNoLoans();
}

TEST(DiskProducerBuffers, RejectsInvalidBoundsBeforeAcquiringViews) {
  LegacyBufferDisk disk;
  uint8_t byte = 0;
  EXPECT_FALSE(disk.readInto(disk.getSize(), &byte, 1));
  EXPECT_FALSE(disk.readInto(disk.getSize() - 1, &byte, 2));
  EXPECT_FALSE(disk.readInto(~uint64_t{0}, &byte, 1));
  EXPECT_FALSE(disk.writeFrom(0, nullptr, 1));
  EXPECT_FALSE(disk.writeFrom(disk.getSize() - 1, &byte, 2));
  EXPECT_TRUE(disk.readInto(disk.getSize(), nullptr, 0));
  EXPECT_TRUE(disk.writeFrom(disk.getSize(), nullptr, 0));
  EXPECT_TRUE(disk.reads.empty());
  EXPECT_TRUE(disk.writes.empty());
  disk.expectNoLoans();
}

TEST(DiskProducerBuffers, BatchFallbackDrainsLaterReadsAndMarksOnlyCompleteBuffers) {
  LegacyBufferDisk disk;
  disk.failedReadPage = 1;
  uint8_t output[3][33];
  for (auto& buffer : output)
    std::fill_n(buffer, sizeof(buffer), 0xfd);
  Disk::ReadBuffer requests[] = {{0, output[0] + 1, 31, true},
                                 {PageBytes - 8, output[1] + 1, 31, true},
                                 {2 * PageBytes + 3, output[2] + 1, 31, true}};
  EXPECT_FALSE(disk.readIntoBatch(requests, 3));
  EXPECT_TRUE(requests[0].complete);
  EXPECT_FALSE(requests[1].complete);
  EXPECT_TRUE(requests[2].complete);
  EXPECT_TRUE(std::equal(output[0] + 1, output[0] + 32, disk.cached.begin()));
  EXPECT_TRUE(std::equal(output[1] + 1, output[1] + 9, disk.cached.begin() + PageBytes - 8));
  EXPECT_TRUE(std::all_of(output[1] + 9, output[1] + 32, [](uint8_t v) { return v == 0xfd; }));
  EXPECT_TRUE(std::equal(output[2] + 1, output[2] + 32, disk.cached.begin() + 2 * PageBytes + 3));
  for (const auto& buffer : output) {
    EXPECT_EQ(buffer[0], 0xfd);
    EXPECT_EQ(buffer[32], 0xfd);
  }
  disk.expectNoLoans();
}

TEST(DiskProducerBuffers, BatchFallbackHandlesRepeatedSubpagesAndBounds) {
  LegacyBufferDisk disk;
  uint8_t output[3][7] = {};
  Disk::ReadBuffer requests[] = {
      {5, output[0], 7, true}, {5, output[1], 7, true}, {disk.getSize() - 6, output[2], 7, true}};
  EXPECT_FALSE(disk.readIntoBatch(requests, 3));
  EXPECT_TRUE(requests[0].complete);
  EXPECT_TRUE(requests[1].complete);
  EXPECT_FALSE(requests[2].complete);
  EXPECT_TRUE(std::equal(output[0], output[0] + 7, disk.cached.begin() + 5));
  EXPECT_TRUE(std::equal(output[1], output[1] + 7, output[0]));
  EXPECT_TRUE(std::all_of(output[2], output[2] + 7, [](uint8_t v) { return !v; }));
  const size_t reads = disk.reads.size();
  EXPECT_TRUE(disk.readIntoBatch(nullptr, 0));
  EXPECT_FALSE(disk.readIntoBatch(nullptr, 1));
  Disk::ReadBuffer oversized[Disk::MaxReadBuffers + 1] = {};
  EXPECT_FALSE(disk.readIntoBatch(oversized, Disk::MaxReadBuffers + 1));
  EXPECT_EQ(disk.reads.size(), reads);
  disk.expectNoLoans();
}

TEST(DiskProducerBuffers, PartitionTranslatesOneBatchAndPreservesIndependentResults) {
  ForwardingDisk parent;
  Partition partition(String("batch"), 512, 2 * PageBytes);
  partition.setParent(&parent);
  parent.failedBatchIndex = 1;
  uint8_t output[3][9] = {};
  Disk::ReadBuffer requests[] = {{7, output[0] + 1, 7, true},
                                 {PageBytes, output[1] + 1, 7, true},
                                 {2 * PageBytes - 7, output[2] + 1, 7, true}};
  EXPECT_FALSE(partition.readIntoBatch(requests, 3));
  ASSERT_EQ(parent.batches.size(), 1U);
  ASSERT_EQ(parent.batches[0].size(), 3U);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(parent.batches[0][i].location, requests[i].location + 512);
    EXPECT_EQ(parent.batches[0][i].buffer, requests[i].buffer);
    EXPECT_EQ(parent.batches[0][i].length, requests[i].length);
    EXPECT_FALSE(parent.batches[0][i].complete);
    EXPECT_EQ(requests[i].complete, i != 1);
    EXPECT_TRUE(std::all_of(output[i] + 1, output[i] + 8,
                            [i](uint8_t v) { return v == (i == 1 ? 0 : 0xb6); }));
    EXPECT_EQ(output[i][0], 0);
    EXPECT_EQ(output[i][8], 0);
  }
  EXPECT_EQ(requests[0].location, 7U);
  EXPECT_EQ(requests[1].location, PageBytes);
  EXPECT_EQ(requests[2].location, 2 * PageBytes - 7);
  EXPECT_EQ(parent.alignments, (std::vector<uint64_t>{512}));
  EXPECT_TRUE(parent.reads.empty());

  requests[2].length = 8;
  EXPECT_FALSE(partition.readIntoBatch(requests, 3));
  EXPECT_EQ(parent.batches.size(), 1U);
  for (const auto& request : requests)
    EXPECT_FALSE(request.complete);
}

TEST(DiskProducerBuffers, PartitionForwardsExactTailRangesAndFinalBarrier) {
  ForwardingDisk parent;
  constexpr size_t start = 512;
  constexpr size_t length = PageBytes + 37;
  Partition partition(String("producer"), start, length);
  partition.setParent(&parent);
  uint8_t buffer[7] = {};
  ASSERT_TRUE(partition.readInto(length - sizeof(buffer), buffer, sizeof(buffer)));
  EXPECT_EQ(parent.reads, (std::vector<uint64_t>{start + length - sizeof(buffer)}));
  EXPECT_EQ(parent.lastBuffer, buffer);
  EXPECT_EQ(parent.lastLength, sizeof(buffer));
  ASSERT_TRUE(partition.writeFrom(3, buffer, 5));
  EXPECT_EQ(parent.writes, (std::vector<uint64_t>{start + 3}));
  EXPECT_EQ(parent.lastBuffer, buffer);
  EXPECT_EQ(parent.lastLength, 5U);
  EXPECT_EQ(parent.alignments, (std::vector<uint64_t>{start}));
  EXPECT_FALSE(partition.readInto(length - 6, buffer, 7));
  EXPECT_FALSE(partition.writeFrom(length, buffer, 1));
  EXPECT_EQ(parent.reads.size(), 1U);
  EXPECT_EQ(parent.writes.size(), 1U);
  parent.result = false;
  EXPECT_FALSE(partition.syncData());
  EXPECT_EQ(parent.barriers, 1U);
  EXPECT_FALSE(partition.readInto(0, buffer, sizeof(buffer)));
}

TEST(DiskProducerBuffers, PartitionRejectsParentBoundaryOverflowAndDetachedAccess) {
  ForwardingDisk parent;
  uint8_t byte = 0;
  Partition outside(String("outside"), parent.getSize() - 4, 32);
  outside.setParent(&parent);
  EXPECT_FALSE(outside.readInto(3, &byte, 2));
  EXPECT_FALSE(outside.writeFrom(5, &byte, 1));
  parent.size = std::numeric_limits<size_t>::max();
  Partition overflow(String("overflow"), ~uint64_t{0} - 63, 128);
  overflow.setParent(&parent);
  EXPECT_FALSE(overflow.readInto(64, &byte, 1));
  EXPECT_FALSE(overflow.writeFrom(0, &byte, 65));
  Partition detached(String("detached"), 0, PageBytes);
  EXPECT_FALSE(detached.readInto(0, &byte, 1));
  EXPECT_FALSE(detached.writeFrom(0, &byte, 1));
  EXPECT_FALSE(detached.syncData());
  EXPECT_TRUE(parent.reads.empty());
  EXPECT_TRUE(parent.writes.empty());
  EXPECT_TRUE(parent.alignments.empty());
}
