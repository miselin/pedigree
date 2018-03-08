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

#include "select-syscalls.h"
#include "poll-syscalls.h"
#include "net-syscalls.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/utilities/assert.h"

#include "modules/system/console/Console.h"
#include "modules/system/network-stack/NetManager.h"
#include "modules/system/network-stack/Tcp.h"
#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/LockedFile.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/utility.h"

#include "pedigree/kernel/Subsystem.h"
#include <PosixSubsystem.h>

#include <sys/socket.h>

static void selectEventHandler(uint8_t *pBuffer);

enum TimeoutType
{
    ReturnImmediately,
    SpecificTimeout,
    InfiniteTimeout
};

SelectEvent::SelectEvent()
    : Event(0, false), m_pSemaphore(0), m_pFdSet(0), m_FdIdx(0), m_pFile(0)
{
}

SelectEvent::SelectEvent(
    Semaphore *pSemaphore, fd_set *pFdSet, size_t fdIdx, File *pFile)
    : Event(reinterpret_cast<uintptr_t>(&selectEventHandler), false),
      m_pSemaphore(pSemaphore), m_pFdSet(pFdSet), m_FdIdx(fdIdx), m_pFile(pFile)
{
    assert(pSemaphore);
}

SelectEvent::~SelectEvent()
{
}

void SelectEvent::fire()
{
    FD_SET(m_FdIdx, m_pFdSet);

    m_pSemaphore->release();
}

size_t SelectEvent::serialize(uint8_t *pBuffer)
{
    void *alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(size_t));
    size_t *pBuf = reinterpret_cast<size_t *>(alignedBuffer);
    pBuf[0] = EventNumbers::SelectEvent;
    pBuf[1] = reinterpret_cast<size_t>(m_pSemaphore);
    pBuf[2] = reinterpret_cast<size_t>(m_pFdSet);
    pBuf[3] = m_FdIdx;
    pBuf[4] = reinterpret_cast<size_t>(m_pFile);

    return 5 * sizeof(size_t);
}

bool SelectEvent::unserialize(uint8_t *pBuffer, SelectEvent &event)
{
    void *alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(size_t));
    size_t *pBuf = reinterpret_cast<size_t *>(alignedBuffer);
    if (pBuf[0] != EventNumbers::SelectEvent)
        return false;

    event.m_pSemaphore = reinterpret_cast<Semaphore *>(pBuf[1]);
    event.m_pFdSet = reinterpret_cast<fd_set *>(pBuf[2]);
    event.m_FdIdx = pBuf[3];
    event.m_pFile = reinterpret_cast<File *>(pBuf[4]);

    return true;
}

void selectEventHandler(uint8_t *pBuffer)
{
    SelectEvent e;
    if (!SelectEvent::unserialize(pBuffer, e))
    {
        FATAL("SelectEventHandler: unable to unserialize event!");
    }
    e.fire();
}

static const char *timeoutTypeName(TimeoutType type)
{
    switch (type)
    {
        case InfiniteTimeout:
            return "infinite";
        case ReturnImmediately:
            return "immediate";
        case SpecificTimeout:
            return "timeout";
        default:
            return "unknown";
    }
}

int posix_select(
    int nfds, fd_set *readfds, fd_set *writefds, fd_set *errorfds,
    timeval *timeout)
{
    POLL_NOTICE("select(" << nfds << ", " << readfds << ", " << writefds << ", " << errorfds << ", " << timeout << ")");
    bool bValidAddresses = true;
    if (readfds)
        bValidAddresses =
            bValidAddresses && PosixSubsystem::checkAddress(
                                   reinterpret_cast<uintptr_t>(readfds),
                                   sizeof(fd_set), PosixSubsystem::SafeWrite);
    if (writefds)
        bValidAddresses =
            bValidAddresses && PosixSubsystem::checkAddress(
                                   reinterpret_cast<uintptr_t>(writefds),
                                   sizeof(fd_set), PosixSubsystem::SafeWrite);
    if (errorfds)
        bValidAddresses =
            bValidAddresses && PosixSubsystem::checkAddress(
                                   reinterpret_cast<uintptr_t>(errorfds),
                                   sizeof(fd_set), PosixSubsystem::SafeWrite);
    if (timeout)
        bValidAddresses =
            bValidAddresses && PosixSubsystem::checkAddress(
                                   reinterpret_cast<uintptr_t>(timeout),
                                   sizeof(timeval), PosixSubsystem::SafeWrite);

    if (!bValidAddresses)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Count the actual number of fds we have.
    size_t trueFdCount = 0;
    for (int i = 0; i < nfds; ++i)
    {
        if ((readfds && FD_ISSET(i, readfds)) ||
            (writefds && FD_ISSET(i, writefds)) ||
            (errorfds && FD_ISSET(i, errorfds)))
        {
            POLL_NOTICE("fd " << i << " is acceptable");
            ++trueFdCount;
        }
    }

    // Set up pollfds
    struct pollfd *fds = new struct pollfd[trueFdCount];
    size_t j = 0;
    for (int i = 0; i < nfds; ++i)
    {
        bool checkRead = readfds && FD_ISSET(i, readfds);
        bool checkWrite = writefds && FD_ISSET(i, writefds);
        bool checkError = errorfds && FD_ISSET(i, errorfds);

        if (!(checkRead || checkWrite || checkError))
        {
            continue;
        }

        POLL_NOTICE("registering fd " << i << " in slot " << j);

        fds[j].fd = i;
        fds[j].events = 0;
        if (checkRead) fds[j].events |= POLLIN;
        if (checkWrite) fds[j].events |= POLLOUT;
        if (checkError) fds[j].events |= POLLERR;
        fds[j].revents = 0;

        ++j;
    }

    // Default to infinite wait, but handle immediate wait or a specific timeout too.
    int timeoutMs = -1;
    if (timeout)
    {
        timeoutMs = (timeout->tv_sec * 1000) + (timeout->tv_usec / 1000);
    }

    // Go!
    POLL_NOTICE(" -> redirecting select() to poll() with " << trueFdCount << " actual fds");
    int r = posix_poll_safe(fds, trueFdCount, timeoutMs);

    // Fill fd_sets as needed.
    j = 0;
    for (int i = 0; i < nfds; ++i)
    {
        /// \todo this could be done MUCH better
        bool checkRead = readfds && FD_ISSET(i, readfds);
        bool checkWrite = writefds && FD_ISSET(i, writefds);
        bool checkError = errorfds && FD_ISSET(i, errorfds);

        if (!(checkRead || checkWrite || checkError))
        {
            continue;
        }

        if (checkRead)
        {
            if (fds[j].revents & POLLIN)
            {
                FD_SET(i, readfds);
            }
            else
            {
                FD_CLR(i, readfds);
            }
        }

        if (checkWrite)
        {
            if (fds[j].revents & POLLOUT)
            {
                FD_SET(i, writefds);
            }
            else
            {
                FD_CLR(i, writefds);
            }
        }

        if (checkError)
        {
            if (fds[j].revents & POLLERR)
            {
                FD_SET(i, errorfds);
            }
            else
            {
                FD_CLR(i, errorfds);
            }
        }

        ++j;
    }

    delete [] fds;

    POLL_NOTICE(" -> select via poll returns " << r);
    return r;
}
