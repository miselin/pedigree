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

#include "modules/system/vfs/File.h"

#include "modules/subsys/posix/PollEvent.h"

static void pollEventHandler(uint8_t *pBuffer);

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
