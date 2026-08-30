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

TEST(PedigreeBufferViewSequence, CopiesAcrossDiscontiguousViews) {
  uint8_t first[] = {0, 1, 2};
  uint8_t second[] = {3, 4};
  uint8_t third[] = {5, 6, 7, 8};
  BufferView storage[3];
  BufferViewSequence sequence(storage, 3);

  ASSERT_TRUE(sequence.append(BufferView(first, sizeof(first))));
  ASSERT_TRUE(sequence.append(BufferView(second, sizeof(second))));
  ASSERT_TRUE(sequence.append(BufferView(third, sizeof(third))));
  EXPECT_EQ(sequence.count(), 3U);
  EXPECT_EQ(sequence.size(), 9U);

  uint8_t copied[6] = {};
  ASSERT_TRUE(sequence.copyTo(copied, sizeof(copied), 2));
  const uint8_t expected[] = {2, 3, 4, 5, 6, 7};
  EXPECT_EQ(MemoryCompare(copied, expected, sizeof(expected)), 0);

  const uint8_t replacement[] = {0xA1, 0xA2, 0xA3, 0xA4};
  ASSERT_TRUE(sequence.copyFrom(replacement, sizeof(replacement), 1));
  EXPECT_EQ(first[1], 0xA1);
  EXPECT_EQ(first[2], 0xA2);
  EXPECT_EQ(second[0], 0xA3);
  EXPECT_EQ(second[1], 0xA4);

  sequence.clear();
  EXPECT_TRUE(sequence.empty());
  EXPECT_EQ(sequence.size(), 0U);
}

TEST(PedigreeBufferViewSequence, RejectsDescriptorExhaustion) {
  uint8_t bytes[2] = {};
  BufferView storage[1];
  BufferViewSequence sequence(storage, 1);

  ASSERT_TRUE(sequence.append(BufferView(bytes, 1)));
  EXPECT_FALSE(sequence.append(BufferView(bytes + 1, 1)));
  EXPECT_EQ(sequence.count(), 1U);
  EXPECT_EQ(sequence.size(), 1U);
}

#if !defined(NDEBUG)
TEST(PedigreeBufferView, RejectsOutOfBoundsArithmeticInDebugBuilds) {
  alignas(uint32_t) uint8_t bytes[8] = {};
  const BufferView view(bytes, sizeof(bytes));

  EXPECT_DEATH((void)(view + 9), "");
  EXPECT_DEATH((void)view.subview(7, 2), "");
  EXPECT_DEATH((void)view.as<uint32_t>(1), "");
  EXPECT_DEATH((void)view[8], "");

  BufferView storage[1];
  BufferViewSequence sequence(storage, 1);
  ASSERT_TRUE(sequence.append(view));
  uint8_t output[1] = {};
  EXPECT_DEATH((void)sequence.copyTo(output, 1, 9), "");
}
#endif
