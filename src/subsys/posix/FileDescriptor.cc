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

#include "FileDescriptor.h"

#include "modules/system/vfs/File.h"

#include <fcntl.h>

#define ENABLE_LOCKED_FILES 0

#if ENABLE_LOCKED_FILES
RadixTree<LockedFile *> g_PosixGlobalLockedFiles;
#endif

/// Default constructor
FileDescriptor::FileDescriptor()
    : socket(0), file(0), offset(0), fd(0xFFFFFFFF), fdflags(0), flflags(0), so_domain(0),
      so_type(0), so_local(0), lockedFile(0)
{
}

/// Parameterised constructor
FileDescriptor::FileDescriptor(
    File *newFile, uint64_t newOffset, size_t newFd, int fdFlags, int flFlags,
    LockedFile *lf)
    : socket(0), file(newFile), offset(newOffset), fd(newFd), fdflags(fdFlags),
      flflags(flFlags), lockedFile(lf)
{
    if (file)
    {
#if ENABLE_LOCKED_FILES
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
#endif
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
}

/// Copy constructor
FileDescriptor::FileDescriptor(FileDescriptor &desc)
    : socket(desc.socket), file(desc.file), offset(desc.offset), fd(desc.fd), fdflags(desc.fdflags),
      flflags(desc.flflags), lockedFile(0)
{
    if (file)
    {
#if ENABLE_LOCKED_FILES
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
#endif
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
}

/// Pointer copy constructor
FileDescriptor::FileDescriptor(FileDescriptor *desc)
    : socket(0), file(0), offset(0), fd(0), fdflags(0), flflags(0), lockedFile(0)
{
    if (!desc)
        return;

    file = desc->file;
    offset = desc->offset;
    fd = desc->fd;
    fdflags = desc->fdflags;
    flflags = desc->flflags;
    if (file)
    {
#if ENABLE_LOCKED_FILES
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
#endif
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
}

/// Assignment operator implementation
FileDescriptor &FileDescriptor::operator=(FileDescriptor &desc)
{
    socket = desc.socket;
    file = desc.file;
    offset = desc.offset;
    fd = desc.fd;
    fdflags = desc.fdflags;
    flflags = desc.flflags;
    if (file)
    {
#if ENABLE_LOCKED_FILES
        lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
#endif
        file->increaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }
    return *this;
}

/// Destructor - decreases file reference count
FileDescriptor::~FileDescriptor()
{
#if ENABLE_LOCKED_FILES
    if (file)
    {
        // Unlock the file we have a lock on, release from the global lock table
        if (lockedFile)
        {
            g_PosixGlobalLockedFiles.remove(file->getFullPath());
            lockedFile->unlock();
            delete lockedFile;
        }
        file->decreaseRefCount((flflags & O_RDWR) || (flflags & O_WRONLY));
    }

    /// \todo how do we make sure sockets get cleaned up properly?
    /// we can't delete here as this is called on fork()/execve() and that
    /// would destroy existing sockets in other processes
#endif
}
