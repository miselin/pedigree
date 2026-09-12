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

#include "RawFsDir.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/types.h"

#include "RawFs.h"
#include "RawFsFile.h"
#include "modules/system/vfs/File.h"

class Filesystem;

RawFsDir::RawFsDir(String name, RawFs* pFs, File* pParent)
    : Directory(name, 0 /* Accessed time */, 0 /* Modified time */, 0 /* Creation time */,
                0 /* Inode number */, static_cast<Filesystem*>(pFs), 0 /* Size */, pParent) {
  // RW for root, readable only by others.
  uint32_t permissions = FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GX | FILE_OR | FILE_OX;
  setPermissions(permissions);
}

void RawFsDir::addEntry(File* pEntry) {
  addDirectoryEntry(pEntry->getName(), pEntry);
}

void RawFsDir::removeRecursive() {
  /// \todo Leaky.
  NOTICE("rawfs: removing '" << getName() << "'");
  /// \todo do this
}

bool RawFsDir::syncFiles(bool terminal) {
  struct Context {
    RawFsDir* directory;
    bool terminal;
    bool succeeded;
  } context = {this, terminal, true};
  auto syncChild = [](void* opaque, const DirectoryEntryView& entry) -> bool {
    auto* context = static_cast<Context*>(opaque);
    ChildLease child;
    if (context->directory->lookupChild(HashedStringView(entry.name), child) !=
        LookupStatus::Found) {
      context->succeeded = false;
      return true;
    }
    const bool synced = child.get()->isDirectory()
                            ? static_cast<RawFsDir*>(child.get())->syncFiles(context->terminal)
                        : context->terminal ? static_cast<RawFsFile*>(child.get())->shutdown()
                                            : child.get()->sync();
    context->succeeded = synced && context->succeeded;
    return true;
  };
  uint64_t cookie = 2;  // Skip the synthetic self and parent entries.
  const ReadStatus result = enumerate(cookie, syncChild, &context);
  return result == ReadStatus::Complete && context.succeeded;
}
