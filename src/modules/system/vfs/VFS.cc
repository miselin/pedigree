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

#include "VFS.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/syscallError.h"
#include <users/UserManager.h>
#include "pedigree/kernel/utilities/utility.h"

#ifndef VFS_STANDALONE
#include <Module.h>
#include "pedigree/kernel/processor/Processor.h"
#endif

#include <ramfs/RamFs.h>

/// \todo Figure out a way to clean up files after deletion. Directory::remove()
///       is not the right place to do this. There needs to be a way to add a
///       File to some sort of queue that cleans it up once it hits refcount
///       zero or something like that.

VFS VFS::m_Instance;

VFS &VFS::instance()
{
    return m_Instance;
}

VFS::VFS() : m_Aliases(), m_ProbeCallbacks(), m_MountCallbacks()
{
}

VFS::~VFS()
{
    // Wipe out probe callbacks we know about.
    for (auto it = m_ProbeCallbacks.begin(); it != m_ProbeCallbacks.end(); ++it)
    {
        delete *it;
    }

    // Unmount aliases.
    for (auto it = m_Mounts.begin(); it != m_Mounts.end(); ++it)
    {
        auto fs = it.key();
        auto aliases = it.value();
        for (auto it2 = aliases->begin(); it2 != aliases->end(); ++it2)
        {
            delete *it2;
        }

        delete aliases;
        delete fs;
    }
}

bool VFS::mount(Disk *pDisk, String &alias)
{
    for (List<Filesystem::ProbeCallback *>::Iterator it =
             m_ProbeCallbacks.begin();
         it != m_ProbeCallbacks.end(); it++)
    {
        Filesystem::ProbeCallback cb = **it;
        Filesystem *pFs = cb(pDisk);
        if (pFs)
        {
            if (alias.length() == 0)
            {
                alias = pFs->getVolumeLabel();
            }
            alias = getUniqueAlias(alias);
            addAlias(pFs, alias);

            if (m_Mounts.lookup(pFs) == 0)
                m_Mounts.insert(pFs, new List<String *>);

            m_Mounts.lookup(pFs)->pushBack(new String(alias));

            for (List<MountCallback *>::Iterator it2 = m_MountCallbacks.begin();
                 it2 != m_MountCallbacks.end(); it2++)
            {
                MountCallback mc = *(*it2);
                mc();
            }

            return true;
        }
    }
    return false;
}

void VFS::addAlias(Filesystem *pFs, const String &alias)
{
    if (!pFs)
        return;

    pFs->m_nAliases++;
    m_Aliases.insert(alias, pFs);

    if (m_Mounts.lookup(pFs) == 0)
        m_Mounts.insert(pFs, new List<String *>);

    m_Mounts.lookup(pFs)->pushBack(new String(alias));
}

void VFS::addAlias(const String &oldAlias, const String &newAlias)
{
    Filesystem *pFs = m_Aliases.lookup(oldAlias);
    if (pFs)
    {
        m_Aliases.insert(newAlias, pFs);

        if (m_Mounts.lookup(pFs) == 0)
            m_Mounts.insert(pFs, new List<String *>);

        m_Mounts.lookup(pFs)->pushBack(new String(newAlias));
    }
}

String VFS::getUniqueAlias(const String &alias)
{
    if (!aliasExists(alias))
        return alias;

    // <alias>-n is how we keep them unique
    // negative numbers already have a dash
    int32_t index = -1;
    while (true)
    {
        NormalStaticString tmpAlias;
        tmpAlias += static_cast<const char *>(alias);
        tmpAlias.append(index);

        String s(static_cast<const char *>(tmpAlias));
        if (!aliasExists(s))
            return s;
        index--;
    }
}

bool VFS::aliasExists(const String &alias)
{
    return (m_Aliases.lookup(alias) != 0);
}

void VFS::removeAlias(const String &alias)
{
    /// \todo Remove from m_Mounts
    m_Aliases.remove(alias);
}

void VFS::removeAllAliases(Filesystem *pFs)
{
    if (!pFs)
        return;

    for (RadixTree<Filesystem *>::Iterator it = m_Aliases.begin();
         it != m_Aliases.end();)
    {
        if (pFs == (*it))
        {
            it = m_Aliases.erase(it);
        }
        else
            ++it;
    }

    /// \todo Locking.
    if (m_Mounts.lookup(pFs) != 0)
    {
        List<String *> *pList = m_Mounts.lookup(pFs);
        for (List<String *>::Iterator it = pList->begin(); it != pList->end();
             it++)
        {
            delete *it;
        }

        delete pList;

        m_Mounts.remove(pFs);
    }

    delete pFs;
}

Filesystem *VFS::lookupFilesystem(const String &alias)
{
    return m_Aliases.lookup(alias);
}

File *VFS::find(const String &path, File *pStartNode)
{
    // Search for a colon.
    bool bColon = false;
    size_t i;
    for (i = 0; i < path.length(); i++)
    {
        // Look for the UTF-8 '»'; 0xC2 0xBB.
        if (path[i] == '\xc2' && path[i + 1] == '\xbb')
        {
            bColon = true;
            break;
        }
    }

    if (!bColon)
    {
        // Pass directly through to the filesystem, if one specified.
        if (!pStartNode)
            return 0;
        else
            return pStartNode->getFilesystem()->find(path, pStartNode);
    }
    else
    {
        String tail(path);
        String newPath = tail.split(i + 2);
        tail.chomp();
        tail.chomp();

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(tail);
        if (!pFs)
            return 0;

        return pFs->find(newPath, 0);
    }
}

void VFS::addProbeCallback(Filesystem::ProbeCallback callback)
{
    Filesystem::ProbeCallback *p = new Filesystem::ProbeCallback;
    *p = callback;
    m_ProbeCallbacks.pushBack(p);
}

void VFS::addMountCallback(MountCallback callback)
{
    MountCallback *p = new MountCallback;
    *p = callback;
    m_MountCallbacks.pushBack(p);
}

bool VFS::createFile(const String &path, uint32_t mask, File *pStartNode)
{
    // Search for a colon.
    bool bColon = false;
    size_t i;
    for (i = 0; i < path.length(); i++)
    {
        // Look for the UTF-8 '»'; 0xC2 0xBB.
        if (path[i] == '\xc2' && path[i + 1] == '\xbb')
        {
            bColon = true;
            break;
        }
    }

    if (!bColon)
    {
        // Pass directly through to the filesystem, if one specified.
        if (!pStartNode)
            return false;
        else
            return pStartNode->getFilesystem()->createFile(
                path, mask, pStartNode);
    }
    else
    {
        String tail(path);
        String newPath = tail.split(i + 2);
        tail.chomp();
        tail.chomp();

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(tail);
        if (!pFs)
            return false;
        return pFs->createFile(newPath, mask, 0);
    }
}

bool VFS::createDirectory(const String &path, uint32_t mask, File *pStartNode)
{
    // Search for a colon.
    bool bColon = false;
    size_t i;
    for (i = 0; i < path.length(); i++)
    {
        // Look for the UTF-8 '»'; 0xC2 0xBB.
        if (path[i] == '\xc2' && path[i + 1] == '\xbb')
        {
            bColon = true;
            break;
        }
    }

    if (!bColon)
    {
        // Pass directly through to the filesystem, if one specified.
        if (!pStartNode)
            return false;
        else
            return pStartNode->getFilesystem()->createDirectory(
                path, mask, pStartNode);
    }
    else
    {
        // i+2 as the delimiter character (») is two bytes long.
        String tail(path);
        String newPath = tail.split(i + 2);
        tail.chomp();
        tail.chomp();

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(tail);
        if (!pFs)
            return false;
        return pFs->createDirectory(newPath, mask, 0);
    }
}

bool VFS::createSymlink(
    const String &path, const String &value, File *pStartNode)
{
    // Search for a colon.
    bool bColon = false;
    size_t i;
    for (i = 0; i < path.length(); i++)
    {
        // Look for the UTF-8 '»'; 0xC2 0xBB.
        if (path[i] == '\xc2' && path[i + 1] == '\xbb')
        {
            bColon = true;
            break;
        }
    }

    if (!bColon)
    {
        // Pass directly through to the filesystem, if one specified.
        if (!pStartNode)
            return false;
        else
            return pStartNode->getFilesystem()->createSymlink(
                path, value, pStartNode);
    }
    else
    {
        String tail(path);
        String newPath = tail.split(i + 2);
        tail.chomp();
        tail.chomp();

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(tail);
        if (!pFs)
            return false;
        return pFs->createSymlink(newPath, value, 0);
    }
}

bool VFS::createLink(const String &path, File *target, File *pStartNode)
{
    // Search for a colon.
    bool bColon = false;
    size_t i;
    for (i = 0; i < path.length(); i++)
    {
        // Look for the UTF-8 '»'; 0xC2 0xBB.
        if (path[i] == '\xc2' && path[i + 1] == '\xbb')
        {
            bColon = true;
            break;
        }
    }

    if (!bColon)
    {
        // Pass directly through to the filesystem, if one specified.
        if (!pStartNode)
            return false;
        else
            return pStartNode->getFilesystem()->createLink(
                path, target, pStartNode);
    }
    else
    {
        String tail(path);
        String newPath = tail.split(i + 2);
        tail.chomp();
        tail.chomp();

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(tail);
        if (!pFs)
            return false;
        return pFs->createLink(newPath, target, 0);
    }
}

bool VFS::remove(const String &path, File *pStartNode)
{
    // Search for a colon.
    bool bColon = false;
    size_t i;
    for (i = 0; i < path.length(); i++)
    {
        // Look for the UTF-8 '»'; 0xC2 0xBB.
        if (path[i] == '\xc2' && path[i + 1] == '\xbb')
        {
            bColon = true;
            break;
        }
    }

    if (!bColon)
    {
        // Pass directly through to the filesystem, if one specified.
        if (!pStartNode)
            return false;
        else
            return pStartNode->getFilesystem()->remove(path, pStartNode);
    }
    else
    {
        String tail(path);
        String newPath = tail.split(i + 2);
        tail.chomp();
        tail.chomp();

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(tail);
        if (!pFs)
            return false;
        return pFs->remove(newPath, 0);
    }
}

bool VFS::checkAccess(File *pFile, bool bRead, bool bWrite, bool bExecute)
{
    return true;
#ifdef VFS_STANDALONE
    // We don't check permissions on standalone builds of the VFS.
    return true;
#else
    if (!pFile)
    {
        // The error for a null file is not EPERM or EACCESS.
        return true;
    }

    User *pCurrentUser =
        Processor::information().getCurrentThread()->getParent()->getUser();

    size_t uid = pFile->getUid();
    size_t gid = pFile->getGid();

    User *pUser = UserManager::instance().getUser(uid);
    Group *pGroup = UserManager::instance().getGroup(gid);

    uint32_t permissions = pFile->getPermissions();

    // Are we owner?
    uint32_t check = 0;
    if (pUser == pCurrentUser)
    {
        check = (permissions >> FILE_UBITS) & 0x7;
    }
    else if (pUser && pGroup && pUser->isMember(pGroup))
    {
        check = (permissions >> FILE_GBITS) & 0x7;
    }
    else
    {
        check = (permissions >> FILE_OBITS) & 0x7;
    }

    // Needed permissions.
    uint32_t needed = (bRead ? FILE_UR : 0) | (bWrite ? FILE_UW : 0) |
                      (bExecute ? FILE_UX : 0);
    if ((check & needed) != needed)
    {
        SYSCALL_ERROR(PermissionDenied);
        return false;
    }

    return true;
#endif
}

#ifndef VFS_STANDALONE
static bool initVFS()
{
    // Mount scratch filesystem (ie, pure ram filesystem, for POSIX /tmp etc)
    RamFs *pRamFs = new RamFs;
    pRamFs->initialise(0);
    VFS::instance().addAlias(pRamFs, String("scratch"));

    // Mount runtime filesystem.
    // The runtime filesystem assigns a Process ownership to each file, only
    // that process can modify/remove it. If the Process terminates without
    // removing the file, the file is not removed.
    RamFs *pRuntimeFs = new RamFs;
    pRuntimeFs->initialise(0);
    pRuntimeFs->setProcessOwnership(true);
    VFS::instance().addAlias(pRuntimeFs, String("runtime"));

    return true;
}

static void destroyVFS()
{
}

MODULE_INFO("vfs", &initVFS, &destroyVFS, "ramfs", "users");
#endif
