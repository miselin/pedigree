#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/machine/PciFunctionState.h"

#include <array>
#include <vector>

#include <gtest/gtest.h>
namespace {
struct Config {
  std::array<uint8_t, 256> bytes{};
  std::vector<uint16_t> writes;
  bool refuse = false;
  Config() {
    set16(0, 0x1234);
    set16(2, 0x5678);
    set16(4, 0x140);
    set16(6, 0xf910);
    bytes[0x3c] = 11;
    bytes[0x3d] = 1;
    bytes[0x34] = 0x40;
    bytes[0x40] = 1;
    bytes[0x41] = 0x50;
    set16(0x44, 0x8100);
    bytes[0x50] = 5;
    bytes[0x51] = 0x70;
    set16(0x52, 0x181);
    bytes[0x70] = 0x11;
    set16(0x72, 0x8007);
    set16(0x10, 0x0004);
    set16(0x12, 0x8000);
    set16(0x14, 1);
  }
  void set16(unsigned offset, uint16_t value) {
    bytes[offset] = value;
    bytes[offset + 1] = value >> 8;
  }
  bool read8(uint16_t offset, uint8_t& value) {
    if (offset >= bytes.size())
      return false;
    value = bytes[offset];
    return true;
  }
  bool read16(uint16_t offset, uint16_t& value) {
    if (offset > 254 || offset & 1)
      return false;
    value = bytes[offset] | (uint16_t{bytes[offset + 1]} << 8);
    return true;
  }
  bool read32(uint16_t offset, uint32_t& value) {
    if (offset > 252 || offset & 3)
      return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i)
      value |= uint32_t{bytes[offset + i]} << (8 * i);
    return true;
  }
  bool write16(uint16_t offset, uint16_t value) {
    if (offset > 254 || offset & 1)
      return false;
    writes.push_back(offset);
    if (!refuse)
      set16(offset, value);
    return true;
  }
};
}  // namespace
TEST(PciFunctionState, NormalizesMessageControlsWithoutChangingOtherRegisters) {
  Config config;
  PciFunctionState::State state;
  ASSERT_TRUE(PciFunctionState::inspect(config, state));
  const auto before = config.bytes;
  ASSERT_TRUE(PciFunctionState::disableMessageInterrupts(config, state));
  EXPECT_EQ(config.writes, (std::vector<uint16_t>{0x52, 0x72}));
  EXPECT_EQ(config.bytes[0x52], 0x80);
  EXPECT_EQ(config.bytes[0x73], 0x40);
  for (unsigned i = 0; i < 256; ++i)
    if (i != 0x52 && i != 0x73)
      EXPECT_EQ(config.bytes[i], before[i]);
  EXPECT_TRUE(PciFunctionState::resourcesUnchanged(config, state));
  config.bytes[0x14] = 2;
  EXPECT_FALSE(PciFunctionState::resourcesUnchanged(config, state));
}
TEST(PciFunctionState, RejectsPowerModesWithoutPowerOrPmeWrites) {
  for (unsigned power = 1; power <= 3; ++power) {
    Config config;
    config.bytes[0x44] |= power;
    const auto before = config.bytes;
    PciFunctionState::State state;
    EXPECT_FALSE(PciFunctionState::inspect(config, state));
    EXPECT_EQ(config.bytes, before);
    EXPECT_TRUE(config.writes.empty());
  }
}
TEST(PciFunctionState, RejectsTruncatedCapabilitiesAndOverlaps) {
  for (uint8_t kind : {1U, 5U, 0x11U}) {
    Config config;
    config.bytes[0x34] = 0xfc;
    config.bytes[0xfc] = kind;
    PciFunctionState::State state;
    EXPECT_FALSE(PciFunctionState::inspect(config, state));
  }
  for (uint8_t next : {0x40U, 0x44U, 0x51U, 0x20U}) {
    Config config;
    config.bytes[0x41] = next;
    PciFunctionState::State state;
    EXPECT_FALSE(PciFunctionState::inspect(config, state));
  }
}
TEST(PciFunctionState, RejectsUnusablePicRoutesAndMissingFunctions) {
  for (uint8_t line : {0U, 1U, 2U, 8U, 13U, 16U, 255U}) {
    Config config;
    config.bytes[0x3c] = line;
    PciFunctionState::State state;
    EXPECT_FALSE(PciFunctionState::inspect(config, state));
  }
  for (uint8_t pin : {0U, 5U, 255U}) {
    Config config;
    config.bytes[0x3d] = pin;
    PciFunctionState::State state;
    EXPECT_FALSE(PciFunctionState::inspect(config, state));
  }
  Config config;
  config.set16(0, 0xffff);
  PciFunctionState::State state;
  EXPECT_FALSE(PciFunctionState::inspect(config, state));
}
TEST(PciFunctionState, DetectsRejectedMessageControlWrites) {
  Config config;
  PciFunctionState::State state;
  ASSERT_TRUE(PciFunctionState::inspect(config, state));
  config.refuse = true;
  EXPECT_FALSE(PciFunctionState::disableMessageInterrupts(config, state));
}
