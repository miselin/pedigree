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

#ifndef _PROCESS_ZOMBIE_QUEUE_H
#define _PROCESS_ZOMBIE_QUEUE_H

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/RequestQueue.h"

class Process;

/// Wrapper object for ZombieQueue so it can delete any type of object with
/// the correct destructors called in MI situations.
class ZombieObject
{
};

/// Special wrapper object for Process
class ZombieProcess : public ZombieObject
{
  public:
    ZombieProcess(Process *pProcess);
    virtual ~ZombieProcess();

  private:
    Process *m_pProcess;
};

/**
 * ZombieQueue: takes zombie objects and frees them. This is used so those
 * objects do not have to do something like "delete this", which is bad.
 */
class ZombieQueue : public RequestQueue
{
  public:
    ZombieQueue();
    virtual ~ZombieQueue();

    static ZombieQueue &instance();

    void addObject(ZombieObject *pObject);

  private:
    virtual uint64_t executeRequest(
        uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
        uint64_t p6, uint64_t p7, uint64_t p8);

    static ZombieQueue m_Instance;
};

#endif
