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

class SlamAllocatorCorrectnessTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        SlamAllocator::instance().clearAll();
    }

    void TearDown() override
    {
        SlamAllocator::instance().clearAll();
    }
};

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

TEST_F(SlamAllocatorCorrectnessTest, ExactSizeClassBoundaryIsNotRoundedUp)
{
    SlamAllocator &allocator = SlamAllocator::instance();
    const size_t framing = allocator.headerSize() + allocator.footerSize();
    const size_t requested = 128 - framing;

    uintptr_t allocation = allocator.allocate(requested);

    EXPECT_EQ(allocator.allocSize(allocation), requested);
    allocator.free(allocation);
}

TEST_F(SlamAllocatorCorrectnessTest, RecoveryPreservesOtherSlabFreeObjects)
{
    SlamAllocator &allocator = SlamAllocator::instance();
    uintptr_t allocations[64] = {};

    allocations[0] = allocator.allocate(1);
    const size_t fullObjectSize =
        allocator.allocSize(allocations[0]) + allocator.headerSize() +
        allocator.footerSize();
    const size_t objectsPerSlab = SLAB_MINIMUM_SIZE / fullObjectSize;
    ASSERT_LT(objectsPerSlab, 64U);

    for (size_t i = 1; i <= objectsPerSlab; ++i)
    {
        allocations[i] = allocator.allocate(1);
    }

    for (size_t i = 0; i < objectsPerSlab; ++i)
    {
        allocator.free(allocations[i]);
    }

    ASSERT_EQ(allocator.heapPageCount(), 2U);
    ASSERT_EQ(allocator.recovery(1), 1U);
    ASSERT_EQ(allocator.heapPageCount(), 1U);

    uintptr_t reused = allocator.allocate(1);
    EXPECT_EQ(allocator.heapPageCount(), 1U);
    EXPECT_EQ(
        reused & ~(SLAB_MINIMUM_SIZE - 1),
        allocations[objectsPerSlab] & ~(SLAB_MINIMUM_SIZE - 1));

    allocator.free(reused);
    allocator.free(allocations[objectsPerSlab]);
}

TEST_F(SlamAllocatorCorrectnessTest, RecoveryHonoursGlobalSlabLimit)
{
    SlamAllocator &allocator = SlamAllocator::instance();

    uintptr_t firstCache = allocator.allocate(1);
    allocator.free(firstCache);

    uintptr_t secondCache[32] = {};
    secondCache[0] = allocator.allocate(100);
    const size_t fullObjectSize =
        allocator.allocSize(secondCache[0]) + allocator.headerSize() +
        allocator.footerSize();
    const size_t objectsPerSlab = SLAB_MINIMUM_SIZE / fullObjectSize;
    ASSERT_LT(objectsPerSlab, 32U);
    for (size_t i = 1; i <= objectsPerSlab; ++i)
    {
        secondCache[i] = allocator.allocate(100);
    }
    for (size_t i = 0; i <= objectsPerSlab; ++i)
    {
        allocator.free(secondCache[i]);
    }

    ASSERT_EQ(allocator.heapPageCount(), 3U);
    EXPECT_EQ(allocator.recovery(2), 2U);
    EXPECT_EQ(allocator.heapPageCount(), 1U);
}

TEST_F(SlamAllocatorCorrectnessTest, RecoveredPointerIsInvalid)
{
    SlamAllocator &allocator = SlamAllocator::instance();
    uintptr_t allocation = allocator.allocate(1);
    allocator.free(allocation);
    ASSERT_EQ(allocator.recovery(1), 1U);

    EXPECT_FALSE(allocator.isPointerValid(allocation));
}

TEST_F(SlamAllocatorCorrectnessTest, RejectsOverflowingAllocationSize)
{
    EXPECT_DEATH(
        SlamAllocator::instance().allocate(~static_cast<size_t>(0)),
        "allocation");
}

TEST_F(SlamAllocatorCorrectnessTest, FindsFreePagesAcrossBitmapEntries)
{
    SlamAllocator &allocator = SlamAllocator::instance();
    uintptr_t pages[64] = {};

    for (size_t i = 0; i < 64; ++i)
    {
        pages[i] = allocator.getSlab(SLAB_MINIMUM_SIZE);
    }

    allocator.freeSlab(pages[63], SLAB_MINIMUM_SIZE);
    uintptr_t twoPages = allocator.getSlab(2 * SLAB_MINIMUM_SIZE);

    EXPECT_EQ(twoPages, pages[63]);
}

TEST_F(SlamAllocatorCorrectnessTest, LargeSlabCanStartMidBitmapEntry)
{
    SlamAllocator &allocator = SlamAllocator::instance();
    uintptr_t firstPage = allocator.getSlab(SLAB_MINIMUM_SIZE);

    uintptr_t largeSlab = allocator.getSlab(65 * SLAB_MINIMUM_SIZE);

    EXPECT_EQ(largeSlab, firstPage + SLAB_MINIMUM_SIZE);
}
