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

#include "pedigree/kernel/core/SlamAllocator.h"

TEST(PedigreeSlamAllocator, DISABLE_EmptyStartup)
{
    SlamAllocator::instance().initialise();

    EXPECT_EQ(SlamAllocator::instance().heapPageCount(), (size_t) 0);
}

TEST(PedigreeSlamAllocator, DISABLE_Allocation)
{
    SlamAllocator::instance().initialise();

    uintptr_t alloc = SlamAllocator::instance().allocate(4);

    EXPECT_EQ(SlamAllocator::instance().heapPageCount(), (size_t) 1);

    SlamAllocator::instance().free(alloc);
}

TEST(PedigreeSlamAllocator, DISABLE_Deallocate)
{
    SlamAllocator::instance().initialise();

    uintptr_t alloc = SlamAllocator::instance().allocate(4);
    SlamAllocator::instance().free(alloc);

    EXPECT_EQ(SlamAllocator::instance().heapPageCount(), (size_t) 1);
}

TEST(PedigreeSlamAllocator, DISABLE_Recovery)
{
    SlamAllocator::instance().initialise();

    uintptr_t alloc = SlamAllocator::instance().allocate(4);
    SlamAllocator::instance().free(alloc);

    EXPECT_EQ(SlamAllocator::instance().heapPageCount(), (size_t) 1);

    SlamAllocator::instance().recovery();
    EXPECT_EQ(SlamAllocator::instance().heapPageCount(), (size_t) 0);
}

TEST(PedigreeSlamAllocator, DISABLE_Alignment)
{
    SlamAllocator::instance().initialise();

    uintptr_t alloc = SlamAllocator::instance().allocate(4);
    EXPECT_EQ(alloc & 15, (uintptr_t) 0);
    SlamAllocator::instance().free(alloc);
}
