/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <array>
#include <vector>

#include "modules/drivers/x86/pci/Bar.h"
#include "modules/drivers/x86/pci/ProbeBars.h"
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

namespace {
struct PciHeader {
  uint16_t vendor = 0x8086, device = 0x1502, command = 0x547;
  uint8_t class_code = 2, subclass = 0, header_type = 0;
  std::array<uint32_t, 6> bar{0x81000000, 0x82000004, 1, 0x6041, 0, 0};
};
struct PciConfig {
  explicit PciConfig(const PciHeader& header) : command(header.command), bars(header.bar) {}
  uint16_t command, commandReadOnlyOnes = 0;
  std::array<uint32_t, 6> bars;
  const std::array<uint32_t, 6> masks{0xfffe0000, 0xffffc004, 0xffffffff, 0xffffffe1, 0, 0};
  unsigned commandWrites = 0, barWrites = 0, reads = 0;
  unsigned rejectCommandWrite = 0;
  int rejectBarRestore = -1;
  std::vector<uint16_t> commands;

  bool updateCommand(int*, uint16_t clear, uint16_t set) {
    const uint16_t desired = (command & ~clear) | set;
    ++commandWrites;
    commands.push_back(desired);
    if (commandWrites != rejectCommandWrite)
      command = desired | commandReadOnlyOnes;
    return command == desired;
  }
  void writeConfigSpace(int*, uint8_t offset, uint32_t value) {
    ASSERT_GE(offset, 4);
    ASSERT_LT(offset, 10);
    EXPECT_EQ(command & 3U, 0U);
    EXPECT_EQ(command & 4U, 4U);
    ++barWrites;
    const unsigned bar = offset - 4;
    if (value == 0xffffffffU)
      bars[bar] = masks[bar];
    else if (static_cast<int>(bar) != rejectBarRestore)
      bars[bar] = value;
  }
  uint32_t readConfigSpace(int*, uint8_t offset) {
    ++reads;
    return bars.at(offset - 4);
  }
};
PciHeader qm67Lpc() {
  PciHeader header;
  header.device = 0x1c4f;
  header.class_code = 6;
  header.subclass = 1;
  header.header_type = 0x80;
  return header;
}
}  // namespace

TEST(PciBarProbe, Qm67LpcKeepsNodeWithoutCommandOrBarWrites) {
  const auto header = qm67Lpc();
  PciConfig config(header);
  config.commandReadOnlyOnes = 7;
  int device = 0;
  const auto result = PciBar::probe(config, &device, header);
  EXPECT_EQ(result.result, PciBar::ProbeResult::Success);
  EXPECT_EQ(result.count, 0U);
  EXPECT_EQ(config.commandWrites, 0U);
  EXPECT_EQ(config.barWrites, 0U);
  EXPECT_EQ(config.reads, 0U);
  EXPECT_EQ(config.command, header.command);
  EXPECT_EQ(config.bars, header.bar);
  for (uint32_t mask : result.masks)
    EXPECT_EQ(mask, 0U);
}

TEST(PciBarProbe, ExceptionRequiresExactIdentityClassAndHeader) {
  for (unsigned field = 0; field < 5; ++field) {
    auto header = qm67Lpc();
    if (field == 0)
      header.vendor = 0x1234;
    else if (field == 1)
      header.device = 0x1c4e;
    else if (field == 2)
      header.class_code = 2;
    else if (field == 3)
      header.subclass = 0;
    else
      header.header_type = 1;
    PciConfig config(header);
    config.commandReadOnlyOnes = 7;
    int device = 0;
    const auto result = PciBar::probe(config, &device, header);
    EXPECT_EQ(result.result, PciBar::ProbeResult::DecodeDisableFailed);
    EXPECT_EQ(config.commandWrites, 1U);
    EXPECT_EQ(config.barWrites, 0U);
  }
}

TEST(PciBarProbe, OrdinaryFunctionRestoresAllBarsAndCommandWithoutStoppingDma) {
  const PciHeader header;
  PciConfig config(header);
  int device = 0;
  const auto result = PciBar::probe(config, &device, header);
  ASSERT_EQ(result.result, PciBar::ProbeResult::Success);
  EXPECT_EQ(result.count, 6U);
  EXPECT_EQ(config.barWrites, 12U);
  EXPECT_EQ(config.bars, header.bar);
  EXPECT_EQ(config.command, header.command);
  ASSERT_EQ(config.commands.size(), 2U);
  EXPECT_EQ(config.commands[0], header.command & ~3U);
  EXPECT_EQ(config.commands[1], header.command);
  for (unsigned i = 0; i < result.count; ++i)
    EXPECT_EQ(result.masks[i], config.masks[i]);
}

TEST(PciBarProbe, DecodeRefusalPreventsAllBarWrites) {
  const PciHeader header;
  PciConfig config(header);
  config.rejectCommandWrite = 1;
  int device = 0;
  const auto result = PciBar::probe(config, &device, header);
  EXPECT_EQ(result.result, PciBar::ProbeResult::DecodeDisableFailed);
  EXPECT_EQ(config.commandWrites, 1U);
  EXPECT_EQ(config.barWrites, 0U);
  EXPECT_EQ(config.bars, header.bar);
}

TEST(PciBarProbe, BarRestoreFailureStopsProbingAndDisablesDma) {
  const PciHeader header;
  PciConfig config(header);
  config.rejectBarRestore = 0;
  int device = 0;
  const auto result = PciBar::probe(config, &device, header);
  EXPECT_EQ(result.result, PciBar::ProbeResult::RestoreFailed);
  EXPECT_EQ(config.barWrites, 2U);
  EXPECT_EQ(config.commandWrites, 2U);
  EXPECT_EQ(config.command, header.command & ~7U);
  EXPECT_EQ(config.bars[1], header.bar[1]);
}

TEST(PciBarProbe, CommandRestoreFailureDisablesDmaAfterRestoringBars) {
  const PciHeader header;
  PciConfig config(header);
  config.rejectCommandWrite = 2;
  int device = 0;
  const auto result = PciBar::probe(config, &device, header);
  EXPECT_EQ(result.result, PciBar::ProbeResult::RestoreFailed);
  EXPECT_EQ(config.commandWrites, 3U);
  EXPECT_EQ(config.command, header.command & ~7U);
  EXPECT_EQ(config.bars, header.bar);
}
