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
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
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
        failedPlacementPreserved(false),
        fixedReplacement(false),
        fixedSpanReserved(false),
        exactReservation(false),
        partialReclamation(false),
        overlappingReclamation(false),
        inputValidation(false),
        noReplaceAtomic(false),
        returned(0) {}

  bool hintFallback;
  bool noReplaceCollision;
  bool failedPlacementPreserved;
  bool fixedReplacement;
  bool fixedSpanReserved;
  bool exactReservation;
  bool partialReclamation;
  bool overlappingReclamation;
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

struct UnmapRaceContext {
  UnmapRaceContext() : begin(0, false) {}

  Semaphore begin;
};

struct UnmapRacerContext {
  UnmapRacerContext(UnmapRaceContext* race, uintptr_t address, size_t length)
      : race(race), address(address), length(length), result(-1), error(0), returned(0) {}

  UnmapRaceContext* race;
  uintptr_t address;
  size_t length;
  int result;
  int error;
  Atomic<size_t> returned;
};

bool findFreeDynamicRange(Process* process, size_t pageSize, size_t length, uintptr_t& address) {
  Process::UserReservationSnapshot snapshot;
  if (!process->snapshotUserReservations(snapshot)) {
    return false;
  }
  const MemoryAllocator& allocator = snapshot.dynamic;
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

struct ReservationProbe {
  Process* process = nullptr;
  Process::UserRegion region = Process::UserRegion::Normal;

  explicit operator bool() const {
    return process != nullptr;
  }
  bool allocateSpecific(uintptr_t address, size_t length) const {
    return process->allocateSpecificUserRange(region, address, length);
  }
  void free(uintptr_t address, size_t length) const {
    process->freeUserRange(region, address, length);
  }
};

ReservationProbe allocatorFor(Process* process, uintptr_t address, size_t length) {
  VirtualAddressSpace* addressSpace = process->getAddressSpace();
  const uintptr_t end = address + length;
  if (addressSpace->getDynamicStart() && address >= addressSpace->getDynamicStart() &&
      end <= addressSpace->getDynamicEnd()) {
    return {process, Process::UserRegion::Dynamic};
  }
  if (address >= addressSpace->getUserStart() && end <= addressSpace->getUserReservedStart()) {
    return {process, Process::UserRegion::Normal};
  }
  return {};
}

bool reservationHeld(ReservationProbe allocator, uintptr_t address, size_t length) {
  if (!allocator.allocateSpecific(address, length)) {
    return true;
  }
  allocator.free(address, length);
  return false;
}

bool reservationAvailableExactlyOnce(ReservationProbe allocator, uintptr_t address, size_t length) {
  const bool first = allocator.allocateSpecific(address, length);
  const bool second = first && allocator.allocateSpecific(address, length);
  if (first) {
    allocator.free(address, length);
  }
  return first && !second;
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

int unmapRacer(void* parameter) {
  UnmapRacerContext* context = reinterpret_cast<UnmapRacerContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  if (!context->race->begin.acquire()) {
    context->returned += 1;
    return 1;
  }

  thread->setErrno(PreservedErrno);
  context->result = posix_munmap(reinterpret_cast<void*>(context->address), context->length);
  context->error = thread->getErrno();
  context->returned += 1;
  return context->result ? 1 : 0;
}

bool runOverlappingUnmapRace(Process* process, size_t pageSize) {
  const size_t mappingLength = pageSize * 3;
  void* mapping =
      posix_mmap(nullptr, mappingLength, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mapping == MAP_FAILED) {
    return false;
  }

  const uintptr_t address = reinterpret_cast<uintptr_t>(mapping);
  ReservationProbe allocator = allocatorFor(process, address, mappingLength);
  if (!allocator) {
    posix_munmap(mapping, mappingLength);
    return false;
  }

  UnmapRaceContext race;
  UnmapRacerContext first(&race, address, pageSize * 2);
  UnmapRacerContext second(&race, address + pageSize, pageSize * 2);
  Thread* firstThread = new Thread(process, unmapRacer, &first, nullptr, false, true, true);
  Thread* secondThread = new Thread(process, unmapRacer, &second, nullptr, false, true, true);
  firstThread->setName("hosted munmap overlap racer 1");
  secondThread->setName("hosted munmap overlap racer 2");

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

  const bool releasedExactlyOnce =
      reservationAvailableExactlyOnce(allocator, address, mappingLength);
  const bool passed = firstStarted && secondStarted && firstJoined && secondJoined &&
                      first.returned == 1 && second.returned == 1 && !first.result &&
                      !second.result && first.error == PreservedErrno &&
                      second.error == PreservedErrno && releasedExactlyOnce;
  if (!releasedExactlyOnce) {
    posix_munmap(mapping, mappingLength);
  }
  return passed;
}

bool anonymousReservationReclamation(Process* process, size_t pageSize) {
  const size_t mappingLength = pageSize * 3;
  void* mapping =
      posix_mmap(nullptr, mappingLength, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mapping == MAP_FAILED) {
    return false;
  }

  const uintptr_t address = reinterpret_cast<uintptr_t>(mapping);
  ReservationProbe allocator = allocatorFor(process, address, mappingLength + pageSize);
  if (!allocator) {
    posix_munmap(mapping, mappingLength);
    return false;
  }

  const bool adjacentAvailable = allocator.allocateSpecific(address + mappingLength, pageSize);
  if (adjacentAvailable) {
    allocator.free(address + mappingLength, pageSize);
  }

  const uintptr_t middle = address + pageSize;
  const bool middleUnmapped = !posix_munmap(reinterpret_cast<void*>(middle), pageSize);
  const bool prefixHeld = reservationHeld(allocator, address, pageSize);
  const bool suffixHeld = reservationHeld(allocator, address + (pageSize * 2), pageSize);
  const bool middleAvailable = allocator.allocateSpecific(middle, pageSize);
  if (middleAvailable) {
    allocator.free(middle, pageSize);
  }

  void* middleMapping =
      posix_mmap(reinterpret_cast<void*>(middle), pageSize, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
  const bool middleReused = middleMapping == reinterpret_cast<void*>(middle);

  const bool remainderUnmapped = !posix_munmap(mapping, mappingLength);
  const bool releasedExactlyOnce =
      reservationAvailableExactlyOnce(allocator, address, mappingLength);
  if (!releasedExactlyOnce) {
    posix_munmap(mapping, mappingLength);
  }

  return adjacentAvailable && middleUnmapped && prefixHeld && suffixHeld && middleAvailable &&
         middleReused && remainderUnmapped && releasedExactlyOnce;
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
  const bool holesUnavailable = replacement == reinterpret_cast<void*>(address) &&
                                !process->allocateSpecificUserRange(
                                    Process::UserRegion::Dynamic, address + pageSize, pageSize * 2);

  bool releasedExactlyOnce = false;
  if (replacement != MAP_FAILED) {
    if (!posix_munmap(replacement, replacementLength)) {
      releasedExactlyOnce = reservationAvailableExactlyOnce({process, Process::UserRegion::Dynamic},
                                                            address, replacementLength);
    }
  } else if (initial != MAP_FAILED) {
    posix_munmap(initial, pageSize);
  }
  return holesUnavailable && releasedExactlyOnce;
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
  ReservationProbe originalAllocator =
      originalMapped ? allocatorFor(process, reinterpret_cast<uintptr_t>(original), pageSize)
                     : ReservationProbe{};
  context->failedPlacementPreserved =
      context->noReplaceCollision && originalAllocator &&
      reservationHeld(originalAllocator, reinterpret_cast<uintptr_t>(original), pageSize);

  thread->setErrno(0);
  const void* invalidFixed = posix_mmap(nullptr, pageSize, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
  const bool nullFixedRejected =
      invalidFixed == MAP_FAILED && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  const void* overflowLength = posix_mmap(nullptr, ~static_cast<size_t>(0), PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANON, -1, 0);
  const bool overflowMmapRejected =
      overflowLength == MAP_FAILED && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  const int overflowUnmap = originalMapped ? posix_munmap(original, ~static_cast<size_t>(0)) : 0;
  const bool overflowUnmapRejected =
      originalMapped && overflowUnmap == -1 && thread->getErrno() == Error::InvalidArgument &&
      *reinterpret_cast<volatile uint8_t*>(original) == OriginalSentinel && originalAllocator &&
      reservationHeld(originalAllocator, reinterpret_cast<uintptr_t>(original), pageSize);
  context->inputValidation = nullFixedRejected && overflowMmapRejected && overflowUnmapRejected;

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
  context->partialReclamation = anonymousReservationReclamation(process, pageSize);
  context->overlappingReclamation = runOverlappingUnmapRace(process, pageSize);
  context->noReplaceAtomic = runNoReplaceRace(process, pageSize);

  if (hinted != MAP_FAILED) {
    posix_munmap(hinted, pageSize);
  }
  if (replacement != MAP_FAILED) {
    const uintptr_t replacementAddress = reinterpret_cast<uintptr_t>(replacement);
    ReservationProbe replacementAllocator = allocatorFor(process, replacementAddress, pageSize);
    const bool unmapped = !posix_munmap(replacement, pageSize);
    context->exactReservation =
        replacementAllocator && unmapped &&
        reservationAvailableExactlyOnce(replacementAllocator, replacementAddress, pageSize);
  } else if (originalMapped) {
    posix_munmap(original, pageSize);
  }

  context->returned += 1;
  return context->hintFallback && context->noReplaceCollision &&
                 context->failedPlacementPreserved && context->fixedReplacement &&
                 context->fixedSpanReserved && context->exactReservation &&
                 context->partialReclamation && context->overlappingReclamation &&
                 context->inputValidation && context->noReplaceAtomic
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
  const bool passed =
      started && joined && context.returned == 1 && context.hintFallback &&
      context.noReplaceCollision && context.failedPlacementPreserved && context.fixedReplacement &&
      context.fixedSpanReserved && context.exactReservation && context.partialReclamation &&
      context.overlappingReclamation && context.inputValidation && context.noReplaceAtomic;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL mmap-placement: "
        "hint="
        << context.hintFallback << " no-replace=" << context.noReplaceCollision
        << " failed-preserved=" << context.failedPlacementPreserved << " replacement="
        << context.fixedReplacement << " fixed-span=" << context.fixedSpanReserved
        << " exact=" << context.exactReservation << " partial=" << context.partialReclamation
        << " overlap=" << context.overlappingReclamation << " validation="
        << context.inputValidation << " no-replace-race=" << context.noReplaceAtomic);
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS mmap-placement");
  return true;
}
