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

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "buildutil/ext2img/DiskImage.h"
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <type_traits>

namespace {
constexpr size_t kBlockSize = 4096;
constexpr size_t kImageSize = (3 * kBlockSize) + (kBlockSize / 2);

struct MsyncCall {
  uintptr_t address;
  size_t length;
  int flags;
  int result;
};

std::vector<MsyncCall> g_MsyncCalls;

uint8_t patternAt(size_t offset) {
  return static_cast<uint8_t>((offset / kBlockSize) * 37 + (offset % 251));
}

void addMappingPages(std::set<uintptr_t>& pages, uintptr_t pointer, uint64_t location,
                     size_t hostPageSize) {
  const uint64_t pageLocation = location & ~(static_cast<uint64_t>(kBlockSize) - 1);
  const size_t logicalOffset = static_cast<size_t>(pageLocation % hostPageSize);
  const size_t blockOffset = static_cast<size_t>(location - pageLocation);
  const uintptr_t mappingBase = pointer - logicalOffset - blockOffset;
  const size_t mappingLength = logicalOffset + kBlockSize;
  for (size_t offset = 0; offset < mappingLength; offset += hostPageSize) {
    pages.insert(mappingBase + offset);
  }
}

class DiskImageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/pedigree-diskimage-XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    m_Path = path;

    std::vector<uint8_t> contents(kImageSize);
    for (size_t i = 0; i < contents.size(); ++i) {
      contents[i] = patternAt(i);
    }

    ASSERT_EQ(ftruncate(fd, contents.size()), 0);
    ASSERT_EQ(pwrite(fd, contents.data(), contents.size(), 0),
              static_cast<ssize_t>(contents.size()));
    ASSERT_EQ(close(fd), 0);
  }

  void TearDown() override {
    if (!m_Path.empty()) {
      unlink(m_Path.c_str());
    }
  }

  uint8_t readByte(size_t offset) const {
    const int fd = open(m_Path.c_str(), O_RDONLY);
    EXPECT_GE(fd, 0);
    uint8_t value = 0;
    if (fd >= 0) {
      EXPECT_EQ(pread(fd, &value, sizeof(value), offset), static_cast<ssize_t>(sizeof(value)));
      EXPECT_EQ(close(fd), 0);
    }
    return value;
  }

  std::string m_Path;
};
}  // namespace

int diskImageMsync(void* address, size_t length, int flags) {
  const int result = ::msync(address, length, flags);
  const int savedErrno = errno;
  g_MsyncCalls.push_back({reinterpret_cast<uintptr_t>(address), length, flags, result});
  errno = savedErrno;
  return result;
}

TEST_F(DiskImageTest, ReadsOnlyCompleteLogicalBlocks) {
  DiskImage image(m_Path.c_str());
  ASSERT_TRUE(image.initialise());
  EXPECT_EQ(image.getBlockSize(), kBlockSize);
  EXPECT_EQ(image.getSize(), kImageSize);

  const BufferView first = image.read(0);
  const BufferView interior = image.read(1024);
  ASSERT_TRUE(first);
  ASSERT_TRUE(interior);
  EXPECT_EQ(interior.address(), (first + 1024).address());
  EXPECT_EQ(interior.size(), kBlockSize - 1024);
  EXPECT_EQ(image.read(1024).address(), interior.address());
  EXPECT_EQ(*interior.as<const uint8_t>(), patternAt(1024));

  const BufferView second = image.read(kBlockSize);
  const BufferView third = image.read(2 * kBlockSize);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);
  EXPECT_EQ(*second.as<const uint8_t>(), patternAt(kBlockSize));
  EXPECT_EQ(*third.as<const uint8_t>(), patternAt(2 * kBlockSize));

  EXPECT_TRUE(image.pin(2 * kBlockSize));
  EXPECT_FALSE(image.pin(3 * kBlockSize));
  EXPECT_FALSE(image.read(3 * kBlockSize));
  EXPECT_FALSE(image.read(kImageSize));
  EXPECT_FALSE(image.pin(UINT64_MAX));
  EXPECT_FALSE(image.read(UINT64_MAX));
  image.write(UINT64_MAX);
}

TEST_F(DiskImageTest, WritesBackAndUnmapsOnDestruction) {
  auto image = std::make_unique<DiskImage>(m_Path.c_str());
  ASSERT_TRUE(image->initialise());

  const size_t firstOffset = kBlockSize + 137;
  const size_t secondOffset = (2 * kBlockSize) + 511;
  const BufferView first = image->read(firstOffset);
  const BufferView second = image->read(secondOffset);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  first[0] = 0xA5;
  second[0] = 0x5A;
  errno = 0;
  image->write(firstOffset);
  EXPECT_NE(errno, EINVAL);
  errno = 0;
  image->write(secondOffset);
  EXPECT_NE(errno, EINVAL);

  const long hostPageSize = sysconf(_SC_PAGESIZE);
  ASSERT_GT(hostPageSize, 0);
  std::set<uintptr_t> mappedPages;
  addMappingPages(mappedPages, first.address(), firstOffset, static_cast<size_t>(hostPageSize));
  addMappingPages(mappedPages, second.address(), secondOffset, static_cast<size_t>(hostPageSize));
  ASSERT_FALSE(mappedPages.empty());
#if defined(__APPLE__)
  char resident = 0;
#else
  unsigned char resident = 0;
#endif
  for (const uintptr_t mappedPage : mappedPages) {
    ASSERT_EQ(mincore(reinterpret_cast<void*>(mappedPage), hostPageSize, &resident), 0);
  }

  image.reset();

  for (const uintptr_t mappedPage : mappedPages) {
    errno = 0;
    EXPECT_EQ(mprotect(reinterpret_cast<void*>(mappedPage), hostPageSize, PROT_READ | PROT_WRITE),
              -1);
    EXPECT_EQ(errno, ENOMEM);
  }
  EXPECT_EQ(readByte(firstOffset), 0xA5);
  EXPECT_EQ(readByte(secondOffset), 0x5A);
}

TEST_F(DiskImageTest, ProvidesSynchronousFlush) {
  EXPECT_TRUE((std::is_same<decltype(&DiskImage::flush), void (DiskImage::*)(uint64_t)>::value));

  DiskImage image(m_Path.c_str());
  ASSERT_TRUE(image.initialise());

  const size_t offset = kBlockSize + 137;
  const BufferView data = image.read(offset);
  ASSERT_TRUE(data);
  data[0] = 0xC3;

  g_MsyncCalls.clear();
  errno = 0;
  image.flush(offset);
  EXPECT_NE(errno, EINVAL);
  ASSERT_EQ(g_MsyncCalls.size(), 1U);
  const MsyncCall& call = g_MsyncCalls[0];
  EXPECT_EQ(call.flags, MS_SYNC);
  EXPECT_EQ(call.result, 0);

  const long hostPageSize = sysconf(_SC_PAGESIZE);
  ASSERT_GT(hostPageSize, 0);
  EXPECT_EQ(call.address % static_cast<size_t>(hostPageSize), 0U);
  const size_t pageLocation = offset & ~(kBlockSize - 1);
  const size_t logicalOffset = pageLocation % static_cast<size_t>(hostPageSize);
  EXPECT_EQ(call.length, logicalOffset + kBlockSize);
  EXPECT_EQ(readByte(offset), 0xC3);
}

#if !HAS_ADDRESS_SANITIZER
TEST_F(DiskImageTest, BatchesAsynchronousWriteback) {
  constexpr size_t imageSize = 32U * 1024U * 1024U;
  ASSERT_EQ(truncate(m_Path.c_str(), imageSize), 0);

  DiskImage image(m_Path.c_str());
  ASSERT_TRUE(image.initialise());

  const long hostPageSize = sysconf(_SC_PAGESIZE);
  ASSERT_GT(hostPageSize, 0);

  g_MsyncCalls.clear();
  for (size_t offset = 0; offset < imageSize; offset += static_cast<size_t>(hostPageSize)) {
    const BufferView block = image.read(offset);
    ASSERT_TRUE(block);
    block[0] = static_cast<uint8_t>(offset / static_cast<size_t>(hostPageSize));
    image.write(offset);
  }

  size_t asynchronousCalls = 0;
  for (const MsyncCall& call : g_MsyncCalls) {
    if (call.flags != MS_ASYNC) {
      continue;
    }
    ++asynchronousCalls;
    EXPECT_EQ(call.address % static_cast<size_t>(hostPageSize), 0U);
    EXPECT_EQ(call.length % static_cast<size_t>(hostPageSize), 0U);
  }

  EXPECT_GT(asynchronousCalls, 0U);
  EXPECT_LT(asynchronousCalls, imageSize / kBlockSize);
}
#endif

TEST_F(DiskImageTest, FailedInitialisationDoesNotRetainFileDescriptor) {
  ASSERT_EQ(truncate(m_Path.c_str(), 0), 0);
  auto image = std::make_unique<DiskImage>(m_Path.c_str());
  EXPECT_FALSE(image->initialise());

  const int sentinel = open("/dev/null", O_RDONLY);
  ASSERT_GE(sentinel, 0);
  image.reset();

  EXPECT_NE(fcntl(sentinel, F_GETFD), -1);
  EXPECT_EQ(close(sentinel), 0);
}
