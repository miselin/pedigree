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

#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/Vector.h"

TEST(PedigreeVector, Construction)
{
    Vector<int> x;
    EXPECT_EQ(x.size(), 0);
    EXPECT_EQ(x.count(), 0);
    EXPECT_EQ(x.begin(), x.end());
}

TEST(PedigreeVector, ConstructionReserve)
{
    Vector<int> x(5);
    EXPECT_EQ(x.size(), 5);
    EXPECT_EQ(x.count(), 0);
    EXPECT_EQ(x.begin(), x.end());
}

TEST(PedigreeVector, Assignment)
{
    Vector<int> x;
    x.pushBack(5);
    Vector<int> y(x);
    EXPECT_EQ(x.size(), y.size());
    EXPECT_EQ(x.count(), y.count());
    EXPECT_EQ(x.popFront(), y.popFront());
}

TEST(PedigreeVector, AssignmentOperator)
{
    Vector<int> x, y;
    x.pushBack(5);
    y = x;
    EXPECT_EQ(x.size(), y.size());
    EXPECT_EQ(x.count(), y.count());
    EXPECT_EQ(x.popFront(), y.popFront());
}

TEST(PedigreeVector, Indexing)
{
    Vector<int> x;
    x.pushBack(1);
    x.pushBack(2);
    x.pushBack(3);
    EXPECT_EQ(x[0], 1);
    EXPECT_EQ(x[1], 2);
    EXPECT_EQ(x[2], 3);
}

TEST(PedigreeVector, IndexingTooFar)
{
    Vector<int> x;
    x.pushBack(1);
    EXPECT_EQ(x[5], 0);
}

TEST(PedigreeVector, Swapping)
{
    Vector<int> x;
    x.pushBack(1);
    x.pushBack(2);
    x.pushBack(3);
    x.swap(x.begin() + 1, x.begin());
    EXPECT_EQ(x[0], 2);
    EXPECT_EQ(x[1], 1);
    EXPECT_EQ(x[2], 3);
}

TEST(PedigreeVector, SwappingBeyondEnd)
{
    Vector<int> x;
    x.pushBack(1);
    x.pushBack(2);
    x.pushBack(3);
    x.swap(x.end(), x.begin());
    EXPECT_EQ(x[0], 1);
    EXPECT_EQ(x[1], 2);
    EXPECT_EQ(x[2], 3);
}

TEST(PedigreeVector, SwappingSame)
{
    Vector<int> x;
    x.pushBack(1);
    x.pushBack(2);
    x.pushBack(3);
    x.swap(x.begin(), x.begin());
    EXPECT_EQ(x[0], 1);
    EXPECT_EQ(x[1], 2);
    EXPECT_EQ(x[2], 3);
}

TEST(PedigreeVector, Insertion1)
{
    Vector<int> x;
    x.pushFront(1);
    x.pushFront(2);
    x.pushFront(3);
    x.pushFront(4);
    x.pushFront(5);
    x.pushFront(6);
    x.pushFront(7);
    x.pushFront(8);
    x.pushFront(9);

    EXPECT_EQ(x[0], 9);
    EXPECT_EQ(x[1], 8);
    EXPECT_EQ(x[2], 7);
    EXPECT_EQ(x[3], 6);
    EXPECT_EQ(x[4], 5);
    EXPECT_EQ(x[5], 4);
    EXPECT_EQ(x[6], 3);
    EXPECT_EQ(x[7], 2);
    EXPECT_EQ(x[8], 1);
}

TEST(PedigreeVector, Insertion2)
{
    Vector<int> x;
    x.pushBack(1);
    x.pushBack(2);
    x.pushBack(3);
    x.pushBack(4);
    x.pushBack(5);
    x.pushBack(6);
    x.pushBack(7);
    x.pushBack(8);
    x.pushBack(9);

    EXPECT_EQ(x[0], 1);
    EXPECT_EQ(x[1], 2);
    EXPECT_EQ(x[2], 3);
    EXPECT_EQ(x[3], 4);
    EXPECT_EQ(x[4], 5);
    EXPECT_EQ(x[5], 6);
    EXPECT_EQ(x[6], 7);
    EXPECT_EQ(x[7], 8);
    EXPECT_EQ(x[8], 9);
}

TEST(PedigreeVector, Insertion3)
{
    Vector<int> x;
    x.pushFront(1);
    x.pushFront(2);
    x.pushFront(3);
    x.pushFront(4);

    EXPECT_EQ(x[0], 4);
    EXPECT_EQ(x[1], 3);
    EXPECT_EQ(x[2], 2);
    EXPECT_EQ(x[3], 1);
}

TEST(PedigreeVector, InsertionComplex)
{
    typedef SharedPointer<int> sharedintptr_t;

    sharedintptr_t ptr1 = sharedintptr_t::allocate(1);
    sharedintptr_t ptr2 = sharedintptr_t::allocate(2);
    sharedintptr_t ptr3 = sharedintptr_t::allocate(3);
    sharedintptr_t ptr4 = sharedintptr_t::allocate(4);

    Vector<sharedintptr_t> x;
    x.pushFront(ptr1);
    x.pushFront(ptr2);
    x.pushFront(ptr3);
    x.pushFront(ptr4);

    EXPECT_EQ(x[0], ptr4);
    EXPECT_EQ(x[1], ptr3);
    EXPECT_EQ(x[2], ptr2);
    EXPECT_EQ(x[3], ptr1);

    EXPECT_EQ(*(x[0]), 4);
    EXPECT_EQ(*(x[1]), 3);
    EXPECT_EQ(*(x[2]), 2);
    EXPECT_EQ(*(x[3]), 1);
}

TEST(PedigreeVector, SetAt)
{
    Vector<int> x;
    x.pushBack(1);
    x.pushBack(2);
    x.pushBack(3);
    x.setAt(1, 5);
    EXPECT_EQ(x[0], 1);
    EXPECT_EQ(x[1], 5);
    EXPECT_EQ(x[2], 3);
}

TEST(PedigreeVector, Clear)
{
    Vector<int> x;
    x.pushBack(1);
    x.pushBack(2);
    x.pushBack(3);
    EXPECT_EQ(x.count(), 3);
    x.clear();
    EXPECT_EQ(x.count(), 0);
}

TEST(PedigreeVector, ForwardIterate)
{
    Vector<int> x;
    x.pushBack(1);
    x.pushBack(2);
    x.pushBack(3);

    const Vector<int> &y = x;
    EXPECT_NE(y.begin(), y.end());
    auto it = y.begin();
    EXPECT_EQ(*it++, 1);
    EXPECT_EQ(*it++, 2);
    EXPECT_EQ(*it++, 3);
    EXPECT_EQ(it, y.end());
}

TEST(PedigreeVector, PopBack)
{
    Vector<int> x;
    x.pushBack(1);
    x.pushBack(2);
    x.pushBack(3);
    EXPECT_EQ(x.popBack(), 3);
    EXPECT_EQ(x.popBack(), 2);
    EXPECT_EQ(x.popBack(), 1);
    EXPECT_EQ(x.count(), 0);
}

TEST(PedigreeVector, Erase)
{
    Vector<int> x;
    x.pushBack(1);
    x.pushBack(2);
    x.pushBack(3);

    auto it = x.begin();
    ++it;

    x.erase(it);

    EXPECT_EQ(x[0], 1);
    EXPECT_EQ(x[1], 3);
    EXPECT_EQ(x.count(), 2);
}

TEST(PedigreeVector, EraseAtEnd)
{
    Vector<int> x;
    x.pushBack(1);
    x.pushBack(2);

    auto it = x.begin();
    ++it;

    EXPECT_EQ(*it, 2);
    EXPECT_NE(it, x.end());
    it = x.erase(it);
    EXPECT_EQ(it, x.end());
}

TEST(PedigreeVector, ReducedCopies)
{
    Vector<int> x;
    for (size_t i = 0; i < 128; ++i)
    {
        x.pushBack(1);
    }

    auto it = x.begin();

    for (size_t i = 0; i < 64; ++i)
    {
        // Increments the start index.
        x.popFront();
    }

    auto it2 = x.begin();

    for (size_t i = 0; i < 64; ++i)
    {
        x.pushFront(1);
    }

    auto it3 = x.begin();

    EXPECT_EQ(it + 64, it2);
    EXPECT_EQ(it, it3);
}

TEST(PedigreeVector, OffsetPush)
{
    Vector<int> x;
    for (size_t i = 0; i < 128; ++i)
    {
        x.pushBack(i + 1);
    }

    // Remove the first 64 items, moving m_Start forward.
    for (size_t i = 0; i < 64; ++i)
    {
        // Increments the start index.
        x.popFront();
    }

    // Now push to the end of the vector.
    for (size_t i = 0; i < 64; ++i)
    {
        x.pushBack(128 + i);
    }

    EXPECT_EQ(x[0], 65);
}

TEST(PedigreeVector, ReuseAndThenSome)
{
    Vector<int> x;
    for (size_t i = 0; i < 128; ++i)
    {
        x.pushBack(i + 1);
    }

    // Remove the first 64 items, moving m_Start forward.
    for (size_t i = 0; i < 64; ++i)
    {
        // Increments the start index.
        x.popFront();
    }

    // Now push well past the safe zone we popped.
    for (size_t i = 0; i < 128; ++i)
    {
        x.pushBack(128 + i);
    }

    EXPECT_EQ(x[0], 65);
}

TEST(PedigreeVector, NonTrivialObjects)
{
    Vector<SharedPointer<int>> x;

    x.pushBack(SharedPointer<int>::allocate());
    x.pushBack(SharedPointer<int>::allocate());
    x.pushBack(SharedPointer<int>::allocate());
    x.pushBack(SharedPointer<int>::allocate());
    x.pushBack(SharedPointer<int>::allocate());
    x.pushBack(SharedPointer<int>::allocate());

    EXPECT_NE(x[0].get(), (int *) 0);
    EXPECT_NE(x[1].get(), (int *) 0);
    EXPECT_NE(x[2].get(), (int *) 0);
    EXPECT_NE(x[3].get(), (int *) 0);
    EXPECT_NE(x[4].get(), (int *) 0);
    EXPECT_NE(x[5].get(), (int *) 0);
}
