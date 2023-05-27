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

#include "pedigree/kernel/utilities/ZombieQueue.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/utilities/new"

ZombieQueue ZombieQueue::m_Instance;

ZombieQueue::ZombieQueue() : RequestQueue(MakeConstantString("ZombieQueue"))
{
}

ZombieQueue::~ZombieQueue()
{
}

ZombieQueue &ZombieQueue::instance()
{
    return m_Instance;
}

void ZombieQueue::addObject(ZombieObject *pObject)
{
    addAsyncRequest(1, reinterpret_cast<uint64_t>(pObject));
}

uint64_t ZombieQueue::executeRequest(
    uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
    uint64_t p6, uint64_t p7, uint64_t p8)
{
    if (!p1)
        return 0;

    delete reinterpret_cast<ZombieObject *>(p1);

    return 0;
}

ZombieProcess::ZombieProcess(Process *pProcess) : m_pProcess(pProcess)
{
}

ZombieProcess::~ZombieProcess()
{
    delete m_pProcess;
}
