/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"

namespace {
constexpr uint64_t OldMarker = 0x13579BDF2468ACE0ULL;
constexpr uint64_t NewMarker = 0x0FEDCBA987654321ULL;
constexpr uint64_t WrittenMarker = 0xA55A123443215AA5ULL;
constexpr uint64_t ReusedMarker = 0x76543210FEDCBA98ULL;

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

struct PrivateTlbContext {
  VirtualAddressSpace* space;
  void* address;
  size_t processor;
  uint64_t pat = 0;
  Semaphore resume{0};
  Atomic<size_t> ready{0}, stage{0};
  Atomic<bool> aborted{false}, passed{false};
};

#if X64
uint64_t readPat() {
  uint32_t low, high;
  asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x277));
  return (uint64_t(high) << 32) | low;
}
#endif

bool waitPrivateTlbStage(PrivateTlbContext& context, size_t stage) {
  const auto deadline = Time::getTicks() + 5 * Time::Multiplier::Second;
  while (!context.aborted && Time::getTicks() < deadline) {
    if (context.stage == stage) {
      return true;
    }
    Processor::pause();
  }
  return false;
}

bool privateTlbValue(PrivateTlbContext& context, uint64_t expected) {
  return Processor::index() == context.processor &&
         &Processor::information().getVirtualAddressSpace() == context.space &&
         *reinterpret_cast<volatile uint64_t*>(context.address) == expected;
}

bool waitPrivateTlbReady(PrivateTlbContext& context, size_t ready) {
  const auto deadline = Time::getTicks() + 5 * Time::Multiplier::Second;
  while (context.ready != ready && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  return context.ready == ready;
}

int readPrivateTranslations(void* parameter) {
  auto& context = *static_cast<PrivateTlbContext*>(parameter);
  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
#if X64
  uint64_t cr0, savedCr4;
  asm volatile("mov %%cr0, %0" : "=r"(cr0));
  asm volatile("mov %%cr4, %0" : "=r"(savedCr4));
  const bool cacheControlsMatch = !(cr0 & (3ULL << 29)) && readPat() == context.pat;
  const uint64_t withGlobalPages = savedCr4 | (1ULL << 7);
  asm volatile("mov %0, %%cr4" : : "r"(withGlobalPages) : "memory");
#endif
  // A context switch would hide a missing remote invalidation. Keep IRQs
  // masked while warmed; pause services pending TLB requests cooperatively.
  do {
#if X64
    if (!cacheControlsMatch) {
      ERROR("QEMU private TLB reader has inconsistent cache controls");
      break;
    }
#endif
    if (!privateTlbValue(context, OldMarker)) {
      break;
    }
    context.ready = 1;
    if (!waitPrivateTlbStage(context, 1) || !privateTlbValue(context, NewMarker)) {
      break;
    }
    context.ready = 2;
    if (!waitPrivateTlbStage(context, 2) || !privateTlbValue(context, NewMarker)) {
      break;
    }
    *reinterpret_cast<volatile uint64_t*>(context.address) = WrittenMarker;
    context.ready = 3;
    Processor::setInterrupts(interrupts);
    if (!context.resume.acquireForCompletion(1, 5) || context.aborted) {
      break;
    }
    Processor::setInterrupts(false);
    context.passed = privateTlbValue(context, ReusedMarker);
  } while (false);
  Processor::setInterrupts(false);
#if X64
  asm volatile("mov %0, %%cr4" : : "r"(savedCr4) : "memory");
#endif
  Processor::setInterrupts(interrupts);
  if (!context.passed) {
    context.aborted = true;
  }
  return context.passed ? 0 : 1;
}

struct KernelTlbWitness {
  size_t processor;
  Semaphore entered{0}, release{0};
  Atomic<bool> passed{false};
};

int witnessKernelAddressSpace(void* parameter) {
  auto& context = *static_cast<KernelTlbWitness*>(parameter);
  context.passed = Processor::index() == context.processor &&
                   &Processor::information().getVirtualAddressSpace() ==
                       &VirtualAddressSpace::getKernelAddressSpace();
  context.entered.release();
  return context.release.acquireForCompletion(1, 5) ? 0 : 1;
}

bool privateTlbMutator() {
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN private-tlb-shootdown-smp");
  const size_t mutator = Processor::index();
  const CpuAffinityMask online = Scheduler::onlineAffinity();
  size_t remote = CpuAffinityMask::MaximumCpus;
  for (size_t cpu = 0; cpu < CpuAffinityMask::MaximumCpus; ++cpu) {
    if (cpu != mutator && online.contains(cpu)) {
      remote = cpu;
      break;
    }
  }
  if (remote == CpuAffinityMask::MaximumCpus) {
    return false;
  }

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  MemoryRegion backing("QEMU private TLB backing");
  if (!PhysicalMemoryManager::instance().allocateRegion(
          backing, 2, 0, VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write)) {
    return false;
  }
  auto* original = static_cast<volatile uint64_t*>(backing.virtualAddress());
  auto* replacement = reinterpret_cast<volatile uint64_t*>(
      reinterpret_cast<uintptr_t>(backing.virtualAddress()) + pageSize);
  *original = OldMarker;
  *replacement = NewMarker;
  physical_uintptr_t originalPage = 0, replacementPage = 0;
  size_t flags = 0;
  auto& kernel = VirtualAddressSpace::getKernelAddressSpace();
  if (!kernel.getMapping(backing.virtualAddress(), originalPage, flags) ||
      !kernel.getMapping(const_cast<uint64_t*>(replacement), replacementPage, flags)) {
    return false;
  }

  Process* process = new Process(Scheduler::instance().getKernelProcess(), true);
  VirtualAddressSpace* space = process->getAddressSpace();
  void* address = reinterpret_cast<void*>(space->getDynamicStart() + 64 * pageSize);
  // A private lower-half supervisor mapping must not survive a CR3 switch as
  // a global translation. The kernel aliases retain ownership of both pages.
  const size_t readFlags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Borrowed;
  const size_t writeFlags = readFlags | VirtualAddressSpace::Write;
  if (reinterpret_cast<uintptr_t>(address) >= space->getKernelStart() || space->isMapped(address) ||
      !space->map(originalPage, address, readFlags)) {
    delete process;
    return false;
  }
  PrivateTlbContext context{space, address, remote};
#if X64
  context.pat = readPat();
#endif
  ThreadPlacement placement;
  placement.allowed.set(remote);
  Thread* reader = new Thread(process, readPrivateTranslations, &context, nullptr, false, false,
                              true, &placement);
  reader->setName("QEMU private TLB reader");
  const bool started = reader->start();
  bool passed = started && waitPrivateTlbReady(context, 1);
  if (passed) {
    space->unmap(address);
    // Reuse the detached backing before allowing the remote reader to load.
    *original = ReusedMarker;
    passed = space->map(replacementPage, address, readFlags);
    if (passed) {
      context.stage = 1;
      passed = waitPrivateTlbReady(context, 2);
    }
  }
  if (passed) {
    passed = space->trySetFlags(address, writeFlags);
    if (passed) {
      context.stage = 2;
      passed = waitPrivateTlbReady(context, 3) && *replacement == WrittenMarker;
    }
  }

  KernelTlbWitness witness{remote};
  Thread* witnessThread = nullptr;
  if (passed) {
    bool sleeping = false;
    const auto deadline = Time::getTicks() + 5 * Time::Multiplier::Second;
    while (!sleeping && Time::getTicks() < deadline) {
      {
        LockGuard<Spinlock> guard(reader->getLock());
        sleeping = reader->getStatus() == Thread::Sleeping;
      }
      if (!sleeping) {
        Scheduler::instance().yield();
      }
    }
    passed = sleeping;
    if (passed) {
      witnessThread =
          new Thread(Scheduler::instance().getKernelProcess(), witnessKernelAddressSpace, &witness,
                     nullptr, false, false, true, &placement);
      witnessThread->setName("QEMU private TLB switch witness");
      if (!witnessThread->start()) {
        witnessThread->setUnwindState(Thread::TerminateThread);
        passed = false;
      } else {
        passed = witness.entered.acquireForCompletion(1, 5) && witness.passed;
      }
    }
  }
  if (passed) {
    space->unmap(address);
    passed = space->map(originalPage, address, writeFlags);
  }
  context.aborted = !passed;
  context.resume.release();
  witness.release.release();
  if (!started) {
    reader->setUnwindState(Thread::TerminateThread);
  }
  if (!reader->joinForCompletion() || (witnessThread && !witnessThread->joinForCompletion())) {
    FATAL("QEMU private TLB regression could not retire its workers");
  }
  passed = passed && context.passed;
  if (space->isMapped(address)) {
    space->unmap(address);
  }
  delete process;
  if (!passed) {
    ERROR("QEMU private TLB regression failed at stage " << Dec << context.stage.value());
    return false;
  }
  NOTICE("QEMU-CONCURRENCY-TEST: private-tlb-shootdown reader=" << Dec << remote
                                                                << ", mutator=" << mutator);
  NOTICE("QEMU-CONCURRENCY-TEST: PASS private-tlb-shootdown-smp");
  return true;
}

int privateTlbMutatorEntry(void* parameter) {
  auto& passed = *static_cast<Atomic<bool>*>(parameter);
  passed = privateTlbMutator();
  return passed ? 0 : 1;
}

bool privateTlbShootdownRegression() {
  Atomic<bool> passed{false};
  ThreadPlacement placement;
  placement.allowed.set(Processor::index());
  Thread* mutator = new Thread(Scheduler::instance().getKernelProcess(), privateTlbMutatorEntry,
                               &passed, nullptr, false, false, true, &placement);
  mutator->setName("QEMU private TLB mutator");
  if (!mutator->start()) {
    mutator->setUnwindState(Thread::TerminateThread);
  }
  if (!mutator->joinForCompletion()) {
    FATAL("QEMU private TLB regression could not retire its mutator");
  }
  return passed;
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
  return privateTlbShootdownRegression();
}
