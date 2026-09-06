/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/utility.h"

#include <gtest/gtest.h>

namespace {
class RecordingTransport final : public PagingTransport {
 public:
  PagingStatus transfer(PagingOperation operation, uint64_t, void*) override {
    ++calls;
    last = operation;
    return result;
  }
  void release() override {
    ++releases;
  }
  PagingStatus result = PagingStatus::Success;
  PagingOperation last = PagingOperation::Flush;
  size_t calls = 0;
  size_t releases = 0;
};
class PagingDisk final : public Disk {
 public:
  PagingDisk() {
    reserveEndpoint();
    publishEndpoint();
  }
  ~PagingDisk() override {
    retireEndpoint();
  }
  size_t getSize() const override {
    return 2 * PagingChannel::PageBytes;
  }
  bool pin(uint64_t) override {
    return false;
  }
  void unpin(uint64_t) override {}
  PagingStatus preparePagingTransport(PagingTransport*& transport) override {
    ++preparations;
    transport = prepared == PagingStatus::Success ? &recording : nullptr;
    return prepared;
  }
  RecordingTransport recording;
  PagingStatus prepared = PagingStatus::Success;
  size_t preparations = 0;
};
}  // namespace
TEST(DiskPaging, MountAdmissionAndRemovalGateExcludePaging) {
  PagingDisk disk;
  const uint32_t id = disk.endpointId();
  ASSERT_NE(id, 0U);
  DiskUse mount;
  ASSERT_TRUE(disk.acquireUse(mount));
  PagingChannel channel;
  EXPECT_EQ(DiskEndpoints::prepare(id, channel), PagingStatus::Busy);
  EXPECT_FALSE(disk.tryCloseEndpoint());
  EXPECT_EQ(disk.preparations, 0U);
  DiskUse moved(pedigree_std::move(mount));
  EXPECT_FALSE(mount);
  EXPECT_EQ(moved.get(), &disk);
  moved.reset();
  ASSERT_TRUE(disk.tryCloseEndpoint());
  EXPECT_FALSE(DiskEndpoints::acquire(id, mount));
  EXPECT_EQ(DiskEndpoints::prepare(id, channel), PagingStatus::Busy);
  disk.reopenEndpoint();
  EXPECT_TRUE(DiskEndpoints::acquire(id, mount));
}
TEST(DiskPaging, PreparationFailureReopensAndSuccessfulChannelExcludesUsers) {
  PagingDisk disk;
  PagingChannel channel;
  DiskUse use;
  disk.prepared = PagingStatus::IoError;
  EXPECT_EQ(DiskEndpoints::prepare(disk.endpointId(), channel), PagingStatus::IoError);
  EXPECT_TRUE(disk.acquireUse(use));
  use.reset();
  disk.prepared = PagingStatus::Success;
  ASSERT_EQ(DiskEndpoints::prepare(disk.endpointId(), channel), PagingStatus::Success);
  EXPECT_FALSE(disk.acquireUse(use));
  EXPECT_FALSE(disk.tryCloseEndpoint());
  uint64_t bytes = 0;
  EXPECT_TRUE(DiskEndpoints::describe(disk.endpointId(), bytes));
  EXPECT_EQ(bytes, disk.getSize());
  unsigned char page[PagingChannel::PageBytes] = {};
  EXPECT_EQ(channel.readPage(1, page), PagingStatus::Invalid);
  EXPECT_EQ(channel.writePage(disk.getSize(), page), PagingStatus::Invalid);
  EXPECT_EQ(disk.recording.calls, 0U);
  EXPECT_EQ(channel.readPage(0, page), PagingStatus::Success);
  disk.recording.result = PagingStatus::IoError;
  EXPECT_EQ(channel.writePage(0, page), PagingStatus::IoError);
  EXPECT_EQ(channel.flush(), PagingStatus::IoError);
  channel.reset();
  EXPECT_EQ(disk.recording.releases, 1U);
  EXPECT_EQ(channel.flush(), PagingStatus::Closed);
  EXPECT_TRUE(disk.acquireUse(use));
}
TEST(DiskPaging, RemovedNumericIdentityNeverNamesAReplacement) {
  uint32_t old = 0;
  {
    PagingDisk disk;
    old = disk.endpointId();
  }
  PagingDisk replacement;
  EXPECT_NE(old, replacement.endpointId());
  DiskUse use;
  EXPECT_FALSE(DiskEndpoints::acquire(old, use));
  EXPECT_TRUE(DiskEndpoints::acquire(replacement.endpointId(), use));
  EXPECT_EQ(use.get(), &replacement);
}
