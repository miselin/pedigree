/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/drivers/common/ata/AtaWriteCache.h"
#include "modules/drivers/common/scsi/ScsiController.h"
#include "modules/drivers/common/scsi/ScsiDisk.h"

namespace {
constexpr uint64_t AtaOwnershipLocation = 0;
constexpr uint64_t AtaDrainingLocation = 4096;
constexpr uint64_t FailedWriteLocation = 8192;
constexpr uint64_t SuccessfulWriteLocation = 12288;
constexpr size_t PageBytes = 4096;

enum class WriteMode { Initialising, FailAll, PassWrite12 };

class ScriptedScsiController final : public ScsiController {
 public:
  ScriptedScsiController()
      : ScsiController(),
        m_Mode(WriteMode::Initialising),
        m_WriteOpcodes(),
        m_WriteCount(0),
        m_Valid(true) {}

  bool sendCommand(size_t nUnit, uintptr_t pCommand, uint8_t nCommandSize, uintptr_t pRespBuffer,
                   uint16_t nRespBytes, bool bWrite) override {
    if (nUnit || !pCommand || !nCommandSize) {
      m_Valid = false;
      return false;
    }

    const uint8_t opcode = *reinterpret_cast<const uint8_t*>(pCommand);
    switch (opcode) {
      case 0x00:
        m_Valid &= !bWrite && !pRespBuffer && !nRespBytes && nCommandSize == 6;
        return m_Valid;
      case 0x12: {
        if (bWrite || !pRespBuffer || nRespBytes != sizeof(ScsiDisk::Inquiry) ||
            nCommandSize != 6) {
          m_Valid = false;
          return false;
        }
        auto* inquiry = reinterpret_cast<ScsiDisk::Inquiry*>(pRespBuffer);
        ByteSet(inquiry, 0, sizeof(*inquiry));
        return true;
      }
      case 0x25: {
        if (bWrite || !pRespBuffer || nRespBytes != sizeof(ScsiDisk::Capacity) ||
            nCommandSize != 10) {
          m_Valid = false;
          return false;
        }
        auto* capacity = reinterpret_cast<ScsiDisk::Capacity*>(pRespBuffer);
        capacity->LBA = HOST_TO_BIG32(1023);
        capacity->BlockSize = HOST_TO_BIG32(512);
        return true;
      }
      case 0x2a:
      case 0xaa:
      case 0x8a:
        return handleWrite(opcode, nCommandSize, pRespBuffer, nRespBytes, bWrite);
      default:
        m_Valid = false;
        return false;
    }
  }

  void beginWrites(WriteMode mode) {
    m_Mode = mode;
    m_WriteCount = 0;
  }

  bool writeTraceMatches(const uint8_t* expected, size_t count) const {
    return m_Valid && m_WriteCount == count && !MemoryCompare(m_WriteOpcodes, expected, count);
  }

 protected:
  size_t getNumUnits() override {
    return 1;
  }

 private:
  bool handleWrite(uint8_t opcode, uint8_t commandSize, uintptr_t response, uint16_t responseBytes,
                   bool write) {
    const uint8_t expectedSize = opcode == 0x2a ? 10 : (opcode == 0xaa ? 12 : 16);
    if (m_Mode == WriteMode::Initialising || !write || !response || responseBytes != PageBytes ||
        commandSize != expectedSize || m_WriteCount >= sizeof(m_WriteOpcodes)) {
      m_Valid = false;
      return false;
    }

    m_WriteOpcodes[m_WriteCount++] = opcode;
    return m_Mode == WriteMode::PassWrite12 && opcode == 0xaa;
  }

  WriteMode m_Mode;
  uint8_t m_WriteOpcodes[16];
  size_t m_WriteCount;
  bool m_Valid;
};

class HostedScsiDisk final : public ScsiDisk {
 public:
  bool preparePage(uint64_t location) {
    bool alreadyExisted = false;
    const uintptr_t buffer = getCache().insert(location, &alreadyExisted);
    if (!buffer || alreadyExisted)
      return false;
    ByteSet(reinterpret_cast<void*>(buffer), 0x5a, PageBytes);
    getCache().markNoLongerEditing(location);
    return true;
  }

  bool takeAtaQueuedWritePage(uint64_t location) {
    const uintptr_t buffer = ataTakeQueuedWritePage(getCache(), location);
    if (!buffer)
      return false;
    CachePageGuard guard(getCache(), location);
    return true;
  }

  bool missesAtaQueuedWritePage(uint64_t location) {
    const uintptr_t buffer = ataTakeQueuedWritePage(getCache(), location);
    if (buffer) {
      CachePageGuard guard(getCache(), location);
      return false;
    }
    return true;
  }

  bool evictPage(uint64_t location) {
    return getCache().evict(location);
  }

  bool retirePage(uint64_t location, Cache::retirement_writeback_t callback, void* context) {
    return getCache().retireWriteback(location, callback, context);
  }

  bool hasPage(uint64_t location) {
    return getCache().exists(location, PageBytes);
  }

  uintptr_t pageAddress(uint64_t location) {
    const uintptr_t page = getCache().lookup(location);
    if (page)
      getCache().release(location);
    return page;
  }
};

struct RetirementResult {
  bool balanced;
  bool cleaned;
};

RetirementResult retireAndRepair(HostedScsiDisk& disk, uint64_t location,
                                 bool repairOneTransferredPin) {
  if (disk.evictPage(location))
    return {true, true};
  if (!repairOneTransferredPin)
    return {false, false};

  disk.unpin(location);
  return {false, disk.evictPage(location)};
}

struct Fixture {
  Fixture() : controller(), disk(), ready(false) {
    ready = disk.initialise(&controller, 0);
  }

  ScriptedScsiController controller;
  HostedScsiDisk disk;
  bool ready;
};

bool waitUntilQueuedAt(Thread* thread, size_t debugState, uintptr_t debugAddress) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    uintptr_t address = 0;
    if (thread->getWaitDebugInfo(info) && info.queue && info.queued &&
        thread->getDebugState(address) == debugState && address == debugAddress) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool waitUntilSet(Atomic<size_t>& value) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (Time::getTicks() < deadline) {
    if (static_cast<size_t>(value))
      return true;
    Scheduler::instance().yield();
  }
  return static_cast<size_t>(value) != 0;
}

struct AtaRetirementContext {
  AtaRetirementContext(HostedScsiDisk* pDisk, uint64_t pageLocation, uintptr_t pageBuffer)
      : disk(pDisk),
        location(pageLocation),
        page(pageBuffer),
        callbacks(0),
        argumentsValid(0),
        returned(0),
        succeeded(0) {}

  HostedScsiDisk* disk;
  uint64_t location;
  uintptr_t page;
  Atomic<size_t> callbacks;
  Atomic<size_t> argumentsValid;
  Atomic<size_t> returned;
  Atomic<size_t> succeeded;
};

bool retireAtaPageCallback(uintptr_t key, uintptr_t page, void* parameter) {
  auto* context = reinterpret_cast<AtaRetirementContext*>(parameter);
  context->callbacks += 1;
  context->argumentsValid = key == context->location && page == context->page;
  return true;
}

int retireAtaPage(void* parameter) {
  auto* context = reinterpret_cast<AtaRetirementContext*>(parameter);
  if (context->disk->retirePage(context->location, retireAtaPageCallback, context))
    context->succeeded += 1;
  context->returned += 1;
  return 0;
}

bool ataDrainingMissReleasesQueuedPin(Fixture& fixture) {
  const bool prepared = fixture.ready && fixture.disk.preparePage(AtaDrainingLocation);
  const uintptr_t page = prepared ? fixture.disk.pageAddress(AtaDrainingLocation) : 0;
  const bool pinned = page && fixture.disk.pin(AtaDrainingLocation);

  AtaRetirementContext context(&fixture.disk, AtaDrainingLocation, page);
  Thread* retirer = nullptr;
  if (pinned) {
    retirer = new Thread(Scheduler::instance().getKernelProcess(), retireAtaPage, &context, nullptr,
                         false, true);
    retirer->setName("hosted ATA queued-write page retirer");
  }

  const bool drainPublished =
      retirer && waitUntilQueuedAt(retirer, Thread::CallbackDrain, AtaDrainingLocation);
  const bool missed = drainPublished && fixture.disk.missesAtaQueuedWritePage(AtaDrainingLocation);
  const bool completedWithoutRepair = missed && waitUntilSet(context.returned);
  bool repaired = false;
  if (pinned && !static_cast<size_t>(context.returned)) {
    fixture.disk.unpin(AtaDrainingLocation);
    repaired = true;
  }
  const bool completed =
      retirer && (static_cast<size_t>(context.returned) || waitUntilSet(context.returned));
  const bool joined = retirer && retirer->joinForCompletion();
  const bool retired = completed && joined && context.succeeded == 1 && context.callbacks == 1 &&
                       context.argumentsValid == 1 && !fixture.disk.hasPage(AtaDrainingLocation);
  return prepared && page && pinned && drainPublished && missed && completedWithoutRepair &&
         !repaired && retired;
}

bool ataQueuedWriteCacheOwnership(Fixture& fixture) {
  const bool prepared = fixture.ready && fixture.disk.preparePage(AtaOwnershipLocation);
  const bool pinned = prepared && fixture.disk.pin(AtaOwnershipLocation);
  const bool acquired = pinned && fixture.disk.takeAtaQueuedWritePage(AtaOwnershipLocation);
  const RetirementResult retired = prepared
                                       ? retireAndRepair(fixture.disk, AtaOwnershipLocation, pinned)
                                       : RetirementResult{false, true};
  const bool drainingMiss = retired.cleaned && ataDrainingMissReleasesQueuedPin(fixture);
  const bool passed =
      prepared && pinned && acquired && retired.balanced && retired.cleaned && drainingMiss;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-queued-write-cache-ownership");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL ata-queued-write-cache-ownership: queued pin was not traded "
        "for one bounded execution pin");
  }
  return passed;
}

struct ScsiWriteCaseResult {
  uint64_t result;
  bool trace;
  bool balanced;
  bool cleaned;
};

ScsiWriteCaseResult runScsiWriteCase(Fixture& fixture, uint64_t location, WriteMode mode,
                                     const uint8_t* expectedOpcodes, size_t expectedCount) {
  const bool prepared = fixture.ready && fixture.disk.preparePage(location);
  fixture.controller.beginWrites(mode);
  const bool pinned = prepared && fixture.disk.pin(location);
  const uint64_t result =
      pinned ? fixture.controller.addRequest(0, SCSI_REQUEST_WRITE,
                                             reinterpret_cast<uint64_t>(&fixture.disk), location)
             : 0;
  const bool trace = pinned && fixture.controller.writeTraceMatches(expectedOpcodes, expectedCount);
  const RetirementResult retired =
      prepared ? retireAndRepair(fixture.disk, location, pinned) : RetirementResult{false, true};
  return {result, trace, retired.balanced, retired.cleaned};
}

bool scsiWriteResult(Fixture& fixture) {
  constexpr uint8_t FailedOpcodes[] = {0x2a, 0x2a, 0x2a, 0xaa, 0xaa, 0xaa, 0x8a, 0x8a, 0x8a};
  constexpr uint8_t SuccessfulOpcodes[] = {0x2a, 0x2a, 0x2a, 0xaa};

  const ScsiWriteCaseResult failed = runScsiWriteCase(
      fixture, FailedWriteLocation, WriteMode::FailAll, FailedOpcodes, sizeof(FailedOpcodes));
  const ScsiWriteCaseResult successful =
      runScsiWriteCase(fixture, SuccessfulWriteLocation, WriteMode::PassWrite12, SuccessfulOpcodes,
                       sizeof(SuccessfulOpcodes));
  const bool passed = !failed.result && failed.trace && failed.balanced && failed.cleaned &&
                      successful.result == PageBytes && successful.trace && successful.balanced &&
                      successful.cleaned;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS scsi-write-result");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL scsi-write-result: all-command failure was reported as success "
        "or fallback/ownership changed");
  }
  return passed;
}
}  // namespace

EXPORTED_PUBLIC bool runHostedScsiWriteRegressions() {
  Fixture fixture;
  const bool ataOwnership = ataQueuedWriteCacheOwnership(fixture);
  const bool scsiResult = scsiWriteResult(fixture);
  return ataOwnership && scsiResult;
}
