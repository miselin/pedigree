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

#ifndef VFS_H
#define VFS_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/LruCache.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Filesystem.h"

class Disk;
class File;
class StringView;

/** Set to zero to disable the builtin VFS LRU caches. */
#define VFS_WITH_LRU_CACHES 0

/** This class implements a single-root virtual filesystem namespace. */
class EXPORTED_PUBLIC VFS {
 public:
  /** Callback type, called when a disk is mounted or unmounted. */
  typedef void (*MountCallback)();

  struct MountInfo {
    MountInfo(const String& stableName, const String& path) : stableName(stableName), path(path) {}

    String stableName;
    String path;
  };

  typedef Tree<Filesystem*, MountInfo*> MountTable;

  /** Constructor */
  VFS();
  /** Destructor */
  ~VFS();

  /** Returns the singleton VFS instance. */
  static VFS& instance();

  /** Probe and register a filesystem backed by a disk. */
  bool mount(Disk* pDisk, String& stableName, Filesystem** pMountedFs = nullptr);

  /** Register a filesystem for mounting under /media/<stable-name>. */
  String registerFilesystem(Filesystem* pFs, const String& preferredStableName);

  /** Remove a registered filesystem and optionally destroy it. */
  void unregisterFilesystem(Filesystem* pFs, bool canDelete = true);

  /** Select the filesystem that supplies the root namespace. */
  bool setRootFilesystem(Filesystem* pFs);

  Filesystem* getRootFilesystem() const {
    return m_pRootFilesystem;
  }

  /** Obtain the canonical mount path for a filesystem. */
  bool getMountPath(Filesystem* pFs, String& path) const;

  /** Find the filesystem mounted at an exact absolute path. */
  Filesystem* getFilesystemAt(const String& path) const;

  /** Obtains a list of all mounted filesystems */
  inline MountTable& getMounts() {
    return m_Mounts;
  }

  /** Attempts to obtain a File for a specific path. */
  File* find(const String& path, File* pStartNode = 0);

  /** Attempts to create a file. */
  bool createFile(const String& path, uint32_t mask, File* pStartNode = 0);

  /** Attempts to create a directory. */
  bool createDirectory(const String& path, uint32_t mask, File* pStartNode = 0);

  /** Attempts to create a symlink. */
  bool createSymlink(const String& path, const String& value, File* pStartNode = 0);

  /** Attempts to create a hard link. */
  bool createLink(const String& path, File* target, File* pStartNode = 0);

  /** Attempts to remove a file/directory/symlink. WILL FAIL IF DIRECTORY NOT
   * EMPTY */
  bool remove(const String& path, File* pStartNode = 0);

  /** Adds a filesystem probe callback - this is called when a device is
   * mounted. */
  void addProbeCallback(Filesystem::ProbeCallback callback);

  /** Removes a filesystem probe callback before its implementation unloads. */
  bool removeProbeCallback(Filesystem::ProbeCallback callback);

  /** Adds a mount callback - the function is called when a disk is mounted or
      unmounted. */
  void addMountCallback(MountCallback callback);

  /** Removes a mount callback before its implementation unloads. */
  bool removeMountCallback(MountCallback callback);

  /** Checks if the current user can access the given file. */
  static bool checkAccess(File* pFile, bool bRead, bool bWrite, bool bExecute);

  /** \brief Track a File object that exists.
   * It is necessary to keep track of File objects, or at least those that
   * are stored in Directory caches and Filesystem objects, such that they
   * can be correctly tidied up when no more usages exist.
   *
   * It is almost never safe to directly delete a File pointer so this helps
   * maintain that safety.
   */
  void trackFile(File* pFile);

  /**
   * Stop tracking a File object, destroying by default if it is no longer
   * tracked by any other owners.
   * \return true if the File object was deleted (or would have been,
   *        if destroy == false)
   */
  bool untrackFile(File* pFile, bool destroy = true);

 private:
  File* resolveStartNode(const String& path, File* pStartNode);
  String getUniqueStableName(const String& preferredName) const;
  bool attachFilesystem(Filesystem* pFs, const String& path);
  void attachRegisteredFilesystems();

  /** The static instance object. */
  static VFS m_Instance;

  /** A static File object representing an invalid file */
  static File* m_EmptyFile;

  Filesystem* m_pRootFilesystem;
  MountTable m_Mounts;

  List<Filesystem::ProbeCallback*> m_ProbeCallbacks;
  List<MountCallback*> m_MountCallbacks;

  LruCache<String, File*> m_FindCache;

  Tree<File*, size_t> m_TrackedFiles;
};

#endif
