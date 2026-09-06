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

#ifndef EXT2_DIRECTORY_H
#define EXT2_DIRECTORY_H

#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"

#include "Ext2Node.h"
#include "modules/system/vfs/Directory.h"

class File;
struct Inode;
struct Dir;

/** A File is a file, a directory or a symlink. */
class Ext2Directory : public Directory, public Ext2Node {
  friend class Ext2Filesystem;

 private:
  struct RenameRecord {
    RenameRecord();
    ~RenameRecord();
    Ext2Directory* owner;
    uint32_t block;
    uintptr_t buffer;
    size_t offset;
    Dir* entry;
    Dir* split;
    uint16_t splitLength;
    uint16_t length;
  };
  bool prepareRenameRecord(const String& name, uint32_t inode, RenameRecord& record);
  bool prepareRenameSpace(const String& name, RenameRecord& record, const RenameRecord* source);
  /** Copy constructors are hidden - unused! */
  Ext2Directory(const Ext2Directory& file);
  Ext2Directory& operator=(const Ext2Directory&);

 public:
  /** Constructor, should be called only by a Filesystem. */
  Ext2Directory(const String& name, uintptr_t inode_num, Inode* inode, class Ext2Filesystem* pFs,
                File* pParent);
  /** Destructor */
  ~Ext2Directory() override;

  void truncate() override {}

  using Directory::sync;
  bool sync() override;

  /** Adds a directory entry. */
  virtual bool addEntry(const String& filename, File* pFile, size_t type);
  /** Removes a directory entry. */
  virtual bool removeEntry(const String& filename, Ext2Node* pFile);

  /** Updates inode attributes. */
  void fileAttributeChanged() override;
  Attributes getAttributes() const override;

  XattrStatus getExtendedAttribute(const StringView&, void*, size_t, size_t&) override;
  XattrStatus listExtendedAttributes(void*, size_t, size_t&) override;
  XattrStatus setExtendedAttribute(const StringView&, const void*, size_t, unsigned) override;
  XattrStatus removeExtendedAttribute(const StringView&) override;

 protected:
  void updateAttributes(const Attributes& attributes, uint32_t mask) override;
  bool changeOwnership(size_t uid, size_t gid, bool changeUid, bool changeGid) override;

 private:
  struct ParsedEntry {
    uint32_t inode;
    uint16_t recordLength;
    uint16_t nameLength;
    uint8_t fileType;
    char name[256];
  };

  LookupStatus resolveChild(const StringView& name, File*& child) override;
  LookupStatus resolveChildAt(uint64_t cookie, const StringView& name, File*& child) override;
  ReadStatus readDirectory(uint64_t& cookie, DirectoryEntryEmitter emitter, void* context) override;

  bool readBytes(uint64_t offset, size_t length, void* buffer);
  ReadStatus readEntry(uint64_t offset, ParsedEntry& entry);
  LookupStatus resolveEntry(const ParsedEntry& entry, const StringView& name, File*& child);
  LookupStatus resolveChildLocked(const StringView& name, File*& child);
  bool removeEntryLocked(const String& filename, Ext2Node* pFile);
  bool removeFromParent(Ext2Directory* parent, const String& filename);
  bool syncDirectoryBlock(uint32_t block);
  void queueSyncDependency(uint32_t block);

  Mutex m_DirectoryLock;
  bool m_Removed;
};

#endif
