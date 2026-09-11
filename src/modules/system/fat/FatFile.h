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

#ifndef FAT_FILE_H
#define FAT_FILE_H

#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/String.h"

#include "modules/system/vfs/File.h"

/** A File is a file, a directory or a symlink. */
class FatFile : public File {
  friend class FatFilesystem;
  friend class FatWritebackTestPeer;

 private:
  /** Copy constructors are hidden - unused! */
  FatFile(const File& file);
  File& operator=(const File&);

 public:
  /** Constructor, should be called only by a Filesystem. */
  FatFile(String name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
          Time::Timestamp creationTime, uintptr_t inode, class Filesystem* pFs, size_t size,
          uint32_t dirClus = 0, uint32_t dirOffset = 0, File* pParent = 0);
  /** Drains checked page writeback before destroying FAT metadata. */
  ~FatFile() override;

  uint32_t getDirCluster() {
    return m_DirClus;
  }
  void setDirCluster(uint32_t custom) {
    m_DirClus = custom;
  }
  uint32_t getDirOffset() {
    return m_DirOffset;
  }
  void setDirOffset(uint32_t custom) {
    m_DirOffset = custom;
  }

  uintptr_t readBlock(uint64_t location) override;
  void writeBlock(uint64_t location, uintptr_t addr) override;

  void extend(size_t newSize) override;
  void extend(size_t newSize, uint64_t location, uint64_t size) override;

  Attributes getAttributes() const override;
  bool sync() override;
  bool sync(size_t offset, bool async) override;

  bool pinBlock(uint64_t location) override;
  void unpinBlock(uint64_t location) override;

 protected:
  bool useFillCache() const override;
  bool readPage(uint64_t location, uintptr_t destination) override;
  bool prepareShrink(const ShrinkContext& context,
                     UniquePointer<PreparedShrink>& prepared) override;
  bool resizeFile(size_t size) override;

 private:
  static bool checkedWriteCallback(CacheConstants::CallbackCause cause, uintptr_t location,
                                   uintptr_t page, void* meta);

  uint32_t m_DirClus;
  uint32_t m_DirOffset;
  bool m_MetadataDirty;
};

#endif
