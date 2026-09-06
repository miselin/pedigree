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

#ifndef EXT2_NODE_H
#define EXT2_NODE_H

#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "modules/system/vfs/File.h"

struct Inode;
class Ext2File;
class Ext2Filesystem;

struct Ext2InodeState {
  Ext2InodeState(Inode* inode, Ext2Filesystem* filesystem);
  ~Ext2InodeState();
  void reloadMappings(Inode* inode, Ext2Filesystem* filesystem);
  void loadMappings(Ext2Filesystem* filesystem, uint32_t block, unsigned depth, size_t first,
                    size_t span);
  Vector<uint32_t> blocks;
  uint32_t metadataBlocks;
  uint32_t allocatedDataBlocks;
  size_t size;
  bool allocationValid = true;
  Mutex dataLock;
  Mutex writeLock;
  Mutex writebackLock;
  Vector<uint32_t> namespaceSyncBlocks;
  size_t pageLoans;
  size_t references;
  size_t syncReferences = 0;
  bool orphan;
  InodeEventSource inodeEvents;
  uintptr_t futexIdentity;
  Vector<Ext2File*> files;
  File::CacheState* cache = nullptr;
  Inode* metadata;
  Ext2Filesystem* filesystem;
};

/** A node in an ext2 filesystem. */
class Ext2Node {
  friend class Ext2Filesystem;

 private:
  /** Copy constructors are hidden - unused! */
  Ext2Node(uintptr_t inode, Inode* metadata, Ext2Filesystem* filesystem, Ext2InodeState& admitted);
  Ext2Node(const Ext2Node& file);
  Ext2Node& operator=(const Ext2Node&);

 public:
  /** Constructor, should be called only by a Filesystem. */
  Ext2Node(uintptr_t inode_num, Inode* pInode, class Ext2Filesystem* pFs);
  /** Destructor */
  virtual ~Ext2Node();

  Inode* getInode() {
    return m_pInode;
  }

  uint32_t getInodeNumber() {
    return m_InodeNumber;
  }

  /** Updates inode attributes. */
  void fileAttributeChanged(size_t size, size_t atime, size_t mtime, size_t ctime);

  File::Attributes inodeAttributes() const;
  void updateInodeAttributes(const File::Attributes& attributes, uint32_t mask);

  /** Wipes the node of data - frees all blocks. */
  bool wipe(bool allocationLockHeld = false);

  void extend(size_t newSize);
  void extend(size_t newSize, uint64_t location, uint64_t size);

  uint64_t maximumFileSize() const;

  static bool decodeAllocation(const Inode&, uint32_t blockSize, uint32_t& blocks,
                               bool& inlineSymlink);
  static bool encodeAllocation(uint32_t data, uint32_t indirect, bool hasEa, uint32_t blockSize,
                               uint32_t& sectors);
  bool isInlineSymlink() const;
  void updateAllocatedSectorCount();
  XattrStatus getXattr(const StringView&, void*, size_t, size_t&);
  XattrStatus listXattrs(void*, size_t, size_t&);
  XattrStatus setXattr(const StringView&, const void*, size_t, unsigned);
  XattrStatus removeXattr(const StringView&);

  uintptr_t readBlock(uint64_t location);
  void writeBlock(uint64_t location);

  void trackBlock(uint32_t block, bool writeInode = true);

  bool pinBlock(uint64_t location);
  void unpinBlock(uint64_t location);

  bool sync(size_t offset, bool async);

 protected:
  XattrStatus changeXattr(const StringView&, const void*, size_t, unsigned, bool remove);
  bool resizeData(size_t size);
  bool trimToBlocks(size_t keep, bool allocationLockHeld = false);
  bool zeroRange(size_t start, size_t end);
  struct MappingPage {
    uint32_t block;
    uintptr_t buffer;
    size_t first;
    size_t span;
    unsigned depth;
  };
  bool collectMappingPages(uint32_t block, unsigned depth, size_t first, size_t span,
                           Vector<MappingPage>& pages);
  struct TrimPlan {
    explicit TrimPlan(Ext2Filesystem& filesystem);
    ~TrimPlan();
    Ext2Filesystem& filesystem;
    size_t keep = 0;
    size_t retainedData = 0;
    Vector<MappingPage> pages;
    Vector<uint32_t> retiredData;
  };
  class DataShrinkPlan;
  bool prepareTrim(size_t keep, TrimPlan& plan, bool allocationLockHeld = false);
  void commitTrim(TrimPlan& plan, bool allocationLockHeld = false);
  bool prepareDataShrink(size_t size, UniquePointer<File::PreparedShrink>& prepared);

  /**
   * Ensures the inode is at least 'size' big.
   * Set onlyBlocks to true to not change the actual data size, which can be
   * useful for preallocation.
   */
  bool ensureLargeEnough(size_t size, uint64_t location, uint64_t opsize, bool onlyBlocks = false,
                         bool nozeroblocks = false);

  bool addBlock(uint32_t blockValue, Vector<uint32_t>* pendingWrites = 0);

  void writeBlockOrQueue(uint32_t block, Vector<uint32_t>* pendingWrites);

  bool ensureBlockLoaded(size_t nBlock);
  bool getBlockNumber(size_t nBlock);
  bool getBlockNumberIndirect(uint32_t inode_block, size_t nBlocks, size_t nBlock);
  bool getBlockNumberBiindirect(uint32_t inode_block, size_t nBlocks, size_t nBlock);
  bool getBlockNumberTriindirect(uint32_t inode_block, size_t nBlocks, size_t nBlock);

  bool setBlockNumber(size_t blockNum, uint32_t blockValue,
                      Vector<uint32_t>* pendingWrites = nullptr);
  bool ensureWritableRange(size_t location, size_t length);

  uint32_t modeToPermissions(uint32_t mode) const;
  uint32_t permissionsToMode(uint32_t permissions) const;

  Ext2InodeState* m_State;
  Inode* m_pInode;
  uint32_t m_InodeNumber;
  class Ext2Filesystem* m_pExt2Fs;

  Vector<uint32_t>& m_Blocks;
  uint32_t& m_nMetadataBlocks;

  size_t& m_nSize;
};

#endif
