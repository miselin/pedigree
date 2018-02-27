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
#include <FileDescriptor.h>

static void pollEventHandler(uint8_t *pBuffer);

enum TimeoutType
{
    ReturnImmediately,
    SpecificTimeout,
    InfiniteTimeout
};

PollEvent::PollEvent()
    : Event(0, false), m_pSemaphore(0), m_pFd(0), m_nREvent(0), m_pFile(0)
{
}

PollEvent::PollEvent(
    Semaphore *pSemaphore, struct pollfd *fd, int revent, File *pFile)
    : Event(reinterpret_cast<uintptr_t>(&pollEventHandler), false),
      m_pSemaphore(pSemaphore), m_pFd(fd), m_nREvent(revent), m_pFile(pFile)
{
    assert(pSemaphore);
}

PollEvent::~PollEvent()
{
}

void PollEvent::fire()
{
    m_pFd->revents |= m_nREvent;

    m_pSemaphore->release();
}

size_t PollEvent::serialize(uint8_t *pBuffer)
{
    void *alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(size_t));
    size_t *pBuf = reinterpret_cast<size_t *>(alignedBuffer);
    pBuf[0] = EventNumbers::PollEvent;
    pBuf[1] = reinterpret_cast<size_t>(m_pSemaphore);
    pBuf[2] = reinterpret_cast<size_t>(m_pFd);
    pBuf[3] = static_cast<size_t>(m_nREvent);
    pBuf[4] = reinterpret_cast<size_t>(m_pFile);

    return 5 * sizeof(size_t);
}

bool PollEvent::unserialize(uint8_t *pBuffer, PollEvent &event)
{
    void *alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(size_t));
    size_t *pBuf = reinterpret_cast<size_t *>(alignedBuffer);
    if (pBuf[0] != EventNumbers::PollEvent)
        return false;

    event.m_pSemaphore = reinterpret_cast<Semaphore *>(pBuf[1]);
    event.m_pFd = reinterpret_cast<struct pollfd *>(pBuf[2]);
    event.m_nREvent = static_cast<int>(pBuf[3]);
    event.m_pFile = reinterpret_cast<File *>(pBuf[4]);

    return true;
}

void pollEventHandler(uint8_t *pBuffer)
{
    PollEvent e;
    if (!PollEvent::unserialize(pBuffer, e))
    {
        FATAL("PollEventHandler: unable to unserialize event!");
    }
    e.fire();
}

/** poll: determine if a set of file descriptors are writable/readable.
 *
 *  Permits any number of descriptors, unlike select().
 */
int posix_poll(struct pollfd *fds, unsigned int nfds, int timeout)
{
    F_NOTICE("poll(" << Dec << nfds << ", " << timeout << Hex << ")");
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(fds), nfds * sizeof(struct pollfd),
            PosixSubsystem::SafeWrite))
    {
        F_NOTICE(" -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Now checked, and it is safe to continue.
    return posix_poll_safe(fds, nfds, timeout);
}

int posix_poll_safe(struct pollfd *fds, unsigned int nfds, int timeout)
{
    F_NOTICE("poll_safe(" << Dec << nfds << ", " << timeout << Hex << ")");

    // Investigate the timeout parameter.
    TimeoutType timeoutType;
    size_t timeoutSecs = timeout / 1000;
    size_t timeoutUSecs = (timeout % 1000) * 1000;
    if (timeout < 0)
    {
        timeoutType = InfiniteTimeout;

        // Fix timeout to be truly infinite
        // (negative timeout may divide incorrectly)
        timeoutSecs = 0;
        timeoutUSecs = 0;
    }
    else if (timeout == 0)
    {
        timeoutType = ReturnImmediately;
    }
    else
    {
        timeoutType = SpecificTimeout;
    }

    // Grab the subsystem for this process
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    List<PollEvent *> events;

    bool bError = false;
    bool bWillReturnImmediately = (timeoutType == ReturnImmediately);

    // Can be interrupted while waiting for sem - EINTR.
    Semaphore sem(0, true);
    Spinlock reentrancyLock;

    for (unsigned int i = 0; i < nfds; i++)
    {
        // Grab the pollfd structure.
        struct pollfd *me = &fds[i];
        me->revents = 0;
        if (me->fd < 0)
        {
            continue;
        }

        // valid fd?
        FileDescriptor *pFd = pSubsystem->getFileDescriptor(me->fd);
        if (!pFd)
        {
            // Error - no such file descriptor.
            ERROR("poll: no such file descriptor (" << Dec << me->fd << ")");
            me->revents |= POLLNVAL;
            bError = true;
            continue;
        }

        bool checkWrite = false;

        // Check POLLIN, POLLOUT (almost exactly the same code for both).
        /// \todo should move this into a function instead of a loop here.
        for (size_t j = 0; j < 2; ++j)
        {
            short event = POLLIN;
            if (checkWrite)
            {
                event = POLLOUT;
            }

            if (me->events & event)
            {
                if (pFd->file)
                {
                    // Has the file already got data in it?
                    /// \todo Specify read/write/error to select and monitor.
                    if (pFd->file->select(checkWrite, 0))
                    {
                        me->revents |= event;
                        bWillReturnImmediately = true;
                    }
                    else if (!bWillReturnImmediately)
                    {
                        // Need to set up a PollEvent.
                        PollEvent *pEvent = new PollEvent(&sem, me, event, pFd->file);
                        pFd->file->monitor(
                            Processor::information().getCurrentThread(), pEvent);

                        reentrancyLock.acquire();

                        events.pushBack(pEvent);

                        // Quickly check again now we've added the monitoring event,
                        // to avoid a race condition where we could miss the event.
                        //
                        /// \note This is safe because the event above can only be
                        ///       dispatched to this thread, and while we hold the
                        ///       reentrancy spinlock that cannot happen!
                        if (pFd->file->select(checkWrite, 0))
                        {
                            me->revents |= event;
                            bWillReturnImmediately = true;
                        }

                        reentrancyLock.release();
                    }
                }
                else if (pFd->networkImpl && pFd->networkImpl->canPoll())
                {
                    bool checkingWrite = checkWrite;
                    bool checkingRead = !checkWrite;
                    bool checkingError = false;

                    bWillReturnImmediately = bWillReturnImmediately || pFd->networkImpl->poll(checkingRead, checkingWrite, checkingError, &sem);

                    if (bWillReturnImmediately)
                    {
                        if (checkingWrite)
                        {
                            me->revents |= POLLOUT;
                        }

                        if (checkingRead)
                        {
                            me->revents |= POLLIN;
                        }
                    }
                }
            }

            checkWrite = true;
        }

        if (me->events & POLLERR)
        {
            F_NOTICE("    -> POLLERR not yet supported");
        }
    }

    // Grunt work is done, now time to cleanup.
    while (!bWillReturnImmediately && !bError)
    {
        F_NOTICE("    -> no fds ready yet, poll will block");

        // We got here because there is a specific or infinite timeout and
        // no FD was ready immediately.
        //
        // We wait on the semaphore 'sem': Its address has been given to all
        // the events and will be raised whenever an FD has action.
        bool bResult = sem.acquire(1, timeoutSecs, timeoutUSecs);

        // Did we actually get the semaphore or did we timeout?
        if (bResult)
        {
            // We were signalled, so one more FD ready.
            // While the semaphore is nonzero, more FDs are ready.
            while (sem.tryAcquire())
                ;

            // Good to go for checking why we were woken (for sockets).
            // We only break out of the main poll() loop if a file was polled,
            // or a socket actually emits an expected event. This works better
            // as for sockets in particular, we'll get woken up for ALL events,
            // not just the ones we care about polling for.
            bool ok = false;
            for (size_t i = 0; i < nfds; ++i)
            {
                struct pollfd *me = &fds[i];
                FileDescriptor *pFd = pSubsystem->getFileDescriptor(me->fd);
                if (!pFd)
                {
                    continue;
                }

                if (pFd->networkImpl && pFd->networkImpl->canPoll())
                {
                    bool checkingWrite = me->events & POLLOUT;
                    bool checkingRead = me->events & POLLIN;
                    bool checkingError = false;

                    pFd->networkImpl->poll(checkingRead, checkingWrite, checkingError, nullptr);

                    if (checkingWrite && (me->events & POLLOUT))
                    {
                        me->revents |= POLLOUT;
                        ok = true;
                    }

                    if (checkingRead && (me->events & POLLIN))
                    {
                        me->revents |= POLLIN;
                        ok = true;
                    }
                }
                else if (pFd->file)
                {
                    ok = true;
                }
            }

            if (ok)
            {
                break;
            }
        }
        else
        {
            // The timeout event sets the interrupted state, so while this
            // looks unusual, the condition is caused by an interrupted sleep
            // due to something other than timeout.
            if (!Processor::information().getCurrentThread()->wasInterrupted())
            {
                SYSCALL_ERROR(Interrupted);
                bError = true;
            }
            else
            {
                // OK. Timeout - not an error state.
            }

            break;
        }
    }

    // Only do cleanup and lock acquire/release if we set events up.
    if (events.count())
    {
        // Block any more events being sent to us so we can safely clean up.
        reentrancyLock.acquire();
        Processor::information().getCurrentThread()->inhibitEvent(
            EventNumbers::PollEvent, true);
        reentrancyLock.release();

        for (auto pEvent : events)
        {
            pEvent->getFile()->cullMonitorTargets(
                Processor::information().getCurrentThread());
        }

        // Ensure there are no events still pending for this thread.
        Processor::information().getCurrentThread()->cullEvent(
            EventNumbers::PollEvent);

        for (auto pEvent : events)
        {
            delete pEvent;
        }

        // Cleanup is complete, stop inhibiting events now.
        Processor::information().getCurrentThread()->inhibitEvent(
            EventNumbers::PollEvent, false);
    }

    // Prepare return value (number of fds with events).
    size_t nRet = 0;
    for (size_t i = 0; i < nfds; ++i)
    {
        F_NOTICE(
            "    -> pollfd[" << i << "]: fd=" << fds[i].fd
                             << ", events=" << fds[i].events
                             << ", revents=" << fds[i].revents);

        if (fds[i].revents != 0)
        {
            ++nRet;
        }

        // Clean up socket Semaphores that we registered, if any.
        FileDescriptor *pFd = pSubsystem->getFileDescriptor(fds[i].fd);
        if (!pFd)
        {
            continue;
        }

        if (pFd->networkImpl && pFd->networkImpl->canPoll())
        {
            pFd->networkImpl->unPoll(&sem);
        }
    }

    F_NOTICE("    -> " << Dec << ((bError) ? -1 : (int)nRet) << Hex);
    F_NOTICE("    -> nRet is " << nRet << ", error is " << bError);

    return (bError) ? -1 : nRet;
}
