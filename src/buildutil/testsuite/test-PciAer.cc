#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/machine/PciAer.h"

#include <array>
#include <vector>

#include <gtest/gtest.h>

namespace {
struct Config {
  std::array<uint32_t, 1024> words{};
  std::vector<uint16_t> reads;
  uint16_t failAt = 0xffff;

  bool read32(uint16_t offset, uint32_t& value) {
    if (offset > 0xffc || (offset & 3U) || offset == failAt)
      return false;
    reads.push_back(offset);
    value = words[offset / 4];
    return true;
  }
};
}  // namespace

TEST(PciAer, ReadsMandatoryRegistersAndClassifiesOnlyUnmaskedErrors) {
  Config config;
  config.words[0x104 / 4] = 0x7;
  config.words[0x108 / 4] = 0x4;
  config.words[0x10c / 4] = 0x2;
  config.words[0x110 / 4] = 0x30;
  config.words[0x114 / 4] = 0x20;
  config.words[0x118 / 4] = 0x1234;
  PciAer::Status status;
  ASSERT_TRUE(PciAer::read(config, 0x100, 0x140, status));
  EXPECT_EQ(config.reads, (std::vector<uint16_t>{0x104, 0x108, 0x10c, 0x110, 0x114, 0x118}));
  EXPECT_EQ(status.fatal(), 0x2U);
  EXPECT_EQ(status.nonfatal(), 0x1U);
  EXPECT_EQ(status.maskedUncorrectable(), 0x4U);
  EXPECT_EQ(status.activeCorrectable(), 0x10U);
  EXPECT_EQ(status.capabilities, 0x1234U);
}

TEST(PciAer, RejectsTruncatedOrOverlappingCapabilitiesWithoutReading) {
  Config config;
  PciAer::Status status;
  for (const auto [offset, next] : {std::pair<uint16_t, uint16_t>{0xfc, 0},
                                    {0x101, 0},
                                    {0xff0, 0},
                                    {0x100, 0x118},
                                    {0x100, 0x142}}) {
    EXPECT_FALSE(PciAer::read(config, offset, next, status));
  }
  EXPECT_TRUE(config.reads.empty());
}

TEST(PciAer, ReadFailureDoesNotPublishPartialStatus) {
  Config config;
  config.words[0x104 / 4] = 0xffffffffU;
  config.failAt = 0x110;
  PciAer::Status status;
  status.correctable = 0x55;
  EXPECT_FALSE(PciAer::read(config, 0x100, 0, status));
  EXPECT_EQ(status.correctable, 0x55U);
  EXPECT_EQ(status.uncorrectable, 0U);
}

TEST(PciAer, AcceptsAValidatedBackwardCapabilityLink) {
  Config config;
  config.words[0x184 / 4] = 0x20;
  PciAer::Status status;
  ASSERT_TRUE(PciAer::read(config, 0x180, 0x100, status));
  EXPECT_EQ(status.uncorrectable, 0x20U);
}
