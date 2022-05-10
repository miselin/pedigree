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

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/processor/Processor.h"

#if UTILITY_LINUX
#include <vector>

std::vector<FileDescriptor *> g_Descriptors;

FileDescriptor *getDescriptor(int fd)
{
    if ((size_t)fd >= g_Descriptors.size())
    {
        return nullptr;
    }

    return g_Descriptors[fd];
}

void addDescriptor(int fd, FileDescriptor *f)
{
    FileDescriptor *old = getDescriptor(fd);
    if (old)
    {
        delete old;
    }

    if ((size_t)fd > g_Descriptors.capacity())
    {
        g_Descriptors.reserve(fd + 1);
    }

    g_Descriptors.insert(g_Descriptors.begin() + fd, f);
}

size_t getAvailableDescriptor()
{
    return g_Descriptors.size();
}
#else
/// \todo move these into a common area, this code is duplicated EVERYWHERE
PosixSubsystem *getSubsystem()
{
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return nullptr;
    }

    return pSubsystem;
}

FileDescriptor *getDescriptor(int fd)
{
    PosixSubsystem *pSubsystem = getSubsystem();
    return pSubsystem->getFileDescriptor(fd);
}

void addDescriptor(int fd, FileDescriptor *f)
{
    PosixSubsystem *pSubsystem = getSubsystem();
    pSubsystem->addFileDescriptor(fd, f);
}

size_t getAvailableDescriptor()
{
    PosixSubsystem *pSubsystem = getSubsystem();
    return pSubsystem->getFd();
}
#endif
