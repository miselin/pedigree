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

#include "pedigree/kernel/utilities/RangeList.h"

#include <gtest/gtest.h>

template <typename T, bool Reversed = false>
static typename RangeList<T, Reversed>::Range rangeAt(const RangeList<T, Reversed>& list,
                                                      size_t index) {
  typename RangeList<T, Reversed>::Range range(0, 0);
  EXPECT_TRUE(list.getRange(index, range));
  return range;
}

TEST(PedigreeRangeList, Forward) {
  RangeList<int64_t> list;

  list.free(0, 1024);

  int64_t addr = 0;
  EXPECT_TRUE(list.allocate(1, addr));
  EXPECT_EQ(addr, 0);
  EXPECT_TRUE(list.allocate(1, addr));
  EXPECT_EQ(addr, 1);
}

TEST(PedigreeRangeList, Reversed) {
  RangeList<int64_t, true> list;

  list.free(0, 1024);

  int64_t addr = 0;
  EXPECT_TRUE(list.allocate(1, addr));
  EXPECT_EQ(addr, 1023);
  EXPECT_TRUE(list.allocate(1, addr));
  EXPECT_EQ(addr, 1022);
}

TEST(PedigreeRangeList, NeedsFreeFirst) {
  RangeList<int64_t> list;
  int64_t addr = 0;
  EXPECT_FALSE(list.allocate(1, addr));

  list.free(0, 1024);

  EXPECT_TRUE(list.allocate(1, addr));
  EXPECT_EQ(addr, 0);
}

TEST(PedigreeRangeList, AllocateEntireRange) {
  RangeList<int64_t> list;

  // create 3 ranges (don't sweep)
  list.free(0, 128);
  list.free(0, 128);
  list.free(0, 128);

  int64_t addr = 0;
  EXPECT_TRUE(list.allocate(128, addr));
  EXPECT_EQ(addr, 0);
}

TEST(PedigreeRangeList, AllocateSkipsTooSmallRanges) {
  RangeList<int64_t> list;

  // create 3 ranges (don't sweep)
  list.free(0, 64);
  list.free(0, 64);
  list.free(0, 128);

  // the first pass doesn't sweep - just returns the first range that fits
  int64_t addr = 0;
  EXPECT_TRUE(list.allocate(128, addr));
  EXPECT_EQ(addr, 0);
}

TEST(PedigreeRangeList, AllocateSkipsTooSmallRangesWithSweep) {
  RangeList<int64_t> list;

  list.free(0, 64, false);
  list.free(64, 64, false);
  list.free(128, 64, false);

  // this will have to do a retry and sweep to succeed
  int64_t addr = 0;
  EXPECT_TRUE(list.allocate(128, addr));
  EXPECT_EQ(addr, 0);
}

TEST(PedigreeRangeList, AllocateSpecificWorks) {
  RangeList<int64_t> list;
  list.free(0, 1024);

  EXPECT_TRUE(list.allocateSpecific(0, 512));
  int64_t addr = 0;
  EXPECT_TRUE(list.allocate(1, addr));
  EXPECT_EQ(addr, 512);
}

TEST(PedigreeRangeList, AllocateSpecificMergesUnorderedFreeFragments) {
  RangeList<int64_t> list;
  list.free(0, 64);
  list.free(640, 64);
  list.free(128, 64);
  list.free(64, 64);

  ASSERT_EQ(list.size(), 3U);
  EXPECT_TRUE(list.allocateSpecific(0, 192));
  EXPECT_FALSE(list.allocateSpecific(0, 192));
  EXPECT_TRUE(list.allocateSpecific(640, 64));
  EXPECT_EQ(list.size(), 0U);
}

TEST(PedigreeRangeList, AllocateMergesLateBridge) {
  RangeList<int64_t> list;
  list.free(0, 64, false);
  list.free(128, 64, false);
  list.free(64, 64, false);

  int64_t address = -1;
  EXPECT_TRUE(list.allocate(192, address));
  EXPECT_EQ(address, 0);
  EXPECT_FALSE(list.allocate(1, address));
}

TEST(PedigreeRangeList, SweepRetainsOrderOfUnrelatedRanges) {
  RangeList<int64_t> list;
  list.free(128, 64, false);
  list.free(640, 64, false);
  list.free(0, 64, false);
  list.free(64, 64, false);

  list.sweep();
  ASSERT_EQ(list.size(), 2U);
  EXPECT_EQ(rangeAt(list, 0), RangeList<int64_t>::Range(0, 192));
  EXPECT_EQ(rangeAt(list, 1), RangeList<int64_t>::Range(640, 64));
}

TEST(PedigreeRangeList, AllocateSpecificFailsAlreadyAllocated) {
  RangeList<int64_t> list;
  list.free(0, 1024);

  // four allocations to consume 0-512
  int64_t addr = 0;
  EXPECT_TRUE(list.allocate(128, addr));
  EXPECT_TRUE(list.allocate(128, addr));
  EXPECT_TRUE(list.allocate(128, addr));
  EXPECT_TRUE(list.allocate(128, addr));

  // can't allocate addresses 0-256, already allocated
  EXPECT_FALSE(list.allocateSpecific(0, 256));

  // clean up addresses 128-256
  list.free(128, 128);

  // still can't allocate 0-256
  EXPECT_FALSE(list.allocateSpecific(0, 256));

  // clean up so 0-256 is fully freed
  list.free(0, 128);

  // now we can allocate the whole range
  EXPECT_TRUE(list.allocateSpecific(0, 256));
}

TEST(PedigreeRangeList, AllocateSpecificOnlyOnce) {
  RangeList<int64_t> list;
  list.free(0, 1024);

  int64_t addr = 0;
  EXPECT_TRUE(list.allocateSpecific(0, 512));
  EXPECT_FALSE(list.allocateSpecific(0, 512));
}

TEST(PedigreeRangeList, AllocateSpecificWithMerges) {
  RangeList<int64_t> list;
  list.free(0, 128, false);
  list.free(128, 128, false);
  list.free(256, 128, false);
  list.free(384, 128, false);

  int64_t addr = 0;
  // this will need to merge the first two freed ranges
  EXPECT_TRUE(list.allocateSpecific(0, 256));
}

TEST(PedigreeRangeList, AllocateSpecificAtEnd) {
  RangeList<int64_t> list;
  list.free(0, 256);

  int64_t addr = 0;
  // this has to split a range in the middle and return the end half
  EXPECT_TRUE(list.allocateSpecific(128, 128));
}

TEST(PedigreeRangeList, AllocateSpecificMiddle) {
  RangeList<int64_t> list;
  list.free(0, 384);

  int64_t addr = 0;
  EXPECT_TRUE(list.allocateSpecific(128, 128));
}

TEST(PedigreeRangeList, Sweep) {
  RangeList<int64_t> list;
  list.free(0, 64, false);
  list.free(64, 64, false);
  list.free(128, 64, false);

  EXPECT_EQ(list.size(), 3U);
  EXPECT_EQ(rangeAt(list, 0), RangeList<int64_t>::Range(0, 64));
  EXPECT_EQ(rangeAt(list, 1), RangeList<int64_t>::Range(64, 64));
  EXPECT_EQ(rangeAt(list, 2), RangeList<int64_t>::Range(128, 64));

  RangeList<int64_t>::Range missing(1, 1);
  EXPECT_FALSE(list.getRange(3, missing));
  EXPECT_EQ(missing, RangeList<int64_t>::Range(0, 0));

  list.sweep();

  EXPECT_EQ(list.size(), 1U);
  EXPECT_EQ(rangeAt(list, 0), RangeList<int64_t>::Range(0, 192));
}

TEST(PedigreeRangeList, Copy) {
  RangeList<int64_t> list;
  list.free(0, 64, false);
  list.free(64, 64, false);
  list.free(128, 64, false);
  RangeList<int64_t> list2;
  list2.free(0, 1024);
  list2 = list;

  EXPECT_EQ(list2.size(), 3U);
  EXPECT_EQ(rangeAt(list2, 0), RangeList<int64_t>::Range(0, 64));
  EXPECT_EQ(rangeAt(list2, 1), RangeList<int64_t>::Range(64, 64));
  EXPECT_EQ(rangeAt(list2, 2), RangeList<int64_t>::Range(128, 64));

  RangeList<int64_t> list3(list);
  EXPECT_EQ(list3.size(), 3U);
  EXPECT_EQ(rangeAt(list3, 0), RangeList<int64_t>::Range(0, 64));
  EXPECT_EQ(rangeAt(list3, 1), RangeList<int64_t>::Range(64, 64));
  EXPECT_EQ(rangeAt(list3, 2), RangeList<int64_t>::Range(128, 64));
}

TEST(PedigreeRangeList, CopiesPreserveReusePreference) {
  for (bool preferUsed : {false, true}) {
    RangeList<int64_t> source(preferUsed);
    source.free(0, 64);
    RangeList<int64_t> copied(source);
    RangeList<int64_t> assigned(!preferUsed);
    assigned.free(1024, 64);
    assigned = source;
    copied.free(256, 64);
    assigned.free(256, 64);
    int64_t address = -1;
    const int64_t expected = preferUsed ? 256 : 0;
    EXPECT_TRUE(copied.allocate(64, address));
    EXPECT_EQ(address, expected);
    EXPECT_TRUE(assigned.allocate(64, address));
    EXPECT_EQ(address, expected);
    EXPECT_EQ(source.size(), 1U);
    EXPECT_EQ(rangeAt(source, 0), RangeList<int64_t>::Range(0, 64));
  }
}

TEST(PedigreeRangeList, SelfAssignmentPreservesFreeRanges) {
  RangeList<int64_t> list(true);
  list.free(0, 64);
  list.free(256, 64);
  const RangeList<int64_t>& same = list;
  list = same;
  EXPECT_EQ(list.size(), 2U);
  EXPECT_TRUE(list.allocateSpecific(0, 64));
  EXPECT_TRUE(list.allocateSpecific(256, 64));
}

TEST(PedigreeRangeList, StorageSwapTransfersReservationsAndAllocationPreference) {
  RangeList<uintptr_t> first(false), second(true);
  first.free(0x1000, 0x3000);
  ASSERT_TRUE(first.allocateSpecific(0x2000, 0x1000));
  second.free(0x10000, 0x1000);
  second.free(0x20000, 0x1000);
  first.swap(second);
  first.free(0x30000, 0x1000);
  second.free(0x5000, 0x1000);
  uintptr_t address = 0;
  ASSERT_TRUE(first.allocate(0x1000, address));
  EXPECT_EQ(address, 0x30000U);
  EXPECT_FALSE(second.allocateSpecific(0x2000, 0x1000));
  ASSERT_TRUE(second.allocate(0x1000, address));
  EXPECT_EQ(address, 0x1000U);
  ASSERT_TRUE(second.allocate(0x1000, address));
  EXPECT_EQ(address, 0x3000U);
  second.swap(second);
  ASSERT_TRUE(second.allocate(0x1000, address));
  EXPECT_EQ(address, 0x5000U);
  RangeList<uintptr_t> empty;
  first.swap(empty);
  EXPECT_EQ(first.size(), 0U);
  EXPECT_TRUE(empty.allocateSpecific(0x10000, 0x1000));
  EXPECT_TRUE(empty.allocateSpecific(0x20000, 0x1000));
}

TEST(PedigreeRangeList, ReusesExhaustedStorageWithoutAllocation) {
  RangeList<uintptr_t> ranges(false);
  ASSERT_TRUE(ranges.tryFree(0x1000, 0x4000));
  ASSERT_TRUE(ranges.tryFree(0x9000, 0x1000));
  EXPECT_FALSE(ranges.allocateSpecificWithoutAllocation(0x2000, 0x1000));
  uintptr_t address = 0;
  ASSERT_TRUE(ranges.allocateSpecificWithoutAllocation(0x9000, 0x1000));
  ASSERT_TRUE(ranges.allocateSpecificWithoutAllocation(0x2000, 0x1000));
  EXPECT_EQ(ranges.size(), 2U);
  ASSERT_TRUE(ranges.allocateWithoutAllocation(0x1000, address));
  EXPECT_EQ(address, 0x1000U);
  ASSERT_TRUE(ranges.freeWithoutAllocation(0x7000, 0x1000));
  EXPECT_EQ(ranges.size(), 2U);
  ASSERT_TRUE(ranges.freeWithoutAllocation(0x5000, 0x1000));
  ASSERT_TRUE(ranges.allocateWithoutAllocation(0x3000, address));
  EXPECT_EQ(address, 0x3000U);
  ASSERT_TRUE(ranges.allocateWithoutAllocation(0x1000, address));
  EXPECT_EQ(address, 0x7000U);
  EXPECT_FALSE(ranges.allocateWithoutAllocation(1, address));
}

template <bool Reversed>
static void checkFragmentedSnapshots(bool preferUsed) {
  using Ranges = RangeList<int64_t, Reversed>;
  Ranges ranges(preferUsed);
  constexpr size_t count = 256;
  for (size_t i = 0; i < count; ++i) {
    ASSERT_TRUE(ranges.tryFree(i * 256 + 64, 128, false));
  }
  Ranges snapshot(ranges);

  // Splitting crosses storage growth boundaries while preserving both sides
  // and the allocation decisions in a previously captured snapshot.
  for (size_t i = 0; i < count; ++i) {
    ASSERT_TRUE(ranges.allocateSpecific(i * 256 + 96, 32));
  }
  ASSERT_EQ(ranges.size(), count * 2);
  Ranges replacement(!preferUsed);
  replacement = ranges;
  ranges.clear();
  ASSERT_TRUE(ranges.tryFree(0x100000, 64));
  ranges.swap(replacement);
  replacement.clear();
  EXPECT_EQ(ranges.size(), count * 2);

  for (size_t i = 0; i < count; ++i) {
    EXPECT_FALSE(ranges.allocateSpecificWithoutAllocation(i * 256 + 96, 32));
    EXPECT_TRUE(ranges.allocateSpecific(i * 256 + 64, 32));
    EXPECT_TRUE(ranges.allocateSpecific(i * 256 + 128, 64));
    EXPECT_TRUE(snapshot.allocateSpecific(i * 256 + 64, 128));
  }
  EXPECT_EQ(ranges.size(), 0U);
  EXPECT_EQ(snapshot.size(), 0U);
  EXPECT_EQ(replacement.size(), 0U);

  ASSERT_TRUE(ranges.tryFree(0, 64));
  ASSERT_TRUE(ranges.tryFree(256, 64));
  int64_t address = -1;
  ASSERT_TRUE(ranges.allocate(16, address));
  EXPECT_EQ(address, (preferUsed ? 256 : 0) + (Reversed ? 48 : 0));
}

TEST(PedigreeRangeList, FragmentedSnapshotsSurviveGrowthSplitSwapAndClear) {
  checkFragmentedSnapshots<false>(false);
  checkFragmentedSnapshots<false>(true);
  checkFragmentedSnapshots<true>(false);
  checkFragmentedSnapshots<true>(true);
}

TEST(PedigreeRangeList, MissingReusableEntryDoesNotAlterReservations) {
  RangeList<int64_t> ranges;
  ASSERT_TRUE(ranges.tryFree(0, 64));
  ASSERT_TRUE(ranges.tryFree(256, 64));
  ASSERT_TRUE(ranges.allocateSpecific(256, 64));

  // Spare vector capacity is not an exhausted entry available to the
  // allocation-free operations.
  EXPECT_FALSE(ranges.allocateSpecificWithoutAllocation(16, 16));
  EXPECT_FALSE(ranges.freeWithoutAllocation(512, 64));
  EXPECT_EQ(ranges.size(), 1U);
  EXPECT_EQ(rangeAt(ranges, 0), RangeList<int64_t>::Range(0, 64));
  int64_t address = -1;
  EXPECT_FALSE(ranges.allocateWithoutAllocation(65, address));
  EXPECT_EQ(address, -1);
  EXPECT_FALSE(ranges.allocateSpecificWithoutAllocation(0, 0));
  EXPECT_EQ(rangeAt(ranges, 0), RangeList<int64_t>::Range(0, 64));

  ASSERT_TRUE(ranges.allocateWithoutAllocation(64, address));
  ASSERT_TRUE(ranges.freeWithoutAllocation(512, 64));
  EXPECT_EQ(rangeAt(ranges, 0), RangeList<int64_t>::Range(512, 64));
}
