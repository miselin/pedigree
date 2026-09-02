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

#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/drivers/common/DmaBuffer.h"
#include <gtest/gtest.h>

static_assert(BS16(0x0123U) == 0x2301U);
static_assert(BS32(0x01234567U) == 0x67452301U);
static_assert(BS64(0x0123456789ABCDEFULL) == 0xEFCDAB8967452301ULL);
static_assert(TargetInfo::getPageSize() == PAGE_SIZE);
static_assert(TargetInfo::getPointerBits() == sizeof(void*) * __CHAR_BIT__);

namespace {
class TestDmaAddressSpace {
 public:
  bool isMapped(void* address) {
    const uintptr_t page = reinterpret_cast<uintptr_t>(address) & ~uintptr_t(0x3fff);
    return page == 0x10000 || (m_MapSecond && page == 0x14000);
  }

  void getMapping(void* address, physical_uintptr_t& physicalAddress, size_t& flags) {
    const uintptr_t page = reinterpret_cast<uintptr_t>(address) & ~uintptr_t(0x3fff);
    physicalAddress = page == 0x10000 ? 0x80000 : m_SecondPhysical;
    flags = 0;
  }

  physical_uintptr_t m_SecondPhysical = 0x84000;
  bool m_MapSecond = true;
};
}  // namespace

TEST(DriverDma, ExactTranslationUsesTargetPageOffset) {
  TestDmaAddressSpace addressSpace;
  physical_uintptr_t physical = 0;

  ASSERT_TRUE(DriverDma::virtualToPhysical<16384>(addressSpace, 0x12345, physical));
  EXPECT_EQ(physical, 0x82345U);
}

TEST(DriverDma, ContiguousRangeChecksEveryTargetPage) {
  TestDmaAddressSpace addressSpace;
  physical_uintptr_t physical = 0;

  ASSERT_TRUE(
      DriverDma::contiguousVirtualRangeToPhysical<16384>(addressSpace, 0x13f00, 0x200, physical));
  EXPECT_EQ(physical, 0x83f00U);

  addressSpace.m_SecondPhysical = 0x94000;
  EXPECT_FALSE(
      DriverDma::contiguousVirtualRangeToPhysical<16384>(addressSpace, 0x13f00, 0x200, physical));

  addressSpace.m_MapSecond = false;
  EXPECT_FALSE(
      DriverDma::contiguousVirtualRangeToPhysical<16384>(addressSpace, 0x13f00, 0x200, physical));
}

TEST(DriverDma, PhysicalEndpointsRejectDiscontiguousPages) {
  EXPECT_TRUE(DriverDma::physicalEndpointsAreContiguous(0x83f00, 0x840ff, 0x200, 0xffffffff));
  EXPECT_FALSE(DriverDma::physicalEndpointsAreContiguous(0x83f00, 0x940ff, 0x200, 0xffffffff));
}

TEST(PedigreeUtility, PageAlignUsesTargetGeometry) {
  constexpr size_t pageSize = TargetInfo::getPageSize();
  alignas(pageSize) uint8_t pages[pageSize * 2] = {};
  const uintptr_t base = reinterpret_cast<uintptr_t>(pages);
  const auto align = [](void* address) { return reinterpret_cast<uintptr_t>(page_align(address)); };

  EXPECT_EQ(align(pages), base);
  EXPECT_EQ(align(pages + pageSize - 1), base);
  EXPECT_EQ(align(pages + pageSize), base + pageSize);
  EXPECT_EQ(align(pages + pageSize + 1), base + pageSize);
  EXPECT_EQ(align(pages + (pageSize * 2) - 1), base + pageSize);
}

TEST(PedigreeUtility, ChecksumPageUsesTargetGeometry) {
  uint32_t page[TargetInfo::getPageSize() / sizeof(uint32_t)] = {};
  page[0] = 0x12345678;
  page[(sizeof(page) / sizeof(page[0])) - 1] = 0x87654321;

  EXPECT_EQ(checksumPage(reinterpret_cast<uintptr_t>(page)),
            checksum32(page, TargetInfo::getPageSize()));
}

TEST(PedigreeUtility, SDirectoryName) {
  char buf[256];
  EXPECT_STREQ(SDirectoryName("/a/b/c", buf, 256), "/a/b");
  EXPECT_STREQ(SDirectoryName("/a/b/", buf, 256), "/a/b");
  EXPECT_STREQ(SDirectoryName("/a/b", buf, 256), "/a");
  EXPECT_EQ(SDirectoryName("c", buf, 256), nullptr);
}

TEST(PedigreeUtility, SBaseName) {
  char buf[256];
  EXPECT_STREQ(SBaseName("/a/b/c", buf, 256), "c");
  EXPECT_STREQ(SBaseName("/a/b/", buf, 256), nullptr);
  EXPECT_STREQ(SBaseName("/a/b", buf, 256), "b");
  EXPECT_STREQ(SBaseName("c", buf, 256), "c");
}

TEST(PedigreeUtility, DirectoryName) {
  const char* result = DirectoryName("/a/b/c");
  EXPECT_STREQ(result, "/a/b");
  delete[] result;

  result = DirectoryName("c");
  EXPECT_EQ(result, nullptr);
  delete[] result;
}

TEST(PedigreeUtility, BaseName) {
  const char* result = BaseName("/a/b/c");
  EXPECT_STREQ(result, "c");
  delete[] result;

  result = BaseName("/a/b/");
  EXPECT_STREQ(result, nullptr);
  delete[] result;

  result = BaseName("c");
  EXPECT_STREQ(result, "c");
  delete[] result;
}

TEST(PedigreeUtility, Fletcher16) {
  uint16_t buf[2048];
  memset(buf, 'a', 4096);

  uint16_t c1 = checksum16(buf, 4096);
  reinterpret_cast<uint8_t*>(buf)[0] = 'b';
  uint16_t c2 = checksum16(buf, 4096);
  EXPECT_NE(c1, c2);
}

TEST(PedigreeUtility, ByteSwapEvaluatesInputOnce) {
  size_t evaluations = 0;
  auto value = [&evaluations]() {
    ++evaluations;
    return 0x01234567U;
  };

  EXPECT_EQ(BS32(value()), 0x67452301U);
  EXPECT_EQ(evaluations, 1U);
}

TEST(PedigreeUtility, Fletcher32) {
  uint32_t buf[1024];
  memset(buf, 'a', 4096);

  uint32_t c1 = checksum32(buf, 4096);
  reinterpret_cast<uint8_t*>(buf)[0] = 'b';
  uint32_t c2 = checksum32(buf, 4096);
  EXPECT_NE(c1, c2);
}

TEST(PedigreeUtility, Fletcher32MatchesNaive) {
  uint32_t buf[1024];
  memset(buf, 'a', 4096);

  EXPECT_EQ(checksum32(buf, 4096), checksum32_naive(buf, 4096));
}

TEST(PedigreeUtility, ChecksumsDifferCorrectly) {
  uint32_t buf[1024];
  memset(buf, 'a', 4096);

  reinterpret_cast<uint8_t*>(buf)[1] = 'b';
  uint16_t s1 = checksum16(reinterpret_cast<uint16_t*>(buf), 4096);
  uint32_t d1 = checksum32(buf, 4096);

  // swap first two bytes now - checksum should differ as the data has
  // actually changed (will catch out purely naive sums)
  reinterpret_cast<uint8_t*>(buf)[0] = 'b';
  reinterpret_cast<uint8_t*>(buf)[1] = 'a';
  uint16_t s2 = checksum16(reinterpret_cast<uint16_t*>(buf), 4096);
  uint32_t d2 = checksum32(buf, 4096);

  EXPECT_NE(s1, s2);
  EXPECT_NE(d1, d2);
}

TEST(PedigreeUtility, Copies) {
  char buf[] = {1, 2, 3, 4, 5, 6};
  char dst[] = {0, 0, 0, 0, 0, 0};
  const char overlap_expected[] = {1, 1, 2, 3, 4, 5};

  pedigree_std::copy(dst, buf, sizeof buf);
  EXPECT_EQ(memcmp(buf, dst, sizeof buf), 0);

  // overlapping?
  pedigree_std::copy(&buf[1], buf, (sizeof buf) - 1);
  EXPECT_EQ(memcmp(buf, overlap_expected, sizeof buf), 0);
}

TEST(PedigreeUtility, NonTrivialCopies) {
  typedef SharedPointer<int> sharedintptr_t;

  sharedintptr_t ptr1 = sharedintptr_t::allocate(1);
  sharedintptr_t ptr2 = sharedintptr_t::allocate(2);
  sharedintptr_t ptr3 = sharedintptr_t::allocate(3);
  sharedintptr_t ptr4 = sharedintptr_t::allocate(4);

  sharedintptr_t a[] = {ptr1, ptr2, ptr3, ptr4};
  sharedintptr_t expect[] = {ptr1, ptr1, ptr2, ptr3};
  sharedintptr_t b[] = {sharedintptr_t(), sharedintptr_t(), sharedintptr_t(), sharedintptr_t()};
  size_t items = 4;

  // non-overlapping copy
  pedigree_std::copy(b, a, items);
  EXPECT_EQ(a[0].get(), b[0].get());
  EXPECT_EQ(a[1].get(), b[1].get());
  EXPECT_EQ(a[2].get(), b[2].get());
  EXPECT_EQ(a[3].get(), b[3].get());

  // make sure source is left unchanged
  EXPECT_EQ(a[0].get(), ptr1.get());
  EXPECT_EQ(a[1].get(), ptr2.get());
  EXPECT_EQ(a[2].get(), ptr3.get());
  EXPECT_EQ(a[3].get(), ptr4.get());

  // overlapping copy
  pedigree_std::copy(&a[1], a, items - 1);
  EXPECT_EQ(a[0].get(), expect[0].get());
  EXPECT_EQ(a[1].get(), expect[1].get());
  EXPECT_EQ(a[2].get(), expect[2].get());
  EXPECT_EQ(a[3].get(), expect[3].get());
}
