#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/machine/PciSriov.h"

#include <array>

#include <gtest/gtest.h>

namespace {
struct Config {
  std::array<uint32_t, 1024> words{};
  uint16_t failAt = 0xffff;

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
