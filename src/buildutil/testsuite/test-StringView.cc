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

#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/template.h"

TEST(PedigreeStringView, ExplicitConstruction)
{
    StringView view("Hello world", 11);

    EXPECT_EQ(view.length(), 11);
}

TEST(PedigreeStringView, ImplicitConstruction)
{
    StringView view("Hello world");

    EXPECT_EQ(view.length(), 11);
}

TEST(PedigreeStringView, EqualityCString)
{
    StringView view("Hello world", 11);

    EXPECT_EQ(view, "Hello world");
}

TEST(PedigreeStringView, InEqualityCString)
{
    StringView view("Hello world", 11);

    EXPECT_NE(view, "Hello world this shouldn't match");
}

TEST(PedigreeStringView, EqualityStringView)
{
    StringView view1("Hello world", 11);
    StringView view2("Hello world", 11);

    EXPECT_EQ(view1, view2);
}

TEST(PedigreeStringView, EqualityCxxString)
{
    StringView view("Hello world", 11);
    String str("Hello world");

    EXPECT_EQ(view, str);
}

TEST(PedigreeStringView, ToString)
{
    StringView view("Hello world", 11);
    String str = view.toString();

    EXPECT_STREQ(str.cstr(), "Hello world");
}

TEST(PedigreeStringView, Substring)
{
    StringView view("Hello world", 11);

    StringView sub1 = view.substring(0, 5);
    StringView sub2 = view.substring(6, 11);
    StringView sub3 = view.substring(5, 6);
    StringView sub4 = view.substring(6, 5);
    StringView sub5 = view.substring(0, 15);
    StringView sub6 = view.substring(9, 15);

    EXPECT_EQ(sub1, "Hello");
    EXPECT_EQ(sub2, "world");
    EXPECT_EQ(sub3, " ");
    EXPECT_EQ(sub4, "");
    EXPECT_EQ(sub5, "Hello world");
    EXPECT_EQ(sub6, "ld");
}
