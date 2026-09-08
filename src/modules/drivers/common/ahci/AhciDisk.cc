/*
 * Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC
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

#include "AhciDisk.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/utility.h"

#include "AhciController.h"

AhciDisk::AhciDisk(AhciController* controller, size_t port)
    : ScsiDisk(),
      m_Controller(controller),
      m_Port(port),
      m_Sectors(0),
      m_SectorBytes(0),
      m_Bytes(0),
      m_ExtendedFlush(false),
      m_Initialised(false),
      m_Model{} {
  m_pParent = controller;
  setSpecificType(String("ahci-disk"));
}

AhciDisk::~AhciDisk() {
  // Cache callbacks dispatch virtually into this object and need its controller.
  retireEndpoint();
  shutdownCache();
}

bool AhciDisk::initialise() {
  if (m_Initialised)
    return true;
  uint16_t words[256] = {};
  if (!m_Controller || !m_Controller->identify(m_Port, words))
    return false;

  const auto* raw = reinterpret_cast<const uint8_t*>(words);
  if (raw[510] == 0xa5) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < sizeof(words); ++i)
      checksum += raw[i];
    if (checksum) {
      WARNING("AHCI: IDENTIFY checksum failed on port " << m_Port);
      return false;
    }
  }
  for (size_t i = 0; i < 256; ++i)
    words[i] = LITTLE_TO_HOST16(words[i]);

  if ((words[0] & 0x8000) || (words[49] & 0x0300) != 0x0300 || (words[83] & 0xc000) != 0x4000 ||
      !(words[83] & (1U << 10))) {
    WARNING("AHCI: port " << m_Port << " requires an ATA disk with DMA and LBA48");
    return false;
  }

  uint64_t sectorBytes = 512;
  if ((words[106] & 0xc000) == 0x4000 && (words[106] & (1U << 12))) {
    const uint32_t sectorWords =
        static_cast<uint32_t>(words[117]) | (static_cast<uint32_t>(words[118]) << 16);
    if (sectorWords <= 256) {
      WARNING("AHCI: invalid logical sector size on port " << m_Port);
      return false;
    }
    sectorBytes = static_cast<uint64_t>(sectorWords) * 2;
  }
  if (sectorBytes < 512 || (sectorBytes & (sectorBytes - 1))) {
    WARNING("AHCI: port " << m_Port << " has unsupported logical sector size " << sectorBytes);
    return false;
  }
  const size_t pageBytes = TargetInfo::getPageSize();
  if (!pageBytes || pageBytes > 65536 || (pageBytes % sectorBytes))
    return false;

  const uint64_t sectors =
      static_cast<uint64_t>(words[100]) | (static_cast<uint64_t>(words[101]) << 16) |
      (static_cast<uint64_t>(words[102]) << 32) | (static_cast<uint64_t>(words[103]) << 48);
  if (!sectors || sectors > (uint64_t{1} << 48) || sectors > (~size_t{0} / sectorBytes)) {
    WARNING("AHCI: invalid or unaddressable LBA48 capacity on port " << m_Port);
    return false;
  }
  const bool supportsFlush = words[83] & (1U << 12);
  m_ExtendedFlush = words[83] & (1U << 13);
  if (!supportsFlush && !m_ExtendedFlush) {
    WARNING("AHCI: port " << m_Port << " lacks a supported cache flush command");
    return false;
  }

  for (size_t i = 0; i < 40; ++i) {
    const uint8_t ch = words[27 + i / 2] >> ((i & 1) ? 0 : 8);
    m_Model[i] = ch >= 32 && ch <= 126 ? static_cast<char>(ch) : ' ';
  }
  size_t length = 40;
  while (length && m_Model[length - 1] == ' ')
    --length;
  m_Model[length] = 0;

  const bool cacheSupported = words[82] & (1U << 5);
  const bool enabledValid = (words[87] & 0xc000) == 0x4000;
  const bool cacheEnabled = words[85] & (1U << 5);
  const char* cacheState = !cacheSupported ? "unsupported"
                           : !enabledValid ? "unknown"
                           : cacheEnabled  ? "enabled"
                                           : "disabled";

  m_SectorBytes = static_cast<size_t>(sectorBytes);
  m_Controller->configureDisk(
      m_Port, m_SectorBytes,
      words[76] != 0xffff && (words[76] & (1U << 8)) ? (words[75] & 31U) + 1 : 0);
  m_Sectors = static_cast<size_t>(sectors);
  m_Bytes = m_Sectors * sectorBytes;
  m_Initialised = true;
  NOTICE("AHCI: disk port " << m_Port << " model '" << m_Model << "', " << Dec << m_Sectors
                            << " sectors of " << m_SectorBytes << " bytes, " << m_Bytes
                            << " bytes; write cache " << cacheState << ", flush "
                            << (m_ExtendedFlush ? "EXT" : "legacy") << Hex);
  return true;
}

void AhciDisk::getName(String& name) {
  name.assign(m_Model[0] ? m_Model : "AHCI Disk");
}

size_t AhciDisk::getSize() const {
  return m_Bytes;
}

size_t AhciDisk::getBlockCount() const {
  return m_Sectors;
}

size_t AhciDisk::getBlockSize() const {
  return TargetInfo::getPageSize();
}

size_t AhciDisk::getNativeBlockSize() const {
  return m_SectorBytes;
}

size_t AhciDisk::validPageLength(uint64_t location) const {
  if (!m_Initialised || location >= m_Bytes || (location % m_SectorBytes))
    return 0;
  const uint64_t remaining = m_Bytes - location;
  const size_t pageBytes = TargetInfo::getPageSize();
  return remaining < pageBytes ? static_cast<size_t>(remaining) : pageBytes;
}

uint64_t AhciDisk::doRead(uint64_t location) {
  const size_t bytes = getCacheFillLength(location);
  if (!m_Initialised || !bytes)
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

  // Terminal sectors may occupy only part of a cache page.
  ByteSet(reinterpret_cast<void*>(page), 0, TargetInfo::getPageSize());
  if (!m_Controller->readWrite(m_Port, location / m_SectorBytes,
                               static_cast<uint16_t>(bytes / m_SectorBytes),
                               reinterpret_cast<void*>(page), bytes, false)) {
    if (!getCache().discardEditing(location))
      FATAL("AHCI: failed to discard an incomplete cache fill");
    return 0;
  }
  getCache().markNoLongerEditing(location);
  return bytes;
}

uint64_t AhciDisk::doWrite(uint64_t location) {
#if CRIPPLE_HDD
  return 0;
#else
  if (!validPageLength(location))
    return 0;
  const uintptr_t page = getCache().lookup(location);
  if (!page)
    return 0;
  // The controller separately releases the reference transferred by write().
  CachePageGuard guard(getCache(), location);
  return doWriteDirect(location, page);
#endif
}

uint64_t AhciDisk::doWriteDirect(uint64_t location, uintptr_t page) {
#if CRIPPLE_HDD
  return 0;
#else
  const size_t bytes = validPageLength(location);
  if (!page || !bytes)
    return 0;
  return m_Controller->readWrite(m_Port, location / m_SectorBytes,
                                 static_cast<uint16_t>(bytes / m_SectorBytes),
                                 reinterpret_cast<void*>(page), bytes, true)
             ? bytes
             : 0;
#endif
}

uint64_t AhciDisk::doSync(uint64_t location) {
#if CRIPPLE_HDD
  return 0;
#else
  const size_t bytes =
      location == SyncWholeDevice ? (m_Initialised ? 1 : 0) : validPageLength(location);
  if (!bytes)
    return 0;
  return m_Controller->flush(m_Port, m_ExtendedFlush) ? bytes : 0;
#endif
}
