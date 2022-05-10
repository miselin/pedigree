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

#include "pedigree/kernel/utilities/utility.h"

TEST(PedigreeStringLibrary, StringLength)
{
    EXPECT_EQ(StringLength("hello"), 5U);
    EXPECT_EQ(StringLength(""), 0U);
}

TEST(PedigreeStringLibrary, BasicStrcpy)
{
    char buf[32] = {0};
    const char *source = "foobar";
    StringCopy(buf, source);
    EXPECT_STREQ(buf, source);
}

TEST(PedigreeStringLibrary, EmptyStrcpy)
{
    char buf[32] = {0};
    const char *source = "";
    StringCopy(buf, source);
    EXPECT_STREQ(buf, source);
}

TEST(PedigreeStringLibrary, EmbeddedNulStrcpy)
{
    char buf[32] = {0};
    const char *source = "abc\0def";
    StringCopy(buf, source);
    EXPECT_STREQ(buf, "abc");
}

TEST(PedigreeStringLibrary, BasicStrncpy)
{
    char buf[32] = {0};
    const char *source = "abcdef";
    StringCopyN(buf, source, 3);
    EXPECT_STREQ(buf, "abc");
}

TEST(PedigreeStringLibrary, EmbeddedNulStrncpy)
{
    char buf[32] = {0};
    const char *source = "abc\0def";
    StringCopyN(buf, source, 6);
    EXPECT_STREQ(buf, "abc");
}

TEST(PedigreeStringLibrary, EmptyStrncpy)
{
    char buf[32] = {0};
    const char *source = "abcdef";
    StringCopyN(buf, source, 0);
    EXPECT_STREQ(buf, "");
}

TEST(PedigreeStringLibrary, SmallFormat)
{
    char buf[32] = {0};
    StringFormat(buf, "Hello, %s!", "world");
    EXPECT_STREQ(buf, "Hello, world!");
}

TEST(PedigreeStringLibrary, CompareEmpty)
{
    EXPECT_EQ(StringCompare("", ""), 0);
}

TEST(PedigreeStringLibrary, CompareOneEmpty)
{
    EXPECT_EQ(StringCompare("abc", ""), 'a');
}

TEST(PedigreeStringLibrary, CompareOtherEmpty)
{
    // 'a' > '\0'
    EXPECT_EQ(StringCompare("", "abc"), -'a');
}

TEST(PedigreeStringLibrary, CompareSame)
{
    // Have to avoid constant strings pointing to the same place
    const char str_a[] = {'a', 'b', 'c', 0};
    const char str_b[] = {'a', 'b', 'c', 0};
    EXPECT_EQ(StringCompare(str_a, str_b), 0);
}

TEST(PedigreeStringLibrary, CompareLess)
{
    EXPECT_EQ(StringCompare("abc", "bcd"), 'a' - 'b');
}

TEST(PedigreeStringLibrary, CompareSome)
{
    EXPECT_EQ(StringCompareN("abcdef", "abc", 3), 0);
    EXPECT_EQ(StringCompareN("abcdef", "abc", 4), 'd');
    EXPECT_EQ(StringCompareN("abcdef", "abc", 1), 0);
    EXPECT_EQ(StringCompareN("abcdef", "abc", 0), 0);
}

TEST(PedigreeStringLibrary, CompareSomeOtherLonger)
{
    EXPECT_EQ(StringCompareN("abc", "abcdef", 3), 0);
    EXPECT_EQ(StringCompareN("abc", "abcdef", 4), -'d');
    EXPECT_EQ(StringCompareN("abc", "abcdef", 1), 0);
    EXPECT_EQ(StringCompareN("abc", "abcdef", 0), 0);
}

TEST(PedigreeStringLibrary, MatchEmpty)
{
    EXPECT_EQ(StringMatch("", ""), 0);
}

TEST(PedigreeStringLibrary, MatchOneEmpty)
{
    EXPECT_EQ(StringMatch("abc", ""), 1);
}

TEST(PedigreeStringLibrary, MatchOtherEmpty)
{
    // 'a' > '\0'
    EXPECT_EQ(StringMatch("", "abc"), 1);
}

TEST(PedigreeStringLibrary, MatchSame)
{
    // Have to avoid constant strings pointing to the same place
    const char str_a[] = {'a', 'b', 'c', 0};
    const char str_b[] = {'a', 'b', 'c', 0};
    EXPECT_EQ(StringMatch(str_a, str_b), 0);
}

TEST(PedigreeStringLibrary, MatchLess)
{
    EXPECT_EQ(StringMatch("abc", "bcd"), 1);
}

TEST(PedigreeStringLibrary, MatchSome)
{
    EXPECT_EQ(StringMatchN("abcdef", "abc", 3), 0);
    EXPECT_EQ(StringMatchN("abcdef", "abc", 4), 1);
    EXPECT_EQ(StringMatchN("abcdef", "abc", 1), 0);
    EXPECT_EQ(StringMatchN("abcdef", "abc", 0), 0);
}

TEST(PedigreeStringLibrary, MatchSomeOtherLonger)
{
    EXPECT_EQ(StringMatchN("abc", "abcdef", 3), 0);
    EXPECT_EQ(StringMatchN("abc", "abcdef", 4), 1);
    EXPECT_EQ(StringMatchN("abc", "abcdef", 1), 0);
    EXPECT_EQ(StringMatchN("abc", "abcdef", 0), 0);
}

TEST(PedigreeStringLibrary, CompareCaseEmpty)
{
    size_t off = 0;
    EXPECT_EQ(StringCompareCase("", "", false, 1, &off), 0);
    EXPECT_EQ(off, 0U);
}

TEST(PedigreeStringLibrary, CompareCaseOneEmpty)
{
    size_t off = 0;
    EXPECT_EQ(StringCompareCase("abc", "", false, 3, &off), 'a');
    EXPECT_EQ(off, 0U);
}

TEST(PedigreeStringLibrary, CompareCaseOtherEmpty)
{
    // 'a' > '\0'
    size_t off = 0;
    EXPECT_EQ(StringCompareCase("", "abc", false, 3, &off), -'a');
    EXPECT_EQ(off, 0U);
}

TEST(PedigreeStringLibrary, CompareCaseSame)
{
    // Have to avoid constant strings pointing to the same place
    const char str_lower[] = {'a', 'b', 'c', 0};
    const char str_upper[] = {'A', 'B', 'C', 0};

    size_t off = 0;
    EXPECT_EQ(StringCompareCase(str_lower, str_lower, false, 3, &off), 0);
    EXPECT_EQ(off, 3U);
    EXPECT_EQ(StringCompareCase(str_upper, str_upper, true, 3, &off), 0);
    EXPECT_EQ(off, 3U);
    EXPECT_EQ(
        StringCompareCase(str_upper, str_lower, true, 3, &off), 'A' - 'a');
    EXPECT_EQ(off, 0U);
}

TEST(PedigreeStringLibrary, CompareCaseLess)
{
    size_t off = 0;
    EXPECT_EQ(StringCompareCase("abc", "bcd", false, 3, &off), 'a' - 'b');
    EXPECT_EQ(off, 0U);
    EXPECT_EQ(StringCompareCase("ABC", "bcd", false, 3, &off), 'A' - 'b');
    EXPECT_EQ(off, 0U);
    EXPECT_EQ(StringCompareCase("ABC", "bcd", true, 3, &off), 'A' - 'b');
    EXPECT_EQ(off, 0U);
}

TEST(PedigreeStringLibrary, BasicStrcat)
{
    char buf[32] = {0};
    char *r = StringConcat(buf, "hello");
    EXPECT_STREQ(r, "hello");
}

TEST(PedigreeStringLibrary, EmptyStrcat)
{
    char buf[32] = {0};
    char *r = StringConcat(buf, "");
    EXPECT_STREQ(r, "");
}

TEST(PedigreeStringLibrary, BasicStrncat)
{
    char buf[32] = {0};
    char *r = StringConcatN(buf, "abcdef", 3);
    EXPECT_STREQ(r, "abc");
}

TEST(PedigreeStringLibrary, IsDigit)
{
    for (size_t i = 0; i < 10; ++i)
    {
        EXPECT_TRUE(isdigit('0' + i));
    }
}

TEST(PedigreeStringLibrary, StringFind)
{
    EXPECT_STREQ(StringFind("abc", 'a'), "abc");
    EXPECT_STREQ(StringFind("abc", 'c'), "c");
    EXPECT_EQ(StringFind("abc", 'd'), nullptr);
}

TEST(PedigreeStringLibrary, StringReverseFind)
{
    EXPECT_STREQ(StringReverseFind("abc", 'a'), "abc");
    EXPECT_STREQ(StringReverseFind("abc", 'c'), "c");
    EXPECT_STREQ(StringReverseFind("ccc", 'c'), "c");  // proper reverse find
    EXPECT_EQ(StringReverseFind("abc", 'd'), nullptr);
}

TEST(PedigreeStringLibrary, StringContains)
{
    EXPECT_EQ(StringContains("abc", "d"), 0);
    EXPECT_EQ(StringContains("abc", "b"), 1);
    EXPECT_EQ(StringContains("hello world", "llo"), 1);
    EXPECT_EQ(StringContains("hello world", " "), 1);
    EXPECT_EQ(StringContains("hello world", "world"), 1);
    EXPECT_EQ(StringContains("hello world", "hello world"), 1);
    EXPECT_EQ(StringContains("hello world", "hello world!"), 0);
    EXPECT_EQ(StringContains("hello world", "hi"), 0);
}
