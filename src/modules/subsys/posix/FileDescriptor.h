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

#ifndef POSIX_FILEDESCRIPTOR_H
#define POSIX_FILEDESCRIPTOR_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/String.h"

class File;
class LockedFile;
class UnixSocket;
class IoEvent;

/** Abstraction of a file descriptor, which defines an open file
 * and related flags.
 */
class EXPORTED_PUBLIC FileDescriptor
{
  public:
    /// Default constructor
    FileDescriptor();

    /// Parameterised constructor
    FileDescriptor(
        File *newFile, uint64_t newOffset = 0, size_t newFd = 0xFFFFFFFF,
        int fdFlags = 0, int flFlags = 0, LockedFile *lf = 0);

    /// Copy constructor
    FileDescriptor(FileDescriptor &desc);

    /// Pointer copy constructor
    FileDescriptor(FileDescriptor *desc);

    /// Assignment operator implementation
    FileDescriptor &operator=(FileDescriptor &desc);

    /// Destructor - decreases file reference count
    virtual ~FileDescriptor();

    /// Our open file pointer
    File *file;

    /// Offset within the file for I/O
    uint64_t offset;

    /// Descriptor number
    size_t fd;

    /// File descriptor flags (fcntl)
    int fdflags;

    /// File status flags (fcntl)
    int flflags;

    /// Locked file, non-zero if there is an advisory lock on the file
    LockedFile *lockedFile;

    /// Network syscall implementation for this descriptor (if it's a socket).
    SharedPointer<class NetworkSyscalls> networkImpl;

    /// IO event for reporting changes to files
    IoEvent *ioevent;
};

#endif
