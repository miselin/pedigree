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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/MemoryPool.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/pocketknife.h"
#include "pedigree/kernel/utilities/utility.h"

static void map(uintptr_t location) {
  VirtualAddressSpace& va = VirtualAddressSpace::getKernelAddressSpace();

  pocketknife::VirtualAddressSpaceSwitch vaswitch;

  void* page = page_align(reinterpret_cast<void*>(location));
  if (!va.isMapped(page)) {
    physical_uintptr_t phys = PhysicalMemoryManager::instance().allocatePage();
    va.map(phys, page, VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write);
  }

  vaswitch.restore();
}

static bool unmap(uintptr_t location) {
  VirtualAddressSpace& va = VirtualAddressSpace::getKernelAddressSpace();

  pocketknife::VirtualAddressSpaceSwitch vaswitch;

  void* page = page_align(reinterpret_cast<void*>(location));
  bool result = false;
  if ((result = va.isMapped(page))) {
    size_t flags = 0;
    physical_uintptr_t phys = 0;
    va.getMapping(page, phys, flags);

    va.unmap(page);
    PhysicalMemoryManager::instance().freePage(phys);
  }

  vaswitch.restore();

  return result;
}

MemoryPoolPressureHandler::MemoryPoolPressureHandler(MemoryPool* pool) : m_Pool(pool) {}

MemoryPoolPressureHandler::~MemoryPoolPressureHandler() {}

const char* MemoryPoolPressureHandler::getMemoryPressureDescription() {
  return "MemoryPool: freeing unused pages";
}

bool MemoryPoolPressureHandler::compact() {
  return m_Pool->trim();
}

MemoryPool::MemoryPool()
    :
#if THREADS
      m_Condition(),
      m_DrainCondition(),
      m_Lock(),
      m_MappingLock(),
#endif
      m_BufferSize(1024),
      m_BufferCount(0),
      m_Pool("memory-pool"),
      m_bInitialised(false),
      m_bClosing(false),
      m_ActiveOperations(0),
      m_AllocBitmap(),
      m_PressureHandler(this) {
}

MemoryPool::MemoryPool(const char* poolName)
    :
#if THREADS
      m_Condition(),
      m_DrainCondition(),
      m_Lock(),
      m_MappingLock(),
#endif
      m_BufferSize(1024),
      m_BufferCount(0),
      m_Pool(poolName),
      m_bInitialised(false),
      m_bClosing(false),
      m_ActiveOperations(0),
      m_AllocBitmap(),
      m_PressureHandler(this) {
}

MemoryPool::~MemoryPool() {
  TerminationDeferral terminationDeferral;
#if THREADS
  m_Lock.acquire();
#endif
  bool wasInitialised = m_bInitialised;
  m_bClosing = true;
  m_bInitialised = false;
#if THREADS
  m_Condition.broadcast();
  while (m_ActiveOperations) {
    m_DrainCondition.waitForCompletion(m_Lock);
  }
  m_Lock.release();
#endif

  if (wasInitialised) {
    MemoryPressureManager::instance().removeHandler(&m_PressureHandler);
  }
}

MemoryPool::ActiveOperation::ActiveOperation(MemoryPool& pool)
    : m_TerminationDeferral(), m_Pool(pool.beginOperation() ? &pool : nullptr) {}

MemoryPool::ActiveOperation::~ActiveOperation() {
  if (m_Pool) {
    m_Pool->endOperation();
  }
}

bool MemoryPool::beginOperation() {
#if THREADS
  LockGuard<Mutex> guard(m_Lock);
#endif
  if (m_bClosing || !m_bInitialised) {
    return false;
  }

  ++m_ActiveOperations;
  return true;
}

void MemoryPool::endOperation() {
#if THREADS
  LockGuard<Mutex> guard(m_Lock);
#endif
  assert(m_ActiveOperations);
  --m_ActiveOperations;
#if THREADS
  if (m_bClosing && !m_ActiveOperations) {
    m_DrainCondition.signal();
  }
#endif
}

bool MemoryPool::initialise(size_t poolSize, size_t bufferSize) {
#if THREADS
  LockGuard<Mutex> guard(m_Lock);
#endif

  if (m_bClosing)
    return false;

  if (m_bInitialised)
    return true;

  if (!poolSize || !bufferSize)
    return false;

  const size_t maxSize = ~static_cast<size_t>(0);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (poolSize > (maxSize / pageSize))
    return false;
  const size_t poolBytes = poolSize * pageSize;

  // Find the next power of two for bufferSize, if it isn't already one
  if ((bufferSize & (bufferSize - 1))) {
    size_t powerOf2 = 1;
    while (powerOf2 < bufferSize) {
      if (powerOf2 > (maxSize >> 1))
        return false;
      powerOf2 <<= 1;
    }
    bufferSize = powerOf2;
  }

  if (bufferSize > poolBytes)
    return false;

  m_BufferSize = bufferSize;

  NOTICE("MemoryPool: allocating memory pool '" << m_Pool.name() << "', " << Dec
                                                << ((poolSize * 4096) / 1024) << Hex
                                                << "K. Buffer size is " << m_BufferSize << ".");
  m_bInitialised = PhysicalMemoryManager::instance().allocateRegion(
      m_Pool, poolSize, PhysicalMemoryManager::virtualOnly,
      VirtualAddressSpace::Write | VirtualAddressSpace::KernelMode);
  if (!m_bInitialised)
    return false;

  m_BufferCount = m_Pool.size() / m_BufferSize;

  // Register us as a memory pressure handler, with top priority. We should
  // very easily be able to free pages in most cases.
  MemoryPressureManager::instance().registerHandler(MemoryPressureManager::HighestPriority,
                                                    &m_PressureHandler);

  return true;
}

uintptr_t MemoryPool::allocate() {
  return allocateDoer(true);
}

uintptr_t MemoryPool::allocateNow() {
  return allocateDoer(false);
}

uintptr_t MemoryPool::allocateDoer(bool canBlock) {
  ActiveOperation operation(*this);
  if (!operation) {
    return 0;
  }

  uintptr_t result = 0;
  {
#if THREADS
    LockGuard<Mutex> guard(m_Lock);
#endif

    size_t poolSize = m_Pool.size();
    size_t nBuffers = poolSize / m_BufferSize;
    uintptr_t poolBase = reinterpret_cast<uintptr_t>(m_Pool.virtualAddress());

#if THREADS
    while (m_bInitialised && !m_BufferCount) {
      if (!canBlock) {
        return 0;
      }

      ConditionVariable::Error error = ConditionVariable::NoError;
      if (!m_Condition.wait(m_Lock, error)) {
        if (!ConditionVariable::mutexAcquired(error)) {
          guard.disown();
        }
        return 0;
      }
    }

    if (!m_bInitialised) {
      return 0;
    }
#else
    if (!m_bInitialised || !m_BufferCount) {
      return 0;
    }
#endif

    size_t n = m_AllocBitmap.getFirstClear();
    assert(n < nBuffers);
    m_AllocBitmap.set(n);

    size_t offset = n * m_BufferSize;
    assert((offset % m_BufferSize) == 0);
    assert(offset < poolSize);
    assert(m_BufferSize <= (poolSize - offset));
    result = poolBase + offset;

    --m_BufferCount;
  }

#if THREADS
  {
    LockGuard<Mutex> mappingGuard(m_MappingLock);
    map(result);
  }
#else
  map(result);
#endif
  return result;
}

void MemoryPool::free(uintptr_t buffer) {
  ActiveOperation operation(*this);
  if (!operation) {
    return;
  }

#if THREADS
  LockGuard<Mutex> guard(m_Lock);
#endif

  if (!m_bInitialised || m_bClosing)
    return;

  uintptr_t poolBase = reinterpret_cast<uintptr_t>(m_Pool.virtualAddress());
  size_t poolSize = m_Pool.size();
  assert(buffer >= poolBase);
  size_t offset = buffer - poolBase;
  assert(offset < poolSize);
  assert((offset % m_BufferSize) == 0);

  size_t n = offset / m_BufferSize;
  assert(n < (poolSize / m_BufferSize));
  assert(m_AllocBitmap.test(n));
  m_AllocBitmap.clear(n);

  ++m_BufferCount;
  assert(m_BufferCount <= (poolSize / m_BufferSize));

#if THREADS
  m_Condition.signal();
#endif
}

bool MemoryPool::trim() {
  ActiveOperation operation(*this);
  if (!operation) {
    return false;
  }

#if THREADS
  LockGuard<Mutex> guard(m_Lock);
#endif

  if (!m_bInitialised || m_bClosing) {
    return false;
  }

#if THREADS
  // Compaction is opportunistic and can be entered by allocatePage() while
  // this same execution context owns the mapping lock. Never wait on the
  // allocator whose failure invoked us.
  if (!m_MappingLock.tryAcquire()) {
    return false;
  }
#endif

  size_t poolSize = m_Pool.size();
  size_t nBuffers = poolSize / m_BufferSize;
  uintptr_t poolBase = reinterpret_cast<uintptr_t>(m_Pool.virtualAddress());

  // Easy trim if buffers are pages or larger (remember that buffer sizes are
  // rounded up to the next power of two).
  size_t nFreed = 0;
  if (m_BufferSize >= PhysicalMemoryManager::getPageSize()) {
    for (size_t n = 0; n < nBuffers; ++n) {
      if (!m_AllocBitmap.test(n)) {
        uintptr_t page = poolBase + (n * m_BufferSize);
        for (size_t off = 0; off < m_BufferSize; off += PhysicalMemoryManager::getPageSize()) {
          if (unmap(page + off))
            ++nFreed;
        }
      }
    }
  } else {
    // Need to find N contiguous sets of bits.
    // We also need to navigate in blocks of pages.
    size_t N = PhysicalMemoryManager::getPageSize() / m_BufferSize;
    for (size_t n = 0, m = 0; n < nBuffers; n += N, ++m) {
      if (m_AllocBitmap.test(n))
        continue;

      bool ok = true;
      for (size_t y = 1; y < N; ++y) {
        if (m_AllocBitmap.test(n + y)) {
          ok = false;
          break;
        }
      }

      if (!ok)
        continue;

      uintptr_t page = poolBase + (m * PhysicalMemoryManager::getPageSize());
      if (unmap(page))
        ++nFreed;
    }
  }

#if THREADS
  m_MappingLock.release();
#endif
  return nFreed > 0;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void MemoryPool::acquireHostedOperationLock() {
  m_Lock.acquire();
}

void MemoryPool::releaseHostedOperationLock() {
  m_Lock.release();
}

void MemoryPool::acquireHostedMappingLock() {
  m_MappingLock.acquire();
}

void MemoryPool::releaseHostedMappingLock() {
  m_MappingLock.release();
}

size_t MemoryPool::getHostedActiveOperationCount() {
  LockGuard<Mutex> guard(m_Lock);
  return m_ActiveOperations;
}
#endif
