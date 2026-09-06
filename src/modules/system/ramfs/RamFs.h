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

#ifndef RAMFS_H
#define RAMFS_H

/**\file  RamFs.h
 *\author Matthew Iselin
 *\date   Sun May 17 10:00:00 2009
 *\brief  An in-RAM filesystem. */

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/String.h"

#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/Filesystem.h"
#include "modules/system/vfs/MemoryExtendedAttributes.h"

class Disk;

class EXPORTED_PUBLIC RamFile : public File {
 public:
  RamFile(const String& name, uintptr_t inode, Filesystem* pParentFS, File* pParent);

  virtual ~RamFile();

  virtual Attributes getAttributes() const;

  XattrStatus getExtendedAttribute(const StringView& name, void* buffer, size_t capacity,
                                   size_t& required) override;
  XattrStatus listExtendedAttributes(void* buffer, size_t capacity, size_t& required) override;
  XattrStatus setExtendedAttribute(const StringView& name, const void* value, size_t length,
                                   unsigned flags) override;
  XattrStatus removeExtendedAttribute(const StringView& name) override;

  virtual void truncate();

  bool canWrite();

 protected:
  virtual bool resizeFile(size_t size);
  virtual bool allocateFileRange(size_t offset, size_t length);
  virtual bool prepareShrink(const ShrinkContext& context, UniquePointer<PreparedShrink>& prepared);
  virtual uintptr_t readBlock(uint64_t location);

  virtual bool pinBlock(uint64_t location);

  virtual void unpinBlock(uint64_t location);

 private:
  class ShrinkPlan;
  Cache m_FileBlocks;
  mutable Mutex m_FileBlocksLock;
  Vector<uint64_t> m_BlockOffsets;

  size_t m_nOwnerPid;
  MemoryExtendedAttributes m_ExtendedAttributes;
};

/** Defines a directory in the RamFS */
class EXPORTED_PUBLIC RamDir : public Directory {
 private:
  RamDir(const RamDir&);
  RamDir& operator=(const RamDir&);

 public:
  RamDir(const String& name, size_t inode, class Filesystem* pFs, File* pParent);
  virtual ~RamDir();

  XattrStatus getExtendedAttribute(const StringView& name, void* buffer, size_t capacity,
                                   size_t& required) override;
  XattrStatus listExtendedAttributes(void* buffer, size_t capacity, size_t& required) override;
  XattrStatus setExtendedAttribute(const StringView& name, const void* value, size_t length,
                                   unsigned flags) override;
  XattrStatus removeExtendedAttribute(const StringView& name) override;

  virtual void cacheDirectoryContents() {}

  virtual bool addEntry(String filename, File* pFile);

  virtual bool removeEntry(const String& filename, File* pFile);

  bool removeFromParent(RamDir* parent, const String& filename);

 private:
  Mutex m_DirectoryLock;
  MemoryExtendedAttributes m_ExtendedAttributes;
};

/** Defines a filesystem that is completely in RAM. */
class EXPORTED_PUBLIC RamFs : public Filesystem {
 public:
  RamFs();
  virtual ~RamFs();

  virtual bool initialise(Disk* pDisk);

  SyncStatus sync() override;

  void setProcessOwnership(bool bEnable) {
    m_bProcessOwners = bEnable;
  }

  bool getProcessOwnership() const {
    return m_bProcessOwners;
  }

  virtual File* getRoot() const {
    return m_pRoot;
  }
  virtual const String& getVolumeLabel() const {
    return m_VolumeLabel;
  }

 protected:
  virtual bool createFile(File* parent, const String& filename, uint32_t mask);
  virtual bool createDirectory(File* parent, const String& filename, uint32_t mask);
  virtual bool createSymlink(File* parent, const String& filename, const String& value);
  virtual bool removeNode(File* parent, const String& filename, File* file);
  virtual bool renameNode(Directory* oldParent, const String& oldName, File* source,
                          Directory* newParent, const String& newName, File* replaced);

  RamFs(const RamFs&);
  void operator=(const RamFs&);

  static String m_VolumeLabel;

  /** Root filesystem node. */
  File* m_pRoot;

  bool m_bProcessOwners;
};

#endif
