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

    // Investigate the timeout parameter.
    TimeoutType timeoutType;
    size_t timeoutSecs = 0, timeoutUSecs = 0;
    if (timeout == 0)
        timeoutType = InfiniteTimeout;
    else if (timeout->tv_sec == 0 && timeout->tv_usec == 0)
        timeoutType = ReturnImmediately;
    else
    {
        timeoutType = SpecificTimeout;
        timeoutSecs = timeout->tv_sec + (timeout->tv_usec / 1000000);
        timeoutUSecs = timeout->tv_usec % 1000000;
    }

    F_NOTICE(
        "select(" << Dec << nfds << ", ?, ?, ?, {"
                  << timeoutTypeName(timeoutType) << ", " << timeoutSecs << "})"
                  << Hex);

    Thread *pThread = Processor::information().getCurrentThread();
    Process *pProcess = pThread->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    if ((nfds == 0) && (timeout == 0))
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    List<SelectEvent *> events;

    bool bError = false;
    bool bWillReturnImmediately = (timeoutType == ReturnImmediately);
    ssize_t nRet = 0;
    // Can be interrupted while waiting for sem - EINTR.
    Semaphore sem(0, true);
    Spinlock reentrancyLock;

    // Walk the fd_sets.
    for (int i = 0; i < nfds; i++)
    {
        // valid fd?
        FileDescriptor *pFd = 0;
        if ((readfds && FD_ISSET(i, readfds)) ||
            (writefds && FD_ISSET(i, writefds)) ||
            (errorfds && FD_ISSET(i, errorfds)))
        {
            pFd = pSubsystem->getFileDescriptor(i);
            if (!pFd)
            {
                // Error - no such file descriptor.
                ERROR("select: no such file descriptor (" << Dec << i << ")");
                bError = true;
                break;
            }
        }
        else
        {
            continue;
        }

        File *pFile = pFd->file;
        if (pFd->so_domain == AF_UNIX)
        {
            // Special magic for UNIX sockets. Yay.
            if (pFd->so_local && (readfds && FD_ISSET(i, readfds)))
            {
                // Check local socket for readability.
                pFile = pFd->so_local;
            }
        }

        if (!pFile && !pFd->socket)
        {
            ERROR("select: a file descriptor was given that did not have a "
                  "file object.");
            bError = true;
            break;
        }

        fd_set *checks[2] = {readfds, writefds};

        for (size_t j = 0; j < 2 && !bWillReturnImmediately; ++j)
        {
            F_NOTICE(" -> check " << (j ? "writable" : "readable") << " fd=" << i);

            if (!checks[j])
            {
                continue;
            }
            else if (!FD_ISSET(i, checks[j]))
            {
                continue;
            }

            if (pFile)
            {
                // Has the file already got data in it?
                /// \todo Specify read/write/error to select and monitor.
                if (pFile->select(j > 0, 0))
                {
                    bWillReturnImmediately = true;
                    nRet++;
                }
                else if (bWillReturnImmediately)
                    FD_CLR(i, checks[j]);
                else
                {
                    FD_CLR(i, checks[j]);
                    // Need to set up a SelectEvent.
                    SelectEvent *pEvent = new SelectEvent(&sem, checks[j], i, pFile);
                    reentrancyLock.acquire();
                    pFile->monitor(pThread, pEvent);
                    events.pushBack(pEvent);

                    // Quickly check again now we've added the monitoring event,
                    // to avoid a race condition where we could miss the event.
                    //
                    /// \note This is safe because the event above can only be
                    ///       dispatched to this thread, and while we hold the
                    ///       reentrancy spinlock that cannot happen!
                    if (pFile->select(j > 0, 0))
                    {
                        bWillReturnImmediately = true;
                        nRet++;
                    }

                    reentrancyLock.release();
                }
            }
            else if (pFd->socket)
            {
                struct netconnMetadata *meta = getNetconnMetadata(pFd->socket);

                meta->lock.acquire();
                if ((j && meta->send) || ((!j) && (meta->recv || meta->pb)))
                {
                    // Immediately readable or writable.
                    bWillReturnImmediately = true;
                    nRet++;

                    // Fix up opposite fd_set
                    if (j && checks[0])
                    {
                        FD_CLR(i, checks[0]);
                    }
                    if ((!j) && checks[1])
                    {
                        FD_CLR(i, checks[1]);
                    }
                }
                else
                {
                    meta->semaphores.pushBack(&sem);
                }
                meta->lock.release();
            }
        }

        if (errorfds && FD_ISSET(i, errorfds))
        {
            F_NOTICE(" -> check error fd=" << i);

            FD_CLR(i, errorfds);
        }
    }

    // Grunt work is done, now time to cleanup.
    if (!bWillReturnImmediately && !bError)
    {
        // We got here because there is a specific or infinite timeout and
        // no FD was ready immediately.
        //
        // We wait on the semaphore 'sem': Its address has been given to all
        // the events and will be raised whenever an FD has action.
        assert(nRet == 0);
        bool bResult = sem.acquire(1, timeoutSecs, timeoutUSecs);

        // Did we actually get the semaphore or did we timeout?
        if (bResult)
        {
            // We were signalled, so one more FD ready.
            nRet++;
            // While the semaphore is nonzero, more FDs are ready.
            while (sem.tryAcquire())
                nRet++;
        }
        else
        {
            // The timeout event sets the interrupted state, so while this
            // looks unusual, the condition is caused by an interrupted sleep
            // due to something other than timeout.
            if (!pThread->wasInterrupted())
            {
                SYSCALL_ERROR(Interrupted);
                bError = true;
            }
            else
            {
                // OK. Timeout - not an error state.
            }
        }
    }

    // Only do cleanup and lock acquire/release if we set events up.
    if (events.count())
    {
        // Block any more events being sent to us so we can safely clean up.
        reentrancyLock.acquire();
        pThread->inhibitEvent(EventNumbers::SelectEvent, true);
        reentrancyLock.release();

        // Cull monitor targets first. While we're doing this, events might be
        // sent to this thread, but they're inhibited. We need to cull first to
        // stop further events getting created.
        for (auto pEvent : events)
        {
            pEvent->getFile()->cullMonitorTargets(pThread);
        }

        // Cull any leftover events that came in while we were culling monitor
        // targets, so we don't have any leftover events in Thread queues.
        pThread->cullEvent(EventNumbers::SelectEvent);

        // Now, we can finally clean up our events.
        for (auto pEvent : events)
        {
            delete pEvent;
        }

        // Cleanup is complete, stop inhibiting events now.
        pThread->inhibitEvent(EventNumbers::SelectEvent, false);
    }

    // Clean up socket metadata.
    for (int i = 0; i < nfds; i++)
    {
        // valid fd?
        FileDescriptor *pFd = 0;
        if ((readfds && FD_ISSET(i, readfds)) ||
            (writefds && FD_ISSET(i, writefds)) ||
            (errorfds && FD_ISSET(i, errorfds)))
        {
            pFd = pSubsystem->getFileDescriptor(i);
        }

        if (!pFd)
        {
            continue;
        }

        if (pFd->socket)
        {
            struct netconnMetadata *meta = getNetconnMetadata(pFd->socket);
            meta->lock.acquire();
            for (auto it = meta->semaphores.begin(); it != meta->semaphores.end(); ++it)
            {
                if ((*it) == &sem)
                {
                    it = meta->semaphores.erase(it);
                }
            }
            meta->lock.release();
        }
    }

    F_NOTICE("    -> select returns " << Dec << ((bError) ? -1 : nRet) << Hex);

    return (bError) ? -1 : nRet;
}
