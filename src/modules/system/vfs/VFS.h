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

#include "Filesystem.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/HashTable.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/LruCache.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/utility.h"

class Disk;
class File;
class StringView;
class HashedStringView;

/** Set to zero to disable the builtin VFS LRU caches. */
#define VFS_WITH_LRU_CACHES 0

/** This class implements a virtual file system.
 *
 * The pedigree VFS is structured in a similar way to windows' - every
 * filesystem is identified by a unique name and accessed thus:
 *
 * myfs:/mydir/myfile
 *
 * No UNIX-style mounting of filesystems inside filesystems is possible.
 * A filesystem may be referred to by multiple names - a reference count is
 * maintained by the filesystem - when no aliases point to it, it is unmounted
 * totally.
 *
 * The 'root' filesystem - that is the FS with system data on, is visible by the
 * alias 'root', thus; 'root:/System/Boot/kernel' could be used to access the
 * kernel image.
 */
class EXPORTED_PUBLIC VFS
{
  public:
    /** Callback type, called when a disk is mounted or unmounted. */
    typedef void (*MountCallback)();

    /** Type of the alias lookup table. */
    typedef HashTable<String, Filesystem *, HashedStringView> AliasTable;

    /** Constructor */
    VFS();
    /** Destructor */
    ~VFS();

    /** Returns the singleton VFS instance. */
    static VFS &instance();

    /** Mounts a Disk device as the alias "alias".
        If alias is zero-length, the Filesystem is asked for its preferred name
        (usually a volume name of some sort), and returned in "alias" */
    bool mount(Disk *pDisk, String &alias, Filesystem **pMountedFs = nullptr);

    /** Adds an alias to an existing filesystem.
     *\param pFs The filesystem to add an alias for.
     *\param pAlias The alias to add. */
    void addAlias(Filesystem *pFs, const String &alias);
    void addAlias(const String &oldAlias, const String &newAlias);

    /** Gets a unique alias for a filesystem. */
    String getUniqueAlias(const String &alias);

    /** Does a given alias exist? */
    bool aliasExists(const String &alias);

    /** Obtains a list of all filesystem aliases */
    inline AliasTable &getAliases()
    {
        return m_Aliases;
    }

    /** Obtains a list of all mounted filesystems */
    inline Tree<Filesystem *, List<String *> *> &getMounts()
    {
        return m_Mounts;
    }

    /** Removes an alias from a filesystem. If no aliases remain for that
     *filesystem, the filesystem is destroyed. \param pAlias The alias to
     *remove. */
    void removeAlias(const String &alias);

    /** Removes all aliases from a filesystem - the filesystem is destroyed.
     *\param pFs The filesystem to destroy. */
    void removeAllAliases(Filesystem *pFs, bool canDelete=true);

    /** Looks up the Filesystem from a given alias.
     *\param pAlias The alias to search for.
     *\return The filesystem aliased by pAlias or 0 if none found. */
    Filesystem *lookupFilesystem(const String &alias);
    Filesystem *lookupFilesystem(const HashedStringView &alias);

    /** Attempts to obtain a File for a specific path. */
    File *find(const String &path, File *pStartNode = 0);

    /** Attempts to create a file. */
    bool createFile(const String &path, uint32_t mask, File *pStartNode = 0);

    /** Attempts to create a directory. */
    bool
    createDirectory(const String &path, uint32_t mask, File *pStartNode = 0);

    /** Attempts to create a symlink. */
    bool createSymlink(
        const String &path, const String &value, File *pStartNode = 0);

    /** Attempts to create a hard link. */
    bool createLink(const String &path, File *target, File *pStartNode = 0);

    /** Attempts to remove a file/directory/symlink. WILL FAIL IF DIRECTORY NOT
     * EMPTY */
    bool remove(const String &path, File *pStartNode = 0);

    /** Adds a filesystem probe callback - this is called when a device is
     * mounted. */
    void addProbeCallback(Filesystem::ProbeCallback callback);

    /** Adds a mount callback - the function is called when a disk is mounted or
        unmounted. */
    void addMountCallback(MountCallback callback);

    /** Checks if the current user can access the given file. */
    static bool
    checkAccess(File *pFile, bool bRead, bool bWrite, bool bExecute);

    /** Separator between mount point and filesystem path. */
    static constexpr const char *mountSeparator()
    {
        return "»";
    }

  private:
    ssize_t findColon(const String &path);

    /** The static instance object. */
    static VFS m_Instance;

    /** A static File object representing an invalid file */
    static File *m_EmptyFile;

    AliasTable m_Aliases;
    Tree<Filesystem *, List<String *> *> m_Mounts;

    List<Filesystem::ProbeCallback *> m_ProbeCallbacks;
    List<MountCallback *> m_MountCallbacks;

    LruCache<String, Filesystem *> m_AliasCache;
    LruCache<String, File *> m_FindCache;
};

#endif
