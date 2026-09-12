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
#include "pedigree/kernel/utilities/Vector.h"

#include "modules/system/vfs/File.h"

class FatFilesystem;

/** A File is a file, a directory or a symlink. */
class FatFile : public File {
  friend class FatFilesystem;
  friend class FatWritebackTestPeer;

 private:
  /** Copy constructors are hidden - unused! */
  FatFile(const File& file);
  File& operator=(const File&);

 public:
  struct State {
    explicit State(FatFilesystem* owner);
    ~State();
    FatFilesystem* filesystem;
    CacheState cache;
    Mutex dataLock;
    Mutex writeLock;
    size_t pageLoans = 0;
    Time::Timestamp accessed = 0;
    Time::Timestamp modified = 0;
    Time::Timestamp changed = 0;
    size_t size = 0;
    uintptr_t inode = 0;
    uintptr_t identifier = 0;
    uint32_t directoryCluster = 0;
    uint32_t directoryOffset = 0;
    bool metadataDirty = false;
    bool trimPending = false;
    bool unlinked = false;
    bool registered = false;
    bool retiring = false;
    size_t syncReferences = 0;
    FatFile* aliases = nullptr;
    State* next = nullptr;
    Vector<uint32_t> retiredClusters;
    Vector<uint32_t> clusters;
    uint64_t chainRevision = 0;
  };

  void truncate() override;
  size_t getSize() override;
  void setInode(uintptr_t inode) override;
  uint64_t maximumFileSize() const override;
  uintptr_t futexIdentity() override;
  void fileAttributeChanged() override;
  bool syncPages(const uint64_t* offsets, size_t count) override;

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
  CacheState& cacheState() override;
  Mutex& dataMutationLock() override;
  Mutex& writeSerializationLock() override;
  size_t& physicalPageLoans() override;
  bool useFillCache() const override;
  bool readPage(uint64_t location, uintptr_t destination) override;
  bool prepareShrink(const ShrinkContext& context,
                     UniquePointer<PreparedShrink>& prepared) override;
  bool resizeFile(size_t size) override;

 private:
  void copyStateAttributes();
  explicit FatFile(State& state);
  static bool checkedBatchCallback(const Cache::WritebackPage*, size_t, void*);
  static bool checkedWriteCallback(CacheConstants::CallbackCause cause, uintptr_t location,
                                   uintptr_t page, void* meta);

  State* m_State;
  FatFile* m_NextAlias = nullptr;
  bool m_Proxy = false;
  uint32_t& m_DirClus;
  uint32_t& m_DirOffset;
  bool& m_MetadataDirty;
  bool& m_TrimPending;
  Vector<uint32_t>& m_RetiredClusters;
};

#endif
