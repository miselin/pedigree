#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/core/SlamAllocator.h"

extern "C" size_t slamBenchmarkObjectMinimumSize() {
  return OBJECT_MINIMUM_SIZE;
}

extern "C" uintptr_t slamBenchmarkAllocate(size_t size) {
  return SlamAllocator::instance().allocate(size);
}

extern "C" void slamBenchmarkFree(uintptr_t object) {
  SlamAllocator::instance().free(object);
}

extern "C" void slamBenchmarkClearAll() {
  SlamAllocator::instance().clearAll();
}

extern "C" size_t slamBenchmarkRecovery(size_t maxSlabs) {
  return SlamAllocator::instance().recovery(maxSlabs);
}
