/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "modules/drivers/common/usb-hcd/EhciLegacy.h"
#include <gtest/gtest.h>

namespace {
struct LegacyConfig {
  uint32_t registers[64]{};
  bool readable = true, writable = true;
  uint32_t stuckEnables = 0;
  unsigned writes = 0, reads = 0;
  uint16_t lastOffset = 0;
  uint32_t lastValue = 0;

  bool disable(uint16_t offset) {
    return EhciLegacy::disableSmis(
        offset,
        [this](uint16_t reg, uint32_t& value) {
          ++reads;
          value = registers[reg / 4];
          return readable;
        },
        [this](uint16_t reg, uint32_t value) {
          ++writes;
          lastOffset = reg;
          lastValue = value;
          if (!writable)
            return false;
          auto& stored = registers[reg / 4];
          stored = (stored & 0x003f0000U) | (stored & ~value & 0xe0000000U) | (value & 0xe03fU) |
                   stuckEnables;
          return true;
        });
  }
};
}  // namespace

TEST(EhciLegacy, DisablesInheritedSmisWithoutChangingSemaphoresOrAdjacentRegisters) {
  LegacyConfig config;
  config.registers[0x40 / 4] = 0x01000001U;
  config.registers[0x44 / 4] = 0xe03fe03fU;
  config.registers[0x48 / 4] = 0xdeadbeefU;
  ASSERT_TRUE(config.disable(0x40));
  EXPECT_EQ(config.lastOffset, 0x44);
  EXPECT_EQ(config.lastValue, 0xe0000000U);
  EXPECT_EQ(config.registers[0x40 / 4], 0x01000001U);
  EXPECT_EQ(config.registers[0x44 / 4], 0x003f0000U);
  EXPECT_EQ(config.registers[0x48 / 4], 0xdeadbeefU);
}

TEST(EhciLegacy, DisablesSmisWhenBiosWasAlreadyUnowned) {
  LegacyConfig config;
  config.registers[0xf8 / 4] = 1;
  config.registers[0xfc / 4] = 0xe03f;
  ASSERT_TRUE(config.disable(0xf8));
  EXPECT_EQ(config.registers[0xf8 / 4], 1U);
  EXPECT_EQ(config.registers[0xfc / 4], 0U);
}

TEST(EhciLegacy, RejectsUnrepresentableControlRegisterWithoutAccess) {
  for (uint16_t offset : {0U, 0x3cU, 0x41U, 0xfcU, 0x100U}) {
    LegacyConfig config;
    EXPECT_FALSE(config.disable(offset));
    EXPECT_EQ(config.writes, 0U);
    EXPECT_EQ(config.reads, 0U);
  }
}

TEST(EhciLegacy, RejectsEverySmiEnableThatRemainsSet) {
  for (unsigned bit = 0; bit < 16; ++bit) {
    if (!(0xe03fU & (1U << bit)))
      continue;
    LegacyConfig config;
    config.stuckEnables = 1U << bit;
    EXPECT_FALSE(config.disable(0x40));
  }
}

TEST(EhciLegacy, PropagatesConfigurationAccessFailure) {
  LegacyConfig config;
  config.writable = false;
  EXPECT_FALSE(config.disable(0x40));
  EXPECT_EQ(config.reads, 0U);
  config.writable = true;
  config.readable = false;
  EXPECT_FALSE(config.disable(0x40));
}
