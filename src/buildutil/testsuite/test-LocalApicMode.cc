/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "system/kernel/machine/mach_pc/LocalApicMode.h"
#include <gtest/gtest.h>

TEST(LocalApicMode, DecodesEveryArchitecturalMode) {
  using Mode = LocalApicMode::Mode;
  EXPECT_EQ(LocalApicMode::decode(0), Mode::Disabled);
  EXPECT_EQ(LocalApicMode::decode(LocalApicMode::Enabled), Mode::XApic);
  EXPECT_EQ(LocalApicMode::decode(LocalApicMode::Enabled | LocalApicMode::Extended), Mode::X2Apic);
  EXPECT_EQ(LocalApicMode::decode(LocalApicMode::Extended), Mode::Invalid);
}

TEST(LocalApicMode, FirmwareAddressAndBootstrapBitDoNotChangeMode) {
  constexpr uint64_t FirmwareState = 0xFEE00000ULL | (1ULL << 8);
  using Mode = LocalApicMode::Mode;
  EXPECT_EQ(LocalApicMode::decode(FirmwareState), Mode::Disabled);
  EXPECT_EQ(LocalApicMode::decode(FirmwareState | LocalApicMode::Enabled), Mode::XApic);
  EXPECT_EQ(LocalApicMode::decode(FirmwareState | LocalApicMode::Enabled | LocalApicMode::Extended),
            Mode::X2Apic);
}

TEST(LocalApicMode, X2ApicRegisterAndIcrEncoding) {
  EXPECT_EQ(LocalApicMode::x2ApicMsr(0x20), 0x802U);
  EXPECT_EQ(LocalApicMode::x2ApicMsr(0xB0), 0x80BU);
  EXPECT_EQ(LocalApicMode::x2ApicMsr(0x3E0), 0x83EU);
  EXPECT_EQ(LocalApicMode::x2ApicIcr(0x12000000, 0x000040FA), 0x12000040FAULL);
}
