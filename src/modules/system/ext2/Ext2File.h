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

#ifndef EXT2_FILE_H
#define EXT2_FILE_H

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"

#include "Ext2Node.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/QuotaTable.h"

struct Inode;

/** A File is a file, a directory or a symlink. */
class Ext2File : public File, public Ext2Node {
  friend class Ext2Filesystem;

 private:
  /** Copy constructors are hidden - unused! */
  Ext2File(const Ext2File& file);
  Ext2File& operator=(const Ext2File&);

 public:
  /** Constructor, should be called only by a Filesystem. */
  Ext2File(const String& name, uintptr_t inode_num, Inode* inode, class Ext2Filesystem* pFs,
           File* pParent = 0);
  /** Destructor */
  virtual ~Ext2File();
  bool valid() const {
    return m_Initialized;
  }
  virtual FileHandleStatus subscribeInodeEvents(FileEventMask,
                                                const SharedPointer<FileEventObserver>&,
                                                FileEventSubscription&);
  virtual void finishInodeRetirement();

  virtual void preallocate(size_t expectedSize, bool zero = true);

  virtual void extend(size_t newSize);
  virtual void extend(size_t newSize, uint64_t location, uint64_t size);

  virtual void truncate();
  virtual size_t getSize();
  virtual uint64_t maximumFileSize() const;
  virtual uintptr_t futexIdentity();
  virtual bool tryBeginMappingRelease();
  virtual Attributes getAttributes() const;
  virtual bool prepareSharedMapping(size_t offset, size_t length);

  /** Updates inode attributes. */
  void fileAttributeChanged();

  virtual uintptr_t readBlock(uint64_t location);
  virtual void writeBlock(uint64_t location, uintptr_t addr);

  virtual bool pinBlock(uint64_t location);
  virtual void unpinBlock(uint64_t location);

  virtual bool sync();
  virtual bool sync(size_t offset, bool async);
  bool syncPages(const uint64_t* offsets, size_t count) override;

  virtual size_t getBlockSize() const;

  virtual XattrStatus getExtendedAttribute(const StringView&, void*, size_t, size_t&);
  virtual XattrStatus listExtendedAttributes(void*, size_t, size_t&);
  virtual XattrStatus setExtendedAttribute(const StringView&, const void*, size_t, unsigned);
  virtual XattrStatus removeExtendedAttribute(const StringView&);

 protected:
  virtual void publishInodeEvent(const FileEvent&);
  virtual CacheState& cacheState();
  virtual bool useFillCache() const;
  virtual void updateAttributes(const Attributes& attributes, uint32_t mask);
  virtual bool changeOwnership(size_t uid, size_t gid, bool changeUid, bool changeGid);
  virtual bool allowResize(size_t oldSize, size_t newSize);
  virtual bool allowPhysicalPage() const;
  virtual bool prepareWrite(uint64_t location, uint64_t size);
  virtual bool resizeFile(size_t size);
  virtual bool allocateFileRange(size_t offset, size_t length);
  virtual bool prepareShrink(const ShrinkContext& context, UniquePointer<PreparedShrink>& prepared);
  virtual Mutex& writeSerializationLock();
  virtual Mutex& dataMutationLock();
  virtual size_t& physicalPageLoans();
  virtual void writeBlocks(uint64_t location, uintptr_t addr, size_t length);

 private:
  QuotaStatus beginQuota(QuotaTable& loaded);
  QuotaStatus endQuota(QuotaType type, bool requireClean);
  QuotaStatus writeQuotaRecord(uint32_t id, const QuotaRecord& record);
  bool m_OwnsQuotaProtection = false;
  bool m_Initialized = false;
  static bool sharedFillCallback(CacheConstants::CallbackCause cause, uintptr_t location,
                                 uintptr_t page, void* state);
  static bool writeBlocksLocked(Ext2InodeState* state, uint64_t location, uintptr_t address,
                                size_t length, bool async);
};

#endif
