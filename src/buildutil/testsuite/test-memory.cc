/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/utility.h"

#include <string.h>

#include <gtest/gtest.h>

TEST(PedigreeMemoryLibrary, ForwardMemoryCopy) {
  char buf1[16];
  char buf2[16];

  memset(buf1, 0, 16);
  memset(buf2, 0xAB, 16);

  MemoryCopy(buf1, buf2, 16);

  EXPECT_EQ(memcmp(buf1, buf2, 16), 0);
}

TEST(PedigreeMemoryLibrary, OverlappingMemoryCopy) {
  char buf[16] = {0};
  char expected[16] = {0};

  // This copy will bring across the null terminating byte.
  strcpy(buf, "ABCDEFGHIJKLMNO");
  strcpy(expected, "BCDEFGHIJKLMNO");

  MemoryCopy(buf, buf + 1, 15);

  EXPECT_STREQ(buf, expected);
}

TEST(PedigreeMemoryLibrary, ReversedMemoryCopy) {
  char buf[32] = {0};
  char expected[32] = {0};

  // This copy will bring across the null terminating byte.
  strcpy(buf, "ABCDEFGHIJKLMNO");
  strcpy(expected, "AABCDEFGHIJKLMNO");

  MemoryCopy(buf + 1, buf, 16);

  EXPECT_STREQ(buf, expected);
}

TEST(PedigreeMemoryLibrary, CopyAlignmentAndBounds) {
  const size_t lengths[] = {0, 1, 7, 8, 9, 63, 64, 65, 71, 72, 73, 127, 128, 129, 4095, 4096, 4097};
  unsigned char source[4128], actual[4128], expected[4128];
  for (size_t i = 0; i < sizeof(source); ++i) {
    source[i] = static_cast<unsigned char>(i * 37 + (i >> 8));
  }
  for (auto copy : {MemoryCopy, ForwardMemoryCopy}) {
    for (size_t length : lengths) {
      for (size_t src = 0; src < 8; ++src) {
        for (size_t dest = 0; dest < 8; ++dest) {
          memset(actual, 0xA5, sizeof(actual));
          memset(expected, 0xA5, sizeof(expected));
          memcpy(expected + 8 + dest, source + src, length);
          ASSERT_EQ(copy(actual + 8 + dest, source + src, length), actual + 8 + dest);
          ASSERT_EQ(memcmp(actual, expected, sizeof(actual)), 0)
              << "length=" << length << " src=" << src << " dest=" << dest;
        }
      }
    }
  }
}

TEST(PedigreeMemoryLibrary, OverlappingCopyAlignmentAndBounds) {
  const size_t lengths[] = {0, 1, 7, 8, 9, 63, 64, 65, 71, 72, 73, 127, 128, 129, 4095, 4096, 4097};
  unsigned char original[4160], actual[4160], expected[4160];
  for (size_t i = 0; i < sizeof(original); ++i) {
    original[i] = static_cast<unsigned char>(i * 37 + (i >> 8));
  }
  for (size_t length : lengths) {
    for (size_t src = 16; src < 24; ++src) {
      for (int distance = -9; distance <= 9; ++distance) {
        const size_t dest = static_cast<size_t>(static_cast<int>(src) + distance);
        memcpy(actual, original, sizeof(actual));
        memcpy(expected, original, sizeof(expected));
        memmove(expected + dest, expected + src, length);
        ASSERT_EQ(MemoryCopy(actual + dest, actual + src, length), actual + dest);
        ASSERT_EQ(memcmp(actual, expected, sizeof(actual)), 0)
            << "length=" << length << " src=" << src << " distance=" << distance;
      }
    }
  }
}

TEST(PedigreeMemoryLibrary, ByteSetAlignmentAndBounds) {
  const size_t lengths[] = {0, 1, 7, 8, 9, 63, 64, 65, 71, 72, 73, 127, 128, 129, 4095, 4096, 4097};
  unsigned char actual[4128], expected[4128];
  for (size_t length : lengths) {
    for (size_t offset = 0; offset < 8; ++offset) {
      for (int value : {0, -1, 0xAB, 0x12AB, 0x100}) {
        memset(actual, 0xA5, sizeof(actual));
        memset(expected, 0xA5, sizeof(expected));
        memset(expected + 8 + offset, value, length);
        ASSERT_EQ(ByteSet(actual + 8 + offset, value, length), actual + 8 + offset);
        ASSERT_EQ(memcmp(actual, expected, sizeof(actual)), 0)
            << "length=" << length << " offset=" << offset << " value=" << value;
      }
    }
  }
}

TEST(PedigreeMemoryLibrary, MemoryCompareSame) {
  char buf1[32];
  char buf2[32];

  memset(buf1, 'a', 32);
  memset(buf2, 'a', 32);

  EXPECT_EQ(MemoryCompare(buf1, buf2, 32), 0);
}

TEST(PedigreeMemoryLibrary, MemoryCompareDiffers) {
  char buf1[32];
  char buf2[32];

  memset(buf1, 'a', 32);
  memset(buf2, 'b', 32);

  EXPECT_NE(MemoryCompare(buf1, buf2, 32), 0);
}

TEST(PedigreeMemoryLibrary, MemoryCompareHalfDiffers) {
  char buf1[32];
  char buf2[32];

  memset(buf1, 'a', 32);
  memset(buf2, 'a', 16);
  memset(buf2 + 16, 'b', 16);

  EXPECT_NE(MemoryCompare(buf1, buf2, 32), 0);
}
