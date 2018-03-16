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

#include "pedigree/kernel/utilities/Pointers.h"

TEST(PedigreeUniquePointer, Construction)
{
    UniquePointer<int> p;
    EXPECT_EQ(p.get(), nullptr);
}

TEST(PedigreeUniquePointer, Allocate)
{
    UniquePointer<int> p;
    p = UniquePointer<int>::allocate();
    EXPECT_NE(p.get(), nullptr);
    p.reset();
    EXPECT_EQ(p.get(), nullptr);
}

TEST(PedigreeUniquePointer, Move)
{
    UniquePointer<int> p1(UniquePointer<int>::allocate()), p2;
    EXPECT_NE(p1.get(), nullptr);

    p2 = pedigree_std::move(p1);
    EXPECT_EQ(p1.get(), nullptr);
    EXPECT_NE(p2.get(), nullptr);
}
