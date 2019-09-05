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

#include "Filesystem.h"
#include "Directory.h"
#include "File.h"
#include "Symlink.h"
#include "VFS.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/LazyEvaluate.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/utility.h"

Filesystem::Filesystem() : m_bReadOnly(false), m_pDisk(0), m_nAliases(0)
{
}

Filesystem::~Filesystem() = default;

File *Filesystem::getTrueRoot()
{
    EMIT_IF(THREADS)
    {
        Process *pProcess =
            Processor::information().getCurrentThread()->getParent();
        File *maybeRoot = pProcess->getRootFile();
        if (maybeRoot)
            return maybeRoot;
    }
    return getRoot();
}

File *Filesystem::find(const StringView &path)
{
    return findNode(getTrueRoot(), path);
}

File *Filesystem::find(const String &path)
{
    return findNode(getTrueRoot(), path.view());
}

File *Filesystem::find(const StringView &path, File *pStartNode)
{
    assert(pStartNode != nullptr);
    File *a = findNode(pStartNode, path);
    return a;
}

File *Filesystem::find(const String &path, File *pStartNode)
{
    return find(path.view(), pStartNode);
}

bool Filesystem::createFile(
    const StringView &path, uint32_t mask, File *pStartNode)
{
    if (!pStartNode)
        pStartNode = getTrueRoot();
    File *pFile = findNode(pStartNode, path);

    if (pFile)
    {
        SYSCALL_ERROR(FileExists);
        return false;
    }

    String filename;
    File *pParent = findParent(path, pStartNode, filename);

    // Check the parent existed.
    if (!pParent)
    {
        SYSCALL_ERROR(DoesNotExist);
        return false;
    }

    // Are we allowed to make the file?
    if (!VFS::checkAccess(pParent, false, true, true))
    {
        return false;
    }

    // May need to create on a different filesytem (if the traversal crossed
    // over to a different fs)
    Filesystem *pFs = pParent->getFilesystem();

    // Now make the file.
    return pFs->createFile(pParent, filename, mask);
}

bool Filesystem::createDirectory(
    const StringView &path, uint32_t mask, File *pStartNode)
{
    if (!pStartNode)
        pStartNode = getTrueRoot();
    File *pFile = findNode(pStartNode, path);

    if (pFile)
    {
        SYSCALL_ERROR(FileExists);
        return false;
    }

    String filename;
    File *pParent = findParent(path, pStartNode, filename);

    // Check the parent existed.
    if (!pParent)
    {
        SYSCALL_ERROR(DoesNotExist);
        return false;
    }

    // Are we allowed to make the file?
    if (!VFS::checkAccess(pParent, false, true, true))
    {
        return false;
    }

    // May need to create on a different filesytem (if the traversal crossed
    // over to a different fs)
    Filesystem *pFs = pParent->getFilesystem();

    // Now make the directory.
    return pFs->createDirectory(pParent, filename, mask);
}

bool Filesystem::createSymlink(
    const StringView &path, const String &value, File *pStartNode)
{
    if (!pStartNode)
        pStartNode = getTrueRoot();
    File *pFile = findNode(pStartNode, path);

    if (pFile)
    {
        SYSCALL_ERROR(FileExists);
        return false;
    }

    String filename;
    File *pParent = findParent(path, pStartNode, filename);

    // Check the parent existed.
    if (!pParent)
    {
        SYSCALL_ERROR(DoesNotExist);
        return false;
    }

    // Are we allowed to make the file?
    if (!VFS::checkAccess(pParent, false, true, true))
    {
        return false;
    }

    // May need to create on a different filesytem (if the traversal crossed
    // over to a different fs)
    Filesystem *pFs = pParent->getFilesystem();

    // Now make the symlink.
    pFs->createSymlink(pParent, filename, value);

    return true;
}

bool Filesystem::createLink(
    const StringView &path, File *target, File *pStartNode)
{
    if (!pStartNode)
        pStartNode = getTrueRoot();
    File *pFile = findNode(pStartNode, path);

    if (pFile)
    {
        SYSCALL_ERROR(FileExists);
        return false;
    }

    String filename;
    File *pParent = findParent(path, pStartNode, filename);

    // Check the parent existed.
    if (!pParent)
    {
        SYSCALL_ERROR(DoesNotExist);
        return false;
    }

    // Are we allowed to make the file?
    if (!VFS::checkAccess(pParent, false, true, true))
    {
        return false;
    }

    // Links can't cross filesystems (symlinks can, though).
    if (this != target->getFilesystem())
    {
        SYSCALL_ERROR(CrossDeviceLink);
        return false;
    }

    // May need to create on a different filesytem (if the traversal crossed
    // over to a different fs)
    Filesystem *pFs = pParent->getFilesystem();

    // Now make the symlink.
    pFs->createLink(pParent, filename, target);

    return true;
}

bool Filesystem::remove(const StringView &path, File *pStartNode)
{
    if (!pStartNode)
        pStartNode = getTrueRoot();

    File *pFile = findNode(pStartNode, path);

    if (!pFile)
    {
        SYSCALL_ERROR(DoesNotExist);
        return false;
    }

    String filename;
    File *pParent = findParent(path, pStartNode, filename);

    // Check the parent existed.
    if (!pParent)
    {
        FATAL("Filesystem::remove: Massive algorithmic error.");
        return false;
    }

    // Are we allowed to delete the file?
    if (!VFS::checkAccess(pParent, false, true, true))
    {
        return false;
    }

    Directory *pDParent = Directory::fromFile(pParent);
    if (!pDParent)
    {
        FATAL("Filesystem::remove: Massive algorithmic error (2)");
        return false;
    }

    // May need to create on a different filesytem (if the traversal crossed
    // over to a different fs)
    Filesystem *pFs = pParent->getFilesystem();

    if (pFile->isDirectory())
    {
        Directory *removalDir = Directory::fromFile(pFile);
        if (removalDir->getNumChildren())
        {
            if (removalDir->getNumChildren() > 2)
            {
                // There's definitely more than just . and .. here.
                SYSCALL_ERROR(NotEmpty);
                return false;
            }

            // Are the entries only ., ..?
            for (auto it : removalDir->getCache())
            {
                const String &name = (*it)->getName();
                if (!(name.compare(".", 1) || name.compare("..", 2)))
                {
                    SYSCALL_ERROR(NotEmpty);
                    return false;
                }
            }

            // Clean out the . and .. entries
            if (!removalDir->empty())
            {
                // ?????
                SYSCALL_ERROR(IoError);
                return false;
            }
        }
    }

    // Remove the file from disk & parent directory cache.
    bool bRemoved = pFs->remove(pParent, pFile);
    if (bRemoved)
    {
        pDParent->remove(filename);
    }
    return bRemoved;
}

File *Filesystem::findNode(File *pNode, StringView path)
{
    if (UNLIKELY(path.length() == 0))
    {
        return pNode;
    }

    // If the pathname has a leading slash, cd to root and remove it.
    else if (path[0] == '/')
    {
        pNode = getTrueRoot();
        path = path.substring(1, path.length());
    }

    // Grab the next filename component.
    size_t i = 0;
    size_t nExtra = 0;
    while ((i < path.length()) && path[i] != '/')
    {
        i = path.nextCharacter(i);
    }
    while (i < path.length())
    {
        size_t n = path.nextCharacter(i);
        if (n >= path.length())
        {
            break;
        }
        else if (path[n] == '/')
        {
            i = n;
            ++nExtra;
        }
        else
        {
            break;
        }
    }

    StringView currentComponent = path.substring(0, i - nExtra);
    StringView restOfPath =
        path.substring(path.nextCharacter(i), path.length());

    // At this point 'currentComponent' contains the token to search for.
    // 'restOfPath' contains the path for the next recursion (or nil).

    // If 'path' is zero-lengthed, ignore and recurse.
    if (currentComponent.length() == 0)
    {
        return findNode(pNode, restOfPath);
    }

    // Firstly, if the current node is a symlink, follow it.
    /// \todo do we need to do permissions checks at each intermediate step?
    while (pNode->isSymlink())
    {
        pNode = Symlink::fromFile(pNode)->followLink();
    }

    // Next, if the current node isn't a directory, die.
    if (!pNode->isDirectory())
    {
        SYSCALL_ERROR(NotADirectory);
        return 0;
    }

    bool dot = currentComponent == ".";
    bool dotdot = currentComponent == "..";

    // '.' section, or '..' with no parent, or '..' and we're at the root.
    if (dot || (dotdot && pNode->m_pParent == 0) ||
        (dotdot && pNode == getTrueRoot()))
    {
        return findNode(pNode, restOfPath);
    }
    else if (dotdot)
    {
        return findNode(pNode->m_pParent, restOfPath);
    }

    Directory *pDir = Directory::fromFile(pNode);
    if (!pDir)
    {
        SYSCALL_ERROR(NotADirectory);
        return 0;
    }

    // Is this a reparse point? If so we need to change where we perform the
    // next lookup.
    Directory *reparse = pDir->getReparsePoint();
    if (reparse)
    {
        String fullPath, reparseFullPath;
        pDir->getFullPath(fullPath);
        pDir->getFullPath(reparseFullPath);
        WARNING(
            "VFS: found reparse point at '" << fullPath
                                            << "', following it (new target: "
                                            << reparseFullPath << ")");
        pDir = reparse;
    }

    // Are we allowed to access files in this directory?
    if (!VFS::checkAccess(pNode, false, false, true))
    {
        return 0;
    }

    // Cache lookup.
    File *pFile;
    if (!pDir->isCachePopulated())
    {
        // Directory contents not cached - cache them now.
        pDir->cacheDirectoryContents();
    }

    pFile = pDir->lookup(currentComponent);
    if (pFile)
    {
        // Cache lookup succeeded, recurse and return.
        return findNode(pFile, restOfPath);
    }
    else
    {
        // Cache lookup failed, does not exist.
        return 0;
    }
}

File *
Filesystem::findParent(StringView path, File *pStartNode, String &filename)
{
    // If the final character of the string is '/', this log falls apart. So,
    // check for that and chomp it. But, we also need to not do that for e.g.
    // path == '/'.
    if (path.length() > 1 && path[path.length() - 1] == '/')
    {
        path = path.substring(0, path.length() - 1);
    }

    // Work forwards to the end of the path string, attempting to find the last
    // '/'.
    ssize_t lastSlash = -1;
    for (ssize_t i = path.length() - 1; i >= 0; i = path.prevCharacter(i))
    {
        if (path[i] == '/')
        {
            lastSlash = i;
            break;
        }
    }

    // Now, if there were no slashes, the parent node is pStartNode.
    File *parentNode = nullptr;
    if (lastSlash == -1)
    {
        filename = path.toString();
        parentNode = pStartNode;
    }
    else
    {
        // Else split the filename off from the rest of the path and follow it.
        filename = path.substring(path.nextCharacter(lastSlash), path.length())
                       .toString();
        path = path.substring(0, lastSlash);
        parentNode = findNode(pStartNode, path);
    }

    // Handle immediate parent node being a reparse point.
    if (parentNode)
    {
        if (parentNode->isDirectory())
        {
            File *reparseNode =
                Directory::fromFile(parentNode)->getReparsePoint();
            if (reparseNode)
            {
                parentNode = reparseNode;
            }
        }
    }

    return parentNode;
}

bool Filesystem::createLink(File *parent, const String &filename, File *target)
{
    // Default stubbed implementation, works for filesystems that can't handle
    // hard links.
    return false;
}
