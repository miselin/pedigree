/*
 * Copyright (c) 2010 Matthew Iselin
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
 #include <process/ZombieQueue.h>

ZombieQueue ZombieQueue::m_Instance;

ZombieQueue::ZombieQueue() : RequestQueue()
{
    initialise();
}

ZombieQueue::~ZombieQueue()
{
    destroy();
}

void ZombieQueue::addProcess(Process *pProcess)
{
    addAsyncRequest(1, reinterpret_cast<uint64_t>(pProcess));
}

void ZombieQueue::addThread(Thread *pThread)
{
    addAsyncRequest(2, reinterpret_cast<uint64_t>(pThread));
}

uint64_t ZombieQueue::executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
                                     uint64_t p6, uint64_t p7, uint64_t p8)
{
    if(!p1 || !p2)
        return 0;
    
    if(p1 == 1)
        delete reinterpret_cast<Process*>(p2);
    else if(p1 == 2)
        delete reinterpret_cast<Thread*>(p2);
    
    return 0;
}

