#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/core/SlamBitmap.h"

#include <gtest/gtest.h>

TEST(SlamBitmap, FindsRunsAcrossEntries) {
  SlamBitmap::Entry entries[2] = {};
  SlamBitmap bitmap(entries, 2, 128);
  bitmap.reserve(0, 64);
  bitmap.reserve(70, 2);

  EXPECT_EQ(bitmap.findFreeRun(2), 64U);
  EXPECT_EQ(bitmap.findFreeRun(8), 72U);
}

TEST(SlamBitmap, TracksPlanesIndependently) {
  SlamBitmap::Entry entries[1] = {};
  SlamBitmap bitmap(entries, 1, 64);
  bitmap.reserve(3, 1);
  bitmap.setMapped(3);

  EXPECT_TRUE(bitmap.isReserved(3));
  EXPECT_TRUE(bitmap.isMapped(3));
  bitmap.release(3, 1);
  EXPECT_FALSE(bitmap.isReserved(3));
  EXPECT_FALSE(bitmap.isMapped(3));
}
