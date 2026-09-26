/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <vector>

#include "system/kernel/machine/mach_pc/AcpiDmar.h"
#include <gtest/gtest.h>

namespace {
void write16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
  bytes[offset] = value;
  bytes[offset + 1] = value >> 8;
}

void write32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  write16(bytes, offset, value);
  write16(bytes, offset + 2, value >> 16);
}

void write64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
  write32(bytes, offset, value);
  write32(bytes, offset + 4, value >> 32);
}

void seal(std::vector<uint8_t>& bytes) {
  write32(bytes, 4, bytes.size());
  bytes[9] = 0;
  uint8_t sum = 0;
  for (uint8_t byte : bytes) {
    sum += byte;
  }
  bytes[9] = uint8_t(0U - sum);
}

std::vector<uint8_t> dmar() {
  std::vector<uint8_t> bytes(48 + 16 + 8);
  write32(bytes, 0, 0x52414d44U);
  bytes[36] = 47;
  bytes[37] = 1;
  write16(bytes, 48, 0);
  write16(bytes, 50, 24);
  bytes[52] = 1;
  write64(bytes, 56, 0xfed90000U);
  bytes[64] = 3;  // IOAPIC device scope.
  bytes[65] = 8;
  bytes[70] = 31;
  seal(bytes);
  return bytes;
}
}  // namespace

TEST(AcpiDmar, DiscoversValidatedSegmentZeroHardwareAndInterruptRemapping) {
  const auto bytes = dmar();
  AcpiDmar::Info info;
  ASSERT_TRUE(AcpiDmar::parse(bytes.data(), bytes.size(), info));
  EXPECT_TRUE(info.interruptRemapping);
  EXPECT_EQ(info.hardwareUnitCount, 1U);
  EXPECT_EQ(info.segmentZeroUnitCount, 1U);
  EXPECT_EQ(info.segmentZeroIncludeAllCount, 1U);
  EXPECT_EQ(info.firstSegmentZeroUnitAddress, 0xfed90000U);
  EXPECT_EQ(info.firstSegmentZeroIncludeAllAddress, 0xfed90000U);
  EXPECT_EQ(info.hostAddressWidth, 48U);
  EXPECT_EQ(info.firstSegmentZeroIncludeAllRegisterPagesLog2, 0U);
  EXPECT_FALSE(info.reservedMemoryRegions);
}

TEST(AcpiDmar, TracksRemappingRegisterSetSize) {
  auto bytes = dmar();
  bytes[53] = 2;  // Four 4-KiB register pages.
  seal(bytes);
  AcpiDmar::Info info;
  ASSERT_TRUE(AcpiDmar::parse(bytes.data(), bytes.size(), info));
  EXPECT_EQ(info.firstSegmentZeroIncludeAllRegisterPagesLog2, 2U);

  write64(bytes, 56, 0xfed91000U);
  seal(bytes);
  EXPECT_FALSE(AcpiDmar::parse(bytes.data(), bytes.size(), info));
}

TEST(AcpiDmar, MatchesOnlyExplicitDirectEndpoints) {
  auto bytes = dmar();
  bytes[52] = 0;  // The sole hardware unit has explicit scopes, not INCLUDE_ALL.
  bytes[64] = 1;
  bytes[69] = 2;
  bytes[70] = 3;
  bytes[71] = 4;
  bytes.resize(90);
  write16(bytes, 50, 42);
  bytes[72] = 2;  // A bridge scope is not a direct endpoint.
  bytes[73] = 8;
  bytes[78] = 4;
  bytes[80] = 1;  // A two-hop endpoint scope needs PCI bridge traversal.
  bytes[81] = 10;
  bytes[86] = 1;
  bytes[88] = 3;
  seal(bytes);

  AcpiDmar::Info info;
  ASSERT_TRUE(AcpiDmar::parse(bytes.data(), bytes.size(), info));
  EXPECT_EQ(info.segmentZeroIncludeAllCount, 0U);
  EXPECT_EQ(info.firstSegmentZeroUnitAddress, 0xfed90000U);
  EXPECT_EQ(info.directEndpointCount, 1U);
  ASSERT_EQ(info.directBridgeCount, 1U);
  EXPECT_EQ(info.directBridges[0], 4U << 3);
  EXPECT_TRUE(info.includesDirectEndpoint(2, 3, 4));
  EXPECT_FALSE(info.includesDirectEndpoint(2, 3, 5));
  EXPECT_FALSE(info.includesDirectEndpoint(0, 4, 0));
  EXPECT_FALSE(info.includesDirectEndpoint(0, 1, 0));
}

TEST(AcpiDmar, RecordsOnlyValidatedDirectSegmentZeroBridges) {
  auto bytes = dmar();
  bytes[52] = 0;
  bytes[64] = 2;
  bytes[70] = 3;
  seal(bytes);
  AcpiDmar::Info info;
  ASSERT_TRUE(AcpiDmar::parse(bytes.data(), bytes.size(), info));
  ASSERT_EQ(info.directBridgeCount, 1U);
  EXPECT_EQ(info.directBridges[0], 3U << 3);
  EXPECT_EQ(info.directEndpointCount, 0U);

  const auto direct = bytes;
  for (size_t offset : {66U, 67U, 68U, 70U, 71U}) {
    bytes = direct;
    bytes[offset] = 0xff;
    seal(bytes);
    ASSERT_TRUE(AcpiDmar::parse(bytes.data(), bytes.size(), info));
    EXPECT_EQ(info.directBridgeCount, 0U);
  }
  bytes = direct;
  write16(bytes, 54, 1);
  seal(bytes);
  ASSERT_TRUE(AcpiDmar::parse(bytes.data(), bytes.size(), info));
  EXPECT_EQ(info.directBridgeCount, 0U);

  bytes = direct;
  bytes.resize(74);
  write16(bytes, 50, 26);
  bytes[65] = 10;
  bytes[72] = 1;
  seal(bytes);
  ASSERT_TRUE(AcpiDmar::parse(bytes.data(), bytes.size(), info));
  EXPECT_EQ(info.directBridgeCount, 0U);
}

TEST(AcpiDmar, DistinguishesOtherSegmentsAndReservedMemory) {
  auto bytes = dmar();
  bytes.resize(bytes.size() + 16 + 24);
  write16(bytes, 72, 0);
  write16(bytes, 74, 16);
  write16(bytes, 78, 1);
  write64(bytes, 80, 0xfeda0000U);
  write16(bytes, 88, 1);
  write16(bytes, 90, 24);
  seal(bytes);

  AcpiDmar::Info info;
  ASSERT_TRUE(AcpiDmar::parse(bytes.data(), bytes.size(), info));
  EXPECT_EQ(info.hardwareUnitCount, 2U);
  EXPECT_EQ(info.segmentZeroUnitCount, 1U);
  EXPECT_TRUE(info.reservedMemoryRegions);
}

TEST(AcpiDmar, RejectsBadChecksumLengthsAndHardwareEntries) {
  const auto valid = dmar();
  AcpiDmar::Info info;
  info.hardwareUnitCount = 99;

  auto bad = valid;
  bad[9] ^= 1;
  EXPECT_FALSE(AcpiDmar::parse(bad.data(), bad.size(), info));
  bad = valid;
  write32(bad, 4, bad.size() + 1);
  EXPECT_FALSE(AcpiDmar::parse(bad.data(), bad.size(), info));
  bad = valid;
  write16(bad, 50, 15);
  seal(bad);
  EXPECT_FALSE(AcpiDmar::parse(bad.data(), bad.size(), info));
  bad = valid;
  bad.resize(53);
  write16(bad, 50, 5);
  seal(bad);
  EXPECT_FALSE(AcpiDmar::parse(bad.data(), bad.size(), info));
  bad = valid;
  bad[65] = 7;
  seal(bad);
  EXPECT_FALSE(AcpiDmar::parse(bad.data(), bad.size(), info));
  bad = valid;
  bad[52] = 2;
  seal(bad);
  EXPECT_FALSE(AcpiDmar::parse(bad.data(), bad.size(), info));
  bad = valid;
  bad[53] = 0x10;
  seal(bad);
  EXPECT_FALSE(AcpiDmar::parse(bad.data(), bad.size(), info));
  bad = valid;
  write64(bad, 56, 0xfed90001U);
  seal(bad);
  EXPECT_FALSE(AcpiDmar::parse(bad.data(), bad.size(), info));
  EXPECT_EQ(info.hardwareUnitCount, 99U);
}
