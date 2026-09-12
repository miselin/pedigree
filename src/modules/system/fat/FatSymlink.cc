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

#include "FatSymlink.h"
#include "pedigree/kernel/LockGuard.h"

#include "FatFilesystem.h"
#include "modules/system/vfs/File.h"

FatSymlink::FatSymlink(String name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
                       Time::Timestamp creationTime, uintptr_t inode, class Filesystem* pFs,
                       size_t size, uint32_t dirClus, uint32_t dirOffset, File* pParent)
    : Symlink(name, accessedTime, modifiedTime, creationTime, inode, pFs, size, pParent),
      m_DirClus(dirClus),
      m_DirOffset(dirOffset) {
  // No permissions on FAT - set all to RWX.
  setPermissionsOnly(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR | FILE_OW |
                     FILE_OX);
  static_cast<FatFilesystem*>(pFs)->registerNode(this);
}

uint64_t FatSymlink::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                  bool bCanBlock) {
  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);
  return pFs->read(this, location, size, buffer);
}

File::Attributes FatSymlink::getAttributes() const {
  FatSymlink* file = const_cast<FatSymlink*>(this);
  LockGuard<Mutex> dataGuard(file->dataMutationLock());
  FatFilesystem* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  LockGuard<Mutex> guard(filesystem->m_FileMutationLock);
  Attributes attributes = File::getAttributes();
  attributes.blocks = filesystem->allocatedBlocks(file);
  return attributes;
}

uint64_t FatSymlink::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                   bool bCanBlock) {
  LockGuard<Mutex> guard(m_TargetLock);
  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);
  uint64_t ret = pFs->write(this, location, size, buffer);

  // Reset the symlink target.
  initialise(true);

  return ret;
}

FatSymlink::~FatSymlink() {
  static_cast<FatFilesystem*>(m_pFilesystem)->releaseNode(this);
}
