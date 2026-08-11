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

#include "DiskImage.h"
#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/utility.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

extern BootstrapStruct_t* g_pBootstrapInfo;

#define USE_FILE_IO 0

#if defined(TESTSUITE)
extern int diskImageMsync(void* address, size_t length, int flags);
#define DISKIMAGE_MSYNC diskImageMsync
#else
#define DISKIMAGE_MSYNC msync
#endif

DiskImage::DiskImage(const char* path)
    : Disk(),
      m_pFileName(path),
      m_nSize(0),
      m_pFile(0),
      m_FileNo(-1),
      m_pBuffer(0),
      m_HostPageSize(0) {}

DiskImage::~DiskImage() {
#if USE_FILE_IO
  if (m_pBuffer) {
    fflush(m_pFile);
    delete[] (char*)m_pBuffer;
  }
#elif HAS_ADDRESS_SANITIZER
  for (const auto& entry : m_BufferMap) {
    const BufferMapping& mapping = entry.second;
    DISKIMAGE_MSYNC(mapping.base, mapping.length, MS_SYNC);
    munmap(mapping.base, mapping.length);
  }
  m_BufferMap.clear();
#else
  if (m_pBuffer) {
    DISKIMAGE_MSYNC(m_pBuffer, m_nSize, MS_SYNC);
    munmap(m_pBuffer, m_nSize);
  }
#endif
  m_pBuffer = 0;

  if (m_pFile) {
    fflush(m_pFile);
    fclose(m_pFile);
    m_pFile = 0;
  }
  m_FileNo = -1;
}

bool DiskImage::initialise() {
  if (m_pFile)
    return false;

  m_pFile = fopen(m_pFileName, "rb+");
  if (!m_pFile)
    return false;

  m_FileNo = fileno(m_pFile);
  if (m_FileNo < 0) {
    fclose(m_pFile);
    m_pFile = 0;
    m_FileNo = -1;
    m_nSize = 0;
    m_HostPageSize = 0;
    return false;
  }

  struct stat st;
  int r = fstat(m_FileNo, &st);
  const long hostPageSize = sysconf(_SC_PAGESIZE);
  if (r < 0 || st.st_size <= 0 || hostPageSize <= 0 ||
      static_cast<std::uintmax_t>(st.st_size) > std::numeric_limits<size_t>::max() ||
      static_cast<std::uintmax_t>(hostPageSize) > std::numeric_limits<size_t>::max()) {
    fclose(m_pFile);
    m_pFile = 0;
    m_FileNo = -1;
    m_nSize = 0;
    m_HostPageSize = 0;
    return false;
  }

  m_nSize = static_cast<size_t>(st.st_size);
  m_HostPageSize = static_cast<size_t>(hostPageSize);

#if USE_FILE_IO
  m_pBuffer = (void*)new char[m_nSize];
#elif HAS_ADDRESS_SANITIZER
  m_BufferMap.clear();
#else
  m_pBuffer = mmap(0, m_nSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_FileNo, 0);
  if (m_pBuffer == MAP_FAILED) {
    m_pBuffer = 0;
    fclose(m_pFile);
    m_pFile = 0;
    m_FileNo = -1;
    m_nSize = 0;
    m_HostPageSize = 0;
    return false;
  }

  posix_madvise(m_pBuffer, m_nSize, POSIX_MADV_SEQUENTIAL);
#endif

  return true;
}

uintptr_t DiskImage::read(uint64_t location) {
  const uint64_t pageLocation = location & ~0xFFFULL;
  if (!m_pFile || location >= m_nSize || pageLocation >= m_nSize ||
      (m_nSize - pageLocation) < 4096) {
    fprintf(stderr, "DiskImage::read: read past EOF (%lu vs %lu)\n", location, m_nSize);
    return 0;
  }

  uint64_t off = location & 0xFFF;
#if USE_FILE_IO
  location &= ~0xFFF;
  fseek(m_pFile, location, SEEK_SET);
  ssize_t x = fread(adjust_pointer(m_pBuffer, location), 4096, 1, m_pFile);
  if (!x)
    return 0;
  return reinterpret_cast<uintptr_t>(m_pBuffer) + location + off;
#elif HAS_ADDRESS_SANITIZER
  auto it = m_BufferMap.find(pageLocation);
  if (it != m_BufferMap.end()) {
    const BufferMapping& mapping = it->second;
    return reinterpret_cast<uintptr_t>(mapping.base) + mapping.logicalOffset + off;
  }

  const uint64_t mappingLocation = (pageLocation / m_HostPageSize) * m_HostPageSize;
  const size_t logicalOffset = static_cast<size_t>(pageLocation - mappingLocation);
  if (logicalOffset > std::numeric_limits<size_t>::max() - getBlockSize()) {
    return 0;
  }
  const size_t mappingLength = logicalOffset + getBlockSize();
  void* p = mmap(0, mappingLength, PROT_READ | PROT_WRITE, MAP_SHARED, m_FileNo,
                 static_cast<off_t>(mappingLocation));
  if (p == MAP_FAILED) {
    fprintf(stderr, "DiskImage::read: mmap failed (%s)\n", std::strerror(errno));
    return 0;
  }

  m_BufferMap.insert({pageLocation, {p, mappingLength, logicalOffset}});
  return reinterpret_cast<uintptr_t>(p) + logicalOffset + off;
#else
  return reinterpret_cast<uintptr_t>(m_pBuffer) + location;
#endif
}

void DiskImage::write(uint64_t location) {
  writeback(location, MS_ASYNC);
}

void DiskImage::flush(uint64_t location) {
  writeback(location, MS_SYNC);
}

void DiskImage::writeback(uint64_t location, int flags) {
  const uint64_t pageLocation = location & ~0xFFFULL;
  if (!m_pFile || location >= m_nSize || pageLocation >= m_nSize ||
      (m_nSize - pageLocation) < 4096) {
    return;
  }

#if USE_FILE_IO
  location &= ~0xFFF;
  fseek(m_pFile, location, SEEK_SET);
  fwrite(adjust_pointer(m_pBuffer, location), 4096, 1, m_pFile);
  if (flags == MS_SYNC) {
    fflush(m_pFile);
  }
#elif HAS_ADDRESS_SANITIZER
  auto it = m_BufferMap.find(pageLocation);
  if (it != m_BufferMap.end()) {
    const BufferMapping& mapping = it->second;
    DISKIMAGE_MSYNC(mapping.base, mapping.length, flags);
  }
#else
  const uint64_t syncLocation = (pageLocation / m_HostPageSize) * m_HostPageSize;
  const size_t logicalOffset = static_cast<size_t>(pageLocation - syncLocation);
  if (logicalOffset > std::numeric_limits<size_t>::max() - getBlockSize()) {
    return;
  }
  DISKIMAGE_MSYNC(adjust_pointer(m_pBuffer, syncLocation), logicalOffset + getBlockSize(), flags);
#endif
}

size_t DiskImage::getSize() const {
  return m_nSize;
}

bool DiskImage::pin(uint64_t location) {
  const uint64_t pageLocation = location & ~0xFFFULL;
  return m_pFile && location < m_nSize && pageLocation < m_nSize &&
         (m_nSize - pageLocation) >= 4096;
}

void DiskImage::unpin(uint64_t location) {}
