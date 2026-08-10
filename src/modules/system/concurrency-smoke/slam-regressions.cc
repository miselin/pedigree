/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

namespace {
constexpr size_t ObjectSize = 128;
constexpr size_t ObjectCount = SLAB_MINIMUM_SIZE / ObjectSize;

void prepareAllocation(SlamCache& cache, uintptr_t object) {
  SlamAllocator::AllocHeader* header = reinterpret_cast<SlamAllocator::AllocHeader*>(object);
  header->cache = &cache;
#if OVERRUN_CHECK
  header->magic = VIGILANT_MAGIC;
  SlamAllocator::AllocFooter* footer = reinterpret_cast<SlamAllocator::AllocFooter*>(
      object + cache.objectSize() - sizeof(SlamAllocator::AllocFooter));
  footer->magic = VIGILANT_MAGIC;
#endif
}

struct RemoteFreeContext {
  RemoteFreeContext(SlamCache& cache, uintptr_t* objects, size_t count,
                    size_t allocatingProcessor)
      : cache(cache),
        objects(objects),
        count(count),
        allocatingProcessor(allocatingProcessor),
        processor(static_cast<size_t>(-1)) {}

  SlamCache& cache;
  uintptr_t* objects;
  size_t count;
  size_t allocatingProcessor;
  Atomic<size_t> processor;
};

int freeRemotely(void* parameter) {
  RemoteFreeContext* context = reinterpret_cast<RemoteFreeContext*>(parameter);
  const size_t processor = Processor::index();
  if (processor == context->allocatingProcessor ||
      !context->processor.compareAndSwap(static_cast<size_t>(-1), processor)) {
    return 0;
  }
  for (size_t i = 0; i < context->count; ++i) {
    context->cache.free(context->objects[i]);
  }
  return 0;
}

bool runRemoteFree(RemoteFreeContext& context) {
  const size_t processorCount = Processor::getCount();
  Thread* workers[64] = {};
  size_t started = 0;
  for (; started < processorCount; ++started) {
    workers[started] = new Thread(Scheduler::instance().getKernelProcess(), freeRemotely,
                                  &context, nullptr, false, false, true);
    workers[started]->setName("QEMU SLAM remote freer");
    if (!workers[started]->start()) {
      break;
    }
  }

  bool joined = true;
  for (size_t i = 0; i < started; ++i) {
    joined = workers[i]->joinForCompletion() && joined;
  }
  return started == processorCount && joined &&
         context.processor.value() != static_cast<size_t>(-1);
}
}  // namespace

bool runSlamAllocatorConcurrencyRegression() {
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN slam-cross-cpu-recovery-smp");

  const size_t processorCount = Processor::getCount();
  if (processorCount < 2 || processorCount > 64) {
    ERROR("QEMU SLAM regression requires between two and 64 processors");
    return false;
  }

  SlamAllocator& allocator = SlamAllocator::instance();
  SlamCache cache;
  cache.initialise(&allocator, ObjectSize);

  uintptr_t objects[ObjectCount] = {};
  for (size_t i = 0; i < ObjectCount; ++i) {
    objects[i] = cache.allocate();
    prepareAllocation(cache, objects[i]);
  }

  const size_t allocatingProcessor = Processor::index();
  RemoteFreeContext firstFree(cache, objects, ObjectCount, allocatingProcessor);
  if (!runRemoteFree(firstFree) || firstFree.processor == allocatingProcessor) {
    ERROR("QEMU-CONCURRENCY-TEST: FAIL slam-cross-cpu-recovery-smp: "
          "free worker did not complete on a remote CPU");
    return false;
  }

  uintptr_t reused = cache.allocate();
  if ((reused & ~(SLAB_MINIMUM_SIZE - 1)) !=
      (objects[0] & ~(SLAB_MINIMUM_SIZE - 1))) {
    ERROR("QEMU-CONCURRENCY-TEST: FAIL slam-cross-cpu-recovery-smp: "
          "remote free list was stranded");
    return false;
  }
  prepareAllocation(cache, reused);

  RemoteFreeContext finalFree(cache, &reused, 1, allocatingProcessor);
  if (!runRemoteFree(finalFree) || cache.recovery(1) != 1) {
    ERROR("QEMU-CONCURRENCY-TEST: FAIL slam-cross-cpu-recovery-smp: "
          "cross-CPU sub-page slab was not reclaimed");
    return false;
  }

  NOTICE("QEMU-CONCURRENCY-TEST: slam cpus="
         << Dec << allocatingProcessor << "/" << static_cast<size_t>(firstFree.processor) << "/"
         << static_cast<size_t>(finalFree.processor));
  NOTICE("QEMU-CONCURRENCY-TEST: PASS slam-cross-cpu-recovery-smp");
  return true;
}
