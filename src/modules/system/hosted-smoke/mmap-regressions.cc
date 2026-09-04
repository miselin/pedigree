/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/MemoryAllocator.h"

#include <stddef.h>
#include <stdint.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/file-syscalls.h"
#include <sys/mman.h>

namespace {
constexpr int PreservedErrno = 123;
constexpr uint8_t OriginalSentinel = 0x5A;

struct PlacementContext {
  PlacementContext()
      : hintFallback(false),
        noReplaceCollision(false),
        fixedReplacement(false),
        fixedSpanReserved(false),
        inputValidation(false),
        noReplaceAtomic(false),
        returned(0) {}

  bool hintFallback;
  bool noReplaceCollision;
  bool fixedReplacement;
  bool fixedSpanReserved;
  bool inputValidation;
  bool noReplaceAtomic;
  Atomic<size_t> returned;
};

struct RaceContext {
  RaceContext(uintptr_t address, size_t length)
      : begin(0, false), address(address), length(length) {}

  Semaphore begin;
  uintptr_t address;
  size_t length;
};

struct RacerContext {
  RacerContext(RaceContext* race, uint8_t sentinel)
      : race(race), sentinel(sentinel), result(MAP_FAILED), error(0), returned(0) {}

  RaceContext* race;
  uint8_t sentinel;
  void* result;
  int error;
  Atomic<size_t> returned;
};

bool findFreeDynamicRange(Process* process, size_t pageSize, size_t length, uintptr_t& address) {
  MemoryAllocator& allocator = process->getDynamicSpaceAllocator();
  const uintptr_t mask = pageSize - 1;
  for (size_t i = 0; i < allocator.size(); ++i) {
    MemoryAllocator::Range range(0, 0);
    if (!allocator.getRange(i, range) || range.address > ~static_cast<uintptr_t>(0) - mask) {
      continue;
    }

    const uintptr_t aligned = (range.address + mask) & ~mask;
    if (aligned >= range.address && aligned - range.address <= range.length &&
        length <= range.length - (aligned - range.address)) {
      address = aligned;
      return true;
    }
  }
  return false;
}

int noReplaceRacer(void* parameter) {
  RacerContext* context = reinterpret_cast<RacerContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  if (!context->race->begin.acquire()) {
    context->returned += 1;
    return 1;
  }

  thread->setErrno(0);
  context->result =
      posix_mmap(reinterpret_cast<void*>(context->race->address), context->race->length,
                 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
  context->error = thread->getErrno();
  if (context->result != MAP_FAILED) {
    *reinterpret_cast<volatile uint8_t*>(context->result) = context->sentinel;
  }
  context->returned += 1;
  return 0;
}

bool runNoReplaceRace(Process* process, size_t pageSize) {
  uintptr_t address = 0;
  if (!findFreeDynamicRange(process, pageSize, pageSize, address)) {
    return false;
  }

  RaceContext race(address, pageSize);
  RacerContext first(&race, 0x31);
  RacerContext second(&race, 0x42);
  Thread* firstThread = new Thread(process, noReplaceRacer, &first, nullptr, false, true, true);
  Thread* secondThread = new Thread(process, noReplaceRacer, &second, nullptr, false, true, true);
  firstThread->setName("hosted mmap no-replace racer 1");
  secondThread->setName("hosted mmap no-replace racer 2");

  const bool firstStarted = firstThread->start();
  const bool secondStarted = secondThread->start();
  race.begin.release(2);
  const bool firstJoined = firstStarted && firstThread->joinForCompletion();
  const bool secondJoined = secondStarted && secondThread->joinForCompletion();
  if (!firstStarted) {
    delete firstThread;
  }
  if (!secondStarted) {
    delete secondThread;
  }

  const bool firstWon = first.result == reinterpret_cast<void*>(address) && !first.error;
  const bool secondWon = second.result == reinterpret_cast<void*>(address) && !second.error;
  const bool firstLost = first.result == MAP_FAILED && first.error == Error::FileExists;
  const bool secondLost = second.result == MAP_FAILED && second.error == Error::FileExists;
  const uint8_t expected = firstWon ? first.sentinel : second.sentinel;
  const bool sentinelPreserved =
      (firstWon || secondWon) && *reinterpret_cast<volatile uint8_t*>(address) == expected;
  const bool passed = firstStarted && secondStarted && firstJoined && secondJoined &&
                      first.returned == 1 && second.returned == 1 &&
                      ((firstWon && secondLost) || (secondWon && firstLost)) && sentinelPreserved;

  if (firstWon || secondWon) {
    posix_munmap(reinterpret_cast<void*>(address), pageSize);
  }
  return passed;
}

bool fixedReplacementReservesHoles(Process* process, size_t pageSize) {
  const size_t replacementLength = pageSize * 3;
  uintptr_t address = 0;
  if (!findFreeDynamicRange(process, pageSize, replacementLength, address)) {
    return false;
  }

  void* initial = posix_mmap(reinterpret_cast<void*>(address), pageSize, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  void* replacement =
      initial == reinterpret_cast<void*>(address)
          ? posix_mmap(reinterpret_cast<void*>(address), replacementLength, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0)
          : MAP_FAILED;
  const bool holesUnavailable =
      replacement == reinterpret_cast<void*>(address) &&
      !process->getDynamicSpaceAllocator().allocateSpecific(address + pageSize, pageSize * 2);

  if (replacement != MAP_FAILED) {
    posix_munmap(replacement, replacementLength);
  } else if (initial != MAP_FAILED) {
    posix_munmap(initial, pageSize);
  }
  return holesUnavailable;
}

int exerciseMmapPlacement(void* parameter) {
  PlacementContext* context = reinterpret_cast<PlacementContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread->getParent();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();

  thread->setErrno(PreservedErrno);
  void* original =
      posix_mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  const bool originalMapped = original != MAP_FAILED && thread->getErrno() == PreservedErrno;
  if (originalMapped) {
    *reinterpret_cast<volatile uint8_t*>(original) = OriginalSentinel;
  }

  thread->setErrno(PreservedErrno);
  void* hinted =
      originalMapped
          ? posix_mmap(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(original) + 17),
                       pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)
          : MAP_FAILED;
  context->hintFallback = originalMapped && hinted != MAP_FAILED && hinted != original &&
                          !(reinterpret_cast<uintptr_t>(hinted) & (pageSize - 1)) &&
                          *reinterpret_cast<volatile uint8_t*>(original) == OriginalSentinel &&
                          thread->getErrno() == PreservedErrno;

  thread->setErrno(0);
  void* noReplace = originalMapped ? posix_mmap(original, pageSize, PROT_READ | PROT_WRITE,
                                                MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0)
                                   : nullptr;
  context->noReplaceCollision = originalMapped && noReplace == MAP_FAILED &&
                                thread->getErrno() == Error::FileExists &&
                                *reinterpret_cast<volatile uint8_t*>(original) == OriginalSentinel;

  thread->setErrno(0);
  const void* invalidFixed = posix_mmap(nullptr, pageSize, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
  const bool nullFixedRejected =
      invalidFixed == MAP_FAILED && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  const void* overflowLength = posix_mmap(nullptr, ~static_cast<size_t>(0), PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANON, -1, 0);
  context->inputValidation = nullFixedRejected && overflowLength == MAP_FAILED &&
                             thread->getErrno() == Error::InvalidArgument;

  thread->setErrno(PreservedErrno);
  void* replacement = originalMapped ? posix_mmap(original, pageSize, PROT_READ | PROT_WRITE,
                                                  MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0)
                                     : MAP_FAILED;
  context->fixedReplacement = originalMapped && replacement == original &&
                              thread->getErrno() == PreservedErrno &&
                              !*reinterpret_cast<volatile uint8_t*>(replacement);
  if (replacement != MAP_FAILED) {
    *reinterpret_cast<volatile uint8_t*>(replacement) = 0x7C;
  }

  context->fixedSpanReserved = fixedReplacementReservesHoles(process, pageSize);
  context->noReplaceAtomic = runNoReplaceRace(process, pageSize);

  if (hinted != MAP_FAILED) {
    posix_munmap(hinted, pageSize);
  }
  if (replacement != MAP_FAILED) {
    posix_munmap(replacement, pageSize);
  } else if (originalMapped) {
    posix_munmap(original, pageSize);
  }

  context->returned += 1;
  return context->hintFallback && context->noReplaceCollision && context->fixedReplacement &&
                 context->fixedSpanReserved && context->inputValidation && context->noReplaceAtomic
             ? 0
             : 1;
}
}  // namespace

bool runHostedMmapPlacementRegressions(Process* kernelProcess) {
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  PlacementContext context;
  Thread* worker = new Thread(process, exerciseMmapPlacement, &context, nullptr, false, true, true);
  worker->setName("hosted mmap placement worker");
  process->publish();

  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  const bool passed = started && joined && context.returned == 1 && context.hintFallback &&
                      context.noReplaceCollision && context.fixedReplacement &&
                      context.fixedSpanReserved && context.inputValidation &&
                      context.noReplaceAtomic;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL mmap-placement: "
        "hint fallback, fixed replacement, no-replace, or input validation regressed");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS mmap-placement");
  return true;
}
