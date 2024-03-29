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

#include "pedigree/kernel/utilities/Cord.h"
#include "pedigree/kernel/utilities/StaticCord.h"
#include "pedigree/kernel/utilities/String.h"

TEST(PedigreeCord, ToString)
{
    const char *a = "hello ";
    const char *b = "world!";

    Cord s;
    s.append(a);
    s.append(b);
    EXPECT_EQ(s.length(), 12U);

    String str = s.toString();

    EXPECT_STREQ(str.cstr(), "hello world!");
}

TEST(PedigreeCord, Append)
{
    const char *a = "a ";
    const char *b = "b ";
    const char *c = "c";

    Cord s;
    s.append(a);
    s.append(b);
    s.append(c);
    EXPECT_EQ(s.length(), 5U);

    String str = s.toString();
    EXPECT_STREQ(str.cstr(), "a b c");
}

TEST(PedigreeCord, Prepend)
{
    const char *a = "a ";
    const char *b = "b ";
    const char *c = "c";

    Cord s;
    s.prepend(c);
    s.prepend(b);
    s.prepend(a);
    EXPECT_EQ(s.length(), 5U);

    String str = s.toString();
    EXPECT_STREQ(str.cstr(), "a b c");
}

TEST(PedigreeCord, Indexing)
{
    const char *a = "a ";
    const char *b = "b ";
    const char *c = "c";

    Cord s;
    s.append(a);
    s.append(b);
    s.append(c);

    EXPECT_EQ(s.length(), 5U);
    EXPECT_EQ(s[0], 'a');
    EXPECT_EQ(s[1], ' ');
    EXPECT_EQ(s[2], 'b');
    EXPECT_EQ(s[3], ' ');
    EXPECT_EQ(s[4], 'c');
    EXPECT_EQ(s[5], '\0');
}

TEST(PedigreeCord, Iteration)
{
    const char *a = "a ";
    const char *b = "b ";
    const char *c = "c";

    Cord s;
    s.append(a);
    s.append(b);
    s.append(c);

    auto it = s.begin();

    EXPECT_EQ(s.length(), 5U);
    EXPECT_EQ(it, s.begin());
    EXPECT_EQ(*it++, 'a');
    EXPECT_EQ(*it++, ' ');
    EXPECT_EQ(*it++, 'b');
    EXPECT_EQ(*it++, ' ');
    EXPECT_EQ(*it++, 'c');
    EXPECT_EQ(it, s.end());
}

TEST(PedigreeCord, StaticAppend)
{
    const char *a = "a ";
    const char *b = "b ";
    const char *c = "c";

    StaticCord<8> s;
    s.append(a);
    s.append(b);
    s.append(c);
    EXPECT_EQ(s.length(), 5U);

    String str = s.toString();
    EXPECT_STREQ(str.cstr(), "a b c");
}

TEST(PedigreeCord, StaticIndexing)
{
    const char *a = "a ";
    const char *b = "b ";
    const char *c = "c";

    StaticCord<8> s;
    s.append(a);
    s.append(b);
    s.append(c);

    EXPECT_EQ(s.length(), 5U);
    EXPECT_EQ(s[0], 'a');
    EXPECT_EQ(s[1], ' ');
    EXPECT_EQ(s[2], 'b');
    EXPECT_EQ(s[3], ' ');
    EXPECT_EQ(s[4], 'c');
    EXPECT_EQ(s[5], '\0');
}

TEST(PedigreeCord, StaticIteration)
{
    const char *a = "a ";
    const char *b = "b ";
    const char *c = "c";

    StaticCord<8> s;
    s.append(a);
    s.append(b);
    s.append(c);

    auto it = s.begin();

    EXPECT_EQ(s.length(), 5U);
    EXPECT_EQ(it, s.begin());
    EXPECT_EQ(*it++, 'a');
    EXPECT_EQ(*it++, ' ');
    EXPECT_EQ(*it++, 'b');
    EXPECT_EQ(*it++, ' ');
    EXPECT_EQ(*it++, 'c');
    EXPECT_EQ(it, s.end());
}
