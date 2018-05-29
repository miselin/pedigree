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

#ifndef _UTILITY_MEMORY_POOL
#define _UTILITY_MEMORY_POOL

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#ifdef THREADS
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#endif
#include "pedigree/kernel/process/MemoryPressureManager.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/String.h"

class MemoryPool;

/**
 * MemoryPoolPressureHandler - handles removing unused pages from a MemoryPool
 * when memory pressure is seen on the system. Because MemoryPools tend to be
 * full of bursty allocations, it's fairly typical to get a couple pages free.
 */
class MemoryPoolPressureHandler : public MemoryPressureHandler
{
  public:
    MemoryPoolPressureHandler(MemoryPool *pool);
    virtual ~MemoryPoolPressureHandler();

    virtual const String getMemoryPressureDescription();

    virtual bool compact();

  private:
    MemoryPool *m_Pool;
};

/** MemoryPool - a class which encapsulates a pool of memory with constant
 * sized buffers that can be allocated around the kernel. Intended to be
 * used instead of the heap for areas where similar sized buffers are
 * regularly allocated, such as networking code. */
class EXPORTED_PUBLIC MemoryPool
{
  public:
    MemoryPool();
    MemoryPool(const char *poolName);
    virtual ~MemoryPool();

    /// Initialises the pool, preparing it for use
    /// @param poolSize Number of pages in the pool.
    /// @param bufferSize Size of each buffer. Will be rounded to the
    ///                   next power of two.
    bool initialise(size_t poolSize, size_t bufferSize = 1024);

    /// Call if you aren't certain that the object has been initialised yet
    inline bool initialised()
    {
        return m_bInitialised;
    }

    /// Allocates a buffer from the pool. Will block if no buffers are
    /// available yet.
    uintptr_t allocate();

    /// Allocates a buffer from the pool. If no buffers are available, this
    /// function will return straight away.
    /// @return Zero if a buffer couldn't be allocated.
    uintptr_t allocateNow();

    /// Frees an allocated buffer, allowing it to be used elsewhere
    void free(uintptr_t buffer);

    /// Trims the pool, freeing pages that are not otherwise in use.
    bool trim();

  private:
#ifdef THREADS
    ConditionVariable m_Condition;
    Mutex m_Lock;
#endif

    /// Size of each buffer in this pool
    size_t m_BufferSize;

    /// Number of buffers we have available.
    size_t m_BufferCount;

#ifndef STANDALONE_MEMPOOL
    /// MemoryRegion describing the actual pool of memory
    MemoryRegion m_Pool;
#endif

    /// Has this instance been initialised yet?
    bool m_bInitialised;

    /// Allocation bitmap
    ExtensibleBitmap m_AllocBitmap;

#ifndef STANDALONE_MEMPOOL
    /// Memory pressure handler for this pool.
    MemoryPoolPressureHandler m_PressureHandler;
#endif

    /// Allocation doer
    uintptr_t allocateDoer(bool canBlock);
};

#endif
