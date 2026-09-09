/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1

#include <array>
#include <vector>

#include "modules/drivers/x86/pci/IntelPirq.h"
#include <gtest/gtest.h>

namespace {
constexpr uint16_t Irq10 = 1U << 10;
constexpr uint16_t Irq11 = 1U << 11;

struct RouteConfig {
  RouteConfig() {
    bytes.fill(0x80);
  }
  bool read8(uint16_t offset, uint8_t& value) {
    events.push_back('r');
    lastOffset = offset;
    if (++reads == 1 && failInitialRead)
      return false;
    value = bytes.at(offset);
    if (reads == 2 && mismatchReadback)
      value ^= 1;
    return true;
  }
  bool write8(uint16_t offset, uint8_t value) {
    events.push_back('w');
    lastOffset = offset;
    if (++writes == 2 && failRestore)
      return false;
    bytes.at(offset) = value;
    // A failed posted write can still have reached the routing register.
    return !(writes == 1 && failFirstWrite);
  }
  IntelPirq::Result establish(uint8_t pirq, uint16_t excluded, uint8_t& irq) {
    return IntelPirq::establish(
        *this,
        [this](uint8_t selected) {
          events.push_back('s');
          if (!(available & (1U << selected)))
            return false;
          reserved |= 1U << selected;
          return true;
        },
        pirq, excluded, irq);
  }

  std::array<uint8_t, 128> bytes;
  std::vector<char> events;
  uint16_t available = 0xffff, reserved = 0, lastOffset = 0;
  unsigned reads = 0, writes = 0;
  bool failInitialRead = false, mismatchReadback = false;
  bool failFirstWrite = false, failRestore = false;
};
}  // namespace

TEST(IntelPirq, EnabledFirmwareRouteIsReservedWithoutAnyWrite) {
  RouteConfig config;
  config.bytes[0x60] = 0x5b;
  uint8_t irq = 0xee;
  EXPECT_EQ(config.establish(0, 0, irq), IntelPirq::Result::Ready);
  EXPECT_EQ(irq, 11);
  EXPECT_EQ(config.bytes[0x60], 0x5b);
  EXPECT_EQ(config.reserved, Irq11);
  EXPECT_EQ(config.events, (std::vector<char>{'r', 's'}));
}

TEST(IntelPirq, DisabledRouteReservesBeforeWritingAndPreservesReservedBits) {
  RouteConfig config;
  config.bytes[0x60] = 0xdf;
  uint8_t irq = 0xee;
  EXPECT_EQ(config.establish(0, 0, irq), IntelPirq::Result::Ready);
  EXPECT_EQ(irq, 10);
  EXPECT_EQ(config.bytes[0x60], 0x5a);
  EXPECT_EQ(config.reserved, Irq10);
  EXPECT_EQ(config.events, (std::vector<char>{'r', 's', 'w', 'r'}));
}

TEST(IntelPirq, EveryPirqUsesItsDocumentedConfigByte) {
  const uint16_t offsets[] = {0x60, 0x61, 0x62, 0x63, 0x68, 0x69, 0x6a, 0x6b};
  for (uint8_t pirq = 0; pirq < 8; ++pirq) {
    RouteConfig config;
    uint8_t irq = 0xee;
    ASSERT_EQ(config.establish(pirq, 0, irq), IntelPirq::Result::Ready);
    EXPECT_EQ(config.lastOffset, offsets[pirq]);
    EXPECT_EQ(IntelPirq::configOffset(pirq), offsets[pirq]);
    EXPECT_EQ(irq, 10 + (pirq & 1));
  }
}

TEST(IntelPirq, PicConflictAndChipsetExclusionsNeverWriteUnavailableRoutes) {
  uint16_t chipsetMask = 0;
  ASSERT_TRUE(IntelPirq::chipsetExclusions(0, 0x81, chipsetMask));
  chipsetMask |= IntelPirq::hpetExclusion((11U << 9) | 4, 0, 2, false);
  RouteConfig chipset;
  uint8_t unchanged = 0xee;
  EXPECT_EQ(chipset.establish(0, chipsetMask, unchanged), IntelPirq::Result::Conflict);
  EXPECT_EQ(chipset.events, (std::vector<char>{'r'}));
  EXPECT_EQ(unchanged, 0xee);
  for (uint16_t excluded : {uint16_t{0}, Irq10, Irq11, uint16_t(Irq10 | Irq11)}) {
    RouteConfig config;
    config.available = excluded;
    uint8_t irq = 0xee;
    EXPECT_EQ(config.establish(0, excluded, irq), IntelPirq::Result::Conflict);
    EXPECT_EQ(config.writes, 0U);
    EXPECT_EQ(config.reserved, 0);
    EXPECT_EQ(irq, 0xee);
  }
  RouteConfig fallback;
  fallback.available = Irq11;
  uint8_t irq = 0xee;
  EXPECT_EQ(fallback.establish(0, 0, irq), IntelPirq::Result::Ready);
  EXPECT_EQ(irq, 11);
  EXPECT_EQ(fallback.events, (std::vector<char>{'r', 's', 's', 'w', 'r'}));
}

TEST(IntelPirq, EnabledRouteIsNeverMovedAroundAnExclusionOrConflict) {
  for (bool excluded : {false, true}) {
    RouteConfig config;
    config.bytes[0x60] = 10;
    config.available = Irq11;
    uint8_t irq = 0xee;
    EXPECT_EQ(config.establish(0, excluded ? Irq10 : 0, irq),
              excluded ? IntelPirq::Result::Unsupported : IntelPirq::Result::Conflict);
    EXPECT_EQ(config.writes, 0U);
    EXPECT_EQ(config.bytes[0x60], 10);
    EXPECT_EQ(irq, 0xee);
  }
}

TEST(IntelPirq, FailedProgrammingRollsBackAndKeepsReservationAndOutput) {
  for (bool failWrite : {false, true}) {
    RouteConfig config;
    config.bytes[0x60] = 0xc3;
    config.failFirstWrite = failWrite;
    config.mismatchReadback = !failWrite;
    uint8_t irq = 0xee;
    EXPECT_EQ(config.establish(0, 0, irq), IntelPirq::Result::WriteFailed);
    EXPECT_EQ(config.bytes[0x60], 0xc3);
    EXPECT_EQ(config.writes, 2U);
    EXPECT_EQ(config.reserved, Irq10);
    EXPECT_EQ(irq, 0xee);
  }
  RouteConfig broken;
  broken.failFirstWrite = broken.failRestore = true;
  uint8_t irq = 0xee;
  EXPECT_EQ(broken.establish(0, 0, irq), IntelPirq::Result::RestoreFailed);
  EXPECT_EQ(broken.bytes[0x60], 10);
  EXPECT_EQ(broken.reserved, Irq10);
  EXPECT_EQ(irq, 0xee);
}

TEST(IntelPirq, InvalidConfigAndSelectorsDoNotTouchRoutingOrOutput) {
  for (uint8_t original : {0xff, 0x00, 0x01, 0x02, 0x08, 0x0d}) {
    RouteConfig config;
    config.bytes[0x60] = original;
    uint8_t irq = 0xee;
    EXPECT_EQ(config.establish(0, 0, irq),
              original == 0xff ? IntelPirq::Result::ReadFailed : IntelPirq::Result::Unsupported);
    EXPECT_EQ(config.events, (std::vector<char>{'r'}));
    EXPECT_EQ(irq, 0xee);
  }
  RouteConfig config;
  uint8_t irq = 0xee;
  EXPECT_EQ(config.establish(8, 0, irq), IntelPirq::Result::Unsupported);
  EXPECT_TRUE(config.events.empty());
  config.failInitialRead = true;
  EXPECT_EQ(config.establish(0, 0, irq), IntelPirq::Result::ReadFailed);
  EXPECT_EQ(config.writes, 0U);
  EXPECT_EQ(irq, 0xee);
}

TEST(IntelPirq, NativePinsAndPirqSelectorsFollowFirmwareFunctionNumbers) {
  for (uint8_t pin = 1; pin <= 4; ++pin) {
    uint8_t pirq = 0xee;
    ASSERT_TRUE(IntelPirq::decode(0x7654, pin, pirq));
    EXPECT_EQ(pirq, pin + 3);
  }
  EXPECT_TRUE(IntelPirq::nativePinMatches(0x00000004, 0, 4));  // EHCI is function zero.
  EXPECT_FALSE(IntelPirq::nativePinMatches(0x00000004, 7, 4));
  EXPECT_TRUE(IntelPirq::nativePinMatches(0x00000100, 2, 1));  // SATA AHCI is function two.
  EXPECT_FALSE(IntelPirq::nativePinMatches(0x00000100, 0, 1));
  const uint8_t devices[] = {31, 29, 27, 26, 25, 22};
  const uint16_t pins[] = {0x3100, 0x3108, 0x3110, 0x3114, 0x3118, 0x3124};
  const uint16_t routes[] = {0x3140, 0x3144, 0x3148, 0x314c, 0x3150, 0x315c};
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(IntelPirq::pinOffset(devices[i]), pins[i]);
    EXPECT_EQ(IntelPirq::routeOffset(devices[i]), routes[i]);
  }
  for (uint8_t device : {0, 28, 30, 255}) {
    EXPECT_EQ(IntelPirq::pinOffset(device), 0);
    EXPECT_EQ(IntelPirq::routeOffset(device), 0);
  }
  uint8_t pirq = 0xee;
  for (uint8_t pin : {0, 5, 255})
    EXPECT_FALSE(IntelPirq::decode(0, pin, pirq));
  EXPECT_FALSE(IntelPirq::decode(0xffff, 1, pirq));
  EXPECT_FALSE(IntelPirq::decode(0x0008, 1, pirq));
  EXPECT_EQ(pirq, 0xee);
  EXPECT_FALSE(IntelPirq::nativePinMatches(0xffffffff, 0, 1));
  EXPECT_FALSE(IntelPirq::nativePinMatches(1, 8, 1));
}

TEST(IntelPirq, SciAndEnabledTcoSelectorsExcludeOnlyTheirPicLines) {
  uint16_t excluded = 0;
  ASSERT_TRUE(IntelPirq::chipsetExclusions(1, 0x82, excluded));
  EXPECT_EQ(excluded, Irq10 | Irq11);
  ASSERT_TRUE(IntelPirq::chipsetExclusions(0, 1, excluded));
  EXPECT_EQ(excluded, 1U << 9);
  ASSERT_TRUE(IntelPirq::chipsetExclusions(4, 0x85, excluded));
  EXPECT_EQ(excluded, 0);
  for (auto controls : {std::array<uint8_t, 2>{0xff, 0}, {0, 0xff}, {3, 0}, {0, 0x83}}) {
    excluded = 0x55aa;
    EXPECT_FALSE(IntelPirq::chipsetExclusions(controls[0], controls[1], excluded));
    EXPECT_EQ(excluded, 0x55aa);
  }
}

TEST(IntelPirq, HpetExclusionsRespectPendingLevelLegacyAndFsbDelivery) {
  const uint32_t irq10 = 10U << 9, irq11 = 11U << 9;
  // Globally stopping the HPET clock does not release its timer routes.
  EXPECT_EQ(IntelPirq::hpetExclusion(irq10 | 4, 0, 2, false), Irq10);
  EXPECT_EQ(IntelPirq::hpetExclusion(irq11 | 2, 4, 2, false), Irq11);
  EXPECT_EQ(IntelPirq::hpetExclusion(irq11 | 2, 0, 2, false), 0);
  EXPECT_EQ(IntelPirq::hpetExclusion(irq11, 4, 2, false), 0);
  EXPECT_EQ(IntelPirq::hpetExclusion(irq10 | 4, 0, 0, true), 0);
  EXPECT_EQ(IntelPirq::hpetExclusion(irq10 | 4, 0, 1, true), 0);
  EXPECT_EQ(IntelPirq::hpetExclusion(irq11 | 4, 0, 2, true), Irq11);
  EXPECT_EQ(IntelPirq::hpetExclusion(irq10 | 6 | (1U << 14), 4, 2, false), 0);
  EXPECT_EQ(IntelPirq::hpetExclusion((31U << 9) | 4, 0, 2, false), 0);
  EXPECT_EQ(IntelPirq::hpetExclusion(0xffffffff, 0, 2, false), Irq10 | Irq11);
  EXPECT_EQ(IntelPirq::hpetExclusion(0, 0xffffffff, 2, false), Irq10 | Irq11);
}
