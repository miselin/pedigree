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

#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/DiskPaging.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"

#include "Directory.h"
#include "Quota.h"

class Disk;
class File;
class StringView;

/** This class provides the abstract skeleton that all filesystems must
 * implement.
 *
 * Thanks to gr00ber at #osdev for the inspiration for the caching algorithms.
 */
class EXPORTED_PUBLIC Filesystem {
  friend class Directory;

  /** VFS can access nAliases */
  friend class VFS;
  friend class VfsMountView;

 public:
  enum class SyncStatus { Success, Unsupported, NoMemory, IoError };

  //
  // Public interface
  //

  /** Constructor - creates a blank object. */
  Filesystem();
  /** Destructor */
  virtual ~Filesystem();

  /** Populates this filesystem with data from the given Disk device.
   * \return true on success, false on failure. */
  virtual bool initialise(Disk* pDisk) = 0;

  /** Type of the probing callback given to the VFS.
      Probe function - if this filesystem is found on the given Disk
      device, create a new instance of it and return that. Else return 0. */
  typedef Filesystem* (*ProbeCallback)(Disk*);

  /** Attempt to find a file or directory in this filesystem.
      \param path The path to the file, in UTF-8 format, without filesystem
     identifier (e.g. not "root:/file", but "/file"). \param pStartNode The
     node to start parsing 'path' from - defaults to / but is expected to
     contain the current working directory. \return The file if one was found,
     or 0 otherwise or if there was an error.
  */
  virtual File* find(const StringView& path);
  virtual File* find(const String& path);
  virtual File* find(const StringView& path, File* pStartNode);
  virtual File* find(const String& path, File* pStartNode);

  /**
   * Find a node and retain its VFS lifetime for the lexical lifetime of
   * result. Filesystem-owned roots are returned without a lease.
   */
  File* findRetained(const StringView& path, Directory::ChildLease& result,
                     File* pStartNode = nullptr);

  /** Returns the root filesystem node. */
  virtual File* getRoot() const = 0;

  /** Returns a string identifying the volume label. */
  virtual const String& getVolumeLabel() const = 0;

  /** Returns the canonical filesystem UUID when this filesystem has one. */
  virtual bool getUuid(String& uuid) const {
    uuid.clear();
    return false;
  }

  /** Caller holds a VFS mount operation through handle access and publication. */
  virtual FileHandleStatus encodeFileHandle(File& file, FileHandle& handle);
  virtual FileHandleStatus decodeFileHandle(const FileHandle& handle, RetainedFile& file);
  virtual FileHandleStatus fileHandleFsid(FileSystemId& id);

  /** Completes data, metadata and device writeback for changes made before entry. */
  virtual SyncStatus sync();
  /** After all users drain, finish teardown writes before backend destruction. */
  virtual SyncStatus shutdown();
  virtual QuotaStatus quotaControl(const QuotaRequest&, QuotaResponse&, File* quotaFile = nullptr);

  /** Creates a file on the filesystem - fails if the file's parent directory
   * does not exist. */
  bool createFile(const StringView& path, uint32_t mask, File* pStartNode = 0);

  /** Creates a directory on the filesystem. Fails if the dir's parent
   * directory does not exist. */
  bool createDirectory(const StringView& path, uint32_t mask, File* pStartNode = 0);

  /** Creates a symlink on the filesystem, with the given value. */
  bool createSymlink(const StringView& path, const String& value, File* pStartNode = 0);

  /** Creates a hard link on the filesystem to the given target. */
  bool createLink(const StringView& path, File* target, File* pStartNode = 0);

  /** Removes a file, directory or symlink.
      \note Will fail if it is a directory and is not empty. The failure mode
      is unspecified. */
  bool remove(const StringView& path, File* pStartNode = 0);

  /** Remove a path only if its terminal entry still has the expected identity. */
  bool remove(const StringView& path, File* pStartNode, File* expected);

  /** Move one terminal namespace entry without following its symlink target. */
  bool rename(const StringView& oldPath, File* oldStart, const StringView& newPath, File* newStart,
              bool noReplace = false);

  /** Returns the disk in use */
  Disk* getDisk() {
    return m_pDisk;
  }

  /** Is the filesystem readonly? */
  bool isReadOnly() {
    return m_bReadOnly;
  }

  /** Does the filesystem care about case sensitivity? */
  virtual bool isCaseSensitive() {
    return true;
  }

  /** Remove the file only if it is still the given child of parent. */
  bool remove(File* parent, File* file);

 protected:
  /** createFile calls this after it has parsed the string path. */
  virtual bool createFile(File* parent, const String& filename, uint32_t mask) = 0;
  /** createDirectory calls this after it has parsed the string path. */
  virtual bool createDirectory(File* parent, const String& filename, uint32_t mask) = 0;
  /** createSymlink calls this after it has parsed the string path. */
  virtual bool createSymlink(File* parent, const String& filename, const String& value) = 0;
  /** createLink calls this after it has parsed the string path. */
  virtual bool createLink(File* parent, const String& filename, File* target);
  /**
   * Remove a backing node while the VFS holds the parent's namespace lock.
   * A successful implementation must retire the parent's cached entry before
   * returning and must recheck directory emptiness while holding the child's
   * namespace lock.
   */
  virtual bool removeNode(File* parent, const String& filename, File* file) = 0;
  /**
   * Change backing entries while the VFS reserves both names and owns their
   * namespace locks. Do not call ordinary lookup/add/remove APIs here. Failure
   * must leave both names unchanged; the VFS publishes cache changes on success.
   */
  virtual bool renameNode(Directory* oldParent, const String& oldName, File* source,
                          Directory* newParent, const String& newName, File* replaced);
  /** is this entire filesystem read-only?  */
  bool m_bReadOnly;
  /** Disk device(if any). */
  Disk* m_pDisk;
  DiskUse m_DiskUse;

 private:
  /** Serializes changes to directory ancestry against removal. */
  static Mutex m_StructureLock;
  /** Resolve and remove one child at a namespace-locked linearization point. */
  bool removeChild(File* parent, const String& filename, File* expected);
  bool renameChildren(File* oldParent, const String& oldName, File* newParent,
                      const String& newName, bool noReplace, bool sourceMustBeDirectory);

  /** Internal function to find a node - Returns 0 on failure or the node.
      \param pNode The node to start parsing 'path' from.
      \param path  The path from pNode to the destination node. */
  File* findNode(File* pNode, StringView path);

  /** Traverse while retaining the terminal tracked node for a mutation. */
  File* findNode(File* pNode, StringView path, File* stableStart, File* trueRoot,
                 File** retainedResult);

  /** Internal function to find a node's parent directory.
      \param path The path from pStartNode to the original file.
      \param pStartNode The node to start parsing 'path' from.
      \param[out] filename The child file's name. */
  File* findParent(StringView path, File* pStartNode, String& filename,
                   File** retainedParent = nullptr);

  /** Copy constructor.
      \note NOT implemented. */
  Filesystem(const Filesystem&);
  /** Assignment operator.
      \note NOT implemented. */
  void operator=(const Filesystem&);
};

#endif
