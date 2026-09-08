/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "modules/drivers/x86/pci/Bar.h"
#include <gtest/gtest.h>
TEST(PciBar, PreservesAbove4GiBFirmwareMmio) {
  PciBar::Mapping mapping;
  ASSERT_TRUE(PciBar::decode(0x4, 0xc0, 0xffffc004, 0xffffffff, mapping));
  EXPECT_EQ(mapping.base, 0xc000000000ULL);
  EXPECT_EQ(mapping.bytes, 0x4000U);
  EXPECT_FALSE(mapping.io);
}
TEST(PciBar, LegacyMmioAndFourByteIoAlignment) {
  PciBar::Mapping mapping;
  ASSERT_TRUE(PciBar::decode(0x81010000, 0, 0xfffff000, 0, mapping));
  EXPECT_EQ(mapping.base, 0x81010000U);
  EXPECT_EQ(mapping.bytes, 4096U);
  ASSERT_TRUE(PciBar::decode(0x6045, 0, 0xfffffffd, 0, mapping));
  EXPECT_EQ(mapping.base, 0x6044U);
  EXPECT_EQ(mapping.bytes, 4U);
  EXPECT_TRUE(mapping.io);
  ASSERT_TRUE(PciBar::decode(0x6045, 0, 0x0000fffd, 0, mapping));
  EXPECT_EQ(mapping.base, 0x6044U);
  EXPECT_EQ(mapping.bytes, 4U);
}
TEST(PciBar, RejectsUnassignedReservedMisalignedAndOversizeIo) {
  PciBar::Mapping mapping;
  EXPECT_FALSE(PciBar::decode(0, 0, 0, 0, mapping));
  EXPECT_FALSE(PciBar::decode(0x1006, 0, 0xfffff000, 0, mapping));
  EXPECT_FALSE(PciBar::decode(0x81011000, 0, 0xffffc000, 0, mapping));
  EXPECT_FALSE(PciBar::decode(0x10001, 0, 0xfffffffd, 0, mapping));
}
