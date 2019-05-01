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

#include "pedigree/kernel/syscallError.h"

#include "file-syscalls.h"
#include "modules/system/vfs/Pipe.h"
#include "modules/system/vfs/VFS.h"
#include "pipe-syscalls.h"

#include "pedigree/kernel/Subsystem.h"
#include <FileDescriptor.h>
#include <PosixSubsystem.h>

#include "modules/Module.h"

#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/processor/Processor.h"

#include <fcntl.h>

typedef Tree<size_t, FileDescriptor *> FdMap;

int posix_pipe(int filedes[2])
{
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(filedes), sizeof(int) * 2,
            PosixSubsystem::SafeWrite))
    {
        F_NOTICE("pipe -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("pipe");

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for the process!");
        return -1;
    }

    size_t readFd = pSubsystem->getFd();
    size_t writeFd = pSubsystem->getFd();

    filedes[0] = readFd;
    filedes[1] = writeFd;

    File *p = new Pipe(String(""), 0, 0, 0, 0, 0, 0, 0, true);

    // Create the file descriptor for both
    FileDescriptor *read = new FileDescriptor(p, 0, readFd, 0, O_RDONLY);
    pSubsystem->addFileDescriptor(readFd, read);

    FileDescriptor *write = new FileDescriptor(p, 0, writeFd, 0, O_WRONLY);
    pSubsystem->addFileDescriptor(writeFd, write);

    F_NOTICE("pipe: returning " << readFd << " and " << writeFd << ".");

    return 0;
}
