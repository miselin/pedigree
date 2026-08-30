/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Ipc.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/MemoryPool.h"

static_assert(Event::getHostedHandlerBufferSize(1024) == EVENT_LIMIT);
static_assert(Event::getHostedHandlerBufferSize(4096) == EVENT_LIMIT);
static_assert(Event::getHostedHandlerBufferSize(16384) == 16384);
static_assert(Ipc::IpcMessage::getHostedInlinePageCount(1024) == 4);
static_assert(Ipc::IpcMessage::getHostedInlinePageCount(4096) == 1);
static_assert(Ipc::IpcMessage::getHostedInlinePageCount(16384) == 1);
static_assert(Ipc::IpcMessage::getHostedInlineSlotSize(1024) == 4096);
static_assert(Ipc::IpcMessage::getHostedInlineSlotSize(4096) == 4096);
static_assert(Ipc::IpcMessage::getHostedInlineSlotSize(16384) == 16384);
static_assert(Ipc::IpcMessage::getHostedInlinePoolPageCount(1024) == 4096);
static_assert(Ipc::IpcMessage::getHostedInlinePoolPageCount(4096) == 1024);
static_assert(Ipc::IpcMessage::getHostedInlinePoolPageCount(16384) == 1024);

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

bool ipcPayloadGeometry() {
  constexpr const char* Test = "ipc-payload-page-span";
  bool passed = true;
  passed &= check(Ipc::IpcMessage::InlineCapacity == 4096, Test,
                  "the conventional inline IPC capacity changed");
  passed &= check(Ipc::IpcMessage::getHostedInlinePageCount(1024) == 4, Test,
                  "the 4 KiB IPC buffer did not span four 1 KiB pages");
  passed &= check(Ipc::IpcMessage::getHostedInlinePageCount(4096) == 1, Test,
                  "the 4 KiB target changed its IPC buffer span");
  passed &= check(Ipc::IpcMessage::getHostedInlinePageCount(16384) == 1, Test,
                  "the IPC buffer used more than one 16 KiB page");
  passed &= check(Ipc::IpcMessage::getHostedInlineSlotSize(1024) == 4096 &&
                      Ipc::IpcMessage::getHostedInlineSlotSize(4096) == 4096 &&
                      Ipc::IpcMessage::getHostedInlineSlotSize(16384) == 16384,
                  Test, "an inline IPC slot shared a target page with another message");
  passed &= check(Ipc::IpcMessage::getHostedInlinePoolPageCount(1024) == 4096 &&
                      Ipc::IpcMessage::getHostedInlinePoolPageCount(4096) == 1024 &&
                      Ipc::IpcMessage::getHostedInlinePoolPageCount(16384) == 1024,
                  Test, "the IPC pool did not retain its logical buffer capacity");

  Ipc::IpcMessage inlineMessage(Ipc::IpcMessage::InlineCapacity - 1);
  uint8_t* buffer = reinterpret_cast<uint8_t*>(inlineMessage.getBuffer());
  passed &= check(buffer != nullptr && inlineMessage.getHandle() == nullptr, Test,
                  "an inline IPC message did not use the message pool");
  if (buffer) {
    VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    const size_t pageCount = Ipc::IpcMessage::getHostedInlinePageCount(pageSize);
    passed &= check(!(reinterpret_cast<uintptr_t>(buffer) & (pageSize - 1)), Test,
                    "the inline IPC slot was not target-page aligned");
    for (size_t page = 0; page < pageCount; ++page) {
      void* address = buffer + (page * pageSize);
      const bool mapped = va.isMapped(address);
      passed &= check(mapped, Test, "part of the inline IPC buffer is unmapped");
      if (mapped) {
        physical_uintptr_t physicalAddress = 0;
        size_t flags = 0;
        va.getMapping(address, physicalAddress, flags);
        passed &= check(
            (flags & VirtualAddressSpace::Write) && !(flags & VirtualAddressSpace::KernelMode),
            Test, "part of the inline IPC buffer is not user-writable");
      }
    }
    buffer[0] = 0xA5;
    buffer[Ipc::IpcMessage::InlineCapacity - 1] = 0x5A;
    passed &= check(buffer[0] == 0xA5 && buffer[Ipc::IpcMessage::InlineCapacity - 1] == 0x5A, Test,
                    "the inline IPC payload was not writable end-to-end");
  }

  Ipc::IpcMessage sharedMessage(Ipc::IpcMessage::InlineCapacity);
  passed &= check(sharedMessage.getBuffer() != nullptr && sharedMessage.getHandle() != nullptr,
                  Test, "the exact 4 KiB threshold did not use a shared region");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ipc-payload-page-span");
  }
  return passed;
}
}  // namespace

bool runHostedPayloadPageRegressions() {
  return eventPayloadGeometry() && multiPagePoolMapping() && ipcPayloadGeometry();
}
