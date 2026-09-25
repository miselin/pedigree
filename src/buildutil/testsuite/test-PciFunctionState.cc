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
  uint16_t refuseOffset = 0xffff;
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
  void set32(unsigned offset, uint32_t value) {
    set16(offset, value);
    set16(offset + 2, value >> 16);
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
    if (!refuse && offset != refuseOffset)
      set16(offset, value);
    return true;
  }
  bool write32(uint16_t offset, uint32_t value) {
    if (offset > 252 || offset & 3)
      return false;
    writes.push_back(offset);
    if (!refuse && offset != refuseOffset)
      set32(offset, value);
    return true;
  }
};
struct Table {
  std::array<uint32_t, 128> words{};
  size_t size() const {
    return words.size() * 4;
  }
  uint32_t read32(size_t offset) {
    return words[offset / 4];
  }
  void write32(uint32_t value, size_t offset) {
    words[offset / 4] = value;
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
TEST(PciFunctionState, AcceptsMessageOnlyFunctionsButStillRequiresAUsableCapability) {
  Config config;
  config.bytes[0x3c] = 255;
  config.bytes[0x3d] = 0;
  PciFunctionState::State state;
  EXPECT_FALSE(PciFunctionState::inspect(config, state));
  ASSERT_TRUE(PciFunctionState::inspect(config, state, false));
  EXPECT_EQ(state.msi, 0x50);
  EXPECT_EQ(state.msix, 0x70);
  config.bytes[0x41] = 0;
  EXPECT_FALSE(PciFunctionState::inspect(config, state, false));
}
TEST(PciFunctionState, EnablesAndDisablesOneMsiMessage) {
  Config config;
  PciFunctionState::State state;
  ASSERT_TRUE(PciFunctionState::inspect(config, state));
  ASSERT_TRUE(PciFunctionState::disableMessageInterrupts(config, state));
  config.set32(0x60, 0xffffffffU);
  ASSERT_TRUE(PciFunctionState::enableMsi(config, state, 0xfee01234ULL, 0x45));
  uint32_t address = 0, high = 0, mask = 0;
  uint16_t data = 0, control = 0, command = 0;
  ASSERT_TRUE(config.read32(0x54, address));
  ASSERT_TRUE(config.read32(0x58, high));
  ASSERT_TRUE(config.read16(0x5c, data));
  ASSERT_TRUE(config.read32(0x60, mask));
  ASSERT_TRUE(config.read16(0x52, control));
  ASSERT_TRUE(config.read16(4, command));
  EXPECT_EQ(address, 0xfee01234U);
  EXPECT_EQ(high, 0U);
  EXPECT_EQ(data, 0x45);
  EXPECT_EQ(mask, 0xfffffffeU);
  EXPECT_EQ(control & 0x71U, 1U);
  EXPECT_NE(command & 0x400U, 0U);
  ASSERT_TRUE(PciFunctionState::disableMsi(config, state));
  ASSERT_TRUE(config.read16(0x52, control));
  ASSERT_TRUE(config.read32(0x60, mask));
  EXPECT_EQ(control & 1U, 0U);
  EXPECT_EQ(mask & 1U, 1U);
}
TEST(PciFunctionState, RejectsUnrepresentableMsiAndRestoresRejectedWrites) {
  Config config;
  config.set16(0x52, 0);
  PciFunctionState::State state;
  ASSERT_TRUE(PciFunctionState::inspect(config, state));
  ASSERT_TRUE(PciFunctionState::disableMessageInterrupts(config, state));
  const auto before = config.bytes;
  EXPECT_FALSE(PciFunctionState::enableMsi(config, state, 0x1fee00000ULL, 0x45));
  EXPECT_EQ(config.bytes, before);
  config.refuse = true;
  EXPECT_FALSE(PciFunctionState::enableMsi(config, state, 0xfee00000ULL, 0x45));
  EXPECT_EQ(config.bytes, before);
  config.refuse = false;
  config.refuseOffset = 0x58;
  EXPECT_FALSE(PciFunctionState::enableMsi(config, state, 0xfee00000ULL, 0x45));
  EXPECT_EQ(config.bytes, before);
}
TEST(PciFunctionState, ValidatesAndProgramsOneMsixEntry) {
  Config config;
  config.set32(0x74, 0x100);
  config.set32(0x78, 0x200);
  PciFunctionState::State state;
  ASSERT_TRUE(PciFunctionState::inspect(config, state));
  PciFunctionState::MsixTable descriptor;
  ASSERT_TRUE(PciFunctionState::msixTable(config, state, descriptor));
  EXPECT_EQ(descriptor.bar, 0);
  EXPECT_EQ(descriptor.offset, 0x100U);
  EXPECT_EQ(descriptor.vectors, 8);
  ASSERT_TRUE(PciFunctionState::disableMessageInterrupts(config, state));
  Table table;
  ASSERT_TRUE(
      PciFunctionState::enableMsix(config, state, table, descriptor.offset, 0xfee00000ULL, 0x44));
  uint16_t control = 0, command = 0;
  ASSERT_TRUE(config.read16(0x72, control));
  ASSERT_TRUE(config.read16(4, command));
  EXPECT_EQ(control & 0xc000U, 0x8000U);
  EXPECT_NE(command & 0x400U, 0U);
  EXPECT_EQ(table.read32(0x100), 0xfee00000U);
  EXPECT_EQ(table.read32(0x108), 0x44U);
  EXPECT_EQ(table.read32(0x10c) & 1U, 0U);
  ASSERT_TRUE(PciFunctionState::disableMsix(config, state, table, descriptor.offset));
  ASSERT_TRUE(config.read16(0x72, control));
  EXPECT_EQ(control & 0xc000U, 0x4000U);
  EXPECT_EQ(table.read32(0x10c) & 1U, 1U);
}
TEST(PciFunctionState, RejectsMalformedMsixTableAndRollsBack) {
  Config config;
  PciFunctionState::State state;
  ASSERT_TRUE(PciFunctionState::inspect(config, state));
  PciFunctionState::MsixTable descriptor;
  config.set32(0x74, 0x106);
  config.set32(0x78, 0x200);
  EXPECT_FALSE(PciFunctionState::msixTable(config, state, descriptor));
  config.set32(0x74, 0x100);
  config.set32(0x78, 0x110);
  EXPECT_FALSE(PciFunctionState::msixTable(config, state, descriptor));
  config.set32(0x78, 0x200);
  ASSERT_TRUE(PciFunctionState::msixTable(config, state, descriptor));
  ASSERT_TRUE(PciFunctionState::disableMessageInterrupts(config, state));
  const auto before = config.bytes;
  Table table;
  table.write32(1, 0x10c);
  config.refuseOffset = 4;
  EXPECT_FALSE(PciFunctionState::enableMsix(config, state, table, 0x100, 0xfee00000ULL, 0x44));
  EXPECT_EQ(config.bytes, before);
  EXPECT_EQ(table.read32(0x100), 0U);
  EXPECT_EQ(table.read32(0x10c), 1U);
}

TEST(PciFunctionState, ProgramsAndMasksDistinctMsixVectors) {
  Config config;
  PciFunctionState::State state;
  ASSERT_TRUE(PciFunctionState::inspect(config, state));
  ASSERT_TRUE(PciFunctionState::disableMessageInterrupts(config, state));
  Table table;
  const uint32_t data[] = {0x44, 0x45};
  ASSERT_TRUE(
      PciFunctionState::enableMsixVectors(config, state, table, 0x100, 0xfee00000ULL, data, 2));
  EXPECT_EQ(table.read32(0x100), 0xfee00000U);
  EXPECT_EQ(table.read32(0x108), 0x44U);
  EXPECT_EQ(table.read32(0x118), 0x45U);
  EXPECT_EQ(table.read32(0x10c) & 1U, 0U);
  EXPECT_EQ(table.read32(0x11c) & 1U, 0U);
  ASSERT_TRUE(PciFunctionState::setMsixVectorMask(config, state, table, 0x100, 0, true));
  EXPECT_EQ(table.read32(0x10c) & 1U, 1U);
  EXPECT_EQ(table.read32(0x11c) & 1U, 0U);
  ASSERT_TRUE(PciFunctionState::setMsixVectorMask(config, state, table, 0x100, 0, false));
  EXPECT_EQ(table.read32(0x10c) & 1U, 0U);
  EXPECT_FALSE(PciFunctionState::setMsixVectorMask(config, state, table, 0x100, 8, true));
}

TEST(PciFunctionState, RejectsMsixBatchWithoutLeavingPartialEntries) {
  Config config;
  PciFunctionState::State state;
  ASSERT_TRUE(PciFunctionState::inspect(config, state));
  ASSERT_TRUE(PciFunctionState::disableMessageInterrupts(config, state));
  const auto before = config.bytes;
  Table table;
  const uint32_t data[] = {0x44, 0x45};
  config.refuseOffset = 4;
  EXPECT_FALSE(
      PciFunctionState::enableMsixVectors(config, state, table, 0x100, 0xfee00000ULL, data, 2));
  EXPECT_EQ(config.bytes, before);
  EXPECT_EQ(table.read32(0x100), 0U);
  EXPECT_EQ(table.read32(0x108), 0U);
  EXPECT_EQ(table.read32(0x110), 0U);
  EXPECT_EQ(table.read32(0x118), 0U);
  config.refuseOffset = 0xffff;
  config.set16(0x72, 0);
  EXPECT_FALSE(
      PciFunctionState::enableMsixVectors(config, state, table, 0x100, 0xfee00000ULL, data, 2));
}
