/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/MemoryPool.h"

static_assert(Event::getHostedHandlerBufferSize(1024) == EVENT_LIMIT);
static_assert(Event::getHostedHandlerBufferSize(4096) == EVENT_LIMIT);
static_assert(Event::getHostedHandlerBufferSize(16384) == 16384);

namespace {
bool check(bool condition, const char* test, const char* detail) {
  if (condition) {
    return true;
  }
  ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
  return false;
}

bool eventPayloadGeometry() {
  constexpr const char* Test = "event-payload-page-span";
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t pageCount = (EVENT_LIMIT / pageSize) + ((EVENT_LIMIT % pageSize) ? 1 : 0);
  const size_t expectedSpan = pageCount * pageSize;

  bool passed = true;
  passed &= check(Event::getHostedHandlerBufferSize(1024) == EVENT_LIMIT, Test,
                  "a 4 KiB event did not span four 1 KiB pages");
  passed &= check(Event::getHostedHandlerBufferSize(4096) == EVENT_LIMIT, Test,
                  "the 4 KiB target changed its event slot size");
  passed &= check(Event::getHostedHandlerBufferSize(16384) == 16384, Test,
                  "a sub-page event slot was not rounded to one target page");
  passed &= check(Event::getHandlerBufferSize() == expectedSpan, Test,
                  "the live event slot does not match target-page geometry");
  passed &= check(Event::getLastHandlerBuffer() - Event::getHandlerBuffer() ==
                      (EVENT_TID_MAX * MAX_NESTED_EVENTS * expectedSpan),
                  Test, "the reserved handler-buffer range does not use the rounded slot span");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS event-payload-page-span");
  }
  return passed;
}

bool multiPagePoolMapping() {
  constexpr const char* Test = "memory-pool-page-span";
  constexpr size_t PageCount = 4;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (pageSize > (~size_t{0} / PageCount)) {
    return check(false, Test, "the target page size overflows the regression buffer");
  }

  MemoryPool pool("hosted-payload-page-regression");
  if (!pool.initialise(PageCount, PageCount * pageSize)) {
    return check(false, Test, "the multi-page pool could not be initialised");
  }

  const uintptr_t buffer = pool.allocateNow();
  bool passed = check(buffer != 0, Test, "the multi-page buffer could not be allocated");
  VirtualAddressSpace& va = VirtualAddressSpace::getKernelAddressSpace();
  if (buffer) {
    for (size_t page = 0; page < PageCount; ++page) {
      passed &= check(va.isMapped(reinterpret_cast<void*>(buffer + (page * pageSize))), Test,
                      "allocation left part of the buffer unmapped");
    }

    pool.free(buffer);
    passed &= check(pool.trim(), Test, "the released multi-page buffer was not reclaimed");
    for (size_t page = 0; page < PageCount; ++page) {
      passed &= check(!va.isMapped(reinterpret_cast<void*>(buffer + (page * pageSize))), Test,
                      "trim left part of the buffer mapped");
    }
  }

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS memory-pool-page-span");
  }
  return passed;
}

}  // namespace

bool runHostedPayloadPageRegressions() {
  return eventPayloadGeometry() && multiPagePoolMapping();
}
