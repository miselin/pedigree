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

#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

TEST(PedigreeTree, Construction)
{
    Tree<int, int> x;
    EXPECT_EQ(x.count(), (size_t)0);
    EXPECT_EQ(x.begin(), x.end());
}

TEST(PedigreeTree, CopyConstruction)
{
    Tree<int, int> x;
    x.insert(1, 1);

    Tree<int, int> y(x);
    EXPECT_EQ(x.count(), y.count());

    EXPECT_EQ(x.lookup(1), y.lookup(1));
}

TEST(PedigreeTree, Insertion)
{
    Tree<int, int> x;
    for (int i = 1; i < 9; ++i)
    {
        x.insert(i, i);
    }

    for (int i = 1; i < 9; ++i)
    {
        EXPECT_EQ(x.lookup(i), i);
    }
}

TEST(PedigreeTree, Clear)
{
    Tree<int, int> x;
    // try and rebalance the tree a bit
    for (int i = 0; i < 256; ++i)
    {
        x.insert(i, 1234);
    }
    for (int i = 512; i < 768; ++i)
    {
        x.insert(i, 1234);
    }
    for (int i = 256; i < 512; ++i)
    {
        x.insert(i, 1234);
    }
    for (int i = 768; i < 1024; ++i)
    {
        x.insert(i, 1234);
    }
    EXPECT_EQ(x.count(), (size_t) 1024);
    x.clear();
    EXPECT_EQ(x.count(), (size_t) 0);
}

TEST(PedigreeTree, Removal)
{
    Tree<int, int> x;
    x.insert(1, 1);
    x.insert(2, 2);
    x.remove(1);
    EXPECT_EQ(x.count(), (size_t) 1);

    EXPECT_EQ(x.lookup(2), 2);
}

TEST(PedigreeTree, RootRemoval)
{
    Tree<int, int> x;
    x.insert(1, 1);
    x.insert(2, 2);  // should become the root of the tree
    x.insert(3, 3);
    x.remove(2);
    EXPECT_EQ(x.count(), (size_t) 2);

    EXPECT_EQ(x.lookup(1), 1);
    EXPECT_EQ(x.lookup(2), 0);
    EXPECT_EQ(x.lookup(3), 3);
}

TEST(PedigreeTree, LeftRemoval)
{
    Tree<int, int> x;
    x.insert(1, 1);
    x.insert(2, 2);  // should become the root of the tree
    x.insert(3, 3);
    x.remove(1);
    EXPECT_EQ(x.count(), (size_t) 2);

    EXPECT_EQ(x.lookup(1), 0);
    EXPECT_EQ(x.lookup(2), 2);
    EXPECT_EQ(x.lookup(3), 3);
}

TEST(PedigreeTree, RightRemoval)
{
    Tree<int, int> x;
    x.insert(1, 1);
    x.insert(2, 2);  // should become the root of the tree
    x.insert(3, 3);
    x.remove(3);
    EXPECT_EQ(x.count(), (size_t) 2);

    EXPECT_EQ(x.lookup(1), 1);
    EXPECT_EQ(x.lookup(2), 2);
    EXPECT_EQ(x.lookup(3), 0);
}

TEST(PedigreeTree, DoubleInsertionValueChanged)
{
    Tree<int, int> x;
    x.insert(1, 1);
    x.insert(1, 2);
    EXPECT_EQ(x.lookup(1), 2);
}

TEST(PedigreeTree, SortedInsertion)
{
    Tree<int, int> x;
    size_t insertions = 0;
    for (int i = 1; i < 1024; ++i)
    {
        x.insert(i, i);
        ++insertions;
    }

    EXPECT_EQ(x.count(), insertions);
    for (int i = 1; i < 1024; ++i)
    {
        EXPECT_EQ(x.lookup(i), i);
    }
}

TEST(PedigreeTree, ReverseSortedInsertion)
{
    Tree<int, int> x;
    size_t insertions = 0;
    for (int i = 1023; i > 0; --i)
    {
        x.insert(i, i);
        ++insertions;
    }

    EXPECT_EQ(x.count(), insertions);
    for (int i = 1; i < 1024; ++i)
    {
        EXPECT_EQ(x.lookup(i), i);
    }
}

TEST(PedigreeTree, Iteration)
{
    Tree<int, int> x;
    x.insert(1, 1);
    x.insert(2, 2);
    x.insert(3, 3);
    x.insert(4, 4);
    x.insert(5, 5);

    auto it = x.begin();
    EXPECT_EQ(it.value(), 1);
    ++it;
    EXPECT_EQ(it.value(), 2);
    ++it;
    EXPECT_EQ(it.value(), 3);
    ++it;
    EXPECT_EQ(it.value(), 4);
    ++it;
    EXPECT_EQ(it.value(), 5);
    ++it;
    EXPECT_EQ(it, x.end());
}

TEST(PedigreeTree, InsertMove)
{
    Tree<int, SharedPointer<int>> x;
    auto y = SharedPointer<int>::allocate();
    auto ptr = y.get();

    x.insert(1, pedigree_std::move(y));

    EXPECT_EQ(x.lookup(1).get(), ptr);
    EXPECT_EQ(x.lookupRef(1).get(), ptr);
    EXPECT_EQ(y.get(), nullptr);
}
