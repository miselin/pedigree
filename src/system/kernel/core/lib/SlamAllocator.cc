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

#if !SLAM_USE_DEBUG_ALLOCATOR

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/debugger/Backtrace.h"
#include "pedigree/kernel/debugger/commands/SlamCommand.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/MemoryTracing.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#if X64 && !PEDIGREE_BENCHMARK
#include "system/kernel/core/processor/x64/utils.h"
#endif

#if MULTIPROCESSOR
#define ATOMIC_POP_MEMORY_ORDER __ATOMIC_ACQUIRE
#define ATOMIC_POP_FAILURE_MEMORY_ORDER __ATOMIC_ACQUIRE
#define ATOMIC_PUSH_MEMORY_ORDER __ATOMIC_RELEASE
#define ATOMIC_CAS_WEAK true
#else
#define ATOMIC_POP_MEMORY_ORDER __ATOMIC_RELAXED
#define ATOMIC_POP_FAILURE_MEMORY_ORDER __ATOMIC_RELAXED
#define ATOMIC_PUSH_MEMORY_ORDER __ATOMIC_RELAXED
#define ATOMIC_CAS_WEAK true
#endif

SlamAllocator SlamAllocator::m_Instance;

#if BITS_32
static constexpr uintptr_t POINTER_MASK = ~uintptr_t(0);
static constexpr uintptr_t POINTER_TAG_MASK = 0;
static constexpr uintptr_t POINTER_TAG_INCREMENT = 0;
#else
static constexpr uintptr_t POINTER_MASK = 0x0000FFFFFFFFFFFFULL;
static constexpr uintptr_t POINTER_TAG_MASK = ~POINTER_MASK;
static constexpr uintptr_t POINTER_TAG_INCREMENT = 0x0001000000000000ULL;
#endif

template <typename T>
inline T* untagged(T* p) PURE;

template <typename T>
inline T* tagged(T* p) PURE;

template <typename T>
inline T* next_tag(T* p, T* currentHead) PURE;

template <typename T>
inline T* untagged(T* p) {
#if BITS_32
  return p;
#else
  /// \todo this now requires 64-bit pointers everywhere.
  // All heap pointers begin with 32 bits of ones. So we shove a tag there.
  uintptr_t ptr = reinterpret_cast<uintptr_t>(p);
  EMIT_IF(PEDIGREE_BENCHMARK || HOSTED) {
    // The upper 16 bits are available for hosted userspace addresses.
    ptr &= POINTER_MASK;
  }
  else {
    // Restore the upper 16 bits to make this a canonical kernel address.
    ptr |= 0xFFFF000000000000ULL;
  }
  return reinterpret_cast<T*>(ptr);
#endif
}

template <typename T>
inline T* tagged(T* p) {
#if BITS_32
  return p;
#else
  uintptr_t ptr = reinterpret_cast<uintptr_t>(p);
  ptr &= POINTER_MASK;
  return reinterpret_cast<T*>(ptr);
#endif
}

template <typename T>
inline T* next_tag(T* p, T* currentHead) {
#if BITS_32
  (void) currentHead;
  return p;
#else
  uintptr_t ptr = reinterpret_cast<uintptr_t>(p) & POINTER_MASK;
  uintptr_t tag =
      (reinterpret_cast<uintptr_t>(currentHead) + POINTER_TAG_INCREMENT) & POINTER_TAG_MASK;
  return reinterpret_cast<T*>(ptr | tag);
#endif
}

inline void spin_pause() {
#if PEDIGREE_BENCHMARK
#if defined(__i386__) || defined(__x86_64__)
  asm("pause");
#elif defined(__aarch64__)
  asm("yield");
#endif
#else
  Processor::pause();
#endif
}

inline uintptr_t getHeapBase() {
#if PEDIGREE_BENCHMARK
  return SlamSupport::getHeapBase();
#else
  return VirtualAddressSpace::getKernelAddressSpace().getKernelHeapStart();
#endif
}

inline uintptr_t getHeapEnd() {
#if PEDIGREE_BENCHMARK
  return SlamSupport::getHeapEnd();
#else
  return VirtualAddressSpace::getKernelAddressSpace().getKernelHeapEnd();
#endif
}

inline size_t getPageSize() {
#if PEDIGREE_BENCHMARK
  return TargetInfo::getPageSize();
#else
  return PhysicalMemoryManager::getPageSize();
#endif
}

inline void allocateAndMapAt(void* addr, bool cowOk = false) {
#if PEDIGREE_BENCHMARK
  SlamSupport::getPageAt(addr);
#else
  size_t standardFlags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;

  static physical_uintptr_t physZero = 0;
  bool needZeroPage = false;
  size_t extraFlags = 0;

  physical_uintptr_t phys = 0;
  if (cowOk) {
    if (!physZero) {
      // allocate the zero page, we'll zero it shortly
      physZero = PhysicalMemoryManager::instance().allocatePage();
      // Retain one permanent owner in addition to the reference held by
      // every CoW mapping. A fault drops only its mapping reference.
      PhysicalMemoryManager::instance().pin(physZero);
      needZeroPage = true;

      // allow us to zero out the page
      extraFlags |= VirtualAddressSpace::Write;
    } else {
      extraFlags |= VirtualAddressSpace::CopyOnWrite;
    }

    // disable writing (for CoW to work properly)
    standardFlags &= ~VirtualAddressSpace::Write;

    phys = physZero;
  } else {
    phys = PhysicalMemoryManager::instance().allocatePage();
  }

  VirtualAddressSpace& va = VirtualAddressSpace::getKernelAddressSpace();
  if (!va.map(phys, addr, standardFlags | extraFlags)) {
    FATAL("SlamAllocator: failed to allocate and map at " << addr);
  }

  if (cowOk) {
    PhysicalMemoryManager::instance().pin(physZero);
  }

  if (needZeroPage) {
    ByteSet(addr, 0, PhysicalMemoryManager::getPageSize());

    // Page zeroed - mark page copy on write now so the zero page works
    va.setFlags(addr, standardFlags | VirtualAddressSpace::CopyOnWrite);
  }
#endif
}

inline void unmap(void* addr) {
#if PEDIGREE_BENCHMARK
  SlamSupport::unmapPage(addr);
// munmap(addr, getPageSize());
#else
  VirtualAddressSpace& va = VirtualAddressSpace::getKernelAddressSpace();
  if (!va.isMapped(addr))
    return;

  physical_uintptr_t phys;
  size_t flags;
  va.getMapping(addr, phys, flags);
  va.unmap(addr);

  PhysicalMemoryManager::instance().freePage(phys);
#endif
}

SlamCache::SlamCache()
    : m_PartialLists(),
      m_FastSlabs(),
      m_LargeFreeList(nullptr),
      m_ObjectSize(0),
      m_SlabSize(0),
      m_SlabObjectOffset(0),
      m_SlabObjectCount(0),
      m_FirstSlab(),
      m_FastPathState(0),
      m_RecoveryLock(false, true)
#if BITS_32
      , m_FreeLock(false)
#endif
{}

SlamCache::~SlamCache() {}

void SlamCache::initialise(SlamAllocator* parent, size_t objectSize) {
  if (objectSize < OBJECT_MINIMUM_SIZE)
    return;

  m_ObjectSize = objectSize;
  if (m_ObjectSize > SLAB_MINIMUM_SIZE)
    m_SlabSize = m_ObjectSize;
  else
    m_SlabSize = SLAB_MINIMUM_SIZE;

  for (size_t i = 0; i < NUM_LISTS; i++)
    m_PartialLists[i] = nullptr;
  for (size_t i = 0; i < NUM_LISTS; i++)
    m_FastSlabs[i] = nullptr;
  m_LargeFreeList = nullptr;
  m_FastPathState = 0;

  if (m_ObjectSize < getPageSize()) {
    m_SlabObjectOffset = ((sizeof(Slab) + m_ObjectSize - 1) / m_ObjectSize) * m_ObjectSize;
    m_SlabObjectCount = (m_SlabSize - m_SlabObjectOffset) / m_ObjectSize;
  }

  m_pParentAllocator = parent;

  assert((m_SlabSize % m_ObjectSize) == 0);
  assert(m_ObjectSize >= sizeof(Node));
  if (m_ObjectSize < getPageSize()) {
    assert(m_SlabObjectOffset < m_SlabSize);
    assert(m_SlabObjectCount);
  }
}

size_t SlamCache::currentList() const {
#if defined(PEDIGREE_BUILDUTILS)
  return m_TestList;
#else
  size_t list = 0;
  EMIT_IF(MULTIPROCESSOR) {
    list = Processor::id();
  }
  assert(list < NUM_LISTS);
  return list;
#endif
}

#if defined(PEDIGREE_BUILDUTILS)
void SlamCache::setListForTest(size_t list) {
  assert(list < NUM_LISTS);
  m_TestList = list;
}
#endif

void SlamCache::addSlab(Slab* slab, size_t list) {
  assert(!slab->onList);
  slab->list = list;
  slab->previous = nullptr;
  slab->next = m_PartialLists[list];
  if (slab->next)
    slab->next->previous = slab;
  m_PartialLists[list] = slab;
  slab->onList = true;
}

void SlamCache::removeSlab(Slab* slab) {
  assert(slab->onList);
  if (slab->previous)
    slab->previous->next = slab->next;
  else
    m_PartialLists[slab->list] = slab->next;
  if (slab->next)
    slab->next->previous = slab->previous;
  slab->next = nullptr;
  slab->previous = nullptr;
  slab->onList = false;
}

bool SlamCache::beginFastPath() {
  constexpr size_t writer = static_cast<size_t>(1) << ((sizeof(size_t) * 8) - 1);
  size_t state = __atomic_load_n(&m_FastPathState, __ATOMIC_ACQUIRE);
  while (!(state & writer)) {
    if (__atomic_compare_exchange_n(&m_FastPathState, &state, state + 1, false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
      return true;
  }
  return false;
}

void SlamCache::endFastPath() {
  __atomic_fetch_sub(&m_FastPathState, static_cast<size_t>(1), __ATOMIC_RELEASE);
}

SlamCache::Node* SlamCache::popFreeObject(Slab* slab) {
#if BITS_32
  LockGuard<Spinlock> guard(m_FreeLock);
  Node* head = slab->freeHead;
  if (head) {
    slab->freeHead = head->next;
    __atomic_fetch_sub(&slab->freeObjects, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
  }
  return head;
#else
  if (!__atomic_load_n(&slab->freeObjects, __ATOMIC_ACQUIRE))
    return nullptr;

  Node* taggedHead = __atomic_load_n(&slab->freeHead, ATOMIC_POP_MEMORY_ORDER);
  // An empty head still carries an ABA tag; the count can lag behind a pop.
  while (reinterpret_cast<uintptr_t>(taggedHead) & POINTER_MASK) {
    Node* head = untagged(taggedHead);
    Node* next = head->next;
    if (__atomic_compare_exchange_n(&slab->freeHead, &taggedHead, next_tag(next, taggedHead),
                                    ATOMIC_CAS_WEAK, ATOMIC_POP_MEMORY_ORDER,
                                    ATOMIC_POP_FAILURE_MEMORY_ORDER)) {
      __atomic_fetch_sub(&slab->freeObjects, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
      return head;
    }
    if (!__atomic_load_n(&slab->freeObjects, __ATOMIC_ACQUIRE))
      return nullptr;
  }

  return nullptr;
#endif
}

void SlamCache::pushFreeObject(Slab* slab, Node* node) {
#if BITS_32
  LockGuard<Spinlock> guard(m_FreeLock);
  node->next = slab->freeHead;
  slab->freeHead = node;
  __atomic_fetch_add(&slab->freeObjects, static_cast<size_t>(1), __ATOMIC_RELEASE);
#else
  Node* head = __atomic_load_n(&slab->freeHead, __ATOMIC_RELAXED);
  do {
    node->next = head;
  } while (!__atomic_compare_exchange_n(&slab->freeHead, &head, next_tag(node, head),
                                        ATOMIC_CAS_WEAK, ATOMIC_PUSH_MEMORY_ORDER,
                                        __ATOMIC_RELAXED));
  __atomic_fetch_add(&slab->freeObjects, static_cast<size_t>(1), __ATOMIC_RELEASE);
#endif
}

SlamCache::Node* SlamCache::objectAt(uintptr_t slab, size_t index) const {
  return reinterpret_cast<Node*>(slab + m_SlabObjectOffset + (index * m_ObjectSize));
}

SlamCache::Slab* SlamCache::slabForObject(uintptr_t object) const {
  assert(m_ObjectSize < getPageSize());
  return reinterpret_cast<Slab*>(object & ~(getPageSize() - 1));
}

uintptr_t SlamCache::allocate() {
  EMIT_IF(EVERY_ALLOCATION_IS_A_SLAB) {
    return getSlab();
  }

  EMIT_IF(SLABS_FOR_HUGE_ALLOCS) {
    if (m_ObjectSize >= getPageSize()) {
      // just return a big-enough slab - allocation is page-sized or bigger
      return getSlab();
    }
  }

  if (m_ObjectSize >= getPageSize()) {
    {
      LockGuard<Spinlock> guard(m_RecoveryLock);
      if (m_LargeFreeList) {
        Node* node = m_LargeFreeList;
        m_LargeFreeList = node->next;
        EMIT_IF(USING_MAGIC) {
          assert(node->magic == MAGIC_VALUE);
          node->magic = TEMP_MAGIC;
        }
        reinterpret_cast<SlamAllocator::AllocHeader*>(node)->cache = this;
        return reinterpret_cast<uintptr_t>(node);
      }
    }
    return reinterpret_cast<uintptr_t>(initialiseSlab(getSlab()));
  }

  const size_t thisList = currentList();
  Node* N = nullptr;
  Slab* fastSlab = nullptr;
  const bool fastPath = beginFastPath();
  if (fastPath) {
    fastSlab = __atomic_load_n(&m_FastSlabs[thisList], __ATOMIC_ACQUIRE);
    if (fastSlab) {
      N = popFreeObject(fastSlab);
      if (N && !__atomic_load_n(&fastSlab->freeObjects, __ATOMIC_ACQUIRE))
        __atomic_store_n(&m_FastSlabs[thisList], static_cast<Slab*>(nullptr), __ATOMIC_RELEASE);
    }
    endFastPath();
  }

  if (N && fastSlab && !__atomic_load_n(&fastSlab->freeObjects, __ATOMIC_ACQUIRE)) {
    LockGuard<Spinlock> guard(m_RecoveryLock);
    if (!__atomic_load_n(&fastSlab->freeObjects, __ATOMIC_ACQUIRE) && fastSlab->onList)
      removeSlab(fastSlab);
  }

  if (N) {
    assert(N->next != reinterpret_cast<Node*>(VIGILANT_MAGIC));
    EMIT_IF(USING_MAGIC) {
      assert(N->magic == TEMP_MAGIC || N->magic == MAGIC_VALUE);
      N->magic = TEMP_MAGIC;
    }
    reinterpret_cast<SlamAllocator::AllocHeader*>(N)->cache = this;
  }

  if (!N) {
    LockGuard<Spinlock> guard(m_RecoveryLock);
    for (size_t offset = 0; offset < NUM_LISTS; ++offset) {
      const size_t list = (thisList + offset) % NUM_LISTS;
      if (__atomic_load_n(&m_FastSlabs[list], __ATOMIC_ACQUIRE))
        continue;
      for (Slab* slab = m_PartialLists[list]; slab; slab = slab->next) {
        if (!__atomic_load_n(&slab->freeObjects, __ATOMIC_ACQUIRE))
          continue;
        N = popFreeObject(slab);
        if (!N)
          continue;
        if (!__atomic_load_n(&slab->freeObjects, __ATOMIC_ACQUIRE))
          removeSlab(slab);
        else if (list == thisList)
          __atomic_store_n(&m_FastSlabs[thisList], slab, __ATOMIC_RELEASE);
        break;
      }
      if (N)
        break;
    }

    if (N) {
      // Check that the block was indeed free.
      assert(N->next != reinterpret_cast<Node*>(VIGILANT_MAGIC));
      EMIT_IF(USING_MAGIC) {
        assert(N->magic == TEMP_MAGIC || N->magic == MAGIC_VALUE);
        N->magic = TEMP_MAGIC;
      }
      SlamAllocator::AllocHeader* header = reinterpret_cast<SlamAllocator::AllocHeader*>(N);
      header->cache = this;
    }
  }

  // No CPU-local list had a free object. Allocate a new slab without holding
  // the cache lock across physical-memory and page-table work.
  if (UNLIKELY(!N)) {
    Node* pNode = initialiseSlab(getSlab());
    uintptr_t slab = reinterpret_cast<uintptr_t>(pNode);
    EMIT_IF(CRIPPLINGLY_VIGILANT) {
      if (m_pParentAllocator->getVigilance())
        trackSlab(slab);
    }
    return slab;
  }

  return reinterpret_cast<uintptr_t>(N);
}

void SlamCache::free(uintptr_t object) {
  EMIT_IF(EVERY_ALLOCATION_IS_A_SLAB) {
    // Free the slab in the address space, but don't return it to the allocator.
    size_t numPages = m_SlabSize / getPageSize();
    if (m_SlabSize % getPageSize()) {
      ++numPages;
    }
    object = object & ~(getPageSize() - 1);
    for (size_t i = 0; i < numPages; ++i) {
      unmap(reinterpret_cast<void*>(object + (i * getPageSize())));
    }

    return;
  }

  EMIT_IF(SLABS_FOR_HUGE_ALLOCS) {
    if (m_ObjectSize >= getPageSize()) {
      // just free the object directly, it's an entire slab
      freeSlab(object);
      return;
    }
  }

  Node* N = reinterpret_cast<Node*>(object);

  EMIT_IF(OVERRUN_CHECK) {
    // Grab the footer and check it.
    SlamAllocator::AllocFooter* pFoot = reinterpret_cast<SlamAllocator::AllocFooter*>(
        object + m_ObjectSize - sizeof(SlamAllocator::AllocFooter));
    assert(pFoot->magic == VIGILANT_MAGIC);
  }

  SlamAllocator::AllocHeader* pHeader = reinterpret_cast<SlamAllocator::AllocHeader*>(object);
  assert(pHeader->cache == this);
  pHeader->cache = nullptr;

  EMIT_IF(USING_MAGIC) {
    // Possible double free?
    assert(N->magic != MAGIC_VALUE);
    N->magic = MAGIC_VALUE;
  }

  if (m_ObjectSize >= getPageSize()) {
    LockGuard<Spinlock> guard(m_RecoveryLock);
    N->next = m_LargeFreeList;
    m_LargeFreeList = N;
    return;
  }

  Slab* slab = slabForObject(object);
  const bool fastPath = beginFastPath();
  if (fastPath) {
    if (__atomic_load_n(&slab->freeObjects, __ATOMIC_ACQUIRE)) {
      pushFreeObject(slab, N);
      endFastPath();
      return;
    }
    endFastPath();
  }

  if (!fastPath) {
    LockGuard<Spinlock> guard(m_RecoveryLock);
    pushFreeObject(slab, N);
    if (!slab->onList)
      addSlab(slab, slab->list);
    if (slab->list == currentList())
      __atomic_store_n(&m_FastSlabs[slab->list], slab, __ATOMIC_RELEASE);
    return;
  }

  if (!__atomic_load_n(&slab->freeObjects, __ATOMIC_ACQUIRE)) {
    LockGuard<Spinlock> guard(m_RecoveryLock);
    if (!__atomic_load_n(&slab->freeObjects, __ATOMIC_ACQUIRE)) {
      pushFreeObject(slab, N);
      if (!slab->onList)
        addSlab(slab, slab->list);
      if (slab->list == currentList())
        __atomic_store_n(&m_FastSlabs[slab->list], slab, __ATOMIC_RELEASE);
      return;
    }
  }
  pushFreeObject(slab, N);
}

bool SlamCache::isPointerValid(uintptr_t object) const {
  EMIT_IF(SLABS_FOR_HUGE_ALLOCS) {
    if (m_ObjectSize >= getPageSize()) {
      /// \todo need to figure out how to do this
      return true;
    }
  }

  Node* N = reinterpret_cast<Node*>(object);
  EMIT_IF(OVERRUN_CHECK) {
    // Grab the footer and check it.
    SlamAllocator::AllocFooter* pFoot = reinterpret_cast<SlamAllocator::AllocFooter*>(
        object + m_ObjectSize - sizeof(SlamAllocator::AllocFooter));
    if (pFoot->magic != VIGILANT_MAGIC) {
      return false;
    }
  }

  EMIT_IF(USING_MAGIC) {
    // Possible double free?
    if (N->magic == MAGIC_VALUE) {
      EMIT_IF(VERBOSE_ISPOINTERVALID) {
        WARNING("SlamCache::isPointerValid: memory " << Hex << object << " has invalid magic ("
                                                     << N->magic << " != " << MAGIC_VALUE << ").");
      }
      return false;
    }
  }

  return true;
}

uintptr_t SlamCache::getSlab() {
  return m_pParentAllocator->getSlab(m_SlabSize);
}

void SlamCache::freeSlab(uintptr_t slab) {
  m_pParentAllocator->freeSlab(slab, m_SlabSize);
}

size_t SlamCache::recovery(size_t maxSlabs) {
  if (!maxSlabs) {
    return 0;
  }

  EMIT_IF(EVERY_ALLOCATION_IS_A_SLAB) {
    return 0;
  }

  EMIT_IF(SLABS_FOR_HUGE_ALLOCS) {
    if (m_ObjectSize >= getPageSize()) {
      // Caches with slabs page-sized or bigger don't hold onto freed regions
      return 0;
    }
  }

  constexpr size_t writer = static_cast<size_t>(1) << ((sizeof(size_t) * 8) - 1);
  size_t expected = 0;
  while (true) {
    expected = __atomic_load_n(&m_FastPathState, __ATOMIC_ACQUIRE);
    if (expected & writer) {
      spin_pause();
      continue;
    }
    const size_t requested = expected | writer;
    if (__atomic_compare_exchange_n(&m_FastPathState, &expected, requested, false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
      break;
  }
  while (__atomic_load_n(&m_FastPathState, __ATOMIC_ACQUIRE) != writer)
    spin_pause();

  LockGuard<Spinlock> guard(m_RecoveryLock);

  size_t freedSlabs = 0;
  if (m_ObjectSize < getPageSize()) {
    for (size_t list = 0; list < NUM_LISTS && freedSlabs < maxSlabs; ++list) {
      Slab* slab = m_PartialLists[list];
      while (slab && freedSlabs < maxSlabs) {
        Slab* next = slab->next;
        if (__atomic_load_n(&slab->freeObjects, __ATOMIC_ACQUIRE) == slab->objectCount) {
          if (__atomic_load_n(&m_FastSlabs[slab->list], __ATOMIC_ACQUIRE) == slab)
            __atomic_store_n(&m_FastSlabs[slab->list], static_cast<Slab*>(nullptr),
                             __ATOMIC_RELEASE);
          removeSlab(slab);
          freeSlab(reinterpret_cast<uintptr_t>(slab));
          ++freedSlabs;
        }
        slab = next;
      }
    }
  } else {
    while (m_LargeFreeList && freedSlabs < maxSlabs) {
      Node* node = m_LargeFreeList;
      m_LargeFreeList = node->next;
      freeSlab(reinterpret_cast<uintptr_t>(node));
      ++freedSlabs;
    }
  }

  __atomic_store_n(&m_FastPathState, static_cast<size_t>(0), __ATOMIC_RELEASE);
  return freedSlabs;
}

SlamCache::Node* SlamCache::initialiseSlab(uintptr_t slab) {
  EMIT_IF(SLABS_FOR_HUGE_ALLOCS) {
    if (m_ObjectSize >= getPageSize()) {
      return nullptr;
    }
  }

  if (m_ObjectSize >= getPageSize()) {
    LockGuard<Spinlock> guard(m_RecoveryLock);
    reinterpret_cast<SlamAllocator::AllocHeader*>(slab)->cache = this;
    m_pParentAllocator->markSlabReady(slab, m_SlabSize);
    return reinterpret_cast<Node*>(slab);
  }

  const size_t nObjects = m_SlabObjectCount;
  Slab* slabState = reinterpret_cast<Slab*>(slab);
  slabState->freeHead = nullptr;
  slabState->next = nullptr;
  slabState->previous = nullptr;
  slabState->cache = this;
  slabState->freeObjects = nObjects - 1;
  slabState->objectCount = nObjects;
  slabState->onList = false;

  Node* N = objectAt(slab, 0);
  EMIT_IF(USING_MAGIC) {
    N->magic = TEMP_MAGIC;
  }
  for (size_t i = 1; i < nObjects; i++) {
    Node* pNode = objectAt(slab, i);
    pNode->next = slabState->freeHead;
    slabState->freeHead = next_tag(pNode, slabState->freeHead);
    EMIT_IF(USING_MAGIC) {
      pNode->magic = MAGIC_VALUE;
    }
    reinterpret_cast<SlamAllocator::AllocHeader*>(pNode)->cache = nullptr;
  }

  {
    LockGuard<Spinlock> guard(m_RecoveryLock);
    reinterpret_cast<SlamAllocator::AllocHeader*>(N)->cache = this;
    // Even an initially full slab can be cached and later recovered.
    slabState->list = currentList();
    if (slabState->freeObjects)
      addSlab(slabState, slabState->list);
    __atomic_store_n(&m_FastSlabs[slabState->list], slabState, __ATOMIC_RELEASE);
    m_pParentAllocator->markSlabReady(slab, m_SlabSize);
  }

  return N;
}

static Spinlock rarp;

void SlamCache::check() {
  if (m_ObjectSize >= getPageSize()) {
    return;
  }

  EMIT_IF(!HOSTED) {
    if (!Machine::instance().isInitialised() || Processor::m_Initialised != 2)
      return;
  }
  if (m_ObjectSize == 0)
    return;
  rarp.acquire();

  size_t nObjects = m_SlabObjectCount;

  size_t maxPerSlab = (m_SlabSize / sizeof(uintptr_t)) - 2;

  uintptr_t curSlab = m_FirstSlab;
  while (true) {
    if (!curSlab) {
      rarp.release();
      return;
    }
    uintptr_t numAlloced = *reinterpret_cast<uintptr_t*>(curSlab);
    uintptr_t next = *reinterpret_cast<uintptr_t*>(curSlab + sizeof(uintptr_t));

    for (size_t i = 0; i < numAlloced; i++) {
      uintptr_t slab = *reinterpret_cast<uintptr_t*>(curSlab + sizeof(uintptr_t) * (i + 2));
      for (size_t i = 0; i < nObjects; i++) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(objectAt(slab, i));
        Node* pNode = reinterpret_cast<Node*>(addr);
        if (pNode->magic == MAGIC_VALUE || pNode->magic == TEMP_MAGIC)
          // Free, continue.
          continue;
        SlamAllocator::AllocHeader* pHead = reinterpret_cast<SlamAllocator::AllocHeader*>(addr);
        SlamAllocator::AllocFooter* pFoot = reinterpret_cast<SlamAllocator::AllocFooter*>(
            addr + m_ObjectSize - sizeof(SlamAllocator::AllocFooter));
        if (pHead->magic != VIGILANT_MAGIC) {
          ERROR("Possible heap underrun: object starts at "
                << addr << ", size: " << m_ObjectSize
                << ", block: " << (addr + sizeof(SlamAllocator::AllocHeader)));
        }
        if (pFoot->magic != VIGILANT_MAGIC) {
          ERROR("Possible heap overrun: object starts at " << addr);
          assert(false);
        }
      }
    }
    if (numAlloced == maxPerSlab)
      curSlab = next;
    else
      break;
  }
  rarp.release();
}

void SlamCache::trackSlab(uintptr_t slab) {
  EMIT_IF(!HOSTED) {
    if (!Machine::instance().isInitialised() || Processor::m_Initialised != 2)
      return;
  }
  if (m_ObjectSize == 0)
    return;

  if (!m_FirstSlab) {
    m_FirstSlab = getSlab();
    uintptr_t* numAlloced = reinterpret_cast<uintptr_t*>(m_FirstSlab);
    uintptr_t* next = reinterpret_cast<uintptr_t*>(m_FirstSlab + sizeof(uintptr_t));
    *numAlloced = 0;
    *next = 0;
  }

  size_t maxPerSlab = (m_SlabSize / sizeof(uintptr_t)) - 2;

  uintptr_t curSlab = m_FirstSlab;
  while (true) {
    uintptr_t* numAlloced = reinterpret_cast<uintptr_t*>(curSlab);
    uintptr_t* next = reinterpret_cast<uintptr_t*>(curSlab + sizeof(uintptr_t));

    if (*numAlloced < maxPerSlab) {
      uintptr_t* p = reinterpret_cast<uintptr_t*>(curSlab + (*numAlloced + 2) * sizeof(uintptr_t));
      *p = slab;
      *numAlloced = *numAlloced + 1;
      return;
    }

    if (*next)
      curSlab = *next;
    else {
      uintptr_t newSlab = getSlab();
      *next = newSlab;
      curSlab = newSlab;

      uintptr_t* numAlloced = reinterpret_cast<uintptr_t*>(curSlab);
      uintptr_t* next = reinterpret_cast<uintptr_t*>(curSlab + sizeof(uintptr_t));
      *numAlloced = 0;
      *next = 0;
    }
  }
}

SlamAllocator::SlamAllocator()
    : m_bInitialised(false),
      m_bVigilant(false),
      m_SlabRegionLock(false, true),
      m_HeapPageCount(0),
      m_SlabRegionBitmap(),
      m_SlabRegionBitmapEntries(0),
      m_SlabRegionPages(0),
      m_Base(0) {}

SlamAllocator::~SlamAllocator() {
  if (m_bInitialised) {
    // wipe();
  }
}

void SlamAllocator::initialise() {
  LockGuard<Spinlock> guard(m_SlabRegionLock);

  if (m_bInitialised) {
    return;
  }

  // We need to allocate our bitmap for this purpose.
  uintptr_t bitmapBase = getHeapBase();
  uintptr_t heapEnd = getHeapEnd();
  size_t heapSize = heapEnd - bitmapBase;
  size_t heapPages = heapSize / getPageSize();
  size_t bitmapBytes = ((heapPages + 63) / 64) * sizeof(uint64_t) * 3;

  // Ensure the bitmap size is now page-aligned before we allocate it.
  if (bitmapBytes & (getPageSize() - 1)) {
    bitmapBytes &= ~(getPageSize() - 1);
    bitmapBytes += getPageSize();
  }

  // Keep reservation and mapping state adjacent. The bitmap is mostly CoW;
  // interleaving ensures the first mapping-state write is in the same early,
  // private page as its reservation state.
  m_SlabRegionBitmap.useMemory(reinterpret_cast<void*>(bitmapBase), (heapPages + 63) / 64,
                               heapPages);
  m_Base = bitmapBase + bitmapBytes;
  m_SlabRegionPages = (heapEnd - m_Base) / getPageSize();
  m_SlabRegionBitmapEntries = (m_SlabRegionPages + 63) / 64;

  // Allocate bitmap.
  size_t numPages = 0;
  for (uintptr_t addr = bitmapBase; addr < m_Base; addr += getPageSize()) {
    // Don't CoW the first 32 pages so we have some slabs on hand for
    // startup before CoW is viable
    bool cowOk = numPages++ >= 32;
    allocateAndMapAt(reinterpret_cast<void*>(addr), cowOk);
    if (!cowOk) {
      ByteSet(reinterpret_cast<void*>(addr), 0, getPageSize());
    }
  }

  EMIT_IF(!PEDIGREE_BENCHMARK) {
    NOTICE("Kernel heap range prepared from " << Hex << m_Base << " to " << heapEnd
                                              << ", size: " << (heapEnd - m_Base));
    DEBUG_LOG("  -> kernel heap bitmap is " << Dec << (bitmapBytes / 1024) << Hex << "K");
  }

  for (size_t i = 0; i < 32; i++) {
    m_Caches[i].initialise(this, 1ULL << i);
  }

  NOTICE("all caches init-ed");

  m_bInitialised = true;
}

void SlamAllocator::clearAll() {
  EMIT_IF(PEDIGREE_BENCHMARK) {
    wipe();
    initialise();
  }
}

void SlamAllocator::wipe() {
  if (!m_bInitialised) {
    return;
  }

  if (!m_SlabRegionPages) {
    return;
  }

  LockGuard<Spinlock> guard(m_SlabRegionLock);

  m_bInitialised = false;

  // Clean up all slabs we obtained.
  for (size_t entry = 0; entry < m_SlabRegionBitmapEntries; ++entry) {
    if (!m_SlabRegionBitmap.reservedBits(entry)) {
      continue;
    }

    for (size_t bit = 0; bit < 64; ++bit) {
      uint64_t test = 1ULL << bit;
      if ((m_SlabRegionBitmap.reservedBits(entry) & test) == 0) {
        continue;
      }

      uintptr_t slab = m_Base + (((entry * 64) + bit) * getPageSize());
      freeSlabUnlocked(slab, getPageSize());
    }
  }

  // about to destroy the bitmap mappings
  m_SlabRegionBitmap.useMemory(nullptr, 0, 0);
  m_SlabRegionBitmapEntries = 0;
  m_SlabRegionPages = 0;

  // Clean up the bitmap.
  for (uintptr_t addr = getHeapBase(); addr < m_Base; addr += getPageSize()) {
    unmap(reinterpret_cast<void*>(addr));
  }
}

uintptr_t SlamAllocator::getSlab(size_t fullSize) {
  if (fullSize < getPageSize() || (fullSize % getPageSize())) {
    panic("Attempted to get a slab smaller than the native page size.");
  }
  size_t nPages = fullSize / getPageSize();

  SlamBitmap& bitmap = m_SlabRegionBitmap;

  auto findFreeRun = [&]() { return bitmap.findFreeRun(nPages); };

#if X64 && !PEDIGREE_BENCHMARK
  auto firstCowBitmapPage = [&](size_t pageIndex) {
    const size_t firstEntry = pageIndex / 64;
    const size_t lastEntry = (pageIndex + nPages - 1) / 64;
    const uintptr_t firstAddress = bitmap.metadataAddress(firstEntry) & ~(getPageSize() - 1);
    const uintptr_t lastEntryByte = bitmap.metadataAddress(lastEntry) + sizeof(uint64_t) * 3 - 1;
    const uintptr_t lastAddress = lastEntryByte & ~(getPageSize() - 1);
    VirtualAddressSpace& va = VirtualAddressSpace::getKernelAddressSpace();
    for (uintptr_t address = firstAddress; address <= lastAddress; address += getPageSize()) {
      physical_uintptr_t physical = 0;
      size_t flags = 0;
      va.getMapping(reinterpret_cast<void*>(address), physical, flags);
      if (flags & VirtualAddressSpace::CopyOnWrite) {
        return address;
      }
    }
    return static_cast<uintptr_t>(0);
  };
#endif

  size_t pageIndex = ~0UL;
  while (true) {
    m_SlabRegionLock.acquire();
    pageIndex = findFreeRun();
    if (pageIndex == ~0UL) {
      m_SlabRegionLock.release();
      panic("SlamAllocator cannot find contiguous virtual heap space.");
    }

#if X64 && !PEDIGREE_BENCHMARK
    if (firstCowBitmapPage(pageIndex)) {
      m_SlabRegionLock.release();
      physical_uintptr_t replacement = PhysicalMemoryManager::instance().allocatePage();

      m_SlabRegionLock.acquire();
      pageIndex = findFreeRun();
      if (pageIndex == ~0UL) {
        m_SlabRegionLock.release();
        PhysicalMemoryManager::instance().freePage(replacement);
        panic("SlamAllocator cannot find contiguous virtual heap space.");
      }

      const uintptr_t cowAddress = firstCowBitmapPage(pageIndex);
      if (cowAddress) {
        VirtualAddressSpace& va = VirtualAddressSpace::getKernelAddressSpace();
        physical_uintptr_t oldPhysical = 0;
        size_t flags = 0;
        va.getMapping(reinterpret_cast<void*>(cowAddress), oldPhysical, flags);
        MemoryCopy(reinterpret_cast<void*>(physicalAddress(replacement)),
                   reinterpret_cast<void*>(cowAddress), getPageSize());
        va.unmap(reinterpret_cast<void*>(cowAddress));
        flags |= VirtualAddressSpace::Write;
        flags &= ~VirtualAddressSpace::CopyOnWrite;
        if (!va.map(replacement, reinterpret_cast<void*>(cowAddress), flags)) {
          panic("SlamAllocator could not privatise bitmap metadata.");
        }
        PhysicalMemoryManager::instance().freePage(oldPhysical);
        m_SlabRegionLock.release();
        continue;
      }

      bitmap.reserve(pageIndex, nPages);
      m_HeapPageCount += nPages;
      m_SlabRegionLock.release();
      PhysicalMemoryManager::instance().freePage(replacement);
      break;
    }
#endif

    bitmap.reserve(pageIndex, nPages);
    m_HeapPageCount += nPages;
    m_SlabRegionLock.release();
    break;
  }

  const uintptr_t slab = m_Base + (pageIndex * getPageSize());

#if defined(PEDIGREE_BUILDUTILS)
  if (m_SlabTransitionHook) {
    m_SlabTransitionHook(SlabTransitionForTest::Reserved, slab, m_SlabTransitionHookContext);
  }
#endif

  for (size_t i = 0; i < nPages; ++i) {
    void* p = reinterpret_cast<void*>(slab + (i * getPageSize()));
    allocateAndMapAt(p);
  }

  m_SlabRegionLock.acquire();
  for (size_t i = 0; i < nPages; ++i) {
    size_t currentPage = pageIndex + i;
    bitmap.setMapped(currentPage);
  }
  m_SlabRegionLock.release();

#if defined(PEDIGREE_BUILDUTILS)
  if (m_SlabTransitionHook) {
    m_SlabTransitionHook(SlabTransitionForTest::Mapped, slab, m_SlabTransitionHookContext);
  }
#endif

  return slab;
}

void SlamAllocator::markSlabReady(uintptr_t address, size_t length) {
  if (length < getPageSize() || (length % getPageSize()) || (address % getPageSize()) ||
      address < m_Base || address >= getHeapEnd() || length > (getHeapEnd() - address)) {
    panic("Attempted to publish an invalid slab.");
  }

  const size_t firstPage = (address - m_Base) / getPageSize();
  const size_t nPages = length / getPageSize();
  LockGuard<Spinlock> guard(m_SlabRegionLock);
  for (size_t i = 0; i < nPages; ++i) {
    const size_t currentPage = firstPage + i;
    const uint64_t bit = 1ULL << (currentPage % 64);
    if (!m_SlabRegionBitmap.isReserved(currentPage) || !m_SlabRegionBitmap.isMapped(currentPage)) {
      panic("Attempted to publish an unmapped slab.");
    }
    m_SlabRegionBitmap.setReady(currentPage);
  }
}

void SlamAllocator::freeSlab(uintptr_t address, size_t length) {
  LockGuard<Spinlock> guard(m_SlabRegionLock);

  freeSlabUnlocked(address, length);
}

void SlamAllocator::freeSlabUnlocked(uintptr_t address, size_t length) {
  if (length < getPageSize() || (length % getPageSize()) || (address % getPageSize()) ||
      address < m_Base || address >= getHeapEnd() || length > (getHeapEnd() - address)) {
    panic("Attempted to free an invalid slab.");
  }
  size_t nPages = length / getPageSize();
  size_t firstPage = (address - m_Base) / getPageSize();
  SlamBitmap& bitmap = m_SlabRegionBitmap;
  if (firstPage >= m_SlabRegionPages || nPages > (m_SlabRegionPages - firstPage)) {
    panic("Attempted to free a slab outside the allocator bitmap.");
  }

  for (size_t i = 0; i < nPages; ++i) {
    size_t currentPage = firstPage + i;
    const uint64_t bit = 1ULL << (currentPage % 64);
    if (!bitmap.isReserved(currentPage) || !bitmap.isMapped(currentPage)) {
      panic("Attempted to free an unallocated slab.");
    }
  }

  // Perform unmapping first (so we can just modify 'address').
  for (uintptr_t base = address; base < (address + length); base += getPageSize()) {
    void* p = reinterpret_cast<void*>(base);
    unmap(p);
  }

#if defined(PEDIGREE_BUILDUTILS)
  if (m_SlabTransitionHook) {
    m_SlabTransitionHook(SlabTransitionForTest::Unmapped, address, m_SlabTransitionHookContext);
  }
#endif

  // Clear the reservation while validation remains excluded by
  // m_SlabRegionLock.
  for (size_t i = 0; i < nPages; ++i) {
    size_t currentPage = firstPage + i;
    const uint64_t bit = 1ULL << (currentPage % 64);
    bitmap.release(currentPage, 1);
  }

  m_HeapPageCount -= nPages;
}

size_t SlamAllocator::recovery(size_t maxSlabs) {
  size_t nSlabs = 0;
  size_t nPages = 0;

  for (size_t i = 0; i < 32; ++i) {
    // Things without slabs don't get recovered.
    if (!m_Caches[i].slabSize())
      continue;

    size_t thisSlabs = m_Caches[i].recovery(maxSlabs - nSlabs);
    nPages += (thisSlabs * m_Caches[i].slabSize()) / getPageSize();
    nSlabs += thisSlabs;
    if (nSlabs >= maxSlabs) {
      break;
    }
  }

  return nPages;
}

uintptr_t SlamAllocator::allocate(size_t nBytes) {
  EMIT_IF(!PEDIGREE_BENCHMARK) {
    if (!Processor::guardDeviceHardIrqOperation(DeviceHardIrqOperation::HeapAllocate)) {
      return 0;
    }
  }

  EMIT_IF(HOSTED_SYSTEM_MALLOC) {
    FATAL_NOLOCK("SlamAllocator::allocate() called when HOSTED_SYSTEM_MALLOC == 1");
  }

  EMIT_IF(DEBUGGING_SLAB_ALLOCATOR) {
    NOTICE_NOLOCK("SlabAllocator::allocate(" << Dec << nBytes << Hex << ")");
  }

  ConstexprLockGuard<Spinlock, SLAM_LOCKED> guard(m_Lock);

  if (UNLIKELY(!m_bInitialised))
    initialise();

  EMIT_IF(CRIPPLINGLY_VIGILANT) {
    if (m_bVigilant) {
      for (int i = 0; i < 32; i++) {
        m_Caches[i].check();
      }
    }
  }

  size_t origSize = nBytes;

  // Return value.
  uintptr_t ret = 0;

  // Don't allow huge allocations.
  /// \note Even 2G is a stretch on most systems. Use some other allocator
  ///       to allocate such large buffers.
  const size_t framing = sizeof(AllocHeader) + sizeof(AllocFooter);
  if (nBytes >= (1ULL << 31) || nBytes > ((1ULL << 31) - 1 - framing)) {
    ERROR("SlamAllocator: massive allocation: " << origSize);
    panic("SlamAllocator allocation size is too large.");
  }

  nBytes += framing;

  // Default to minimum object size if we must.
  size_t lg2 = 0;
  if (UNLIKELY(nBytes < OBJECT_MINIMUM_SIZE)) {
    nBytes = OBJECT_MINIMUM_SIZE;
  }

  // log2 of nBytes, where nBytes is rounded up to the next power-of-two.
  lg2 = 32 - __builtin_clz(static_cast<unsigned int>(nBytes - 1));
  nBytes = 1ULL << lg2;  // Round up nBytes now.
  ret = m_Caches[lg2].allocate();

  EMIT_IF(WARN_PAGE_SIZE_OR_LARGER) {
    // Does the allocation fit inside a slab?
    // NOTE: use something else to allocate 4K or more.
    if (nBytes >= getPageSize()) {
#if __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
#endif
      // return address of operator new()
      void* ret0 = __builtin_return_address(0);
      void* ret1 = __builtin_return_address(1);
      ERROR("alloc of " << origSize << " rounded to " << nBytes << " exceeds page size [at " << ret0
                        << " " << ret1 << "]!");
#if __GNUC__
#pragma GCC diagnostic pop
#endif
    }
  }

  EMIT_IF(DEBUGGING_SLAB_ALLOCATOR) {
    if (UNLIKELY(!ret)) {
      ERROR_NOLOCK("SlabAllocator::allocate: Allocation failed (" << Dec << nBytes << Hex
                                                                  << " bytes)");
      return ret;
    }
  }
  else {
    assert(ret != 0);
  }

  // Shove some data on the front that we'll use later
  AllocHeader* head = reinterpret_cast<AllocHeader*>(ret);
  AllocFooter* foot = reinterpret_cast<AllocFooter*>(ret + nBytes - sizeof(AllocFooter));
  ret += sizeof(AllocHeader);

  // Set up the header
  head->cache = &m_Caches[lg2];
  EMIT_IF(OVERRUN_CHECK) {
    head->magic = VIGILANT_MAGIC;
    foot->magic = VIGILANT_MAGIC;

    EMIT_IF(VIGILANT_OVERRUN_CHECK) {
      // safe cast in this case as there's an inheritance here
      auto vigilantHead = reinterpret_cast<AllocHeader_VigilantOverrunCheck*>(head);
      if (Processor::m_Initialised == 2) {
        Backtrace bt;
        bt.performBpBacktrace(0, 0);
        MemoryCopy(&vigilantHead->backtrace, bt.m_pReturnAddresses,
                   SLAM_BT_FRAMES * sizeof(uintptr_t));
        vigilantHead->requested = nBytes;
        g_SlamCommand.addAllocation(vigilantHead->backtrace, vigilantHead->requested);
      }
    }
  }

  EMIT_IF(THREADS) {
    if (Processor::m_Initialised == 2) {
      Thread* pThread = Processor::information().getCurrentThread();
      if (pThread) {
        pThread->getParent()->trackHeap(nBytes);
      }
    }
  }

  EMIT_IF(MEMORY_TRACING) {
    traceAllocation(reinterpret_cast<void*>(ret), MemoryTracing::Allocation, origSize);
  }

  return ret;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
uintptr_t SlamAllocator::guardedAllocateForTest(size_t nBytes) {
  return instance().allocate(nBytes);
}

void SlamAllocator::guardedFreeForTest(uintptr_t mem) {
  instance().free(mem);
}
#endif

size_t SlamAllocator::allocSize(uintptr_t mem) {
  if (!mem)
    return 0;

  // Grab the header
  AllocHeader* head = reinterpret_cast<AllocHeader*>(mem - sizeof(AllocHeader));

  // If the cache is null, then the pointer is corrupted.
  assert(head->cache != 0);
  size_t result = head->cache->objectSize();

  // Remove size of header/footer.
  // This is important as we're returning the size of each object itself,
  // but we return memory framed by headers and footers. So, the "true" size
  // of memory pointed to by 'mem' is not the true object size.
  return result - (sizeof(AllocHeader) + sizeof(AllocFooter));
}

bool SlamAllocator::isAllocatedPage(uintptr_t address) const {
  if (!m_bInitialised || !m_SlabRegionPages || address < m_Base || address >= getHeapEnd()) {
    return false;
  }

  size_t page = (address - m_Base) / getPageSize();
  if (page >= m_SlabRegionPages) {
    return false;
  }

  const uint64_t bit = 1ULL << (page % 64);
  return m_SlabRegionBitmap.isReserved(page) && m_SlabRegionBitmap.isMapped(page) &&
         m_SlabRegionBitmap.isReady(page);
}

#if defined(PEDIGREE_BUILDUTILS)
void SlamAllocator::setSlabTransitionHookForTest(SlabTransitionHookForTest hook, void* context) {
  LockGuard<Spinlock> guard(m_SlabRegionLock);
  m_SlabTransitionHook = hook;
  m_SlabTransitionHookContext = context;
}
#endif

SlamAllocator& SlamAllocator::instance() {
  EMIT_IF(PEDIGREE_BENCHMARK) {
    static SlamAllocator instance;
    return instance;
  }
  else {
    return m_Instance;
  }
}

void SlamAllocator::free(uintptr_t mem) {
  EMIT_IF(!PEDIGREE_BENCHMARK) {
    if (!Processor::guardDeviceHardIrqOperation(DeviceHardIrqOperation::HeapFree)) {
      return;
    }
  }

#if DEBUGGING_SLAB_ALLOCATOR
  NOTICE_NOLOCK("SlabAllocator::free");
#endif

#if SLAM_LOCKED
  LockGuard<Spinlock> guard(m_Lock);
#endif

  // If we're not initialised, fix that
  if (UNLIKELY(!m_bInitialised))
    initialise();
  if (UNLIKELY(!mem))
    return;

#if CRIPPLINGLY_VIGILANT
  if (m_bVigilant)
    for (int i = 0; i < 32; i++)
      m_Caches[i].check();
#endif

// Ensure this pointer is even on the heap...
#if !PEDIGREE_BENCHMARK
  if (!Processor::information().getVirtualAddressSpace().memIsInKernelHeap(
          reinterpret_cast<void*>(mem)))
    FATAL_NOLOCK("SlamAllocator::free - given pointer '" << mem << "' was completely invalid.");
#endif

  // Grab the header
  AllocHeader* head = reinterpret_cast<AllocHeader*>(mem - sizeof(AllocHeader));

  // If the cache is null, then the pointer is corrupted.
  assert(head->cache != 0);
#if OVERRUN_CHECK
  assert(head->magic == VIGILANT_MAGIC);
  // Footer gets checked in SlamCache::free, as we don't know the object size.

#if VIGILANT_OVERRUN_CHECK
  if (Processor::m_Initialised == 2)
    g_SlamCommand.removeAllocation(head->backtrace, head->requested);
#endif
#endif

  SlamCache* pCache = head->cache;

// Scribble the freed buffer (both to avoid leaking information, and also
// to ensure anything using a freed object will absolutely fail).
#if SCRIBBLE_FREED_BLOCKS
  size_t size = pCache->objectSize() - sizeof(AllocHeader) - sizeof(AllocFooter);
  ByteSet(reinterpret_cast<void*>(mem), 0xAB, size);
#endif

#if THREADS
  if (Processor::m_Initialised == 2) {
    Thread* pThread = Processor::information().getCurrentThread();
    if (pThread) {
      pThread->getParent()->trackHeap(-pCache->objectSize());
    }
  }
#endif

  // Free now.
  pCache->free(mem - sizeof(AllocHeader));

#if MEMORY_TRACING
  traceAllocation(reinterpret_cast<void*>(mem), MemoryTracing::Free, 0);
#endif
}

bool SlamAllocator::isPointerValid(uintptr_t mem)
#if !SLAM_LOCKED
    const
#endif
{
#if DEBUGGING_SLAB_ALLOCATOR
  NOTICE_NOLOCK("SlabAllocator::isPointerValid");
#endif

#if SLAM_LOCKED
  LockGuard<Spinlock> guard(m_Lock);
#endif

  // Pin the slab mapping until all header, cache, and footer reads complete.
  // freeSlab takes the same lock across unmapping and bitmap retirement.
  LockGuard<Spinlock> slabGuard(m_SlabRegionLock);

  // If we're not initialised, fix that
  if (UNLIKELY(!m_bInitialised)) {
    return false;
  }

  // 0 is fine to free.
  if (!mem) {
    return true;
  }

// On the heap?
#if !PEDIGREE_BENCHMARK
  if (!Processor::information().getVirtualAddressSpace().memIsInKernelHeap(
          reinterpret_cast<void*>(mem))) {
#if VERBOSE_ISPOINTERVALID
    WARNING("SlamAllocator::isPointerValid: memory " << Hex << mem
                                                     << " is not in the heap region.");
#endif
    return false;
  }
#endif

  if (mem < (m_Base + sizeof(AllocHeader))) {
    return false;
  }

  uintptr_t headerAddress = mem - sizeof(AllocHeader);
  if (!isAllocatedPage(headerAddress)) {
    return false;
  }

#if CRIPPLINGLY_VIGILANT
  if (m_bVigilant)
    for (int i = 0; i < 32; i++)
      m_Caches[i].check();
#endif

  // Grab the header
  AllocHeader* head = reinterpret_cast<AllocHeader*>(headerAddress);

#if OVERRUN_CHECK
  if (head->magic != VIGILANT_MAGIC) {
#if VERBOSE_ISPOINTERVALID
    WARNING("SlamAllocator::isPointerValid: memory " << Hex << mem << " failed magic check ("
                                                     << head->magic << " != " << VIGILANT_MAGIC
                                                     << ").");
#endif
    return false;
  }
// Footer gets checked in SlamCache::free, as we don't know the object size.
#endif

  // If the cache is null, then the pointer is corrupted.
  if (head->cache == 0) {
#if VERBOSE_ISPOINTERVALID
    WARNING("SlamAllocator::isPointerValid: memory " << Hex << mem
                                                     << " does not reference a valid SlamCache.");
#endif
    return false;
  }

  // Check for a valid cache
  bool bValid = false;
  for (int i = 0; i < 32; i++) {
    if (head->cache == &m_Caches[i]) {
      bValid = true;
      break;
    }
  }

  if (!bValid) {
    WARNING_NOLOCK("SlamAllocator::isPointerValid - cache pointer '"
                   << reinterpret_cast<uintptr_t>(head->cache) << "' is invalid.");
    return false;
  }

  // Final validation.
  return head->cache->isPointerValid(mem - sizeof(AllocHeader));
}

bool SlamAllocator::isWithinHeap(uintptr_t mem) const {
#if !PEDIGREE_BENCHMARK
  if (!Processor::information().getVirtualAddressSpace().memIsInKernelHeap(
          reinterpret_cast<void*>(mem))) {
#if VERBOSE_ISPOINTERVALID
    WARNING("SlamAllocator::isWithinHeap: memory " << Hex << mem << " is not in the heap region.");
#endif
    return false;
  }
#endif

  return true;
}

bool _assert_ptr_valid(uintptr_t ptr) {
  return SlamAllocator::instance().isPointerValid(ptr);
}

#endif  // !SLAM_USE_DEBUG_ALLOCATOR
