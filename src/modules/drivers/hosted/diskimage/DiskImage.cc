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
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/utilities/assert.h"

extern BootstrapStruct_t* g_pBootstrapInfo;

DiskImage::~DiskImage() {
  m_Cache.shutdown();
}

bool DiskImage::initialise() {
  if (g_pBootstrapInfo->getModuleCount() < 3) {
    NOTICE("not enough modules to create a DiskImage");
    return false;
  }

  uintptr_t baseAddress = g_pBootstrapInfo->getModuleArray()[2].base;
  uintptr_t endAddress = g_pBootstrapInfo->getModuleArray()[2].end;

  m_pBase = reinterpret_cast<void*>(baseAddress);
  m_nSize = endAddress - baseAddress;
  return true;
}

uintptr_t DiskImage::read(uint64_t location) {
  LockGuard<Mutex> guard(m_CacheLock);
  if ((location >= m_nSize) || !m_pBase) {
    ERROR("DiskImage::read() - location " << location << " >= " << m_nSize);
    ERROR("  -> or " << m_pBase << " is null");
    return 0;
  }

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const uint64_t pageLocation = getPageLocation(location);
  const uint64_t pageOffset = location - pageLocation;
  if (pageLocation >= m_nSize || pageSize > (m_nSize - pageLocation)) {
    ERROR("DiskImage::read() - page at " << pageLocation << " extends past " << m_nSize);
    return 0;
  }

  uintptr_t buffer = m_Cache.lookup(pageLocation);
  if (buffer) {
    return buffer + pageOffset;
  }

  bool didExist = false;
  buffer = m_Cache.insert(pageLocation, &didExist);
  if (!buffer) {
    return 0;
  }
  if (!didExist) {
    MemoryCopy(reinterpret_cast<void*>(buffer), adjust_pointer(m_pBase, pageLocation), pageSize);

    m_Cache.markNoLongerEditing(pageLocation);
  }

  buffer = m_Cache.lookup(pageLocation);
  return buffer ? buffer + pageOffset : 0;
}

size_t DiskImage::getSize() const {
  return m_nSize;
}

void DiskImage::align(uint64_t location) {
  LockGuard<Mutex> guard(m_CacheLock);
  for (size_t i = 0; i < m_nAlignPoints; ++i) {
    if (m_AlignPoints[i] == location) {
      return;
    }
  }

  assert(m_nAlignPoints < 8);
  m_AlignPoints[m_nAlignPoints++] = location;
}

bool DiskImage::pin(uint64_t location) {
  LockGuard<Mutex> guard(m_CacheLock);
  return m_Cache.pin(getPageLocation(location));
}

void DiskImage::unpin(uint64_t location) {
  LockGuard<Mutex> guard(m_CacheLock);
  m_Cache.release(getPageLocation(location));
}

uint64_t DiskImage::getAlignmentPoint(uint64_t location) const {
  uint64_t alignPoint = 0;
  for (size_t i = 0; i < m_nAlignPoints; ++i) {
    if (m_AlignPoints[i] <= location && m_AlignPoints[i] > alignPoint) {
      alignPoint = m_AlignPoints[i];
    }
  }
  return alignPoint;
}

uint64_t DiskImage::getPageLocation(uint64_t location) const {
  const uint64_t alignPoint = getAlignmentPoint(location);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  return location - ((location - alignPoint) % pageSize);
}
