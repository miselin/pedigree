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

TEST(PedigreeTree, Construction)
{
    Tree<int, int> x;
    EXPECT_EQ(x.count(), 0);
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
    EXPECT_EQ(x.count(), 1024);
    x.clear();
    EXPECT_EQ(x.count(), 0);
}

TEST(PedigreeTree, Removal)
{
    Tree<int, int> x;
    x.insert(1, 1);
    x.insert(2, 2);
    x.remove(1);
    EXPECT_EQ(x.count(), 1);

    EXPECT_EQ(x.lookup(2), 2);
}
