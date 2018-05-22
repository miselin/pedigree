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
