#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/core/SlamBitmap.h"

#include <gtest/gtest.h>

TEST(SlamBitmap, FindsRunsAcrossEntries) {
  uint64_t entries[2 * 3] = {};
  SlamBitmap bitmap;
  bitmap.useMemory(entries, 2, 128);
  bitmap.reserve(0, 64);
  bitmap.reserve(70, 2);

  EXPECT_EQ(bitmap.findFreeRun(2), 64U);
  EXPECT_EQ(bitmap.findFreeRun(8), 72U);
}

TEST(SlamBitmap, TracksPlanesIndependently) {
  uint64_t entries[3] = {};
  SlamBitmap bitmap;
  bitmap.useMemory(entries, 1, 64);
  bitmap.reserve(3, 1);
  bitmap.setMapped(3);

  EXPECT_TRUE(bitmap.isReserved(3));
  EXPECT_TRUE(bitmap.isMapped(3));
  bitmap.release(3, 1);
  EXPECT_FALSE(bitmap.isReserved(3));
  EXPECT_FALSE(bitmap.isMapped(3));
}

TEST(SlamBitmap, FindsAndReservesDistinctRuns) {
  uint64_t entries[4 * 3] = {};
  SlamBitmap bitmap;
  bitmap.useMemory(entries, 4, 256);

  for (size_t expected = 0; expected < 64; expected += 8) {
    const size_t start = bitmap.findFreeRun(8);
    EXPECT_EQ(start, expected);
    bitmap.reserve(start, 8);
  }

  bitmap.release(24, 8);
  EXPECT_EQ(bitmap.findFreeRun(8), 24U);
}
