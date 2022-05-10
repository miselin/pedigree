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

#include <gtest/gtest.h>

#include "pedigree/kernel/utilities/RangeList.h"

TEST(PedigreeRangeList, Forward)
{
    RangeList<int64_t> list;

    list.free(0, 1024);

    int64_t addr = 0;
    EXPECT_TRUE(list.allocate(1, addr));
    EXPECT_EQ(addr, 0);
    EXPECT_TRUE(list.allocate(1, addr));
    EXPECT_EQ(addr, 1);
}

TEST(PedigreeRangeList, Reversed)
{
    RangeList<int64_t, true> list;

    list.free(0, 1024);

    int64_t addr = 0;
    EXPECT_TRUE(list.allocate(1, addr));
    EXPECT_EQ(addr, 1023);
    EXPECT_TRUE(list.allocate(1, addr));
    EXPECT_EQ(addr, 1022);
}

TEST(PedigreeRangeList, NeedsFreeFirst)
{
    RangeList<int64_t> list;
    int64_t addr = 0;
    EXPECT_FALSE(list.allocate(1, addr));

    list.free(0, 1024);

    EXPECT_TRUE(list.allocate(1, addr));
    EXPECT_EQ(addr, 0);
}

TEST(PedigreeRangeList, AllocateEntireRange)
{
    RangeList<int64_t> list;

    // create 3 ranges (don't sweep)
    list.free(0, 128);
    list.free(0, 128);
    list.free(0, 128);

    int64_t addr = 0;
    EXPECT_TRUE(list.allocate(128, addr));
    EXPECT_EQ(addr, 0);
}

TEST(PedigreeRangeList, AllocateSkipsTooSmallRanges)
{
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

TEST(PedigreeRangeList, AllocateSkipsTooSmallRangesWithSweep)
{
    RangeList<int64_t> list;

    list.free(0, 64, false);
    list.free(64, 64, false);
    list.free(128, 64, false);

    // this will have to do a retry and sweep to succeed
    int64_t addr = 0;
    EXPECT_TRUE(list.allocate(128, addr));
    EXPECT_EQ(addr, 0);
}

TEST(PedigreeRangeList, AllocateSpecificWorks)
{
    RangeList<int64_t> list;
    list.free(0, 1024);

    EXPECT_TRUE(list.allocateSpecific(0, 512));
    int64_t addr = 0;
    EXPECT_TRUE(list.allocate(1, addr));
    EXPECT_EQ(addr, 512);
}

TEST(PedigreeRangeList, AllocateSpecificFailsAlreadyAllocated)
{
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

TEST(PedigreeRangeList, AllocateSpecificOnlyOnce)
{
    RangeList<int64_t> list;
    list.free(0, 1024);

    int64_t addr = 0;
    EXPECT_TRUE(list.allocateSpecific(0, 512));
    EXPECT_FALSE(list.allocateSpecific(0, 512));
}

TEST(PedigreeRangeList, AllocateSpecificWithMerges)
{
    RangeList<int64_t> list;
    list.free(0, 128, false);
    list.free(128, 128, false);
    list.free(256, 128, false);
    list.free(384, 128, false);

    int64_t addr = 0;
    // this will need to merge the first two freed ranges
    EXPECT_TRUE(list.allocateSpecific(0, 256));
}

TEST(PedigreeRangeList, AllocateSpecificAtEnd)
{
    RangeList<int64_t> list;
    list.free(0, 256);

    int64_t addr = 0;
    // this has to split a range in the middle and return the end half
    EXPECT_TRUE(list.allocateSpecific(128, 128));
}

TEST(PedigreeRangeList, AllocateSpecificMiddle)
{
    RangeList<int64_t> list;
    list.free(0, 384);

    int64_t addr = 0;
    EXPECT_TRUE(list.allocateSpecific(128, 128));
}

TEST(PedigreeRangeList, Sweep)
{
    RangeList<int64_t> list;
    list.free(0, 64, false);
    list.free(64, 64, false);
    list.free(128, 64, false);

    EXPECT_EQ(list.size(), 3U);
    EXPECT_EQ(list.getRange(0), RangeList<int64_t>::Range(0, 64));
    EXPECT_EQ(list.getRange(1), RangeList<int64_t>::Range(64, 64));
    EXPECT_EQ(list.getRange(2), RangeList<int64_t>::Range(128, 64));

    list.sweep();

    EXPECT_EQ(list.size(), 1U);
    EXPECT_EQ(list.getRange(0), RangeList<int64_t>::Range(0, 192));
}

TEST(PedigreeRangeList, Copy)
{
    RangeList<int64_t> list;
    list.free(0, 64, false);
    list.free(64, 64, false);
    list.free(128, 64, false);
    RangeList<int64_t> list2;
    list2.free(0, 1024);
    list2 = list;

    EXPECT_EQ(list2.size(), 3U);
    EXPECT_EQ(list2.getRange(0), RangeList<int64_t>::Range(0, 64));
    EXPECT_EQ(list2.getRange(1), RangeList<int64_t>::Range(64, 64));
    EXPECT_EQ(list2.getRange(2), RangeList<int64_t>::Range(128, 64));

    RangeList<int64_t> list3(list);
    EXPECT_EQ(list3.size(), 3U);
    EXPECT_EQ(list3.getRange(0), RangeList<int64_t>::Range(0, 64));
    EXPECT_EQ(list3.getRange(1), RangeList<int64_t>::Range(64, 64));
    EXPECT_EQ(list3.getRange(2), RangeList<int64_t>::Range(128, 64));
}
