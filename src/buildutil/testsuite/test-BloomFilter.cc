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

#include <iostream>

#include <gtest/gtest.h>

#include "pedigree/kernel/utilities/BloomFilter.h"

TEST(PedigreeBloomFilter, EmptyFilterContainsNothing)
{
    BloomFilter<char> filter(32, 2);
    EXPECT_FALSE(filter.contains('t'));
    EXPECT_FALSE(filter.contains("test1", 5));
    EXPECT_FALSE(filter.contains("test2", 5));
    EXPECT_FALSE(filter.contains("hello world", 11));
}

TEST(PedigreeBloomFilter, Contains)
{
    BloomFilter<char> filter(32, 2);

    filter.add('t');
    filter.add("test1", 5);
    filter.add("test2", 5);
    filter.add("hello world", 11);

    EXPECT_TRUE(filter.contains('t'));
    EXPECT_TRUE(filter.contains("test1", 5));
    EXPECT_TRUE(filter.contains("test2", 5));
    EXPECT_TRUE(filter.contains("hello world", 11));
}

TEST(PedigreeBloomFilter, ClearedContainsNothing)
{
    BloomFilter<char> filter(32, 2);

    filter.add('t');
    filter.add("test1", 5);
    filter.add("test2", 5);
    filter.add("hello world", 11);
    filter.clear();

    EXPECT_FALSE(filter.contains('t'));
    EXPECT_FALSE(filter.contains("test1", 5));
    EXPECT_FALSE(filter.contains("test2", 5));
    EXPECT_FALSE(filter.contains("hello world", 11));
}

TEST(PedigreeBloomFilter, ContainsIntList)
{
    size_t n = 4;
    // ln(0.001) - where p (failure rate) = 0.001
    // ln(2) ~= 0.6931
    size_t m = -((n * -6.9078) / (0.6931 * 0.6931));
    size_t k = (m / n) * 0.6931;

    BloomFilter<int64_t> filter(m, k);

    for (size_t i = 0; i < n; ++i)
    {
        filter.add(static_cast<int64_t>(i));
    }

    int64_t a = 0;
    for (size_t i = 0; i < (n * 2); ++i)
    {
        int64_t key = a++ % 4;
        // EXPECT_TRUE(filter.contains(static_cast<int64_t>(i)));
        EXPECT_TRUE(filter.contains(key));
    }
}
