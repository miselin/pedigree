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

#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/IoEvent.h"
#include "modules/system/vfs/File.h"

#include <signal.h>

static void ioEventHandler(uint8_t *pBuffer);

IoEvent::IoEvent()
    : Event(0, false), m_pSubsystem(0), m_pFile(0), m_pRetriggerInstance(0)
{
}

IoEvent::IoEvent(
    PosixSubsystem *subsystem, File *file)
    : Event(reinterpret_cast<uintptr_t>(&ioEventHandler), false),
      m_pSubsystem(subsystem), m_pFile(file), m_pRetriggerInstance(0)
{
    assert(subsystem);
}

void IoEvent::fire()
{
    Thread *pThread = Processor::information().getCurrentThread();
    m_pSubsystem->sendSignal(pThread, SIGIO);

    // Re-monitor now that we've gotten the event.
    if (m_pRetriggerInstance)
    {
        // NOTE: using retrigger instance which is the original serialized
        // event - not the temporary one we deserialize later.
        m_pFile->monitor(pThread, m_pRetriggerInstance);
    }
}

size_t IoEvent::serialize(uint8_t *pBuffer)
{
    void *alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(size_t));
    size_t *pBuf = reinterpret_cast<size_t *>(alignedBuffer);
    pBuf[0] = EventNumbers::IoEvent;
    pBuf[1] = reinterpret_cast<size_t>(m_pSubsystem);
    pBuf[2] = reinterpret_cast<size_t>(m_pFile);
    pBuf[3] = reinterpret_cast<size_t>(this);

    return 4 * sizeof(size_t);
}

bool IoEvent::unserialize(uint8_t *pBuffer, IoEvent &event)
{
    void *alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(size_t));
    size_t *pBuf = reinterpret_cast<size_t *>(alignedBuffer);
    if (pBuf[0] != EventNumbers::IoEvent)
        return false;

    event.m_pSubsystem = reinterpret_cast<PosixSubsystem *>(pBuf[1]);
    event.m_pFile = reinterpret_cast<File *>(pBuf[2]);
    event.m_pRetriggerInstance = reinterpret_cast<IoEvent *>(pBuf[3]);

    return true;
}

void ioEventHandler(uint8_t *pBuffer)
{
    IoEvent e;
    if (!IoEvent::unserialize(pBuffer, e))
    {
        FATAL("PollEventHandler: unable to unserialize event!");
    }
    e.fire();
}
