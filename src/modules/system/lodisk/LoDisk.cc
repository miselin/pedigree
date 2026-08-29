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

#include "LoDisk.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceFeatures.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/assert.h"

#include "modules/Module.h"
#include "modules/system/vfs/VFS.h"

FileDisk::FileDisk(String file, AccessType mode)
    : m_pFile(0),
      m_Mode(mode),
      m_Cache(),
      m_MemRegion("FileDisk"),
      m_ReqMutex(),
      m_nAlignPoints(0) {
  m_pFile = VFS::instance().find(file);
  if (!m_pFile)
    WARNING("FileDisk: '" << file << "' doesn't exist...");
  else {
    m_pFile->increaseRefCount(false);

    // Chat to the partition service and let it pick up that we're around
    // now
    ServiceFeatures* pFeatures =
        ServiceManager::instance().enumerateOperations(String("partition"));
    Service* pService = ServiceManager::instance().getService(String("partition"));
    NOTICE("Asking if the partition provider supports touch");
    if (pFeatures->provides(ServiceFeatures::touch)) {
      NOTICE(
          "It does, attempting to inform the partitioner of our "
          "presence...");
      if (pService) {
        if (pService->serve(ServiceFeatures::touch,
                            reinterpret_cast<void*>(static_cast<Disk*>(this)),
                            sizeof(*static_cast<Disk*>(this)))) {
          NOTICE("Successful.");
        } else
          ERROR("Failed.");
      } else
        ERROR(
            "FileDisk: Couldn't tell the partition service about the "
            "new disk presence");
    } else
      ERROR("FileDisk: Partition service doesn't appear to support touch");
  }
}

FileDisk::~FileDisk() {
  m_Cache.shutdown();
  if (m_pFile) {
    m_pFile->decreaseRefCount(false);
  }
}

bool FileDisk::initialise() {
  return (m_pFile != 0);
}

BufferView FileDisk::read(uint64_t location) {
  LockGuard<Mutex> guard(m_ReqMutex);

  if (location % 512)
    FATAL("Read with location % 512.");

  if (!m_pFile)
    return BufferView();

  const size_t pageSize = TargetInfo::getPageSize();

  // Look through the align points.
  uint64_t alignPoint = 0;
  for (size_t i = 0; i < m_nAlignPoints; i++)
    if (m_AlignPoints[i] <= location && m_AlignPoints[i] > alignPoint)
      alignPoint = m_AlignPoints[i];
  alignPoint %= pageSize;

  // Determine which page the read is in
  const uint64_t pageOffset = (location - alignPoint) % pageSize;
  const uint64_t readPage = location - pageOffset;
  const uint64_t fileSize = m_pFile->getSize();
  if (location >= fileSize || readPage >= fileSize || (fileSize - readPage) < pageSize) {
    return BufferView();
  }

  uintptr_t buffer = m_Cache.lookup(readPage);

  if (buffer)
    return BufferView::fromAddress(buffer + pageOffset, pageSize - pageOffset);

  buffer = m_Cache.insert(readPage);
  if (!buffer)
    return BufferView();

  // Read the data from the file itself
  if (m_pFile->read(readPage, pageSize, buffer) != pageSize) {
    const bool discarded = m_Cache.discardEditing(readPage);
    (void)discarded;
    return BufferView();
  }

  m_Cache.markNoLongerEditing(readPage);

  buffer = m_Cache.lookup(readPage);
  return buffer ? BufferView::fromAddress(buffer + pageOffset, pageSize - pageOffset)
                : BufferView();
}

void FileDisk::write(uint64_t location) {
  LockGuard<Mutex> guard(m_ReqMutex);
  if (!m_pFile)
    return;

  /// \todo implement this
}

void FileDisk::align(uint64_t location) {
  LockGuard<Mutex> guard(m_ReqMutex);
  for (size_t i = 0; i < m_nAlignPoints; ++i) {
    if (m_AlignPoints[i] == location) {
      return;
    }
  }
  assert(m_nAlignPoints < 8);
  m_AlignPoints[m_nAlignPoints++] = location;
}

size_t FileDisk::getSize() const {
  return m_pFile ? m_pFile->getSize() : 0;
}

bool FileDisk::pin(uint64_t location) {
  LockGuard<Mutex> guard(m_ReqMutex);

  const size_t pageSize = TargetInfo::getPageSize();

  uint64_t alignPoint = 0;
  for (size_t i = 0; i < m_nAlignPoints; i++)
    if (m_AlignPoints[i] <= location && m_AlignPoints[i] > alignPoint)
      alignPoint = m_AlignPoints[i];
  alignPoint %= pageSize;

  const uint64_t page = location - ((location - alignPoint) % pageSize);
  const uint64_t fileSize = m_pFile ? m_pFile->getSize() : 0;
  if (location >= fileSize || page >= fileSize || (fileSize - page) < pageSize) {
    return false;
  }
  return m_Cache.pin(page);
}

void FileDisk::unpin(uint64_t location) {
  LockGuard<Mutex> guard(m_ReqMutex);

  const size_t pageSize = TargetInfo::getPageSize();

  uint64_t alignPoint = 0;
  for (size_t i = 0; i < m_nAlignPoints; i++)
    if (m_AlignPoints[i] <= location && m_AlignPoints[i] > alignPoint)
      alignPoint = m_AlignPoints[i];
  alignPoint %= pageSize;

  const uint64_t page = location - ((location - alignPoint) % pageSize);
  const uint64_t fileSize = m_pFile ? m_pFile->getSize() : 0;
  if (location >= fileSize || page >= fileSize || (fileSize - page) < pageSize) {
    return;
  }
  m_Cache.release(page);
}

static bool init() {
  return true;
}

static void destroy() {}

MODULE_INFO("lodisk", &init, &destroy, "vfs");
