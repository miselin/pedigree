/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/drivers/common/ata/AtaWriteCache.h"
#include "modules/drivers/common/scsi/ScsiController.h"
#include "modules/drivers/common/scsi/ScsiDisk.h"

namespace {
constexpr size_t PageBytes = TargetInfo::getPageSize();
constexpr uint64_t AtaOwnershipLocation = 0;
constexpr uint64_t AtaDrainingLocation = PageBytes;
constexpr uint64_t FailedWriteLocation = 2 * PageBytes;
constexpr uint64_t SuccessfulWriteLocation = 3 * PageBytes;
constexpr uint64_t MissingDirectLocation = 4 * PageBytes;
constexpr uint64_t EditingDirectLocation = 5 * PageBytes;
constexpr uint64_t SuccessfulDirectLocation = 6 * PageBytes;
constexpr uint64_t FailedDirectLocation = 7 * PageBytes;
constexpr uint64_t UnreadyDirectLocation = 8 * PageBytes;
constexpr uint64_t ShortDirectLocation = 9 * PageBytes;
constexpr uint64_t PinnedDirectLocation = 10 * PageBytes;
constexpr uint64_t CancelledDirectLocation = 11 * PageBytes;
constexpr uint64_t CanonicalDirectLocation = 12 * PageBytes;
constexpr uint64_t ReadFirstLocation = 16 * PageBytes;
constexpr uint64_t RejectedReadLocation = 18 * PageBytes;
constexpr uint64_t RetireFirstExtent = 24 * PageBytes;
constexpr uint64_t RetireFirstInterior = RetireFirstExtent + PageBytes;
constexpr uint64_t NonoverlapReadLocation = 28 * PageBytes;
constexpr uint64_t RecheckReadLocation = 32 * PageBytes;
constexpr uint64_t LookupPauseLocation = 36 * PageBytes;

enum class WriteMode { Initialising, FailAll, PassWrite12, UnitNotReady };
enum class RequestEvent : uint8_t { Read = 1, Direct = 2 };

class ScriptedScsiController final : public ScsiController {
 public:
  ScriptedScsiController()
      : ScsiController(),
        m_Mode(WriteMode::Initialising),
        m_WriteOpcodes(),
        m_WriteCount(0),
        m_UnitReadyCount(0),
        m_LastWriteBuffer(0),
        m_DirectRequestCount(0),
        m_DirectDisk(0),
        m_DirectLocation(0),
        m_DirectPage(0),
        m_HoldNextRead(0),
        m_ReadHoldsRemaining(0),
        m_DelegateHeldRead(0),
        m_ReadEntered(0, false),
        m_ReadRelease(0, false),
        m_ReadRequestCount(0),
        m_ReadCommandCount(0),
        m_RequestEvents(),
        m_RequestEventCount(0),
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
        if (bWrite || pRespBuffer || nRespBytes || nCommandSize != 6) {
          m_Valid = false;
          return false;
        }
        if (m_Mode != WriteMode::Initialising)
          ++m_UnitReadyCount;
        return m_Mode != WriteMode::UnitNotReady;
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
        capacity->LBA = HOST_TO_BIG32(static_cast<uint32_t>((64 * PageBytes / 512) - 1));
        capacity->BlockSize = HOST_TO_BIG32(512);
        return true;
      }
      case 0x28:
      case 0xa8:
      case 0x88:
        return handleRead(opcode, nCommandSize, pRespBuffer, nRespBytes, bWrite);
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
    m_UnitReadyCount = 0;
    m_LastWriteBuffer = 0;
    m_DirectRequestCount = 0;
    m_DirectDisk = 0;
    m_DirectLocation = 0;
    m_DirectPage = 0;
  }

  void beginRequestTrace() {
    m_ReadRequestCount = 0;
    m_ReadCommandCount = 0;
    m_RequestEventCount = 0;
  }

  void holdNextRead() {
    const size_t staleEntries = m_ReadEntered.drainAvailable();
    const size_t staleReleases = m_ReadRelease.drainAvailable();
    (void)staleEntries;
    (void)staleReleases;
    m_DelegateHeldRead = 0;
    m_HoldNextRead = 1;
    m_ReadHoldsRemaining = 1;
  }

  void holdFollowingRead() {
    m_ReadHoldsRemaining += 1;
  }

  bool waitForHeldRead() {
    return m_ReadEntered.acquireForCompletion(1, 0, 500000);
  }

  void releaseHeldRead(bool delegate) {
    m_DelegateHeldRead = delegate ? 1 : 0;
    m_ReadRelease.release();
  }

  void disarmReadHold() {
    m_ReadHoldsRemaining = 0;
    m_HoldNextRead = 0;
    const size_t staleEntries = m_ReadEntered.drainAvailable();
    const size_t staleReleases = m_ReadRelease.drainAvailable();
    m_ReadRelease.release();
    (void)staleEntries;
    (void)staleReleases;
  }

  size_t readRequestCount() const {
    return m_ReadRequestCount.value();
  }

  size_t readCommandCount() const {
    return m_ReadCommandCount.value();
  }

  size_t directRequestCount() const {
    return m_DirectRequestCount;
  }

  bool requestTraceMatches(const RequestEvent* expected, size_t count) const {
    return m_Valid && m_RequestEventCount == count &&
           !MemoryCompare(m_RequestEvents, expected, count * sizeof(RequestEvent));
  }

  bool writeTraceMatches(const uint8_t* expected, size_t count) const {
    return m_Valid && m_WriteCount == count && !MemoryCompare(m_WriteOpcodes, expected, count);
  }

  bool directTupleMatches(const ScsiDisk* disk, uint64_t location, uintptr_t page) const {
    return m_Valid && m_DirectRequestCount == 1 &&
           m_DirectDisk == reinterpret_cast<uint64_t>(disk) && m_DirectLocation == location &&
           m_DirectPage == page;
  }

  bool hasNoDirectActivity() const {
    return m_Valid && !m_DirectRequestCount && !m_UnitReadyCount && !m_WriteCount;
  }

  size_t unitReadyCount() const {
    return m_UnitReadyCount;
  }

  size_t writeCount() const {
    return m_WriteCount;
  }

  uintptr_t lastWriteBuffer() const {
    return m_LastWriteBuffer;
  }

  uint64_t executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
                          uint64_t p6, uint64_t p7, uint64_t p8) override {
    if (p1 == SCSI_REQUEST_READ) {
      appendRequestEvent(RequestEvent::Read);
      m_ReadRequestCount += 1;
      size_t remaining = m_ReadHoldsRemaining.value();
      bool hold = false;
      while (remaining && !hold) {
        hold = m_ReadHoldsRemaining.compareAndSwap(remaining, remaining - 1);
        remaining = m_ReadHoldsRemaining.value();
      }
      if (hold) {
        m_HoldNextRead = 2;
        m_ReadEntered.release();
        if (!m_ReadRelease.acquireForCompletion()) {
          m_Valid = false;
          m_HoldNextRead = 0;
          return 0;
        }
        const bool delegate = static_cast<size_t>(m_DelegateHeldRead) != 0;
        m_HoldNextRead = 0;
        if (!delegate)
          return 0;
      }
    }
    if (p1 == SCSI_REQUEST_WRITE_DIRECT) {
      appendRequestEvent(RequestEvent::Direct);
      ++m_DirectRequestCount;
      m_DirectDisk = p2;
      m_DirectLocation = p3;
      m_DirectPage = p4;
    }
    return ScsiController::executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
  }

 protected:
  size_t getNumUnits() override {
    return 1;
  }

 private:
  bool handleRead(uint8_t opcode, uint8_t commandSize, uintptr_t response, uint16_t responseBytes,
                  bool write) {
    const uint8_t expectedSize = opcode == 0x28 ? 10 : (opcode == 0xa8 ? 12 : 16);
    if (write || !response || !responseBytes || (responseBytes % PageBytes) ||
        commandSize != expectedSize) {
      m_Valid = false;
      return false;
    }

    ByteSet(reinterpret_cast<void*>(response), 0x3c, responseBytes);
    m_ReadCommandCount += 1;
    return true;
  }

  bool handleWrite(uint8_t opcode, uint8_t commandSize, uintptr_t response, uint16_t responseBytes,
                   bool write) {
    const uint8_t expectedSize = opcode == 0x2a ? 10 : (opcode == 0xaa ? 12 : 16);
    if (m_Mode == WriteMode::Initialising || !write || !response || responseBytes != PageBytes ||
        commandSize != expectedSize || m_WriteCount >= sizeof(m_WriteOpcodes)) {
      m_Valid = false;
      return false;
    }

    m_WriteOpcodes[m_WriteCount++] = opcode;
    m_LastWriteBuffer = response;
    return m_Mode == WriteMode::PassWrite12 && opcode == 0xaa;
  }

  void appendRequestEvent(RequestEvent event) {
    if (m_RequestEventCount >= sizeof(m_RequestEvents) / sizeof(m_RequestEvents[0])) {
      m_Valid = false;
      return;
    }
    m_RequestEvents[m_RequestEventCount++] = event;
  }

  WriteMode m_Mode;
  uint8_t m_WriteOpcodes[16];
  size_t m_WriteCount;
  size_t m_UnitReadyCount;
  uintptr_t m_LastWriteBuffer;
  size_t m_DirectRequestCount;
  uint64_t m_DirectDisk;
  uint64_t m_DirectLocation;
  uintptr_t m_DirectPage;
  Atomic<size_t> m_HoldNextRead;
  Atomic<size_t> m_ReadHoldsRemaining;
  Atomic<size_t> m_DelegateHeldRead;
  Semaphore m_ReadEntered;
  Semaphore m_ReadRelease;
  Atomic<size_t> m_ReadRequestCount;
  Atomic<size_t> m_ReadCommandCount;
  RequestEvent m_RequestEvents[16];
  size_t m_RequestEventCount;
  bool m_Valid;
};

class HostedScsiDisk final : public ScsiDisk {
 public:
  HostedScsiDisk()
      : ScsiDisk(),
        m_OverrideDirectResult(false),
        m_DirectResult(0),
        m_ObserveUnpin(false),
        m_UnpinCalls(0),
        m_LastUnpinLocation(0),
        m_CacheFillSize(PageBytes) {}

  bool preparePage(uint64_t location) {
    bool alreadyExisted = false;
    const uintptr_t buffer = getCache().insert(location, &alreadyExisted);
    if (!buffer || alreadyExisted)
      return false;
    ByteSet(reinterpret_cast<void*>(buffer), 0x5a, PageBytes);
    getCache().markNoLongerEditing(location);
    return true;
  }

  bool prepareEditingPage(uint64_t location) {
    bool alreadyExisted = false;
    return getCache().insert(location, &alreadyExisted) && !alreadyExisted;
  }

  bool discardEditingPage(uint64_t location) {
    return getCache().discardEditing(location);
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

  bool evictRange(uint64_t location, size_t length) {
    bool evicted = true;
    for (size_t offset = 0; offset < length; offset += PageBytes) {
      if (hasPage(location + offset) && !getCache().evict(location + offset))
        evicted = false;
    }
    return evicted;
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

  void overrideDirectResult(uint64_t result) {
    m_DirectResult = result;
    m_OverrideDirectResult = true;
  }

  void restoreDirectResult() {
    m_OverrideDirectResult = false;
  }

  void setCacheFillSize(size_t fillSize) {
    m_CacheFillSize = fillSize;
  }

  void beginUnpinObservation() {
    m_UnpinCalls = 0;
    m_LastUnpinLocation = 0;
    m_ObserveUnpin = true;
  }

  size_t endUnpinObservation() {
    m_ObserveUnpin = false;
    return m_UnpinCalls;
  }

  uint64_t lastUnpinLocation() const {
    return m_LastUnpinLocation;
  }

  void unpin(uint64_t location) override {
    if (m_ObserveUnpin) {
      ++m_UnpinCalls;
      m_LastUnpinLocation = location;
      return;
    }
    ScsiDisk::unpin(location);
  }

  uint64_t doWriteDirect(uint64_t location, uintptr_t page) override {
    if (m_OverrideDirectResult)
      return m_DirectResult;
    return ScsiDisk::doWriteDirect(location, page);
  }

 protected:
  size_t getCacheFillSize() const override {
    return m_CacheFillSize;
  }

 private:
  bool m_OverrideDirectResult;
  uint64_t m_DirectResult;
  bool m_ObserveUnpin;
  size_t m_UnpinCalls;
  uint64_t m_LastUnpinLocation;
  size_t m_CacheFillSize;
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

bool waitForAdmissionOrRead(Thread* thread, uint64_t location, ScriptedScsiController& controller,
                            size_t priorReadCount, bool& admissionWait) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    uintptr_t address = 0;
    if (thread->getWaitDebugInfo(info) && info.queue && info.queued &&
        thread->getDebugState(address) == Thread::CondWait) {
      admissionWait = address == location;
      return true;
    }
    if (controller.readRequestCount() > priorReadCount) {
      admissionWait = false;
      return true;
    }
    Scheduler::instance().yield();
  }
  admissionWait = false;
  return false;
}

bool waitForRetireAdmissionOrDrain(Thread* retirer, HostedScsiDisk& disk, uint64_t location,
                                   bool& admissionWait) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    uintptr_t address = 0;
    if (retirer->getWaitDebugInfo(info) && info.queue && info.queued &&
        retirer->getDebugState(address) == Thread::CondWait && address == location) {
      admissionWait = true;
      return true;
    }
    if (!disk.pageAddress(location)) {
      admissionWait = false;
      return true;
    }
    Scheduler::instance().yield();
  }
  admissionWait = false;
  return false;
}

struct ReadContext {
  ReadContext(HostedScsiDisk* pDisk, uint64_t readLocation)
      : disk(pDisk), location(readLocation), returned(0), result(0) {}

  HostedScsiDisk* disk;
  uint64_t location;
  Atomic<size_t> returned;
  bool result;
};

int readPage(void* parameter) {
  auto* context = reinterpret_cast<ReadContext*>(parameter);
  const BufferView result = context->disk->read(context->location);
  context->result = static_cast<bool>(result);
  if (result)
    context->disk->unpin(context->location);
  context->returned += 1;
  return 0;
}

Thread* startRead(ReadContext& context, const char* name) {
  Thread* thread = new Thread(Scheduler::instance().getKernelProcess(), readPage, &context, nullptr,
                              false, true);
  thread->setName(String(name));
  return thread;
}

struct ReadRequestPause {
  ReadRequestPause(HostedScsiDisk* pDisk, uint64_t pageLocation)
      : disk(pDisk), location(pageLocation), state(0), entered(0, false), release(0, false) {}

  void arm() {
    const size_t staleEntries = entered.drainAvailable();
    const size_t staleReleases = release.drainAvailable();
    (void)staleEntries;
    (void)staleReleases;
    state = 1;
    ScsiDisk::setHostedReadRequestHookForTest(pause, this);
  }

  bool wait() {
    return entered.acquireForCompletion(1, 0, 500000);
  }

  void resume() {
    release.release();
  }

  void clear() {
    ScsiDisk::setHostedReadRequestHookForTest(nullptr, nullptr);
    state = 0;
  }

  static void pause(ScsiDisk* pDisk, uint64_t pageLocation, void* parameter) {
    auto* context = reinterpret_cast<ReadRequestPause*>(parameter);
    if (!context || pDisk != context->disk || pageLocation != context->location ||
        !context->state.compareAndSwap(1, 2)) {
      return;
    }

    context->entered.release();
    if (!context->release.acquireForCompletion())
      context->state = 0;
    context->state = 0;
  }

  HostedScsiDisk* disk;
  uint64_t location;
  Atomic<size_t> state;
  Semaphore entered;
  Semaphore release;
};

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

struct DirectRetirementContext {
  DirectRetirementContext(HostedScsiDisk* pDisk, uint64_t pageLocation)
      : disk(pDisk), location(pageLocation), returned(0), succeeded(0) {}

  HostedScsiDisk* disk;
  uint64_t location;
  Atomic<size_t> returned;
  Atomic<size_t> succeeded;
};

int retireDirectPage(void* parameter) {
  auto* context = reinterpret_cast<DirectRetirementContext*>(parameter);
  if (context->disk->retireCachePage(context->location))
    context->succeeded += 1;
  context->returned += 1;
  return 0;
}

bool scsiDirectRetireResult(Fixture& fixture) {
  constexpr uint8_t FailedOpcodes[] = {0x2a, 0x2a, 0x2a, 0xaa, 0xaa, 0xaa, 0x8a, 0x8a, 0x8a};
  constexpr uint8_t SuccessfulOpcodes[] = {0x2a, 0x2a, 0x2a, 0xaa};

  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool missing = fixture.ready && fixture.disk.retireCachePage(MissingDirectLocation) &&
                       fixture.controller.hasNoDirectActivity();

  const bool editingPrepared = fixture.disk.prepareEditingPage(EditingDirectLocation);
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool editingRejected = editingPrepared &&
                               !fixture.disk.retireCachePage(EditingDirectLocation) &&
                               fixture.controller.hasNoDirectActivity();
  const bool editingDiscarded =
      editingPrepared && fixture.disk.discardEditingPage(EditingDirectLocation);

  const bool successfulPrepared = fixture.disk.preparePage(SuccessfulDirectLocation);
  const uintptr_t successfulPage =
      successfulPrepared ? fixture.disk.pageAddress(SuccessfulDirectLocation) : 0;
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  fixture.disk.beginUnpinObservation();
  const bool successfulRetired =
      successfulPage && fixture.disk.retireCachePage(SuccessfulDirectLocation);
  const size_t successfulUnpins = fixture.disk.endUnpinObservation();
  const bool successfulTrace =
      fixture.controller.writeTraceMatches(SuccessfulOpcodes, sizeof(SuccessfulOpcodes));
  const bool successfulTuple = fixture.controller.directTupleMatches(
      &fixture.disk, SuccessfulDirectLocation, successfulPage);
  const bool successful = successfulPrepared && successfulRetired && successfulTrace &&
                          successfulTuple && fixture.controller.unitReadyCount() == 1 &&
                          fixture.controller.lastWriteBuffer() == successfulPage &&
                          successfulUnpins == 0 && !fixture.disk.hasPage(SuccessfulDirectLocation);

  const bool canonicalPrepared = fixture.disk.preparePage(CanonicalDirectLocation);
  const uintptr_t canonicalPage =
      canonicalPrepared ? fixture.disk.pageAddress(CanonicalDirectLocation) : 0;
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool canonicalRetired = canonicalPage &&
                                fixture.disk.retireCachePage(CanonicalDirectLocation + 512) &&
                                fixture.controller.directTupleMatches(
                                    &fixture.disk, CanonicalDirectLocation, canonicalPage) &&
                                fixture.controller.lastWriteBuffer() == canonicalPage &&
                                !fixture.disk.hasPage(CanonicalDirectLocation);
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool endRejected = !fixture.disk.retireCachePage(fixture.disk.getSize()) &&
                           fixture.controller.hasNoDirectActivity();

  const bool failedPrepared = fixture.disk.preparePage(FailedDirectLocation);
  const uintptr_t failedPage = failedPrepared ? fixture.disk.pageAddress(FailedDirectLocation) : 0;
  fixture.controller.beginWrites(WriteMode::FailAll);
  const bool failedRetirement = failedPage && !fixture.disk.retireCachePage(FailedDirectLocation);
  const bool failedTrace =
      fixture.controller.writeTraceMatches(FailedOpcodes, sizeof(FailedOpcodes));
  const bool failedTuple =
      fixture.controller.directTupleMatches(&fixture.disk, FailedDirectLocation, failedPage);
  const bool failedRetained = failedRetirement && fixture.disk.hasPage(FailedDirectLocation);
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool failedRetry =
      failedRetained && fixture.disk.retireCachePage(FailedDirectLocation) &&
      fixture.controller.writeTraceMatches(SuccessfulOpcodes, sizeof(SuccessfulOpcodes)) &&
      !fixture.disk.hasPage(FailedDirectLocation);

  const bool unreadyPrepared = fixture.disk.preparePage(UnreadyDirectLocation);
  const uintptr_t unreadyPage =
      unreadyPrepared ? fixture.disk.pageAddress(UnreadyDirectLocation) : 0;
  fixture.controller.beginWrites(WriteMode::UnitNotReady);
  const bool unreadyRetirement =
      unreadyPage && !fixture.disk.retireCachePage(UnreadyDirectLocation);
  const bool unready =
      unreadyRetirement &&
      fixture.controller.directTupleMatches(&fixture.disk, UnreadyDirectLocation, unreadyPage) &&
      fixture.controller.unitReadyCount() == 3 && fixture.controller.writeCount() == 0 &&
      fixture.disk.hasPage(UnreadyDirectLocation);
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool unreadyRetry = unready && fixture.disk.retireCachePage(UnreadyDirectLocation) &&
                            !fixture.disk.hasPage(UnreadyDirectLocation);

  const bool shortPrepared = fixture.disk.preparePage(ShortDirectLocation);
  const uintptr_t shortPage = shortPrepared ? fixture.disk.pageAddress(ShortDirectLocation) : 0;
  fixture.disk.overrideDirectResult(1);
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool shortRetirement = shortPage && !fixture.disk.retireCachePage(ShortDirectLocation);
  const bool shortResultRejected =
      shortRetirement &&
      fixture.controller.directTupleMatches(&fixture.disk, ShortDirectLocation, shortPage) &&
      fixture.controller.unitReadyCount() == 0 && fixture.controller.writeCount() == 0 &&
      fixture.disk.hasPage(ShortDirectLocation);
  fixture.disk.restoreDirectResult();
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool shortRetry = shortResultRejected &&
                          fixture.disk.retireCachePage(ShortDirectLocation) &&
                          !fixture.disk.hasPage(ShortDirectLocation);

  const bool passed = missing && editingRejected && editingDiscarded && successful &&
                      canonicalPrepared && canonicalRetired && endRejected && failedRetained &&
                      failedTrace && failedTuple && failedRetry && unreadyPrepared &&
                      unreadyRetry && shortPrepared && shortRetry;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS scsi-direct-retire-result");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL scsi-direct-retire-result: DIRECT routing, exact result, "
        "retry, or supplied-page write semantics changed");
  }
  return passed;
}

bool scsiDirectRetireOwnership(Fixture& fixture) {
  constexpr uint8_t SuccessfulOpcodes[] = {0x2a, 0x2a, 0x2a, 0xaa};

  const bool pinnedPrepared = fixture.disk.preparePage(PinnedDirectLocation);
  const uintptr_t pinnedPage = pinnedPrepared ? fixture.disk.pageAddress(PinnedDirectLocation) : 0;
  const bool pinned = pinnedPage && fixture.disk.pin(PinnedDirectLocation);
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  DirectRetirementContext context(&fixture.disk, PinnedDirectLocation);
  Thread* retirer = nullptr;
  if (pinned) {
    retirer = new Thread(Scheduler::instance().getKernelProcess(), retireDirectPage, &context,
                         nullptr, false, true);
    retirer->setName("hosted SCSI direct-page retirer");
  }

  const bool drainPublished =
      retirer && waitUntilQueuedAt(retirer, Thread::CallbackDrain, PinnedDirectLocation);
  const bool blockedWithoutIo = drainPublished && !static_cast<size_t>(context.returned) &&
                                fixture.controller.hasNoDirectActivity();
  if (pinned)
    fixture.disk.unpin(PinnedDirectLocation);
  const bool completed =
      retirer && (static_cast<size_t>(context.returned) || waitUntilSet(context.returned));
  const bool joined = retirer && retirer->joinForCompletion();
  const bool pinnedRetired =
      completed && joined && context.succeeded == 1 &&
      fixture.controller.directTupleMatches(&fixture.disk, PinnedDirectLocation, pinnedPage) &&
      fixture.controller.writeTraceMatches(SuccessfulOpcodes, sizeof(SuccessfulOpcodes)) &&
      fixture.controller.lastWriteBuffer() == pinnedPage &&
      !fixture.disk.hasPage(PinnedDirectLocation);

  const bool cancelledPrepared = fixture.disk.preparePage(CancelledDirectLocation);
  const uintptr_t cancelledPage =
      cancelledPrepared ? fixture.disk.pageAddress(CancelledDirectLocation) : 0;
  const bool halted = cancelledPage && fixture.controller.halt();
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  fixture.disk.beginUnpinObservation();
  const bool cancelled = halted && !fixture.disk.retireCachePage(CancelledDirectLocation);
  const size_t cancelledUnpins = fixture.disk.endUnpinObservation();
  const uint64_t cancelledUnpinLocation = fixture.disk.lastUnpinLocation();
  const bool cancellationPreserved = cancelled && fixture.controller.hasNoDirectActivity() &&
                                     cancelledUnpins == 0 && cancelledUnpinLocation == 0 &&
                                     fixture.disk.hasPage(CancelledDirectLocation);
  const bool resumed = halted && fixture.controller.resume();
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool cancellationRetry = cancellationPreserved && resumed &&
                                 fixture.disk.retireCachePage(CancelledDirectLocation) &&
                                 fixture.controller.directTupleMatches(
                                     &fixture.disk, CancelledDirectLocation, cancelledPage) &&
                                 fixture.controller.lastWriteBuffer() == cancelledPage &&
                                 !fixture.disk.hasPage(CancelledDirectLocation);

  const bool passed = pinnedPrepared && pinned && drainPublished && blockedWithoutIo &&
                      pinnedRetired && cancelledPrepared && cancellationRetry;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS scsi-direct-retire-ownership");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL scsi-direct-retire-ownership: borrowed DIRECT pages were "
        "used before drain or released by execution/cancellation");
  }
  return passed;
}

bool scsiReadRetireAdmission(Fixture& fixture) {
  constexpr RequestEvent ExpectedOrder[] = {RequestEvent::Read, RequestEvent::Direct,
                                            RequestEvent::Read};

  fixture.disk.setCacheFillSize(PageBytes);
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  fixture.controller.beginRequestTrace();
  fixture.controller.holdNextRead();

  ReadContext firstRead(&fixture.disk, ReadFirstLocation);
  Thread* firstReader = startRead(firstRead, "hosted SCSI admitted first reader");
  const bool firstReadEntered = firstReader && fixture.controller.waitForHeldRead();
  const bool prepared = firstReadEntered && fixture.disk.preparePage(ReadFirstLocation);
  const bool retireSerialPin = prepared && fixture.disk.pin(ReadFirstLocation);

  DirectRetirementContext retirement(&fixture.disk, ReadFirstLocation);
  Thread* retirer = nullptr;
  if (retireSerialPin) {
    retirer = new Thread(Scheduler::instance().getKernelProcess(), retireDirectPage, &retirement,
                         nullptr, false, true);
    retirer->setName("hosted SCSI queued-read retirement");
  }

  bool retireAdmissionWait = false;
  const bool retireStopped =
      retirer &&
      waitForRetireAdmissionOrDrain(retirer, fixture.disk, ReadFirstLocation, retireAdmissionWait);
  const bool retireIntentQueued = retireStopped && retireAdmissionWait;
  const bool pageStayedVisible = retireIntentQueued && fixture.disk.pageAddress(ReadFirstLocation);

  DirectRetirementContext secondRetirement(&fixture.disk, ReadFirstLocation);
  Thread* secondRetirer = nullptr;
  if (retirer) {
    secondRetirer = new Thread(Scheduler::instance().getKernelProcess(), retireDirectPage,
                               &secondRetirement, nullptr, false, true);
    secondRetirer->setName("hosted SCSI second queued retirement");
  }
  bool secondRetireAdmissionWait = false;
  const bool secondRetireStopped =
      secondRetirer && waitForRetireAdmissionOrDrain(secondRetirer, fixture.disk, ReadFirstLocation,
                                                     secondRetireAdmissionWait);
  const bool retiresSerialised = secondRetireStopped && secondRetireAdmissionWait;

  fixture.controller.holdFollowingRead();
  ReadContext secondRead(&fixture.disk, ReadFirstLocation);
  Thread* secondReader = startRead(secondRead, "hosted SCSI writer-preference reader");
  bool secondAdmissionWait = false;
  const bool secondStopped =
      secondReader && waitForAdmissionOrRead(secondReader, ReadFirstLocation, fixture.controller, 1,
                                             secondAdmissionWait);
  const bool writerPreferred = secondStopped && secondAdmissionWait;
  const bool safeToDelegate =
      retireIntentQueued && pageStayedVisible && retiresSerialised && writerPreferred;
  fixture.controller.releaseHeldRead(safeToDelegate);

  const bool firstCompleted =
      firstReader && (static_cast<size_t>(firstRead.returned) || waitUntilSet(firstRead.returned));
  bool secondReadEntered = false;
  if (!writerPreferred) {
    secondReadEntered = fixture.controller.waitForHeldRead();
    fixture.controller.releaseHeldRead(false);
  }
  const bool firstRetireDraining =
      safeToDelegate && firstCompleted &&
      waitUntilQueuedAt(retirer, Thread::CallbackDrain, ReadFirstLocation);
  const bool secondRetireStillQueued =
      firstRetireDraining && waitUntilQueuedAt(secondRetirer, Thread::CondWait, ReadFirstLocation);
  if (retireSerialPin)
    fixture.disk.unpin(ReadFirstLocation);
  const bool retireCompleted =
      retirer && (static_cast<size_t>(retirement.returned) || waitUntilSet(retirement.returned));
  const bool secondRetireCompleted =
      secondRetirer &&
      (static_cast<size_t>(secondRetirement.returned) || waitUntilSet(secondRetirement.returned));
  if (secondReader && writerPreferred) {
    secondReadEntered = fixture.controller.waitForHeldRead();
    fixture.controller.releaseHeldRead(true);
  }
  const bool secondCompleted = secondReader && (static_cast<size_t>(secondRead.returned) ||
                                                waitUntilSet(secondRead.returned));
  const bool firstJoined = firstReader && firstReader->joinForCompletion();
  const bool retireJoined = retirer && retirer->joinForCompletion();
  const bool secondRetireJoined = secondRetirer && secondRetirer->joinForCompletion();
  const bool secondJoined = secondReader && secondReader->joinForCompletion();

  const bool ordered = fixture.controller.requestTraceMatches(
      ExpectedOrder, sizeof(ExpectedOrder) / sizeof(ExpectedOrder[0]));
  const bool queueCycleClosed =
      firstCompleted && firstRetireDraining && secondRetireStillQueued && retireCompleted &&
      secondRetireCompleted && secondCompleted && firstJoined && retireJoined &&
      secondRetireJoined && secondJoined && secondReadEntered && firstRead.result &&
      retirement.succeeded == 1 && secondRetirement.succeeded == 1 && secondRead.result &&
      ordered && fixture.controller.readCommandCount() == 1 &&
      fixture.controller.directRequestCount() == 1 && fixture.disk.hasPage(ReadFirstLocation);

  const bool halted = fixture.controller.halt();
  const bool rejectedRead = halted && static_cast<bool>(fixture.disk.read(RejectedReadLocation));
  DirectRetirementContext rejectedRetirement(&fixture.disk, RejectedReadLocation);
  Thread* rejectedRetirer = nullptr;
  if (halted && !rejectedRead) {
    rejectedRetirer = new Thread(Scheduler::instance().getKernelProcess(), retireDirectPage,
                                 &rejectedRetirement, nullptr, false, true);
    rejectedRetirer->setName("hosted SCSI rejected-read retirement");
  }
  const bool rejectedCompleted =
      rejectedRetirer && (static_cast<size_t>(rejectedRetirement.returned) ||
                          waitUntilSet(rejectedRetirement.returned));
  const bool rejectedJoined = rejectedRetirer && rejectedRetirer->joinForCompletion();
  const bool resumed = halted && fixture.controller.resume();
  const bool rejectionReleased =
      !rejectedRead && rejectedCompleted && rejectedJoined && rejectedRetirement.succeeded == 1;

  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool cleaned = fixture.disk.retireCachePage(ReadFirstLocation);
  const bool passed = fixture.ready && prepared && retireSerialPin && firstReadEntered &&
                      retireIntentQueued && pageStayedVisible && retiresSerialised &&
                      writerPreferred && queueCycleClosed && halted && resumed &&
                      rejectionReleased && cleaned;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS scsi-read-retire-admission");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL scsi-read-retire-admission: queued reads, retirement intent, "
        "writer preference, or rejected-request cleanup changed");
  }
  return passed;
}

bool retireCachedRange(Fixture& fixture, uint64_t location, size_t length) {
  bool retired = true;
  for (size_t offset = 0; offset < length; offset += PageBytes) {
    if (!fixture.disk.hasPage(location + offset))
      continue;
    fixture.controller.beginWrites(WriteMode::PassWrite12);
    if (!fixture.disk.retireCachePage(location + offset))
      retired = false;
  }
  return retired;
}

bool scsiRetireReadRecheck(Fixture& fixture) {
  constexpr size_t WideFill = PageBytes * 2;
  constexpr RequestEvent RangeOrder[] = {RequestEvent::Read, RequestEvent::Direct,
                                         RequestEvent::Read};
  constexpr RequestEvent FailedRetirementOrder[] = {RequestEvent::Direct};

  fixture.disk.setCacheFillSize(WideFill);
  const bool interiorPrepared = fixture.disk.preparePage(RetireFirstInterior);
  const bool interiorPinned = interiorPrepared && fixture.disk.pin(RetireFirstInterior);
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  fixture.controller.beginRequestTrace();

  DirectRetirementContext rangeRetirement(&fixture.disk, RetireFirstInterior);
  Thread* rangeRetirer = nullptr;
  if (interiorPinned) {
    rangeRetirer = new Thread(Scheduler::instance().getKernelProcess(), retireDirectPage,
                              &rangeRetirement, nullptr, false, true);
    rangeRetirer->setName("hosted SCSI interior-page retirer");
  }
  const bool rangeDrainPublished =
      rangeRetirer && waitUntilQueuedAt(rangeRetirer, Thread::CallbackDrain, RetireFirstInterior);

  ReadContext nonoverlapRead(&fixture.disk, NonoverlapReadLocation);
  Thread* nonoverlapReader = startRead(nonoverlapRead, "hosted SCSI disjoint retirement reader");
  bool nonoverlapCompleted = nonoverlapReader && (static_cast<size_t>(nonoverlapRead.returned) ||
                                                  waitUntilSet(nonoverlapRead.returned));
  bool releasedForRepair = false;
  if (interiorPinned && !nonoverlapCompleted) {
    fixture.disk.unpin(RetireFirstInterior);
    releasedForRepair = true;
    nonoverlapCompleted = waitUntilSet(nonoverlapRead.returned);
  }
  const bool nonoverlapJoined = nonoverlapReader && nonoverlapReader->joinForCompletion();
  const bool disjointProgress =
      nonoverlapCompleted && nonoverlapJoined && nonoverlapRead.result && !releasedForRepair;

  bool overlapAdmissionWait = false;
  bool overlapStopped = false;
  bool overlapReadEntered = false;
  bool overlapCompleted = false;
  bool overlapJoined = false;
  ReadContext overlapRead(&fixture.disk, RetireFirstExtent);
  Thread* overlapReader = nullptr;
  if (disjointProgress) {
    fixture.controller.holdNextRead();
    overlapReader = startRead(overlapRead, "hosted SCSI interior-overlap reader");
    overlapStopped =
        overlapReader && waitForAdmissionOrRead(overlapReader, RetireFirstExtent,
                                                fixture.controller, 1, overlapAdmissionWait);
    if (!overlapAdmissionWait) {
      overlapReadEntered = fixture.controller.waitForHeldRead();
      fixture.controller.releaseHeldRead(false);
    }
  }

  if (interiorPinned && !releasedForRepair)
    fixture.disk.unpin(RetireFirstInterior);
  const bool rangeRetireCompleted =
      rangeRetirer &&
      (static_cast<size_t>(rangeRetirement.returned) || waitUntilSet(rangeRetirement.returned));
  const bool rangeRetireJoined = rangeRetirer && rangeRetirer->joinForCompletion();

  if (overlapReader && overlapAdmissionWait) {
    overlapReadEntered = fixture.controller.waitForHeldRead();
    fixture.controller.releaseHeldRead(true);
  }
  if (overlapReader) {
    overlapCompleted =
        static_cast<size_t>(overlapRead.returned) || waitUntilSet(overlapRead.returned);
    overlapJoined = overlapReader->joinForCompletion();
  }

  const bool rangeOrder = fixture.controller.requestTraceMatches(
      RangeOrder, sizeof(RangeOrder) / sizeof(RangeOrder[0]));
  const bool rangeAdmission =
      rangeDrainPublished && disjointProgress && overlapStopped && overlapAdmissionWait &&
      overlapReadEntered && rangeRetireCompleted && rangeRetireJoined &&
      rangeRetirement.succeeded == 1 && overlapCompleted && overlapJoined && overlapRead.result &&
      fixture.controller.readCommandCount() == 2 && rangeOrder &&
      fixture.disk.hasPage(RetireFirstExtent) && fixture.disk.hasPage(RetireFirstInterior);
  const bool rangeCleaned = retireCachedRange(fixture, RetireFirstExtent, WideFill) &&
                            retireCachedRange(fixture, NonoverlapReadLocation, WideFill);

  fixture.disk.setCacheFillSize(PageBytes);
  const bool recheckPrepared = fixture.disk.preparePage(RecheckReadLocation);
  const bool recheckPinned = recheckPrepared && fixture.disk.pin(RecheckReadLocation);
  fixture.disk.overrideDirectResult(0);
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  fixture.controller.beginRequestTrace();

  DirectRetirementContext failedRetirement(&fixture.disk, RecheckReadLocation);
  Thread* failedRetirer = nullptr;
  if (recheckPinned) {
    failedRetirer = new Thread(Scheduler::instance().getKernelProcess(), retireDirectPage,
                               &failedRetirement, nullptr, false, true);
    failedRetirer->setName("hosted SCSI failed retirement recheck");
  }
  const bool failureDrainPublished =
      failedRetirer && waitUntilQueuedAt(failedRetirer, Thread::CallbackDrain, RecheckReadLocation);

  fixture.controller.holdNextRead();
  ReadContext recheckRead(&fixture.disk, RecheckReadLocation);
  Thread* recheckReader = startRead(recheckRead, "hosted SCSI reopened-page reader");
  bool recheckAdmissionWait = false;
  const bool recheckStopped =
      recheckReader && waitForAdmissionOrRead(recheckReader, RecheckReadLocation,
                                              fixture.controller, 0, recheckAdmissionWait);
  bool recheckReadEntered = false;
  if (!recheckAdmissionWait) {
    recheckReadEntered = fixture.controller.waitForHeldRead();
    fixture.controller.releaseHeldRead(false);
  }

  if (recheckPinned)
    fixture.disk.unpin(RecheckReadLocation);
  const bool failedRetireCompleted =
      failedRetirer &&
      (static_cast<size_t>(failedRetirement.returned) || waitUntilSet(failedRetirement.returned));
  const bool failedRetireJoined = failedRetirer && failedRetirer->joinForCompletion();
  bool recheckCompleted = recheckReader && (static_cast<size_t>(recheckRead.returned) ||
                                            waitUntilSet(recheckRead.returned));
  if (!recheckCompleted) {
    recheckReadEntered = fixture.controller.waitForHeldRead();
    fixture.controller.releaseHeldRead(false);
    recheckCompleted = waitUntilSet(recheckRead.returned);
  } else if (!fixture.controller.readRequestCount()) {
    fixture.controller.disarmReadHold();
  }
  const bool recheckJoined = recheckReader && recheckReader->joinForCompletion();
  const bool failedOrder = fixture.controller.requestTraceMatches(
      FailedRetirementOrder, sizeof(FailedRetirementOrder) / sizeof(FailedRetirementOrder[0]));
  const bool failureRechecked = failureDrainPublished && recheckStopped && recheckAdmissionWait &&
                                !recheckReadEntered && failedRetireCompleted &&
                                failedRetireJoined && failedRetirement.succeeded == 0 &&
                                recheckCompleted && recheckJoined && recheckRead.result &&
                                fixture.controller.readRequestCount() == 0 && failedOrder &&
                                fixture.disk.hasPage(RecheckReadLocation);

  fixture.disk.restoreDirectResult();
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool recheckCleaned = fixture.disk.retireCachePage(RecheckReadLocation);

  constexpr RequestEvent LookupPauseOrder[] = {RequestEvent::Read, RequestEvent::Direct};
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  fixture.controller.beginRequestTrace();
  ReadRequestPause lookupPause(&fixture.disk, LookupPauseLocation);
  lookupPause.arm();

  ReadContext pausedRead(&fixture.disk, LookupPauseLocation);
  Thread* pausedReader = startRead(pausedRead, "hosted SCSI pre-lookup reader");
  const bool lookupPauseEntered = pausedReader && lookupPause.wait();
  const bool readPublished = lookupPauseEntered && fixture.disk.hasPage(LookupPauseLocation);

  ReadContext sharedRead(&fixture.disk, LookupPauseLocation);
  Thread* sharedReader = nullptr;
  if (readPublished) {
    sharedReader = startRead(sharedRead, "hosted SCSI shared cache-hit reader");
  }
  const bool sharedCompletedBeforeRelease =
      sharedReader &&
      (static_cast<size_t>(sharedRead.returned) || waitUntilSet(sharedRead.returned));

  DirectRetirementContext lookupRetirement(&fixture.disk, LookupPauseLocation);
  Thread* lookupRetirer = nullptr;
  if (sharedReader) {
    lookupRetirer = new Thread(Scheduler::instance().getKernelProcess(), retireDirectPage,
                               &lookupRetirement, nullptr, false, true);
    lookupRetirer->setName("hosted SCSI pre-lookup retirer");
  }
  bool lookupRetireAdmissionWait = false;
  const bool lookupRetireStopped =
      lookupRetirer &&
      waitForRetireAdmissionOrDrain(lookupRetirer, fixture.disk, LookupPauseLocation,
                                    lookupRetireAdmissionWait);
  const bool admissionHeldThroughLookup = lookupRetireStopped && lookupRetireAdmissionWait &&
                                          fixture.disk.pageAddress(LookupPauseLocation);

  lookupPause.resume();
  const bool pausedCompleted = pausedReader && (static_cast<size_t>(pausedRead.returned) ||
                                                waitUntilSet(pausedRead.returned));
  const bool sharedCompleted = sharedReader && (static_cast<size_t>(sharedRead.returned) ||
                                                waitUntilSet(sharedRead.returned));
  const bool lookupRetireCompleted =
      lookupRetirer &&
      (static_cast<size_t>(lookupRetirement.returned) || waitUntilSet(lookupRetirement.returned));
  const bool pausedJoined = pausedReader && pausedReader->joinForCompletion();
  const bool sharedJoined = sharedReader && sharedReader->joinForCompletion();
  const bool lookupRetireJoined = lookupRetirer && lookupRetirer->joinForCompletion();
  lookupPause.clear();

  const bool lookupOrder = fixture.controller.requestTraceMatches(
      LookupPauseOrder, sizeof(LookupPauseOrder) / sizeof(LookupPauseOrder[0]));
  const bool finalLookupProtected =
      lookupPauseEntered && readPublished && sharedCompletedBeforeRelease &&
      admissionHeldThroughLookup && pausedCompleted && sharedCompleted && lookupRetireCompleted &&
      pausedJoined && sharedJoined && lookupRetireJoined && pausedRead.result &&
      sharedRead.result && lookupRetirement.succeeded == 1 &&
      fixture.controller.readRequestCount() == 1 && fixture.controller.readCommandCount() == 1 &&
      fixture.controller.directRequestCount() == 1 && lookupOrder;
  fixture.controller.beginWrites(WriteMode::PassWrite12);
  const bool lookupCleaned = !fixture.disk.hasPage(LookupPauseLocation) ||
                             fixture.disk.retireCachePage(LookupPauseLocation);

  const bool passed = interiorPrepared && interiorPinned && rangeAdmission && rangeCleaned &&
                      recheckPrepared && recheckPinned && failureRechecked && recheckCleaned &&
                      finalLookupProtected && lookupCleaned;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS scsi-retire-read-recheck");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL scsi-retire-read-recheck: interior overlap, disjoint progress, "
        "failed-retirement cache recheck, shared reads, or final lookup changed");
  }
  return passed;
}
}  // namespace

EXPORTED_PUBLIC bool runHostedScsiWriteRegressions() {
  Fixture fixture;
  const bool ataOwnership = ataQueuedWriteCacheOwnership(fixture);
  const bool scsiResult = scsiWriteResult(fixture);
  const bool directResult = scsiDirectRetireResult(fixture);
  const bool directOwnership = scsiDirectRetireOwnership(fixture);
  const bool readRetireAdmission = scsiReadRetireAdmission(fixture);
  const bool retireReadRecheck = scsiRetireReadRecheck(fixture);
  return ataOwnership && scsiResult && directResult && directOwnership && readRetireAdmission &&
         retireReadRecheck;
}
