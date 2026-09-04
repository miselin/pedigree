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

#ifndef ISO9660DIRECTORY_H
#define ISO9660DIRECTORY_H

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Iso9660File.h"
#include "Iso9660Filesystem.h"
#include "iso9660.h"
#include "modules/system/vfs/Directory.h"

class File;

class Iso9660Directory : public Directory {
  friend class Iso9660File;

 private:
  Iso9660Directory(const Iso9660Directory&);
  Iso9660Directory& operator=(const Iso9660Directory&);

 public:
  Iso9660Directory(String name, size_t inode, class Iso9660Filesystem* pFs, File* pParent,
                   Iso9660DirRecord& dirRec, Time::Timestamp accessedTime = 0,
                   Time::Timestamp modifiedTime = 0, Time::Timestamp creationTime = 0)
      : Directory(name, accessedTime, modifiedTime, creationTime, inode, pFs, 0, pParent),
        m_pFs(pFs),
        m_Dir(dirRec) {}
  virtual ~Iso9660Directory();

  virtual bool addEntry(String filename, File* pFile, size_t type) {
    return false;
  }

  virtual bool removeEntry(File* pFile) {
    return false;
  }

  void fileAttributeChanged() {}

  inline Iso9660DirRecord& getDirRecord() {
    return m_Dir;
  }

 protected:
  LookupStatus resolveChild(const StringView& name, File*& child) override;
  LookupStatus resolveChildAt(uint64_t cookie, const StringView& name, File*& child) override;
  ReadStatus readDirectory(uint64_t& cookie, DirectoryEntryEmitter emitter, void* context) override;

 private:
  struct ScannedEntry {
    const String* name;
    Iso9660DirRecord* record;
    uint64_t currentCookie;
    uint64_t nextCookie;
  };

  using ScannedEntryEmitter = bool (*)(void*, const ScannedEntry&);

  struct ResolveContext {
    Iso9660Directory* directory;
    StringView name;
    File* child;
    uint64_t expectedCookie;
    bool checkCookie;
  };

  struct ReadContext {
    DirectoryEntryEmitter emitter;
    void* context;
  };

  ReadStatus scanDirectory(uint64_t& cookie, ScannedEntryEmitter emitter, void* context);
  static bool resolveEntry(void* context, const ScannedEntry& entry);
  static bool emitEntry(void* context, const ScannedEntry& entry);

  // Filesystem object
  Iso9660Filesystem* m_pFs;

  // Our internal directory information (info about *this* directory, not the
  // child)
  Iso9660DirRecord m_Dir;
};

#endif
