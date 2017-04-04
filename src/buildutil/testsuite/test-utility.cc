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

#include <utilities/utility.h>

TEST(PedigreeUtility, DirectoryName)
{
    EXPECT_STREQ(DirectoryName("/a/b/c"), "/a/b");
    EXPECT_STREQ(DirectoryName("/a/b/"), "/a/b");
    EXPECT_STREQ(DirectoryName("/a/b"), "/a");
    EXPECT_EQ(DirectoryName("c"), nullptr);
}

TEST(PedigreeUtility, BaseName)
{
    EXPECT_STREQ(BaseName("/a/b/c"), "c");
    EXPECT_STREQ(BaseName("/a/b/"), nullptr);
    EXPECT_STREQ(BaseName("/a/b"), "b");
    EXPECT_EQ(BaseName("c"), "c");
}

TEST(PedigreeUtility, Fletcher16)
{
    auto *buf = new uint8_t[4096];
    memset(buf, 'a', 4096);

    uint16_t c1 = checksum16(buf, 4096);
    buf[0] = 'b';
    uint16_t c2 = checksum16(buf, 4096);
    EXPECT_NE(c1, c2);
}

TEST(PedigreeUtility, Fletcher32)
{
    auto *buf = new uint8_t[4096];
    memset(buf, 'a', 4096);

    uint32_t c1 = checksum32(buf, 4096);
    buf[0] = 'b';
    uint32_t c2 = checksum32(buf, 4096);
    EXPECT_NE(c1, c2);
}

TEST(PedigreeUtility, Fletcher32MatchesNaive)
{
    auto *buf = new uint8_t[4096];
    memset(buf, 'a', 4096);

    EXPECT_EQ(checksum32(buf, 4096), checksum32_naive(buf, 4096));
}

TEST(PedigreeUtility, ChecksumsDifferCorrectly)
{
    auto *buf = new uint8_t[4096];
    memset(buf, 'a', 4096);

    buf[1] = 'b';
    uint16_t s1 = checksum16(buf, 4096);
    uint32_t d1 = checksum32(buf, 4096);

    // swap first two bytes now - checksum should differ ads the data has
    // actually changed (will catch out purely naive sums)
    buf[0] = 'b';
    buf[1] = 'a';
    uint16_t s2 = checksum16(buf, 4096);
    uint32_t d2 = checksum32(buf, 4096);

    EXPECT_NE(s1, s2);
    EXPECT_NE(d1, d2);
}
