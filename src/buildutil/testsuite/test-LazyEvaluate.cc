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

#include "pedigree/kernel/utilities/LazyEvaluate.h"

int *createInt(const int &meta)
{
    return new int (meta);
}

void destroyInt(int *v)
{
    delete v;
}

TEST(PedigreeLazyEvaluate, CreationIsLazy)
{
    LazyEvaluate<int, int, createInt, destroyInt> evaluate(5);

    EXPECT_FALSE(evaluate.active());
}

TEST(PedigreeLazyEvaluate, BecomeActive)
{
    LazyEvaluate<int, int, createInt, destroyInt> evaluate(5);

    EXPECT_FALSE(evaluate.active());
    EXPECT_EQ(*evaluate, 5);
    EXPECT_TRUE(evaluate.active());
}

TEST(PedigreeLazyEvaluate, Destruction)
{
    LazyEvaluate<int, int, createInt, destroyInt> evaluate(5);

    evaluate.get();
    EXPECT_TRUE(evaluate.active());
    evaluate.reset();
    EXPECT_FALSE(evaluate.active());
}
