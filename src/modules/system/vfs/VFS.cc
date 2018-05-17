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
#include "modules/system/users/UserManager.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#ifndef VFS_STANDALONE
#include "modules/Module.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/processor/Processor.h"
#endif

/// \todo Figure out a way to clean up files after deletion. Directory::remove()
///       is not the right place to do this. There needs to be a way to add a
///       File to some sort of queue that cleans it up once it hits refcount
///       zero or something like that.

VFS VFS::m_Instance;

static void splitPathOnColon(size_t colonPosition, const StringView &path, StringView &left, StringView &right)
{
    size_t afterColon = path.nextCharacter(colonPosition);
    right = path.substring(afterColon, path.length());
    left = path.substring(0, colonPosition);
}

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
    AliasTable::LookupResult result =
        m_Aliases.lookup(oldAlias);
    if (result.hasValue())
    {
        Filesystem *pFs = result.value();

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
    return m_Aliases.contains(alias);
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

    for (AliasTable::Iterator it = m_Aliases.begin();
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
    Filesystem *fs;
#if VFS_WITH_LRU_CACHES
    if (!m_AliasCache.get(alias, fs))
    {
#endif
        fs = lookupFilesystem(alias.view());
#if VFS_WITH_LRU_CACHES
    }

    m_AliasCache.store(alias, fs);
#endif
    return fs;
}

Filesystem *VFS::lookupFilesystem(const StringView &alias)
{
    AliasTable::LookupResult result =
        m_Aliases.lookup(alias);
    return result.hasValue() ? result.value() : nullptr;
}

File *VFS::find(const String &path, File *pStartNode)
{
    StringView pathView = path.view();

    // Search for a colon.
    bool bColon = false;
    size_t i;
    for (i = 0; i < pathView.length(); i++)
    {
        // Look for the UTF-8 '»'; 0xC2 0xBB.
        if (pathView[i] == '\xc2' && pathView[i + 1] == '\xbb')
        {
            bColon = true;
            break;
        }
    }

    if (!bColon)
    {
        // Pass directly through to the filesystem, if one specified.
        if (!pStartNode)
        {
            return 0;
        }
        else
        {
            return pStartNode->getFilesystem()->find(pathView, pStartNode);
        }
    }
    else
    {
        // Can only cache lookups with the colon as they are not ambiguous
        File *result;
#if VFS_WITH_LRU_CACHES
        if (m_FindCache.get(path, result))
        {
            m_FindCache.store(path, result);
            return result;
        }
#endif

        StringView left, right;
        splitPathOnColon(i, pathView, left, right);

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(left);
        if (!pFs)
        {
            return 0;
        }

        result = pFs->find(right, 0);
#if VFS_WITH_LRU_CACHES
        if (result)
        {
            m_FindCache.store(path, result);
        }
#endif

        return result;
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
        StringView left, right;
        splitPathOnColon(i, path, left, right);

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(left);
        if (!pFs)
            return false;
        return pFs->createFile(right, mask, 0);
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
        {
            return false;
        }
        else
        {
            return pStartNode->getFilesystem()->createDirectory(
                path, mask, pStartNode);
        }
    }
    else
    {
        StringView left, right;
        splitPathOnColon(i, path, left, right);

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(left);
        if (!pFs)
        {
            return false;
        }
        return pFs->createDirectory(right, mask, 0);
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
        StringView left, right;
        splitPathOnColon(i, path, left, right);

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(left);
        if (!pFs)
            return false;
        return pFs->createSymlink(right, value, 0);
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
        StringView left, right;
        splitPathOnColon(i, path, left, right);

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(left);
        if (!pFs)
            return false;
        return pFs->createLink(right, target, 0);
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
        StringView left, right;
        splitPathOnColon(i, path, left, right);

        // Attempt to find a filesystem alias.
        Filesystem *pFs = lookupFilesystem(left);
        if (!pFs)
            return false;
        return pFs->remove(right, 0);
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

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();

    int64_t fuid = pFile->getUid();
    int64_t fgid = pFile->getGid();

    int64_t processUid = pProcess->getEffectiveUserId();
    if (processUid < 0)
    {
        processUid = pProcess->getUserId();
    }

    int64_t processGid = pProcess->getEffectiveGroupId();
    if (processGid < 0)
    {
        processGid = pProcess->getGroupId();
    }

    uint32_t check = 0;
    uint32_t permissions = pFile->getPermissions();

    if (fuid == processUid)
    {
        check = (permissions >> FILE_UBITS) & 0x7;
    }
    else if (fgid == processGid)
    {
        check = (permissions >> FILE_GBITS) & 0x7;
    }
    else
    {
        Vector<int64_t> supplementalGroups;
        pProcess->getSupplementalGroupIds(supplementalGroups);

        for (auto it : supplementalGroups)
        {
            if (it == fgid)
            {
                check = (permissions >> FILE_GBITS) & 0x7;
                break;
            }
        }

        if (!check)
        {
            check = (permissions >> FILE_OBITS) & 0x7;
        }
    }

    if (!check)
    {
        NOTICE(
            "no permissions? perms=" << Oct << permissions
                                     << ", check=" << check);
        return false;
    }

    // Needed permissions.
    uint32_t needed = (bRead ? FILE_UR : 0) | (bWrite ? FILE_UW : 0) |
                      (bExecute ? FILE_UX : 0);
    if ((check & needed) != needed)
    {
        NOTICE(
            "VFS::checkAccess: needed " << Oct << needed << ", check was "
                                        << check);
        SYSCALL_ERROR(PermissionDenied);
        return false;
    }

    return true;
#endif
}

#ifndef VFS_STANDALONE
static bool initVFS()
{
    return true;
}

static void destroyVFS()
{
}

MODULE_INFO("vfs", &initVFS, &destroyVFS, "users");
#endif
