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

#include "pedigree/kernel/utilities/BufferView.h"

#include <gtest/gtest.h>
#include <type_traits>

static_assert(!std::is_convertible<BufferView, void*>::value);
static_assert(!std::is_convertible<BufferView, uintptr_t>::value);
static_assert(std::is_trivially_copyable<BufferView>::value);

TEST(PedigreeBufferView, DefaultViewRepresentsFailure) {
  const BufferView view;
  EXPECT_FALSE(view);
  EXPECT_TRUE(view.empty());
  EXPECT_EQ(view.data(), nullptr);
  EXPECT_EQ(view.address(), 0U);
  EXPECT_EQ(view.size(), 0U);
}

TEST(PedigreeBufferView, ProvidesBoundsAwareByteAccess) {
  alignas(uint32_t) uint8_t bytes[8] = {};
  BufferView view(bytes, sizeof(bytes));

  ASSERT_TRUE(view);
  EXPECT_EQ(view.data(), bytes);
  EXPECT_EQ(view.size(), sizeof(bytes));
  view[3] = 0xA5;
  EXPECT_EQ(bytes[3], 0xA5);

  uint32_t* word = view.as<uint32_t>(4);
  ASSERT_NE(word, nullptr);
  *word = 0x12345678;
  EXPECT_EQ(*reinterpret_cast<uint32_t*>(bytes + 4), 0x12345678U);
}

TEST(PedigreeBufferView, ArithmeticRetainsTheRemainingBounds) {
  uint8_t bytes[8] = {};
  const BufferView view(bytes, sizeof(bytes));

  const BufferView suffix = view + 3;
  EXPECT_EQ(suffix.data(), bytes + 3);
  EXPECT_EQ(suffix.size(), 5U);

  BufferView advanced = view;
  advanced += 6;
  EXPECT_EQ(advanced.data(), bytes + 6);
  EXPECT_EQ(advanced.size(), 2U);

  const BufferView end = view + view.size();
  EXPECT_TRUE(end);
  EXPECT_TRUE(end.empty());
  EXPECT_EQ(end.data(), bytes + sizeof(bytes));
}

TEST(PedigreeBufferView, SubviewRestrictsBothEnds) {
  uint8_t bytes[8] = {};
  const BufferView view(bytes, sizeof(bytes));

  const BufferView middle = view.subview(2, 3);
  EXPECT_EQ(middle.data(), bytes + 2);
  EXPECT_EQ(middle.size(), 3U);

  const BufferView prefix = view.first(4);
  EXPECT_EQ(prefix.data(), bytes);
  EXPECT_EQ(prefix.size(), 4U);
}

#if !defined(NDEBUG)
TEST(PedigreeBufferView, RejectsOutOfBoundsArithmeticInDebugBuilds) {
  alignas(uint32_t) uint8_t bytes[8] = {};
  const BufferView view(bytes, sizeof(bytes));

  EXPECT_DEATH((void)(view + 9), "");
  EXPECT_DEATH((void)view.subview(7, 2), "");
  EXPECT_DEATH((void)view.as<uint32_t>(1), "");
  EXPECT_DEATH((void)view[8], "");
}
#endif
