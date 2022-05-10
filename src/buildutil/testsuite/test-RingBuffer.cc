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

#include "pedigree/kernel/utilities/RingBuffer.h"

TEST(PedigreeRingBuffer, ReadEmpty)
{
    RingBuffer<char> buffer(32768);
    Time::Timestamp zero = 0;
    RingBuffer<char>::ReadResult result = buffer.read(zero);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error(), RingBuffer<char>::Empty);
}

TEST(PedigreeRingBuffer, WriteRead)
{
    RingBuffer<char> buffer(1);

    EXPECT_TRUE(buffer.canWrite());
    EXPECT_FALSE(buffer.dataReady());

    RingBuffer<char>::Error err = buffer.write('a');
    EXPECT_EQ(err, RingBuffer<char>::NoError);

    EXPECT_FALSE(buffer.canWrite());
    EXPECT_TRUE(buffer.dataReady());

    RingBuffer<char>::ReadResult result = buffer.read();
    EXPECT_TRUE(result.hasValue());
    EXPECT_EQ(result.value(), 'a');

    EXPECT_TRUE(buffer.canWrite());
    EXPECT_FALSE(buffer.dataReady());
}

TEST(PedigreeRingBuffer, Overflow)
{
    RingBuffer<char> buffer(8);

    char buf[16];
    memset(buf, 0xAB, 16);

    // Overflow truncates.
    Time::Timestamp zero = 0;
    EXPECT_EQ(buffer.write(buf, 16, zero), 8U);

    for (size_t i = 0; i < 8; ++i)
    {
        RingBuffer<char>::ReadResult result = buffer.read(zero);
        EXPECT_TRUE(result.hasValue());
        EXPECT_EQ(result.value(), '\xAB');
    }

    RingBuffer<char>::ReadResult result = buffer.read(zero);
    EXPECT_TRUE(result.hasError());
}

#if 0
TEST(PedigreeRingBuffer, OverlapReader)
{
    RingBuffer<char> buffer(8);

    char buf[16], buf2[16];
    memset(buf, 0xAB, 16);

    EXPECT_EQ(buffer.write(buf, 6, false), 6);
    EXPECT_EQ(buffer.read(buf2, 2, false), 2);
    EXPECT_EQ(buffer.write(buf, 4, false), 4);
}

TEST(PedigreeRingBuffer, Overlap)
{
    RingBuffer<char> buffer(8);

    char buf[16], buf2[16];
    memset(buf, 0xAB, 16);

    EXPECT_EQ(buffer.write(buf, 6, false), 6);
    EXPECT_EQ(buffer.read(buf2, 6, false), 6);
    EXPECT_TRUE(memcmp(buf, buf2, 6) == 0);

    // Rolls over - two bytes at the end and then two bytes at the start.
    EXPECT_EQ(buffer.write(buf, 4, false), 4);
    EXPECT_EQ(buffer.read(buf2, 4, false), 4);

    // Verify the reader can catch up.
    EXPECT_TRUE(memcmp(buf, buf2, 4) == 0);
}

TEST(PedigreeRingBuffer, FillBuffer)
{
    RingBuffer<char> buffer(32768);

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
    EXPECT_EQ(buffer.write(buf, 16, false), 0);
    EXPECT_EQ(buffer.getDataSize(), 32768);
}

TEST(PedigreeRingBuffer, FillAndRead)
{
    RingBuffer<char> buffer(32768);

    char buf[32768], out[32768];
    memset(buf, 0xAB, 32768);
    memset(out, 0, 32768);

    size_t r = buffer.write(buf, 32768, false);
    EXPECT_EQ(r, 32768);

    r = buffer.read(out, 32768, false);
    EXPECT_EQ(r, 32768);

    EXPECT_TRUE(memcmp(buf, out, 32768) == 0);
}

TEST(PedigreeRingBuffer, Chase)
{
    RingBuffer<size_t> buffer(32768);

    const int n = 0x10000;
    const int readThreshold = 0x500;

    std::unique_ptr<size_t[]> numbers(new size_t[n]);
    size_t offset = 0;

    for (size_t i = 0; i < n; ++i)
    {
        ASSERT_EQ(buffer.write(&i, 1, false), 1);

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
#endif
