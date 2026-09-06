/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include <cstring>

#include "modules/system/vfs/SwapStore.h"
#include <gtest/gtest.h>
namespace {
constexpr size_t Page = PagingChannel::PageBytes;
void word(unsigned char* page, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i)
    page[offset + i] = value >> (i * 8);
}
void format(unsigned char* page, uint32_t last) {
  memset(page, 0, Page);
  memcpy(page + Page - 10, "SWAPSPACE2", 10);
  word(page, 1024, 1);
  word(page, 1028, last);
}
}  // namespace
TEST(SwapHeader, ValidGeometryReservesHeaderAndBoundsCapacity) {
  unsigned char page[Page];
  size_t usable = 0;
  format(page, 1);
  EXPECT_EQ(SwapStore::validateHeader(page, 2 * Page, usable), SwapStatus::Success);
  EXPECT_EQ(usable, 1U);
  format(page, 65535);
  EXPECT_EQ(SwapStore::validateHeader(page, 65536ULL * Page, usable), SwapStatus::Success);
  EXPECT_EQ(usable, SwapStore::MaximumPages);
}
TEST(SwapHeader, InvalidFormatAndUnsupportedBadPageListNeverActivate) {
  unsigned char page[Page];
  size_t usable = 9;
  format(page, 16);
  EXPECT_EQ(SwapStore::validateHeader(nullptr, 17 * Page, usable), SwapStatus::Invalid);
  EXPECT_EQ(usable, 0U);
  EXPECT_EQ(SwapStore::validateHeader(page, 17 * Page - 1, usable), SwapStatus::Invalid);
  EXPECT_EQ(SwapStore::validateHeader(page, 16 * Page, usable), SwapStatus::Invalid);
  page[Page - 10] = 0;
  EXPECT_EQ(SwapStore::validateHeader(page, 17 * Page, usable), SwapStatus::Invalid);
  format(page, 16);
  word(page, 1024, 2);
  EXPECT_EQ(SwapStore::validateHeader(page, 17 * Page, usable), SwapStatus::Invalid);
  format(page, 0);
  EXPECT_EQ(SwapStore::validateHeader(page, 17 * Page, usable), SwapStatus::Invalid);
  format(page, 16);
  word(page, 1032, 1);
  EXPECT_EQ(SwapStore::validateHeader(page, 17 * Page, usable), SwapStatus::Unsupported);
  EXPECT_EQ(usable, 0U);
}
