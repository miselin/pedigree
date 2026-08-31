/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

namespace {
constexpr uint64_t OldMarker = 0x13579BDF2468ACE0ULL;
constexpr uint64_t NewMarker = 0x0FEDCBA987654321ULL;

struct TlbRemapContext {
  explicit TlbRemapContext(void* virtualAddress)
      : address(virtualAddress),
        warmed(0),
        remapped(false),
        failures(0),
        remoteReaders(0),
        remoteSuccesses(0),
        warmedProcessors(0),
        successfulProcessors(0),
        mutatorProcessor(static_cast<size_t>(-1)) {}

  void* address;
  Semaphore warmed;
  Atomic<bool> remapped;
  Atomic<size_t> failures;
  Atomic<size_t> remoteReaders;
  Atomic<size_t> remoteSuccesses;
  Atomic<uint64_t> warmedProcessors;
  Atomic<uint64_t> successfulProcessors;
  Atomic<size_t> mutatorProcessor;
};

int warmRemoteTranslation(void* parameter) {
  TlbRemapContext* context = reinterpret_cast<TlbRemapContext*>(parameter);
  volatile uint64_t* value = reinterpret_cast<volatile uint64_t*>(context->address);
  const size_t processorBefore = Processor::index();
  if (*value != OldMarker) {
    context->failures += 1;
  }
  context->warmedProcessors |= uint64_t(1) << processorBefore;
  context->warmed.release();

  bool remapped = false;
  for (size_t poll = 0; poll < 10000000; ++poll) {
    if (context->remapped.value()) {
      remapped = true;
      break;
    }
    Processor::pause();
  }
  if (!remapped) {
    context->failures += 1;
    return 1;
  }

  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  const uint64_t observedAfter = *value;
  const size_t processorAfter = Processor::index();
  if (observedAfter != NewMarker) {
    context->failures += 1;
    return 1;
  }

  // A migrated reader can still detect a bad value, but it does not prove
  // that one warmed processor retained and then discarded its translation.
  if (processorAfter != processorBefore) {
    return 0;
  }

  if (processorBefore != context->mutatorProcessor.value()) {
    context->remoteReaders += 1;
    context->remoteSuccesses += 1;
  }
  context->successfulProcessors |= uint64_t(1) << processorBefore;
  return 0;
}
}  // namespace

bool runTlbShootdownConcurrencyRegression() {
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN shared-kernel-tlb-shootdown-smp");
  const size_t processorCount = Processor::getCount();
  if (processorCount < 2 || processorCount > 64) {
    ERROR("QEMU TLB shootdown regression requires at least two processors");
    return false;
  }

  PhysicalMemoryManager& memory = PhysicalMemoryManager::instance();
  MemoryRegion reservation("QEMU TLB shootdown regression");
  if (!memory.allocateRegion(reservation, 1, PhysicalMemoryManager::virtualOnly,
                             VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write)) {
    ERROR("QEMU TLB shootdown regression could not reserve a kernel address");
    return false;
  }
  // The fixture owns both physical pages explicitly; the region only keeps
  // its kernel virtual address unavailable to other allocators.
  reservation.setNonRamMemory(true);
  reservation.setForced(true);

  const physical_uintptr_t originalPage = memory.allocatePage();
  const physical_uintptr_t replacementPage = memory.allocatePage();
  VirtualAddressSpace& addressSpace = VirtualAddressSpace::getKernelAddressSpace();
  if (!originalPage || !replacementPage ||
      !addressSpace.map(originalPage, reservation.virtualAddress(),
                        VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write)) {
    if (originalPage) {
      memory.freePage(originalPage);
    }
    if (replacementPage) {
      memory.freePage(replacementPage);
    }
    reservation.free();
    ERROR("QEMU TLB shootdown regression could not prepare physical pages");
    return false;
  }
  *reinterpret_cast<volatile uint64_t*>(reservation.virtualAddress()) = OldMarker;

  TlbRemapContext context(reservation.virtualAddress());
  Process* process = Scheduler::instance().getKernelProcess();
  Thread* readers[64] = {};
  const size_t readerTarget = processorCount <= 32 ? processorCount * 2 : processorCount;
  size_t startedReaders = 0;
  for (; startedReaders < readerTarget; ++startedReaders) {
    readers[startedReaders] =
        new Thread(process, warmRemoteTranslation, &context, nullptr, false, false, true);
    readers[startedReaders]->setName("QEMU remote TLB reader");
    if (!readers[startedReaders]->start()) {
      break;
    }
  }

  for (size_t i = 0; i < startedReaders; ++i) {
    if (!context.warmed.acquireForCompletion()) {
      context.failures += 1;
    }
  }

  context.mutatorProcessor = Processor::index();
  addressSpace.unmap(reservation.virtualAddress());
  const bool replacementMapped =
      addressSpace.map(replacementPage, reservation.virtualAddress(),
                       VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write);
  if (replacementMapped) {
    *reinterpret_cast<volatile uint64_t*>(reservation.virtualAddress()) = NewMarker;
    __atomic_thread_fence(__ATOMIC_RELEASE);
  } else {
    context.failures += 1;
  }
  context.remapped = true;

  bool readersJoined = true;
  for (size_t i = 0; i < startedReaders; ++i) {
    readersJoined = readers[i]->joinForCompletion() && readersJoined;
  }
  const bool mapped = addressSpace.isMapped(reservation.virtualAddress());
  if (mapped) {
    addressSpace.unmap(reservation.virtualAddress());
  }
  memory.freePage(originalPage);
  memory.freePage(replacementPage);
  reservation.free();

  const bool passed = startedReaders == readerTarget && readersJoined && replacementMapped &&
                      mapped && !context.failures && context.remoteReaders &&
                      context.remoteSuccesses == context.remoteReaders;
  if (!passed) {
    ERROR("QEMU TLB shootdown regression did not observe the replacement mapping");
    return false;
  }

  NOTICE("QEMU-CONCURRENCY-TEST: shared-kernel-tlb-shootdown mask="
         << Hex << context.successfulProcessors.value() << Dec
         << ", mutator=" << static_cast<size_t>(context.mutatorProcessor));
  NOTICE("QEMU-CONCURRENCY-TEST: PASS shared-kernel-tlb-shootdown-smp");
  return true;
}
