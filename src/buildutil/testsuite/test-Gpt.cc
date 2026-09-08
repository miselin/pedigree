/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <array>
#include <vector>

#include "modules/drivers/common/partition/Gpt.h"
#include <gtest/gtest.h>

namespace {
void put(uint8_t* data, uint64_t value, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i)
    data[i] = value >> (8 * i);
}
struct Fixture {
  std::array<uint8_t, 4096> data{};
  std::vector<uint8_t> entries = std::vector<uint8_t>(16384);
  size_t sector = 512;
  uint64_t sectors = 65536;
  Fixture(size_t bytes = 512, bool backup = false) : sector(bytes) {
    put(data.data(), 0x5452415020494645ULL, 8);
    put(data.data() + 8, 0x10000, 4);
    put(data.data() + 12, 92, 4);
    put(data.data() + 24, backup ? sectors - 1 : 1, 8);
    put(data.data() + 32, backup ? 1 : sectors - 1, 8);
    put(data.data() + 40, 2 + entries.size() / sector, 8);
    put(data.data() + 48, sectors - 2 - entries.size() / sector, 8);
    put(data.data() + 72, backup ? sectors - 1 - entries.size() / sector : 2, 8);
    put(data.data() + 80, 128, 4);
    put(data.data() + 84, 128, 4);
    partition(1, 256, 1023);
    partition(33, 2048, 4095);
    seal();
  }
  void partition(size_t index, uint64_t start, uint64_t end) {
    uint8_t* entry = entries.data() + index * 128;
    entry[0] = 0xa2;
    entry[16] = index + 1;
    put(entry + 32, start, 8);
    put(entry + 40, end, 8);
  }
  void seal() {
    put(data.data() + 88, Gpt::crc(entries.data(), entries.size()), 4);
    put(data.data() + 16, Gpt::crc(data.data(), 92, true), 4);
  }
  bool decode(bool backup = false) {
    Gpt::Header header;
    return Gpt::decode(data.data(), sector, sectors, backup, header) &&
           Gpt::validEntries(entries.data(), header);
  }
};
}  // namespace
TEST(Gpt, StandardChecksumVector) {
  EXPECT_EQ(Gpt::crc(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xcbf43926U);
}
TEST(Gpt, NativeGeometryAndBackup) {
  for (size_t sector : {512, 4096}) {
    for (bool backup : {false, true}) {
      Fixture fixture(sector, backup);
      const auto original = fixture.data;
      EXPECT_TRUE(fixture.decode(backup));
      EXPECT_EQ(original, fixture.data);
      EXPECT_FALSE(fixture.decode(!backup));
    }
  }
}
TEST(Gpt, IndependentHeaderAndArrayChecksums) {
  Fixture fixture;
  fixture.data[56] ^= 1;
  EXPECT_FALSE(fixture.decode());
  fixture.seal();
  EXPECT_TRUE(fixture.decode());
  fixture.entries[5000] ^= 1;
  EXPECT_FALSE(fixture.decode());
}
TEST(Gpt, RejectsUnsupportedAndOverlappingMetadataGeometry) {
  for (const auto field : {12, 20, 24, 32, 40, 48, 72, 80, 84}) {
    Fixture fixture;
    put(fixture.data.data() + field, field == 80 ? 4097 : 0xffffffffU, 4);
    fixture.seal();
    EXPECT_FALSE(fixture.decode()) << field;
  }
  Fixture fixture;
  put(fixture.data.data() + 72, 256, 8);
  fixture.seal();
  EXPECT_FALSE(fixture.decode());
}
TEST(Gpt, RejectsOverlappingReversedOutOfBoundsAndDuplicateGuid) {
  for (size_t kind = 0; kind < 4; ++kind) {
    Fixture fixture;
    if (kind == 0)
      fixture.partition(2, 1023, 2048);
    if (kind == 1)
      fixture.partition(2, 10000, 9999);
    if (kind == 2)
      fixture.partition(2, 10000, 65535);
    if (kind == 3) {
      fixture.partition(2, 10000, 11000);
      fixture.entries[2 * 128 + 16] = fixture.entries[128 + 16];
    }
    fixture.seal();
    EXPECT_FALSE(fixture.decode()) << kind;
  }
}
TEST(Gpt, RecognizesEmptyTableAndIgnoresSectorPadding) {
  Fixture fixture(4096);
  fixture.entries.assign(16384, 0);
  fixture.seal();
  EXPECT_TRUE(fixture.decode());
  fixture.data[1000] = 123;
  EXPECT_TRUE(fixture.decode());
}
