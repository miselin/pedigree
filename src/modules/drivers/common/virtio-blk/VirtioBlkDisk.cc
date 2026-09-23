/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "VirtioBlkDisk.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/utility.h"

#include "VirtioBlkController.h"

VirtioBlkDisk::VirtioBlkDisk(VirtioBlkController* controller) : m_Controller(controller) {
  m_pParent = controller;
  setSpecificType(String("virtio-blk-disk"));
}

VirtioBlkDisk::~VirtioBlkDisk() {
  retireEndpoint();
  shutdownCache();
}

void VirtioBlkDisk::getName(String& name) {
  name.assign("Virtio Block Disk");
}

size_t VirtioBlkDisk::getSize() const {
  return m_Controller->sizeBytes();
}

size_t VirtioBlkDisk::getBlockCount() const {
  return getSize() / getNativeBlockSize();
}

size_t VirtioBlkDisk::getBlockSize() const {
  return TargetInfo::getPageSize();
}

size_t VirtioBlkDisk::getNativeBlockSize() const {
  return m_Controller->sectorBytes();
}

bool VirtioBlkDisk::writeFrom(uint64_t location, const void* buffer, size_t length) {
  if (m_Controller->readOnly()) {
    return !length && location <= getSize();
  }
  return ScsiDisk::writeFrom(location, buffer, length);
}

bool VirtioBlkDisk::writeFromBatch(WriteBuffer* buffers, size_t count) {
  if (m_Controller->readOnly()) {
    for (size_t i = 0; buffers && i < count; ++i) {
      buffers[i].complete = false;
    }
    return !count;
  }
  return ScsiDisk::writeFromBatch(buffers, count);
}

void VirtioBlkDisk::write(uint64_t location) {
  if (!m_Controller->readOnly()) {
    ScsiDisk::write(location);
  }
}

bool VirtioBlkDisk::zero(uint64_t location, size_t length) {
  return m_Controller->readOnly() ? (!length && location <= getSize())
                                  : ScsiDisk::zero(location, length);
}

BufferView VirtioBlkDisk::acquireView(uint64_t location, bool writable, uint64_t& token) {
  if (writable && m_Controller->readOnly()) {
    return {};
  }
  return ScsiDisk::acquireView(location, writable, token);
}

size_t VirtioBlkDisk::validPageLength(uint64_t location) const {
  const size_t native = getNativeBlockSize();
  if (!native || location >= getSize() || location % native) {
    return 0;
  }
  const size_t remaining = getSize() - location;
  const size_t page = TargetInfo::getPageSize();
  return remaining < page ? remaining : page;
}

uint64_t VirtioBlkDisk::doRead(uint64_t location) {
  const size_t bytes = getCacheFillLength(location);
  if (!bytes || bytes % getNativeBlockSize()) {
    return 0;
  }
  const uintptr_t existing = getCache().lookup(location);
  if (existing) {
    getCache().release(location);
    return bytes;
  }
  bool existed = false;
  const uintptr_t page = getCache().insert(location, &existed);
  if (!page) {
    return 0;
  }
  if (existed) {
    return bytes;
  }
  ByteSet(reinterpret_cast<void*>(page), 0, TargetInfo::getPageSize());
  if (!m_Controller->readWrite(location, reinterpret_cast<void*>(page), bytes, false)) {
    if (!getCache().discardEditing(location)) {
      FATAL("Virtio block: failed to discard incomplete cache fill");
    }
    return 0;
  }
  getCache().markNoLongerEditing(location);
  return bytes;
}

uint64_t VirtioBlkDisk::doWrite(uint64_t location) {
#if CRIPPLE_HDD
  return 0;
#else
  if (m_Controller->readOnly() || !validPageLength(location)) {
    return 0;
  }
  const uintptr_t page = getCache().lookup(location);
  if (!page) {
    return 0;
  }
  CachePageGuard guard(getCache(), location);
  return doWriteDirect(location, page);
#endif
}

uint64_t VirtioBlkDisk::doWriteDirect(uint64_t location, uintptr_t page) {
#if CRIPPLE_HDD
  return 0;
#else
  const size_t bytes = validPageLength(location);
  if (m_Controller->readOnly() || !page || !bytes) {
    return 0;
  }
  return m_Controller->readWrite(location, reinterpret_cast<void*>(page), bytes, true) ? bytes : 0;
#endif
}

uint64_t VirtioBlkDisk::doSync(uint64_t location) {
#if CRIPPLE_HDD
  return 0;
#else
  const size_t bytes = location == SyncWholeDevice ? size_t{1} : validPageLength(location);
  return bytes && m_Controller->flush() ? bytes : 0;
#endif
}

bool VirtioBlkDisk::transferBuffer(uint64_t location, void* buffer, size_t length, bool writing) {
#if CRIPPLE_HDD
  if (writing) {
    return false;
  }
#endif
  const size_t native = getNativeBlockSize();
  if (!buffer || !length || length > TargetInfo::getPageSize() || location >= getSize() ||
      length > getSize() - location || !native || location % native || length % native ||
      (writing && m_Controller->readOnly())) {
    return false;
  }
  return m_Controller->readWrite(location, buffer, length, writing);
}
