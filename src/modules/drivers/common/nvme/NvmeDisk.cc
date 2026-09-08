/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "NvmeDisk.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/utility.h"

#include "NvmeController.h"

NvmeDisk::NvmeDisk(NvmeController* controller, uint32_t nsid)
    : m_Controller(controller), m_Nsid(nsid), m_Blocks(0), m_Bytes(0), m_BlockBytes(0) {
  m_pParent = controller;
  setSpecificType(String("nvme-disk"));
}
NvmeDisk::~NvmeDisk() {
  retireEndpoint();
  shutdownCache();
}
bool NvmeDisk::initialise() {
  uint8_t data[Nvme::PageSize]{};
  if (!m_Controller || !m_Controller->identify(m_Nsid, 0, data))
    return false;
  uint64_t blocks = 0, capacity = 0;
  for (size_t i = 0; i < 8; ++i) {
    blocks |= uint64_t{data[i]} << (8 * i);
    capacity |= uint64_t{data[8 + i]} << (8 * i);
  }
  const uint8_t format = data[26] & 15U;
  const size_t formatOffset = 128 + 4 * format;
  const uint16_t metadata = data[formatOffset] | (uint16_t{data[formatOffset + 1]} << 8);
  const uint8_t lbads = data[formatOffset + 2];
  // Extended LBA formats, protection information, and shared/multipath
  // namespaces require protocols this initial NVM transport does not provide.
  if ((data[26] & 0xe0U) || format > data[25] || metadata || (data[29] & 7U) || (data[30] & 1U) ||
      (lbads != 9 && lbads != 12) || !blocks || !capacity || capacity > blocks) {
    WARNING("NVMe: namespace " << m_Nsid << " unsupported format, protection, or sharing");
    return false;
  }
  const size_t blockBytes = size_t{1} << lbads;
  if (blocks > ~size_t{0} / blockBytes || TargetInfo::getPageSize() % blockBytes ||
      m_Controller->maxTransfer() < TargetInfo::getPageSize())
    return false;
  m_Blocks = blocks;
  m_BlockBytes = blockBytes;
  m_Bytes = m_Blocks * m_BlockBytes;
  NOTICE("NVMe: namespace " << Dec << m_Nsid << ", " << m_Blocks << " blocks of " << m_BlockBytes
                            << " bytes, " << m_Bytes << " bytes" << Hex);
  return true;
}
void NvmeDisk::getName(String& name) {
  name.assign(m_Controller->model());
}
size_t NvmeDisk::getBlockSize() const {
  return TargetInfo::getPageSize();
}
size_t NvmeDisk::validPageLength(uint64_t location) const {
  if (!m_BlockBytes || location >= m_Bytes || location % m_BlockBytes)
    return 0;
  const size_t remaining = m_Bytes - location;
  return remaining < TargetInfo::getPageSize() ? remaining : TargetInfo::getPageSize();
}
uint64_t NvmeDisk::doRead(uint64_t location) {
  const size_t bytes = getCacheFillLength(location);
  if (!m_BlockBytes || !bytes || bytes % m_BlockBytes)
    return 0;
  const uintptr_t existing = getCache().lookup(location);
  if (existing) {
    getCache().release(location);
    return bytes;
  }
  bool existed = false;
  const uintptr_t page = getCache().insert(location, &existed);
  if (!page)
    return 0;
  if (existed)
    return bytes;
  ByteSet(reinterpret_cast<void*>(page), 0, TargetInfo::getPageSize());
  if (!m_Controller->readWrite(m_Nsid, location / m_BlockBytes, bytes / m_BlockBytes,
                               reinterpret_cast<void*>(page), bytes, false)) {
    if (!getCache().discardEditing(location))
      FATAL("NVMe: failed to discard incomplete cache fill");
    return 0;
  }
  getCache().markNoLongerEditing(location);
  return bytes;
}
uint64_t NvmeDisk::doWrite(uint64_t location) {
#if CRIPPLE_HDD
  return 0;
#else
  if (!validPageLength(location))
    return 0;
  const uintptr_t page = getCache().lookup(location);
  if (!page)
    return 0;
  CachePageGuard guard(getCache(), location);
  return doWriteDirect(location, page);
#endif
}
uint64_t NvmeDisk::doWriteDirect(uint64_t location, uintptr_t page) {
#if CRIPPLE_HDD
  return 0;
#else
  const size_t bytes = validPageLength(location);
  if (!page || !bytes)
    return 0;
  return m_Controller->readWrite(m_Nsid, location / m_BlockBytes, bytes / m_BlockBytes,
                                 reinterpret_cast<void*>(page), bytes, true)
             ? bytes
             : 0;
#endif
}
uint64_t NvmeDisk::doSync(uint64_t location) {
#if CRIPPLE_HDD
  return 0;
#else
  const size_t bytes = location == SyncWholeDevice ? bool(m_BlockBytes) : validPageLength(location);
  return bytes && m_Controller->flush(m_Nsid) ? bytes : 0;
#endif
}
