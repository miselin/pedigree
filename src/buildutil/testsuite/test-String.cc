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

#define IN_STRING_TESTSUITE 1

#include "pedigree/kernel/utilities/Cord.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/StringView.h"

// Output our String objects nicely (not as a list of bytes).
std::ostream &operator<<(::std::ostream &os, const String &s)
{
    return os << "\"" << static_cast<const char *>(s) << "\" ["
              << static_cast<const void *>(s.cstr()) << "]";
}

#define BIGSTRING                                                              \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

static const char *bigstring()
{
    return BIGSTRING;
}

TEST(PedigreeString, Construction)
{
    String s("hello");
    EXPECT_EQ(s, "hello");

    String s2(bigstring());
    EXPECT_EQ(s2, bigstring());

    EXPECT_NE(s, s2);
}

TEST(PedigreeString, BigStringStaticCast)
{
    String s(bigstring());
    EXPECT_STREQ(bigstring(), static_cast<const char *>(s));
}

TEST(PedigreeString, Length)
{
    String s("hello");
    EXPECT_EQ(s.length(), (size_t) 5);
}

TEST(PedigreeString, Size)
{
    // 64-byte static strings
    String s("hello");
    EXPECT_EQ(s.size(), (size_t) 64);

    // Dynamic strings are >64 bytes.
    String s2(bigstring());
    EXPECT_EQ(s2.size(), (size_t) 128);
}

TEST(PedigreeString, Chomp)
{
    String s("hello ");
    s.chomp();
    EXPECT_EQ(s, "hello");
}

TEST(PedigreeString, LChomp)
{
    String s(" hello");
    s.lchomp();
    EXPECT_EQ(s, "hello");
}

TEST(PedigreeString, ChompDynamicToStatic)
{
    String s(
        "hello                                                           ");
    s.chomp();
    EXPECT_EQ(s.length(), (size_t) 63);
    EXPECT_EQ(s.size(), (size_t) 65);
}

TEST(PedigreeString, LChompDynamicToStatic)
{
    String s(
        "hello                                                           ");
    s.lchomp();
    EXPECT_EQ(s.length(), (size_t) 63);
    EXPECT_EQ(s.size(), (size_t) 65);
}

TEST(PedigreeString, Strip)
{
    String s(" hello ");
    s.strip();
    EXPECT_EQ(s, "hello");
}

TEST(PedigreeString, Rstrip)
{
    String s(" hello ");
    s.rstrip();
    EXPECT_EQ(s, " hello");
}

TEST(PedigreeString, Lstrip)
{
    String s(" hello ");
    s.lstrip();
    EXPECT_EQ(s, "hello ");
}

TEST(PedigreeString, UnneededLstrip)
{
    String s("hello ");
    s.lstrip();
    EXPECT_EQ(s, "hello ");
}

TEST(PedigreeString, LstripSwitchesToStatic)
{
    String s(
        "                                                            hello");
    s.lstrip();
    EXPECT_EQ(s, "hello");
    EXPECT_EQ(s.size(), (size_t) 66);
    EXPECT_EQ(s.length(), (size_t) 5);
}

TEST(PedigreeString, UnneededRstrip)
{
    String s(" hello");
    s.rstrip();
    EXPECT_EQ(s, " hello");
}

TEST(PedigreeString, RstripSwitchesToStatic)
{
    String s(
        "hello                                                            ");
    s.rstrip();
    EXPECT_EQ(s, "hello");
    EXPECT_EQ(s.size(), (size_t) 66);
    EXPECT_EQ(s.length(), (size_t) 5);
}

TEST(PedigreeString, Split)
{
    String s("hello world");
    String right = s.split(5);
    EXPECT_EQ(s, "hello");
    EXPECT_EQ(right, " world");
}

TEST(PedigreeString, SplitRef)
{
    String s("hello world");
    String other;
    s.split(5, other);
    EXPECT_EQ(s, "hello");
    EXPECT_EQ(other, " world");
}

TEST(PedigreeString, SplitTooFar)
{
    String s("hello world");
    String other;
    s.split(15, other);
    EXPECT_EQ(s, "hello world");
    EXPECT_EQ(other, "");
}

TEST(PedigreeString, EmptyTokenise)
{
    String s("  a  ");
    Vector<String> result = s.tokenise(' ');
    EXPECT_EQ(result.count(), (size_t) 1);
    EXPECT_STREQ(result[0].cstr(), "a");
}

TEST(PedigreeString, Tokenise)
{
    String s("hello world, this is a testcase that exercises tokenise");
    Vector<String> result = s.tokenise(' ');
    EXPECT_EQ(result.count(), (size_t) 9);
    EXPECT_STREQ(result.popFront().cstr(), "hello");
    EXPECT_STREQ(result.popFront().cstr(), "world,");
    EXPECT_STREQ(result.popFront().cstr(), "this");
    EXPECT_STREQ(result.popFront().cstr(), "is");
    EXPECT_STREQ(result.popFront().cstr(), "a");
    EXPECT_STREQ(result.popFront().cstr(), "testcase");
    EXPECT_STREQ(result.popFront().cstr(), "that");
    EXPECT_STREQ(result.popFront().cstr(), "exercises");
    EXPECT_STREQ(result.popFront().cstr(), "tokenise");
    EXPECT_EQ(result.count(), (size_t) 0);  // no more tokens
}

TEST(PedigreeString, TokeniseWithViews)
{
    String s("hello world, this is a testcase that exercises tokenise");
    Vector<StringView> result;
    s.tokenise(' ', result);
    EXPECT_EQ(result.count(), (size_t) 9);
    EXPECT_EQ(result.popFront(), "hello");
    EXPECT_EQ(result.popFront(), "world,");
    EXPECT_EQ(result.popFront(), "this");
    EXPECT_EQ(result.popFront(), "is");
    EXPECT_EQ(result.popFront(), "a");
    EXPECT_EQ(result.popFront(), "testcase");
    EXPECT_EQ(result.popFront(), "that");
    EXPECT_EQ(result.popFront(), "exercises");
    EXPECT_EQ(result.popFront(), "tokenise");
    EXPECT_EQ(result.count(), (size_t) 0);  // no more tokens
}

TEST(PedigreeString, TokeniseLength)
{
    String s("hello world");
    Vector<String> result = s.tokenise(' ');
    EXPECT_EQ(result.popFront().length(), (size_t) 5);
    EXPECT_EQ(result.popFront().length(), (size_t) 5);
}

TEST(PedigreeString, NextCharacter)
{
    String s("hello");
    EXPECT_EQ(s.nextCharacter(0), 1U);
    EXPECT_EQ(s.nextCharacter(1), 2U);
    EXPECT_EQ(s.nextCharacter(2), 3U);
}

TEST(PedigreeString, NextCharacterUnicode2Byte)
{
    // 2-byte UTF-8 in the middle of two single-byte characters.
    String s("h»b");
    EXPECT_EQ(s.nextCharacter(0), 1U);
    EXPECT_EQ(s.nextCharacter(1), 3U);
    EXPECT_EQ(s.nextCharacter(3), 4U);
}

TEST(PedigreeString, NextCharacterUnicode3Byte)
{
    // 3-byte UTF-8 in the middle of two single-byte characters.
    String s("h€b");
    EXPECT_EQ(s.nextCharacter(0), 1U);
    EXPECT_EQ(s.nextCharacter(1), 4U);
    EXPECT_EQ(s.nextCharacter(4), 5U);
}

TEST(PedigreeString, NextCharacterUnicode4Byte)
{
    // 4-byte UTF-8 in the middle of two single-byte characters.
    String s("h𐍈b");
    EXPECT_EQ(s.nextCharacter(0), 1U);
    EXPECT_EQ(s.nextCharacter(1), 5U);
    EXPECT_EQ(s.nextCharacter(5), 6U);
}

TEST(PedigreeString, Equality)
{
    // length differs
    EXPECT_NE(String("a"), String("ab"));
    // text differs
    EXPECT_NE(String("a"), String("b"));
    // big string differs in length
    EXPECT_NE(String("a"), String(bigstring()));
    // big vs big still matches
    EXPECT_EQ(String(bigstring()), String(bigstring()));

    // freed vs freed
    String s1("a"), s2("b");
    s1.clear();
    EXPECT_NE(s1, s2);
    EXPECT_NE(s2, s1);
    s2.clear();
    EXPECT_EQ(s1, s2);
}

TEST(PedigreeString, EqualityRawCharBuffer)
{
    String a("hello");
    EXPECT_EQ(a, "hello");
    EXPECT_NE(a, nullptr);
    a.clear();
    EXPECT_EQ(a, nullptr);
    EXPECT_NE(a, "hello");
}

TEST(PedigreeString, AssignCString)
{
    String s;
    s.assign("hello");
    EXPECT_EQ(s, "hello");
}

TEST(PedigreeString, AssignBig)
{
    String s;
    s.assign(bigstring());
    EXPECT_EQ(s, bigstring());
}

TEST(PedigreeString, AssignNotQuiteAll)
{
    String s;
    s.assign("foobar", 3);
    EXPECT_EQ(s, "foo");
}

TEST(PedigreeString, AssignNothing)
{
    String s;
    s.assign("");
    EXPECT_EQ(s, "");
}

TEST(PedigreeString, AssignNull)
{
    String s;
    s.assign(0);
    EXPECT_EQ(s, "");
}

TEST(PedigreeString, AssignBigEmpty)
{
    String s;
    s.assign("a", 1025);
    EXPECT_EQ(s.size(), (size_t) 1025);
    EXPECT_EQ(s, "a");
    s.downsize();
    EXPECT_EQ(s.size(), (size_t) 64);
}

TEST(PedigreeString, AssignAnother)
{
    String s;
    String s2(bigstring());
    s.assign(s2);
    EXPECT_EQ(s, s2);
}

TEST(PedigreeString, ReduceReserve)
{
    // This should also not leak.
    String s;
    s.reserve(1024);
    EXPECT_EQ(s.size(), (size_t) 1024);
    s.downsize();
    EXPECT_EQ(s.size(), (size_t) 64);
}

TEST(PedigreeString, ReserveBoundary)
{
    String s;
    s.reserve(64);
    EXPECT_EQ(s.size(), (size_t) 64);
}

TEST(PedigreeString, ReserveWithContent)
{
    String s("hello");
    s.reserve(64);
    EXPECT_EQ(s.size(), (size_t) 64);
    EXPECT_EQ(s, "hello");
}

TEST(PedigreeString, ReserveWithHugeContent)
{
    String s(bigstring());
    s.reserve(1024);
    EXPECT_EQ(s.size(), (size_t) 1024);
}

TEST(PedigreeString, SplitHuge)
{
    String s(bigstring());
    String right = s.split(32);

    EXPECT_EQ(s.length(), (size_t) 32);
    EXPECT_EQ(right.length(), (size_t) (128 - 32 - 1));
}

TEST(PedigreeString, Sprintf)
{
    String s;
    s.Format("Hello, %s! %d %d\n", "world", 42, 84);
    EXPECT_EQ(s, "Hello, world! 42 84\n");
}

TEST(PedigreeString, Free)
{
    String s("hello");
    s.clear();
    EXPECT_EQ(s, "");
    EXPECT_EQ(s.length(), (size_t) 0);
    EXPECT_EQ(s.size(), (size_t) 0);
}

TEST(PedigreeString, FreeComparison)
{
    String s1("hello"), s2("hello");
    s1.clear();
    EXPECT_NE(s1, s2);
    s2.clear();
    EXPECT_EQ(s1, s2);
}

TEST(PedigreeString, FreeCharCast)
{
    String s("hello");
    s.clear();
    EXPECT_EQ(static_cast<const char *>(s), nullptr);
}

TEST(PedigreeString, FreeThenUse)
{
    String s("hello");
    s.clear();
    s.assign("hello");
    EXPECT_EQ(s, "hello");
}

TEST(PedigreeString, FreeThenStrip)
{
    String s("hello");
    s.clear();
    // Expecting no asan/valgrind/segfault errors, and no other funniness.
    s.strip();
    EXPECT_EQ(s, "");
    s.lstrip();
    EXPECT_EQ(s, "");
    s.rstrip();
    EXPECT_EQ(s, "");
}

TEST(PedigreeString, EndsWith)
{
    String s("hello");
    EXPECT_TRUE(s.endswith("ello"));
    EXPECT_TRUE(s.endswith(String("ello")));
}

TEST(PedigreeString, EndsWithEmpty)
{
    String s;
    EXPECT_FALSE(s.endswith('x'));
    EXPECT_FALSE(s.endswith("x"));
}

TEST(PedigreeString, EndsWithCharacter)
{
    String s("hello");
    EXPECT_TRUE(s.endswith('o'));
    EXPECT_FALSE(s.endswith('\0'));
}

TEST(PedigreeString, StartsWith)
{
    String s("hello");
    EXPECT_TRUE(s.startswith("hel"));
    EXPECT_TRUE(s.startswith(String("hel")));
}

TEST(PedigreeString, StartsWithEmpty)
{
    String s;
    EXPECT_FALSE(s.startswith('x'));
    EXPECT_FALSE(s.startswith("x"));
}

TEST(PedigreeString, StartsWithCharacter)
{
    String s("hello");
    EXPECT_TRUE(s.startswith('h'));
}

TEST(PedigreeString, EndsWithIsEquality)
{
    String s("hello");
    EXPECT_TRUE(s.endswith("hello"));
    EXPECT_TRUE(s.endswith(String("hello")));
}

TEST(PedigreeString, StartsWithIsEquality)
{
    String s("hello");
    EXPECT_TRUE(s.startswith("hello"));
    EXPECT_TRUE(s.startswith(String("hello")));
}

TEST(PedigreeString, StartsWithTooLong)
{
    String s("he");
    EXPECT_FALSE(s.startswith("hello"));
    EXPECT_FALSE(s.startswith(String("hello")));
}

TEST(PedigreeString, EndsWithTooLong)
{
    String s("he");
    EXPECT_FALSE(s.endswith("hello"));
    EXPECT_FALSE(s.endswith(String("hello")));
}

TEST(PedigreeString, Equality2)
{
    String s1("/dev/tty"), s2("/"), s3("/dev/tty"), s4("/");
    EXPECT_NE(s1, s2);
    EXPECT_NE(s2, s1);
    EXPECT_EQ(s1, "/dev/tty");
    EXPECT_NE(s2.cstr(), "/dev/tty");
    EXPECT_EQ(s1, s3);
    EXPECT_EQ(s2, s4);
}

TEST(PedigreeString, Equality3)
{
    String s1("/boot/kernel"), s2("/boot/kernel");
    EXPECT_STREQ(s1.cstr(), s2.cstr());
    EXPECT_EQ(s1, s2);
    EXPECT_STREQ(s1.cstr(), "/boot/kernel");
    EXPECT_EQ(s1, "/boot/kernel");
}

TEST(PedigreeString, Inequality)
{
    String s1("/dev/tty"), s2("/"), s3("/dev/tty0"), s4("/");
    EXPECT_NE(s1, s2);
    EXPECT_NE(s1, s3);
    EXPECT_EQ(s2, s4);
}

TEST(PedigreeString, InequalityOperators)
{
    String s1("hello world"), s2("hello world"), s3("different");
    // Standard eq/ne
    EXPECT_EQ(s1, s2);
    EXPECT_NE(s1, s3);

    // Explicit operator usage
    EXPECT_TRUE(s1 == s2);
    EXPECT_FALSE(s1 != s2);

    EXPECT_FALSE(s1 == s3);
    EXPECT_TRUE(s1 != s3);
}

TEST(PedigreeString, Find)
{
    String s("hello world");
    EXPECT_EQ(s.find('h'), 0);
    EXPECT_EQ(s.find('w'), 6);
    EXPECT_EQ(s.rfind('w'), 4);
    EXPECT_EQ(s.rfind('d'), 0);
    EXPECT_EQ(s.find('!'), -1);
    EXPECT_EQ(s.rfind('!'), -1);
}

TEST(PedigreeString, FindEmpty)
{
    String s;
    EXPECT_EQ(s.find('x'), -1);
    EXPECT_EQ(s.find('\0'), -1);
}

TEST(PedigreeString, ReverseFindEmpty)
{
    String s;
    EXPECT_EQ(s.rfind('x'), -1);
    EXPECT_EQ(s.rfind('\0'), -1);
}

TEST(PedigreeString, UnicodeConversion)
{
    uint32_t a = 'a';
    uint32_t b = 0x263a;   // 16-bit, U+263A (smiling face)
    uint32_t c = 0x1f389;  // U+1F389 (party popper)
    uint32_t d = 0xbb;     // 8-bit, U+BB (pedigree's path separator)

    char buf[5];
    ByteSet(buf, 0, 5);

    String::Utf32ToUtf8(a, buf);
    EXPECT_STREQ(buf, "a");

    String::Utf32ToUtf8(b, buf);
    EXPECT_STREQ(buf, "☺");

    String::Utf32ToUtf8(c, buf);
    EXPECT_STREQ(buf, "🎉");

    String::Utf32ToUtf8(d, buf);
    EXPECT_STREQ(buf, "»");
}

TEST(PedigreeString, Move)
{
    String s1("hello"), s2;
    EXPECT_STREQ(s1.cstr(), "hello");
    EXPECT_STRNE(s2.cstr(), "hello");
    s2 = pedigree_std::move(s1);
    EXPECT_EQ(s1.cstr(), nullptr);
    EXPECT_STREQ(s2.cstr(), "hello");
}

TEST(PedigreeString, MoveConstruct)
{
    String s1("hello");
    EXPECT_STREQ(s1.cstr(), "hello");
    String s2(pedigree_std::move(s1));
    EXPECT_EQ(s1.cstr(), nullptr);
    EXPECT_STREQ(s2.cstr(), "hello");
}

TEST(PedigreeString, AppendOtherString)
{
    String s1("hello");
    String s2(" world");
    s1 += s2;
    EXPECT_STREQ(s1.cstr(), "hello world");
}

TEST(PedigreeString, AppendOtherCString)
{
    String s1("hello");
    s1 += " world";
    EXPECT_STREQ(s1.cstr(), "hello world");
}

TEST(PedigreeString, AppendOtherStringBig)
{
    String s1("hello");
    String s2(BIGSTRING);
    s1 += s2;
    EXPECT_STREQ(s1.cstr(), "hello" BIGSTRING);
}

TEST(PedigreeString, AppendOtherCStringBig)
{
    String s1("hello");
    s1 += BIGSTRING;
    EXPECT_STREQ(s1.cstr(), "hello" BIGSTRING);
}

TEST(PedigreeString, View)
{
    String s1("hello");
    StringView s1_view = s1.view();
    EXPECT_EQ(s1_view, s1);
}

TEST(PedigreeString, CordAssignment)
{
    Cord cord;
    cord.append("hello ");
    cord.append("world");
    String str;
    str.assign(cord);
    EXPECT_EQ(str, "hello world");
}

TEST(PedigreeString, LTrim)
{
    String s1("hello world");
    s1.ltrim(6);
    EXPECT_STREQ(s1.cstr(), "world");
}

TEST(PedigreeString, RTrim)
{
    String s1("hello world");
    s1.rtrim(6);
    EXPECT_STREQ(s1.cstr(), "hello");
}

TEST(PedigreeString, ConstantCompare)
{
    auto s1 = MakeConstantString("hello world");
    EXPECT_STREQ(s1.cstr(), "hello world");
}

TEST(PedigreeString, DirectCompare)
{
    String s1("hello world");
    EXPECT_TRUE(s1.compare("hello world", 11));  // same string
    EXPECT_TRUE(s1.compare("hello world"));  // same string, using template
    EXPECT_FALSE(s1.compare("hello", 5));  // length mismatch
    EXPECT_FALSE(s1.compare("hello", 6));  // null mismatch
}

TEST(PedigreeString, ConstAssign)
{
    String s1;
    s1.assign("hello world");
    EXPECT_EQ(s1.length(), (size_t) 11);
    EXPECT_STREQ(s1.cstr(), "hello world");
}
