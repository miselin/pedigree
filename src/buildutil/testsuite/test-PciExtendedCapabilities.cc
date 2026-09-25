#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/machine/PciExtendedCapabilities.h"

#include <array>
#include <vector>

#include <gtest/gtest.h>

namespace {
struct Config {
  std::array<uint32_t, 1024> words{};
  std::vector<uint16_t> reads;
  uint16_t failedRead = 0;

  void set(uint16_t offset, uint16_t id, uint8_t version, uint16_t next) {
    words[offset / 4] = id | (uint32_t(version) << 16) | (uint32_t(next) << 20);
  }
  bool read32(uint16_t offset, uint32_t& value) {
    reads.push_back(offset);
    if (offset == failedRead)
      return false;
    value = words[offset / 4];
    return true;
  }
};
}  // namespace

TEST(PciExtendedCapabilities, FindsCapabilityAcrossFullConfigurationSpace) {
  Config config;
  config.set(0x100, 0x0003, 1, 0x800);
  config.set(0x800, 0x0001, 2, 0xffc);
  config.set(0xffc, 0x0023, 1, 0);
  PciExtendedCapabilities::Capability cap;
  EXPECT_EQ(PciExtendedCapabilities::find(config, 0x0001, cap),
            PciExtendedCapabilities::FindResult::Found);
  EXPECT_EQ(cap.id, 0x0001);
  EXPECT_EQ(cap.version, 2);
  EXPECT_EQ(cap.offset, 0x800);
  EXPECT_EQ(cap.next, 0xffc);
  EXPECT_EQ(config.reads, (std::vector<uint16_t>{0x100, 0x800, 0xffc}));
  EXPECT_EQ(PciExtendedCapabilities::find(config, 0x0002, cap),
            PciExtendedCapabilities::FindResult::Absent);
}

TEST(PciExtendedCapabilities, DistinguishesNoCapabilitiesFromUnavailableSpace) {
  Config config;
  PciExtendedCapabilities::Capability cap;
  EXPECT_EQ(PciExtendedCapabilities::find(config, 1, cap),
            PciExtendedCapabilities::FindResult::Absent);
  config.failedRead = 0x100;
  EXPECT_EQ(PciExtendedCapabilities::find(config, 1, cap),
            PciExtendedCapabilities::FindResult::Unavailable);
  config.failedRead = 0;
  config.words[0x100 / 4] = 0xffffffffU;
  EXPECT_EQ(PciExtendedCapabilities::find(config, 1, cap),
            PciExtendedCapabilities::FindResult::Unavailable);
}

TEST(PciExtendedCapabilities, RejectsBadLinksAndCycles) {
  for (uint16_t next : {0x101U, 0x0fcU, 0xffdU, 0x100U}) {
    Config config;
    config.set(0x100, 0x0001, 1, next);
    EXPECT_EQ(PciExtendedCapabilities::walk(config, [](const auto&) { return true; }),
              PciExtendedCapabilities::ScanResult::Malformed);
  }
  Config cycle;
  cycle.set(0x100, 0x0001, 1, 0x200);
  cycle.set(0x200, 0x0002, 1, 0x100);
  EXPECT_EQ(PciExtendedCapabilities::walk(cycle, [](const auto&) { return true; }),
            PciExtendedCapabilities::ScanResult::Malformed);
  EXPECT_EQ(cycle.reads, (std::vector<uint16_t>{0x100, 0x200}));
}

TEST(PciExtendedCapabilities, RejectsBadHeadersAndBrokenReads) {
  for (uint32_t header : {0x00010000U, 0x00000001U, 0x0001ffffU}) {
    Config config;
    config.words[0x100 / 4] = header;
    EXPECT_EQ(PciExtendedCapabilities::walk(config, [](const auto&) { return true; }),
              PciExtendedCapabilities::ScanResult::Malformed);
  }
  Config config;
  config.set(0x100, 0x0001, 1, 0x200);
  config.failedRead = 0x200;
  EXPECT_EQ(PciExtendedCapabilities::walk(config, [](const auto&) { return true; }),
            PciExtendedCapabilities::ScanResult::Malformed);
  config.failedRead = 0;
  EXPECT_EQ(PciExtendedCapabilities::walk(config, [](const auto&) { return true; }),
            PciExtendedCapabilities::ScanResult::Malformed);
}

TEST(PciExtendedCapabilities, FindDoesNotReturnPartiallyValidatedResult) {
  Config config;
  config.set(0x100, 0x0001, 1, 0x200);
  config.set(0x200, 0x0002, 1, 0x200);
  PciExtendedCapabilities::Capability cap{0x1234, 2, 0x444, 0x888};
  EXPECT_EQ(PciExtendedCapabilities::find(config, 0x0001, cap),
            PciExtendedCapabilities::FindResult::Malformed);
  EXPECT_EQ(cap.id, 0x1234);
  EXPECT_EQ(cap.offset, 0x444);
}

TEST(PciExtendedCapabilities, VisitorCanStopEarly) {
  Config config;
  config.set(0x100, 0x0001, 1, 0x200);
  unsigned visited = 0;
  EXPECT_EQ(PciExtendedCapabilities::walk(config,
                                          [&](const auto&) {
                                            ++visited;
                                            return false;
                                          }),
            PciExtendedCapabilities::ScanResult::Stopped);
  EXPECT_EQ(visited, 1U);
  EXPECT_EQ(config.reads, (std::vector<uint16_t>{0x100}));
}
