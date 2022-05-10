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

#include <cstdint>
#include <memory>

#include "pedigree/kernel/utilities/Buffer.h"

TEST(PedigreeBuffer, InitialSettings)
{
    Buffer<uint8_t> buffer(32768);

    EXPECT_EQ(buffer.getDataSize(), 0U);
    EXPECT_EQ(buffer.getSize(), 32768U);
}

TEST(PedigreeBuffer, ReadEmpty)
{
    Buffer<char> buffer(32768);

    char buf[16];
    memset(buf, 0, 16);

    size_t r = buffer.read(buf, 16, false);

    EXPECT_EQ(r, 0U);
}

TEST(PedigreeBuffer, TooManyWithShort)
{
    Buffer<char, true> buffer(8);

    char buf[16];
    memset(buf, 0, 16);

    EXPECT_EQ(buffer.write(buf, 16, false), 8U);
    EXPECT_EQ(buffer.read(buf, 16, false), 8U);
}

TEST(PedigreeBuffer, TooManyWithoutShort)
{
    Buffer<char, false> buffer(8);

    char buf[16];
    memset(buf, 0, 16);

    EXPECT_EQ(buffer.write(buf, 16, false), 8U);
    EXPECT_EQ(buffer.read(buf, 16, false), 8U);
}

TEST(PedigreeBuffer, ReadTooMany)
{
    Buffer<char> buffer(8);

    char buf[16];
    memset(buf, 0, 16);

    EXPECT_EQ(buffer.write(buf, 6, false), 6U);
    EXPECT_EQ(buffer.read(buf, 8, false), 6U);
}

TEST(PedigreeBuffer, Overflow)
{
    Buffer<char, true> buffer(8);

    char buf[16];
    memset(buf, 0xAB, 16);

    // Overflow truncates.
    EXPECT_EQ(buffer.write(buf, 16, false), 8U);
    EXPECT_EQ(buffer.getDataSize(), 8U);
}

TEST(PedigreeBuffer, OverlapReader)
{
    Buffer<char> buffer(8);

    char buf[16], buf2[16];
    memset(buf, 0xAB, 16);

    EXPECT_EQ(buffer.write(buf, 6, false), 6U);
    EXPECT_EQ(buffer.read(buf2, 2, false), 2U);
    EXPECT_EQ(buffer.write(buf, 4, false), 4U);
}

TEST(PedigreeBuffer, Overlap)
{
    Buffer<char> buffer(8);

    char buf[16], buf2[16];
    memset(buf, 0xAB, 16);

    EXPECT_EQ(buffer.write(buf, 6, false), 6U);
    EXPECT_EQ(buffer.read(buf2, 6, false), 6U);
    EXPECT_TRUE(memcmp(buf, buf2, 6) == 0);

    // Rolls over - two bytes at the end and then two bytes at the start.
    EXPECT_EQ(buffer.write(buf, 4, false), 4U);
    EXPECT_EQ(buffer.read(buf2, 4, false), 4U);

    // Verify the reader can catch up.
    EXPECT_TRUE(memcmp(buf, buf2, 4) == 0);
}

TEST(PedigreeBuffer, FillBuffer)
{
    Buffer<char> buffer(32768);

    char buf[16];
    memset(buf, 0xAB, 16);

    // Limited iteration, but should end much sooner.
    for (size_t i = 0; i < 32768; ++i)
    {
        size_t r = buffer.write(buf, 16, false);
        if (r < 16)
            break;
    }

    // Can't write to a full buffer.
    EXPECT_EQ(buffer.write(buf, 16, false), 0U);
    EXPECT_EQ(buffer.getDataSize(), 32768U);
}

TEST(PedigreeBuffer, FillAndRead)
{
    Buffer<char> buffer(32768);

    char buf[32768], out[32768];
    memset(buf, 0xAB, 32768);
    memset(out, 0, 32768);

    size_t r = buffer.write(buf, 32768, false);
    EXPECT_EQ(r, 32768U);

    r = buffer.read(out, 32768, false);
    EXPECT_EQ(r, 32768U);

    EXPECT_TRUE(memcmp(buf, out, 32768) == 0);
}

TEST(PedigreeBuffer, Chase)
{
    Buffer<size_t> buffer(32768);

    const size_t n = 0x10000;
    const size_t readThreshold = 0x500;

    std::unique_ptr<size_t[]> numbers(new size_t[n]);
    size_t offset = 0;

    for (size_t i = 0; i < n; ++i)
    {
        ASSERT_EQ(buffer.write(&i, 1, false), 1U);

        if (i && (i % readThreshold == 0))
        {
            // Read entries into our buffer.
            ASSERT_EQ(
                buffer.read(numbers.get() + offset, readThreshold, false),
                readThreshold);
            offset += readThreshold;
        }
    }

    size_t remaining = buffer.getDataSize();
    ASSERT_EQ(buffer.read(numbers.get() + offset, remaining, false), remaining);
    offset += remaining;

    ASSERT_EQ(offset, n);

    for (size_t i = 0; i < n; ++i)
    {
        // Should match, as we chased the buffer.
        ASSERT_EQ(numbers.get()[i], i);
        if (numbers.get()[i] != i)
            break;
    }
}
