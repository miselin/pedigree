#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/machine/PciSriov.h"

#include <array>

#include <gtest/gtest.h>

namespace {
struct Config {
  std::array<uint32_t, 1024> words{};
  std::array<uint32_t, 6> barMasks{0xfffff000, 0, 0, 0, 0, 0};
  uint16_t failAt = 0xffff;
  uint16_t failWriteAt = 0xffff;
  unsigned failWriteCount = 0;
  int rejectBarProbe = -1;
  int rejectBarRestore = -1;
  uint16_t routingOffsetAfterCount = 0;

  bool read16(uint16_t offset, uint16_t& value) {
    if (offset > 0xffe || (offset & 1U) || offset == failAt) {
      return false;
    }
    value = words[offset / 4] >> ((offset & 2U) * 8);
    return true;
  }
  bool read32(uint16_t offset, uint32_t& value) {
    if (offset > 0xffc || (offset & 3U) || offset == failAt) {
      return false;
    }
    value = words[offset / 4];
    return true;
  }
  bool write16(uint16_t offset, uint16_t value) {
    if (offset > 0xffe || (offset & 1U)) {
      return false;
    }
    if (offset == failWriteAt && failWriteCount) {
      --failWriteCount;
      return false;
    }
    set16(offset, value);
    if (offset == 0x110 && value && routingOffsetAfterCount) {
      set16(0x114, routingOffsetAfterCount);
    }
    return true;
  }
  bool write32(uint16_t offset, uint32_t value) {
    if (offset > 0xffc || (offset & 3U)) {
      return false;
    }
    if (offset >= 0x124 && offset < 0x13c) {
      const unsigned bar = (offset - 0x124) / 4;
      if (value == 0xffffffffU && static_cast<int>(bar) == rejectBarProbe) {
        return false;
      }
      if (value != 0xffffffffU && static_cast<int>(bar) == rejectBarRestore) {
        return false;
      }
      words[offset / 4] = value == 0xffffffffU ? barMasks[bar] : value;
      return true;
    }
    words[offset / 4] = value;
    return true;
  }
  void set16(uint16_t offset, uint16_t value) {
    uint32_t& word = words[offset / 4];
    const unsigned shift = (offset & 2U) * 8;
    word = (word & ~(0xffffU << shift)) | (uint32_t(value) << shift);
  }
};

Config configured() {
  Config config;
  config.set16(0x10c, 4);
  config.set16(0x10e, 8);
  config.set16(0x110, 2);
  config.set16(0x114, 0x10);
  config.set16(0x116, 1);
  config.set16(0x11a, 0x1001);
  config.words[0x11c / 4] = 3;
  config.words[0x120 / 4] = 1;
  config.words[0x124 / 4] = 0x90000000;
  return config;
}
}  // namespace

TEST(PciSriov, ReadsConfigurationAndCalculatesVfRoutingIds) {
  Config config = configured();
  PciSriov::State state;
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0x140, state));
  EXPECT_EQ(state.initialVfs, 4);
  EXPECT_EQ(state.totalVfs, 8);
  EXPECT_EQ(state.numVfs, 2);
  EXPECT_EQ(state.vfDeviceId, 0x1001);
  EXPECT_EQ(state.bars[0], 0x90000000U);
  uint8_t bus = 0, device = 0, function = 0;
  ASSERT_TRUE(PciSriov::vfRoutingId(2, 30, 0, state, 0, bus, device, function));
  EXPECT_EQ(bus, 3);
  EXPECT_EQ(device, 0);
  EXPECT_EQ(function, 0);
  ASSERT_TRUE(PciSriov::vfRoutingId(2, 30, 0, state, 1, bus, device, function));
  EXPECT_EQ(function, 1);
  EXPECT_FALSE(PciSriov::vfRoutingId(2, 30, 0, state, 2, bus, device, function));
}

TEST(PciSriov, RejectsMalformedStateAndPartialReads) {
  Config config = configured();
  PciSriov::State state;
  EXPECT_FALSE(PciSriov::read(config, 0x100, 0x120, state));
  EXPECT_FALSE(PciSriov::read(config, 0xfe0, 0, state));
  config.set16(0x10e, 1);
  EXPECT_FALSE(PciSriov::read(config, 0x100, 0, state));
  config = configured();
  config.failAt = 0x138;
  EXPECT_FALSE(PciSriov::read(config, 0x100, 0, state));
  EXPECT_EQ(state.totalVfs, 0);
}

TEST(PciSriov, RejectsInvalidPageSelectionAndRoutingOverflow) {
  Config config = configured();
  PciSriov::State state;
  config.words[0x120 / 4] = 4;
  EXPECT_FALSE(PciSriov::read(config, 0x100, 0, state));
  config = configured();
  config.set16(0x108, 1);
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  uint8_t bus = 0, device = 0, function = 0;
  EXPECT_FALSE(PciSriov::vfRoutingId(255, 31, 7, state, 0, bus, device, function));
}

TEST(PciSriov, ProbesVfBarSizesAndRestoresConfiguration) {
  Config config = configured();
  config.words[0x12c / 4] = 0xa000000c;
  config.words[0x130 / 4] = 1;
  config.barMasks[2] = 0xffffc00c;
  config.barMasks[3] = 0xffffffffU;
  PciSriov::State state;
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  PciSriov::Geometry geometry;
  ASSERT_EQ(PciSriov::probeBars(config, 0x100, 0, state, geometry), PciSriov::Result::Success);
  EXPECT_EQ(geometry.bars[0].base, 0x90000000U);
  EXPECT_EQ(geometry.bars[0].perVfBytes, 4096U);
  EXPECT_EQ(geometry.bars[0].apertureBytes, 32768U);
  EXPECT_EQ(geometry.bars[2].base, 0x1a0000000ULL);
  EXPECT_EQ(geometry.bars[2].perVfBytes, 16384U);
  EXPECT_EQ(geometry.bars[2].apertureBytes, 131072U);
  EXPECT_TRUE(geometry.bars[2].wide);
  EXPECT_TRUE(geometry.bars[2].prefetchable);
  EXPECT_EQ(geometry.bars[3].perVfBytes, 0U);
  for (unsigned i = 0; i < 6; ++i) {
    EXPECT_EQ(config.words[(0x124 + 4 * i) / 4], state.bars[i]);
  }
}

TEST(PciSriov, RejectsVfBarApertureOverflowAndRestorationFailure) {
  Config config = configured();
  PciSriov::State state;
  config.set16(0x108, PciSriov::VfControl);
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  PciSriov::Geometry geometry;
  EXPECT_EQ(PciSriov::probeBars(config, 0x100, 0, state, geometry), PciSriov::Result::Invalid);
  EXPECT_EQ(config.words[0x124 / 4], state.bars[0]);

  config = configured();
  config.words[0x124 / 4] = 0xfffff000;
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  EXPECT_EQ(PciSriov::probeBars(config, 0x100, 0, state, geometry), PciSriov::Result::Invalid);

  config = configured();
  config.rejectBarProbe = 2;
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  EXPECT_EQ(PciSriov::probeBars(config, 0x100, 0, state, geometry), PciSriov::Result::IoError);
  EXPECT_EQ(config.words[0x124 / 4], state.bars[0]);

  config = configured();
  config.words[0x124 / 4] = 4;
  config.barMasks[0] = 4;
  config.barMasks[1] = 0x80000000;
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  EXPECT_EQ(PciSriov::probeBars(config, 0x100, 0, state, geometry), PciSriov::Result::Invalid);

  config = configured();
  config.rejectBarRestore = 0;
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  EXPECT_EQ(PciSriov::probeBars(config, 0x100, 0, state, geometry),
            PciSriov::Result::RestoreFailed);
  EXPECT_EQ(config.words[0x108 / 4] & PciSriov::VfControl, 0U);
}

TEST(PciSriov, PlansOnlySupportedFourKilobyteVfsWithinRoutingRange) {
  Config config = configured();
  config.set16(0x110, 0);
  PciSriov::State state;
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  PciSriov::Plan request;
  ASSERT_TRUE(PciSriov::plan(2, 30, 0, state, 2, request));
  EXPECT_EQ(request.firstRid, 0x300U);
  EXPECT_EQ(request.lastRid, 0x301U);
  EXPECT_EQ(request.numVfs, 2U);
  EXPECT_FALSE(PciSriov::plan(2, 30, 0, state, 0, request));
  EXPECT_FALSE(PciSriov::plan(2, 30, 0, state, 5, request));
  EXPECT_FALSE(PciSriov::plan(255, 31, 7, state, 1, request));
  state.supportedPageSizes = 2;
  EXPECT_FALSE(PciSriov::plan(2, 30, 0, state, 1, request));
}

TEST(PciSriov, EnablesAndDisablesVfsWithVerifiedWrites) {
  Config config = configured();
  config.set16(0x110, 0);
  PciSriov::State state;
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  PciSriov::Plan request;
  ASSERT_TRUE(PciSriov::plan(2, 30, 0, state, 2, request));
  ASSERT_EQ(PciSriov::enable(config, 0x100, 0, request), PciSriov::Result::Success);
  EXPECT_EQ(config.words[0x108 / 4] & PciSriov::VfControl, PciSriov::VfControl);
  EXPECT_EQ(config.words[0x110 / 4] & 0xffffU, 2U);
  EXPECT_EQ(config.words[0x120 / 4], PciSriov::PageSize4K);
  ASSERT_EQ(PciSriov::disable(config, 0x100, 0), PciSriov::Result::Success);
  EXPECT_EQ(config.words[0x108 / 4] & PciSriov::VfControl, 0U);
  EXPECT_EQ(config.words[0x110 / 4] & 0xffffU, 0U);
}

TEST(PciSriov, RollsBackPartialEnableAndRejectsChangedRouting) {
  Config config = configured();
  config.set16(0x110, 0);
  PciSriov::State state;
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  PciSriov::Plan request;
  ASSERT_TRUE(PciSriov::plan(2, 30, 0, state, 2, request));
  config.failWriteAt = 0x110;
  config.failWriteCount = 1;
  EXPECT_EQ(PciSriov::enable(config, 0x100, 0, request), PciSriov::Result::IoError);
  EXPECT_EQ(config.words[0x108 / 4] & PciSriov::VfControl, 0U);
  EXPECT_EQ(config.words[0x110 / 4] & 0xffffU, 0U);
  EXPECT_EQ(config.words[0x120 / 4], state.systemPageSize);

  config.routingOffsetAfterCount = 0x11;
  EXPECT_EQ(PciSriov::enable(config, 0x100, 0, request), PciSriov::Result::IoError);
  EXPECT_EQ(config.words[0x108 / 4] & PciSriov::VfControl, 0U);
  EXPECT_EQ(config.words[0x110 / 4] & 0xffffU, 0U);
}

TEST(PciSriov, ReportsFailedRollbackWithoutClaimingVfsDisabled) {
  Config config = configured();
  config.set16(0x110, 0);
  PciSriov::State state;
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  PciSriov::Plan request;
  ASSERT_TRUE(PciSriov::plan(2, 30, 0, state, 2, request));
  config.failWriteAt = 0x108;
  config.failWriteCount = 2;
  EXPECT_EQ(PciSriov::enable(config, 0x100, 0, request), PciSriov::Result::RollbackFailed);
}

TEST(PciSriov, ReportsUnverifiedDisableAsFailClosedFailure) {
  Config config = configured();
  config.set16(0x108, PciSriov::VfControl);
  PciSriov::State state;
  ASSERT_TRUE(PciSriov::read(config, 0x100, 0, state));
  config.failWriteAt = 0x108;
  config.failWriteCount = 1;
  EXPECT_EQ(PciSriov::disable(config, 0x100, 0), PciSriov::Result::RollbackFailed);
  EXPECT_NE(config.words[0x108 / 4] & PciSriov::VfControl, 0U);
}
