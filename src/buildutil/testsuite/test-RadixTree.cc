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

#include "pedigree/kernel/utilities/RadixTree.h"

TEST(PedigreeRadixTree, Construction)
{
    RadixTree<int> x;
    EXPECT_EQ(x.count(), 0);
    EXPECT_EQ(x.begin(), x.end());
}

TEST(PedigreeRadixTree, CopyConstruction)
{
    RadixTree<int> x;
    x.insert(String("foo"), 1);
    RadixTree<int> y(x);
    EXPECT_EQ(x.count(), y.count());

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = y.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);
}

TEST(PedigreeRadixTree, Assignment)
{
    RadixTree<int> x, y;
    x.insert(String("foo"), 1);
    y = x;
    EXPECT_EQ(x.count(), y.count());

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = y.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);
}

TEST(PedigreeRadixTree, CaseSensitive)
{
    RadixTree<int> x(true);
    x.insert(String("foo"), 1);

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("Foo"));
    EXPECT_TRUE(result.hasError());
}

TEST(PedigreeRadixTree, CaseInsensitive)
{
    RadixTree<int> x(false);
    x.insert(String("foo"), 1);

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("Foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);
}

TEST(PedigreeRadixTree, Clear)
{
    RadixTree<int> x;
    x.insert(String("foo"), 1);
    x.insert(String("bar"), 2);
    x.clear();
    EXPECT_EQ(x.count(), 0);
}

TEST(PedigreeRadixTree, EmptyLookup)
{
    RadixTree<int> x;
    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_TRUE(result.hasError());
}

TEST(PedigreeRadixTree, EmptyRemove)
{
    RadixTree<int> x;
    x.remove(String("foo"));
    EXPECT_EQ(x.count(), 0);
}

TEST(PedigreeRadixTree, EmptyKeyRemove)
{
    RadixTree<int> x;
    x.remove(String());
    EXPECT_EQ(x.count(), 0);
}

TEST(PedigreeRadixTree, PartialMatchMiss)
{
    RadixTree<int> x;
    x.insert(String("foobar"), 1);

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_TRUE(result.hasError());
}

TEST(PedigreeRadixTree, Removal)
{
    RadixTree<int> x;
    x.insert(String("foo"), 1);
    x.insert(String("bar"), 2);
    x.remove(String("foo"));
    EXPECT_EQ(x.count(), 1);

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_TRUE(result.hasError());

    result = x.lookup(String("bar"));
    EXPECT_EQ(result.value(), 2);
}

TEST(PedigreeRadixTree, RemovalBigRoot)
{
    RadixTree<int> x;
    x.insert(String("foo"), 1);
    x.insert(String("foobar"), 2);
    x.insert(String("foobaz"), 3);
    x.insert(String("fooqux"), 4);
    x.insert(String("fooabc"), 5);
    x.remove(String("foo"));
    EXPECT_EQ(x.count(), 4);
}

TEST(PedigreeRadixTree, Prefixes)
{
    RadixTree<int> x;
    x.insert(String("toast"), 1);
    x.insert(String("toasted"), 2);
    x.insert(String("toaster"), 3);
    x.insert(String("toasting"), 4);
    x.insert(String("toastier"), 5);

    RadixTree<int>::LookupType result = x.lookup(String("toast"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("toasted"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 2);

    result = x.lookup(String("toaster"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 3);

    result = x.lookup(String("toasting"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 4);

    result = x.lookup(String("toastier"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 5);
}

TEST(PedigreeRadixTree, PrefixesCaseInsensitive)
{
    RadixTree<int> x(false);
    x.insert(String("toast"), 1);
    x.insert(String("toasted"), 2);
    x.insert(String("toaster"), 3);
    x.insert(String("toasting"), 4);
    x.insert(String("toastier"), 5);

    RadixTree<int>::LookupType result = x.lookup(String("toast"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("toasted"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 2);

    result = x.lookup(String("toaster"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 3);

    result = x.lookup(String("toasting"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 4);

    result = x.lookup(String("toastier"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 5);
}

TEST(PedigreeRadixTree, SplitKeys)
{
    RadixTree<int> x;
    x.insert(String("foo"), 1);
    x.insert(String("foobar"), 2);
    x.insert(String("foobarbaz"), 3);
    x.insert(String("foobarbazqux"), 4);
    x.insert(String("foobarbazquux"), 5);

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("foobar"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 2);

    result = x.lookup(String("foobarbaz"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 3);

    result = x.lookup(String("foobarbazqux"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 4);

    result = x.lookup(String("foobarbazquux"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 5);
}

TEST(PedigreeRadixTree, SplitKeysBackwards)
{
    RadixTree<int> x;
    x.insert(String("foobarbazquux"), 5);
    x.insert(String("foobarbazqux"), 4);
    x.insert(String("foo"), 1);
    x.insert(String("foobar"), 2);
    x.insert(String("foobarbaz"), 3);

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("foobar"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 2);

    result = x.lookup(String("foobarbaz"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 3);

    result = x.lookup(String("foobarbazqux"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 4);

    result = x.lookup(String("foobarbazquux"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 5);
}

TEST(PedigreeRadixTree, Override)
{
    RadixTree<int> x;
    x.insert(String("foo"), 1);
    x.insert(String("foo"), 2);

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 2);
}

TEST(PedigreeRadixTree, CaseInsensitiveSplitKeys)
{
    RadixTree<int> x(false);
    x.insert(String("foo"), 1);
    x.insert(String("Foobar"), 2);

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("Foobar"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 2);
}

TEST(PedigreeRadixTree, CaseInsensitiveBackwardSplitKeys)
{
    RadixTree<int> x(false);
    x.insert(String("Foobar"), 2);
    x.insert(String("foo"), 1);

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("Foobar"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 2);
}

TEST(PedigreeRadixTree, Iteration)
{
    RadixTree<int> x;
    x.insert(String("foo"), 1);
    x.insert(String("foobar"), 2);
    x.insert(String("bar"), 3);
    x.insert(String("barfoo"), 4);
    auto it = x.begin();
    EXPECT_EQ(*it++, 1);
    EXPECT_EQ(*it++, 2);
    EXPECT_EQ(*it++, 3);
    EXPECT_EQ(*it++, 4);
}

TEST(PedigreeRadixTree, Erase)
{
    RadixTree<int> x;
    x.insert(String("foo"), 1);
    x.insert(String("foobar"), 2);
    x.insert(String("bar"), 3);
    x.insert(String("barfoo"), 4);

    auto it = x.begin();
    EXPECT_EQ(*it++, 1);
    EXPECT_EQ(*it++, 2);
    it = x.erase(it);
    EXPECT_EQ(*it++, 4);
    EXPECT_EQ(x.count(), 3);
}

TEST(PedigreeRadixTree, SplitThis)
{
    RadixTree<int> x;
    x.insert(String("x86_64-pedigree-gcc"), 1);
    x.insert(String("x86_64-pedigree-g++"), 1);
    x.insert(String("x86_64-pedigree-gcc-4.8.2"), 1);
    x.insert(String("x86_64-pedigree-ld"), 1);
    x.insert(String("x86_64-pedigree-objdump"), 1);

    RadixTree<int>::LookupType result = x.lookup(String("x86_64-pedigree-gcc"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("x86_64-pedigree-g++"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("x86_64-pedigree-gcc-4.8.2"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("x86_64-pedigree-ld"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    result = x.lookup(String("x86_64-pedigree-objdump"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);
}

TEST(PedigreeRadixTree, LibrariesIssue)
{
    RadixTree<int> x;
    // create directory layout that seems to blow up ext2img
    x.insert(String("."), 1);
    x.insert(String(".."), 1);
    x.insert(String("lost+found"), 1);
    x.insert(String("keymaps"), 1);
    x.insert(String("linux"), 1);
    x.insert(String("docs"), 1);
    x.insert(String("libraries"), 1);
    x.insert(String("applications"), 1);
    x.insert(String("include"), 1);
    x.insert(String("support"), 1);
    x.insert(String("fonts"), 1);
    x.insert(String("usr"), 1);
    x.insert(String("doc"), 1);
    x.insert(String("initscripts"), 1);
    x.insert(String("lib64"), 1);
    x.insert(String("system"), 1);

    // this should work
    RadixTree<int>::LookupType result = x.lookup(String("libraries"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 1);

    // this should not as 'lib' hasn't been created yet
    result = x.lookup(String("lib"));
    EXPECT_TRUE(result.hasError());
}

TEST(PedigreeRadixTree, hmm)
{
    String foo("foo");
    String bar("bar");

    RadixTree<int> x;

    x.insert(foo, 1);
    x.insert(bar, 2);
    x.clear();

    x.insert(foo, 3);
    x.insert(bar, 4);

    RadixTree<int>::LookupType result = x.lookup(String("foo"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 3);

    result = x.lookup(String("bar"));
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(result.value(), 4);
}
