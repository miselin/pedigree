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

#include "Iso9660File.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

bool Iso9660File::readPage(uint64_t location, uintptr_t destination) {
  const size_t pageSize = TargetInfo::getPageSize();
  if (!destination || location % pageSize || !m_pFs || !m_pFs->m_pDisk)
    return false;
  const size_t size = getSize();
  if (location >= size) {
    ByteSet(reinterpret_cast<void*>(destination), 0, pageSize);
    return true;
  }
  const size_t remaining = size - location;
  const size_t amount = remaining < pageSize ? remaining : pageSize;
  const uint64_t extent = static_cast<uint64_t>(LITTLE_TO_HOST32(m_Dir.ExtentLocation_LE)) * 2048;
  if (location > ~uint64_t(0) - extent ||
      !m_pFs->m_pDisk->readInto(extent + location, reinterpret_cast<void*>(destination), amount))
    return false;
  ByteSet(reinterpret_cast<void*>(destination + amount), 0, pageSize - amount);
  return true;
}

bool Iso9660File::prepareWrite(uint64_t, uint64_t) {
  SYSCALL_ERROR(ReadOnlyFilesystem);
  return false;
}

bool Iso9660File::prepareSharedMapping(size_t, size_t) {
  SYSCALL_ERROR(ReadOnlyFilesystem);
  return false;
}

uintptr_t Iso9660File::readBlock(uint64_t location) {
  return reinterpret_cast<Iso9660Filesystem*>(m_pFilesystem)->readBlock(this, location);
}

bool Iso9660File::pinBlock(uint64_t location) {
  const uint64_t diskLocation =
      (static_cast<uint64_t>(LITTLE_TO_HOST32(m_Dir.ExtentLocation_LE)) * 2048) + location;
  return m_pFs->m_pDisk->pin(diskLocation);
}

void Iso9660File::unpinBlock(uint64_t location) {
  const uint64_t diskLocation =
      (static_cast<uint64_t>(LITTLE_TO_HOST32(m_Dir.ExtentLocation_LE)) * 2048) + location;
  m_pFs->m_pDisk->unpin(diskLocation);
}
