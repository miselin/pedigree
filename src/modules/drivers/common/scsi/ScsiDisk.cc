/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "ScsiDisk.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceFeatures.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/PointerGuard.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "ScsiCommands.h"
#include "ScsiController.h"

#define READAHEAD_ENABLED 0

#ifdef SCSI_DEBUG
#define SCSI_DEBUG_LOG DEBUG_LOG
#else
#define SCSI_DEBUG_LOG(...)
#endif

namespace {
constexpr size_t ScsiCachePageBytes = TargetInfo::getPageSize();

size_t cacheExtentLength(size_t validLength) {
  if (!validLength || validLength > (~static_cast<size_t>(0) - (ScsiCachePageBytes - 1))) {
    return 0;
  }
  return (validLength + ScsiCachePageBytes - 1) & ~(ScsiCachePageBytes - 1);
}

bool discardEditingRange(Cache& cache, uintptr_t key, size_t length) {
  bool discarded = true;
  for (size_t offset = 0; offset < length; offset += ScsiCachePageBytes) {
    if (!cache.discardEditing(key + offset)) {
      discarded = false;
    }
  }
  return discarded;
}

class CacheFillGuard {
 public:
  CacheFillGuard(Cache& cache, uintptr_t key, size_t length)
      : m_Cache(cache), m_Key(key), m_Length(length), m_Published(false) {}

  ~CacheFillGuard() {
    if (m_Published) {
      return;
    }

    if (!discardEditingRange(m_Cache, m_Key, m_Length)) {
      WARNING(
          "ScsiDisk could not discard every page from a failed cache "
          "fill at "
          << m_Key);
    }
  }

  void publish() {
    m_Cache.markNoLongerEditing(m_Key, m_Length);
    m_Published = true;
  }

 private:
  CacheFillGuard(const CacheFillGuard&) = delete;
  CacheFillGuard& operator=(const CacheFillGuard&) = delete;

  Cache& m_Cache;
  uintptr_t m_Key;
  size_t m_Length;
  bool m_Published;
};
}  // namespace

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
Mutex ScsiDisk::m_HostedReadRequestHookLock;
ScsiDisk::HostedReadRequestHook ScsiDisk::m_HostedReadRequestHook = nullptr;
void* ScsiDisk::m_HostedReadRequestHookContext = nullptr;

void ScsiDisk::setHostedReadRequestHookForTest(HostedReadRequestHook hook, void* context) {
  LockGuard<Mutex> guard(m_HostedReadRequestHookLock);
  m_HostedReadRequestHookContext = context;
  m_HostedReadRequestHook = hook;
}
#endif

ScsiDisk::CacheRangeAdmission::CacheRangeAdmission(ScsiDisk& disk, uint64_t start, size_t length,
                                                   bool retire)
    : m_Disk(disk),
      m_Start(start),
      m_Length(length),
      m_Retire(retire),
      m_Linked(false),
      m_Previous(nullptr),
      m_Next(nullptr),
      m_TerminationDeferral(),
      m_StackDiscardScope(&CacheRangeAdmission::discard, this) {
  m_Disk.enterCacheRange(*this);
}

ScsiDisk::CacheRangeAdmission::~CacheRangeAdmission() {
  m_Disk.leaveCacheRange(*this);
}

void ScsiDisk::CacheRangeAdmission::discard(void* context) {
  CacheRangeAdmission* admission = reinterpret_cast<CacheRangeAdmission*>(context);
  admission->m_Disk.leaveCacheRange(*admission);
}

bool ScsiDisk::cacheRangesOverlap(uint64_t firstStart, size_t firstLength, uint64_t secondStart,
                                  size_t secondLength) {
  if (!firstLength || !secondLength) {
    return false;
  }

  if (firstStart <= secondStart) {
    return (secondStart - firstStart) < firstLength;
  }
  return (firstStart - secondStart) < secondLength;
}

bool ScsiDisk::cacheRangeBlocked(const CacheRangeAdmission& admission) const {
  for (CacheRangeAdmission* earlier = m_FirstCacheRangeAdmission; earlier != &admission;
       earlier = earlier->m_Next) {
    assert(earlier);
    if ((admission.m_Retire || earlier->m_Retire) &&
        cacheRangesOverlap(admission.m_Start, admission.m_Length, earlier->m_Start,
                           earlier->m_Length)) {
      return true;
    }
  }
  return false;
}

void ScsiDisk::enterCacheRange(CacheRangeAdmission& admission) {
  while (true) {
    auto guard = m_CacheRangeWaiters.acquire();
    if (!admission.m_Linked) {
      admission.m_Previous = m_LastCacheRangeAdmission;
      if (m_LastCacheRangeAdmission) {
        m_LastCacheRangeAdmission->m_Next = &admission;
      } else {
        m_FirstCacheRangeAdmission = &admission;
      }
      m_LastCacheRangeAdmission = &admission;
      admission.m_Linked = true;
    }

    if (!cacheRangeBlocked(admission)) {
      return;
    }

    const WaitQueue::WakeReason reason =
        guard.waitForCompletion(WaitQueue::Channel(this), Thread::CondWait, admission.m_Start);
    (void)reason;
  }
}

void ScsiDisk::leaveCacheRange(CacheRangeAdmission& admission) {
  auto guard = m_CacheRangeWaiters.acquire();
  if (!admission.m_Linked) {
    return;
  }
  if (admission.m_Previous) {
    admission.m_Previous->m_Next = admission.m_Next;
  } else {
    m_FirstCacheRangeAdmission = admission.m_Next;
  }
  if (admission.m_Next) {
    admission.m_Next->m_Previous = admission.m_Previous;
  } else {
    m_LastCacheRangeAdmission = admission.m_Previous;
  }
  admission.m_Linked = false;
  admission.m_Previous = nullptr;
  admission.m_Next = nullptr;
  guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
}

bool ScsiDisk::cacheCallback(CacheConstants::CallbackCause cause, uintptr_t loc, uintptr_t page,
                             void* meta) {
  ScsiDisk* pDisk = reinterpret_cast<ScsiDisk*>(meta);

  switch (cause) {
    case CacheConstants::WriteBack: {
      // Cache shutdown runs after external operations have been closed,
      // so writeback must use the internal path.
      return pDisk->flushCachePage(loc, page);
    }
    case CacheConstants::Eviction:
      // no-op for ScsiDisk
      return true;
    default:
      WARNING(
          "ScsiDisk: unknown cache callback -- could indicate "
          "potential future I/O issues.");
      return false;
  }
}

bool ScsiDisk::retireCachePageCallback(uintptr_t key, uintptr_t page, void* meta) {
  ScsiDisk* disk = reinterpret_cast<ScsiDisk*>(meta);
  if (!disk || !page) {
    return false;
  }

  ScsiController* controller = static_cast<ScsiController*>(disk->m_pParent);
  if (!controller) {
    return false;
  }

  const uint64_t result =
      controller->addRequest(0, RequestQueue::NewRequest, SCSI_REQUEST_WRITE_DIRECT,
                             reinterpret_cast<uint64_t>(disk), key, page);
  return result == disk->getCachePageValidLength(key);
}

ScsiDisk::ScsiDisk()
    : Disk(),
      m_Cache(PhysicalMemoryManager::below4GB),
      m_Inquiry(0),
      m_CacheRangeWaiters(),
      m_FirstCacheRangeAdmission(nullptr),
      m_LastCacheRangeAdmission(nullptr),
      m_AlignmentLock(),
      m_HasShiftedCacheAlignment(false),
      m_NumBlocks(0),
      m_BlockSize(ScsiCachePageBytes),
      m_NativeBlockSize(0),
      m_DeviceType(NoDevice) {
  reserveEndpoint();
  m_Cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  m_Cache.setCallback(cacheCallback, this);
  m_Cache.setBackgroundWriteback(syncCacheBatch);
}

ScsiDisk::~ScsiDisk() {
  retireEndpoint();
  shutdownCache();
}

void ScsiDisk::shutdownCache() {
  if (!m_Cache.shutdown())
    panic("SCSI: cache shutdown failed; unwritten data remains");
}

void ScsiDisk::shutdownDeviceCache() {
#if !CRIPPLE_HDD
  // Paging and direct writes can outlive every page in the software cache.
  // Controller admission is closed, but its worker still accepts this barrier.
  auto* controller = static_cast<ScsiController*>(m_pParent);
  if (!controller || !controller->addRequest(0, RequestQueue::NewRequest, SCSI_REQUEST_SYNC,
                                             reinterpret_cast<uint64_t>(this), SyncWholeDevice))
    panic("SCSI: shutdown aborted after final device cache flush failure");
#endif
}

bool ScsiDisk::initialise(ScsiController* pController, size_t nUnit) {
  m_pController = pController;
  m_pParent = pController;
  m_nUnit = nUnit;

  m_Inquiry = new Inquiry;

  // Inquire as to the device's state
  /// \todo Use this data to change how read() and write() work
  ScsiCommand* pCommand = new ScsiCommands::Inquiry(sizeof(Inquiry), false);
  bool success = sendCommand(pCommand, reinterpret_cast<uintptr_t>(m_Inquiry), sizeof(Inquiry));
  if (!success) {
    ERROR("ScsiDisk: INQUIRY failed!");
    delete pCommand;
    return false;
  }
  delete pCommand;

  // Get the peripheral type out of the data.
  m_DeviceType = static_cast<ScsiPeripheralType>(m_Inquiry->Peripheral & 0x1F);

  // Ensure the unit is ready before we attempt to do anything more
  if (!unitReady()) {
    // Grab sense data
    Sense* s = new Sense;
    PointerGuard<Sense> guard2(s);
    readSense(s);
    SCSI_DEBUG_LOG("ScsiDisk: Unit not yet ready, sense data: [sk="
                   << s->SenseKey << ", asc=" << s->Asc << ", ascq=" << s->AscQ << "]");

    if (s->SenseKey == 0x2) {
      if (s->Asc == 0x4) {
        if (s->AscQ == 0x2)  // Logical Unit Not Ready, START UNIT Required
        {
          // Start the unit
          pCommand = new ScsiCommands::StartStop(false, true, 1, true);
          success = sendCommand(pCommand, 0, 0, true);
          if (!success) {
            readSense(s);
            ERROR("ScsiDisk: unit startup failed! Sense data: [sk="
                  << s->SenseKey << ", asc=" << s->Asc << ", ascq=" << s->AscQ << "]");
          }
          delete pCommand;
        }
      }
    }

    Time::delay(100 * Time::Multiplier::Millisecond);

    // Attempt to see if the unit is ready again
    if (!unitReady()) {
      readSense(s);
      SCSI_DEBUG_LOG("ScsiDisk: Unit not yet ready, sense data: [sk="
                     << s->SenseKey << ", asc=" << s->Asc << ", ascq=" << s->AscQ << "]");

      Time::delay(100 * Time::Multiplier::Millisecond);

      if (!unitReady()) {
        readSense(s);
        ERROR("ScsiDisk: disk never became ready. Sense data: [sk="
              << s->SenseKey << ", asc=" << s->Asc << ", ascq=" << s->AscQ << "]");
        return false;
      }
    }
  }

  // Get the capacity of the device
  if (!getCapacityInternal(&m_NumBlocks, &m_NativeBlockSize)) {
    ERROR("ScsiDisk: could not determine device capacity");
    return false;
  }
  if (!m_NativeBlockSize || m_NativeBlockSize > ScsiCachePageBytes ||
      (ScsiCachePageBytes % m_NativeBlockSize)) {
    ERROR("ScsiDisk: native block size " << m_NativeBlockSize
                                         << " is incompatible with target cache pages of "
                                         << ScsiCachePageBytes << " bytes");
    return false;
  }
  SCSI_DEBUG_LOG("ScsiDisk: Capacity: "
                 << Dec << m_NumBlocks << " blocks, each " << m_NativeBlockSize << " bytes - "
                 << (m_NativeBlockSize * m_NumBlocks) << Hex << " bytes in total.");

  publishEndpoint();

  // Chat to the partition service and let it pick up that we're around now
  ServiceFeatures* pFeatures = ServiceManager::instance().enumerateOperations(String("partition"));
  Service* pService = ServiceManager::instance().getService(String("partition"));
  if (pFeatures && pFeatures->provides(ServiceFeatures::touch)) {
    NOTICE("Attempting to inform the partitioner of our presence...");
    if (pService) {
      if (pService->serve(ServiceFeatures::touch, static_cast<Disk*>(this),
                          sizeof(static_cast<Disk*>(this))))
        NOTICE("Successful.");
      else
        ERROR("Failed.");
    } else
      ERROR(
          "ScsiDisk: Couldn't tell the partition service about the new "
          "disk presence");
  } else
    ERROR("ScsiDisk: Partition service doesn't appear to support touch");
  return true;
}

bool ScsiDisk::readSense(Sense* sense) {
  ByteSet(sense, 0xFF, sizeof(Sense));

  // Maximum size of sense data is 252 bytes
  ScsiCommand* pCommand = new ScsiCommands::ReadSense(0, sizeof(Sense));

  uint8_t* response = new uint8_t[sizeof(Sense)];
  bool success = sendCommand(pCommand, reinterpret_cast<uintptr_t>(response), sizeof(Sense));
  if (!success) {
    WARNING("ScsiDisk: SENSE command failed");
    delete[] response;
    return false;
  }

  /// \todo get the amount of data received from the SCSI device
  MemoryCopy(sense, response, sizeof(Sense));

  delete[] response;

  return ((sense->ResponseCode & 0x70) == 0x70);
}

bool ScsiDisk::unitReady() {
  ScsiCommand* pCommand = new ScsiCommands::UnitReady();
  bool success = sendCommand(pCommand, 0, 0);
  delete pCommand;

  /// \todo this can fail with UNIT_ATTN or NOT_READY if the device is
  /// removable.
  return success;
}

bool ScsiDisk::getCapacityInternal(size_t* blockNumber, size_t* blockSize) {
  if (!unitReady()) {
    WARNING(
        "ScsiDisk::getCapacityInternal - returning to defaults, unit "
        "not ready");
    *blockNumber = 0;
    *blockSize = defaultBlockSize();
    return false;
  }

  Capacity* capacity = new Capacity;
  PointerGuard<Capacity> guard(capacity);
  ByteSet(capacity, 0, sizeof(Capacity));

  ScsiCommand* pCommand = new ScsiCommands::ReadCapacity10();
  bool success =
      sendCommand(pCommand, reinterpret_cast<uintptr_t>(capacity), sizeof(Capacity), false);
  delete pCommand;
  if (!success) {
    WARNING("ScsiDisk::getCapacityInternal - READ CAPACITY command failed");
    return false;
  }

  *blockNumber = static_cast<size_t>(BIG_TO_HOST32(capacity->LBA)) + 1;
  uint32_t blockSz = BIG_TO_HOST32(capacity->BlockSize);
  *blockSize = blockSz ? blockSz : defaultBlockSize();

  return true;
}

bool ScsiDisk::sendCommand(ScsiCommand* pCommand, uintptr_t pRespBuffer, uint16_t nRespBytes,
                           bool bWrite) {
  uintptr_t pCommandBuffer = 0;
  size_t nCommandSize = pCommand->serialise(pCommandBuffer);
  return m_pController->sendCommand(m_nUnit, pCommandBuffer, nCommandSize, pRespBuffer, nRespBytes,
                                    bWrite);
}

BufferView ScsiDisk::read(uint64_t location) {
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return BufferView();
  uint64_t token;
  return acquireView(location, true, token);
}

BufferView ScsiDisk::acquireView(uint64_t location, bool writable, uint64_t& token) {
  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent) {
    return BufferView();
  }

  OperationBarrier::Lease operation;
  if (!pParent->acquireDiskOperation(operation)) {
    return BufferView();
  }

  const size_t fillSize = getCacheFillSize();
  if (!fillSize || !getNativeBlockSize() || (fillSize % ScsiCachePageBytes) ||
      (fillSize % getNativeBlockSize())) {
    ERROR("ScsiDisk::read - invalid cache or native block size.");
    return BufferView();
  }
  if (location >= getSize()) {
    ERROR("ScsiDisk::read - location too high (" << location << " of " << getSize() << ")");
    return BufferView();
  }
  size_t blockNum = location / getNativeBlockSize();
  if (blockNum >= getBlockCount()) {
    ERROR("ScsiDisk::read - location too high (block " << blockNum << " > " << getBlockCount()
                                                       << ")");
    return BufferView();
  }
  const uint64_t alignPoint = getAlignmentPoint(location);

  const uint64_t pageLocation = location - ((location - alignPoint) % ScsiCachePageBytes);
  token = pageLocation;
  const size_t pageOffset = location - pageLocation;

  // Cache extents follow the most recent alignment point, which may not be
  // aligned to the device's cache block size.
  size_t loc = location - ((location - alignPoint) % fillSize);
  const size_t fillLength = getCacheFillLength(loc);
  const size_t cacheLength = cacheExtentLength(fillLength);
  const size_t validPageLength = getCachePageValidLength(pageLocation);
  if (!cacheLength || pageLocation < loc || (pageLocation - loc) >= cacheLength ||
      pageOffset >= validPageLength) {
    return BufferView();
  }

  // A lookup can return an Editing page. Keep overlapping readers out until
  // the transport has finished filling and publishing the entire extent.
  CacheRangeAdmission admission(*this, loc, cacheLength, true);

  uintptr_t buffer;
  if ((buffer = m_Cache.lookup(pageLocation))) {
    if (writable && !m_Cache.beginMutableLoan(pageLocation)) {
      m_Cache.release(pageLocation);
      return {};
    }
    return BufferView::fromAddress(buffer + pageOffset, validPageLength - pageOffset);
  }

  uint64_t numRead =
      pParent->supportsConcurrentReads()
          ? doRead(loc)
          : pParent->addRequest(0, SCSI_REQUEST_READ, reinterpret_cast<uint64_t>(this), loc);
  if (numRead < fillLength) {
    // Failed to read for some reason, expose the failure to our caller.
    WARNING("ScsiDisk::read - short read!");
    return BufferView();
  }
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  HostedReadRequestHook readRequestHook = nullptr;
  void* readRequestHookContext = nullptr;
  {
    LockGuard<Mutex> guard(m_HostedReadRequestHookLock);
    readRequestHook = m_HostedReadRequestHook;
    readRequestHookContext = m_HostedReadRequestHookContext;
  }
  if (readRequestHook) {
    readRequestHook(this, pageLocation, readRequestHookContext);
  }
#endif
#if READAHEAD_ENABLED
  // Async readahead needs request-owned range admission before it can safely
  // outlive this stack record.
  for (size_t i = 0; i < 2; ++i) {
    loc += fillSize;
    pParent->addAsyncRequest(0, SCSI_REQUEST_READ, reinterpret_cast<uint64_t>(this), loc);
  }
#endif
  buffer = m_Cache.lookup(pageLocation);
  if (!buffer) {
    return BufferView();
  }
  if (writable && !m_Cache.beginMutableLoan(pageLocation)) {
    m_Cache.release(pageLocation);
    return {};
  }
  return BufferView::fromAddress(buffer + pageOffset, validPageLength - pageOffset);
}

bool ScsiDisk::readInto(uint64_t location, void* buffer, size_t length) {
  return transferBufferRange(location, buffer, length, false);
}

bool ScsiDisk::readIntoBatch(ReadBuffer* buffers, size_t count) {
  if (count > MaxReadBuffers || (count && !buffers))
    return false;
  for (size_t i = 0; i < count; ++i)
    buffers[i].complete = false;
  if (!count)
    return true;
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;
  auto* controller = static_cast<ScsiController*>(m_pParent);
  OperationBarrier::Lease operation;
  if (!controller || !controller->acquireDiskOperation(operation))
    return false;

  const size_t native = getNativeBlockSize();
  if (!supportsBufferTransfers() || !native || ScsiCachePageBytes % native ||
      hasShiftedCacheAlignment())
    return Disk::readIntoBatch(buffers, count);

  uint64_t keys[MaxReadBuffers];
  uint64_t first = ~uint64_t{0};
  uint64_t end = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto& request = buffers[i];
    if ((!request.buffer && request.length) || request.location > getSize() ||
        request.length > getSize() - request.location)
      return false;
    // Odd sectors and ranges spanning cache pages retain the ordinary path's
    // merge rules. File page batches use independent, sector-aligned ranges.
    const uint64_t key = request.location - request.location % ScsiCachePageBytes;
    if (!request.length || request.location % native || request.length % native ||
        request.length > ScsiCachePageBytes - (request.location - key) ||
        key > ~uint64_t{0} - ScsiCachePageBytes || key > ~uintptr_t{0})
      return Disk::readIntoBatch(buffers, count);
    keys[i] = key;
    if (key < first)
      first = key;
    if (key + ScsiCachePageBytes > end)
      end = key + ScsiCachePageBytes;
  }
  if (end - first > ~size_t{0})
    return Disk::readIntoBatch(buffers, count);

  for (size_t attempt = 0; attempt < 8; ++attempt) {
    uint64_t retryKey = 0;
    bool retry = false;
    {
      // One admission also covers repeated/sub-page extents without waiting on
      // another range owned by this same batch. Retain it until DMA is drained.
      CacheRangeAdmission admission(*this, first, end - first, true);
      if (hasShiftedCacheAlignment())
        return false;
      ReadBuffer pending[MaxReadBuffers];
      size_t indices[MaxReadBuffers];
      size_t nPending = 0;
      for (size_t i = 0; i < count; ++i) {
        if (buffers[i].complete)
          continue;
        uintptr_t page = 0;
        if (!m_Cache.lookupStable(keys[i], page)) {
          retryKey = keys[i];
          retry = true;
          break;
        }
        if (page) {
          CachePageGuard guard(m_Cache, keys[i]);
          MemoryCopy(buffers[i].buffer,
                     reinterpret_cast<void*>(page + buffers[i].location - keys[i]),
                     buffers[i].length);
          buffers[i].complete = true;
        } else {
          indices[nPending] = i;
          pending[nPending++] = buffers[i];
        }
      }
      if (!retry) {
        const bool success = !nPending || transferReadBuffers(pending, nPending);
        for (size_t i = 0; i < nPending; ++i)
          buffers[indices[i]].complete = pending[i].complete;
        return success;
      }
    }
    // Writeback callbacks may themselves acquire range admission.
    uintptr_t page = 0;
    if (attempt == 7 || !m_Cache.lookupStable(retryKey, page, true))
      return false;
    if (page)
      m_Cache.release(retryKey);
  }
  return false;
}

bool ScsiDisk::transferReadBuffers(ReadBuffer* buffers, size_t count) {
  bool success = true;
  for (size_t i = 0; i < count; ++i) {
    auto& request = buffers[i];
    request.complete = transferBuffer(request.location, request.buffer, request.length, false);
    success &= request.complete;
  }
  return success;
}

bool ScsiDisk::zero(uint64_t location, size_t length) {
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;
  auto* controller = static_cast<ScsiController*>(m_pParent);
  OperationBarrier::Lease operation;
  if (!controller || !controller->acquireDiskOperation(operation) || location > getSize() ||
      length > getSize() - location)
    return false;
  if (!length)
    return true;
  // Legacy read requests can fill larger extents: retain their existing
  // publication rules instead of leaving a partially populated extent.
  if (!getNativeBlockSize() || ScsiCachePageBytes % getNativeBlockSize() ||
      getCacheFillSize() != ScsiCachePageBytes || hasShiftedCacheAlignment() ||
      location % ScsiCachePageBytes || length % ScsiCachePageBytes)
    return Disk::zero(location, length);
  CacheRangeAdmission admission(*this, location, length, true);
  while (length) {
    const uintptr_t existing = m_Cache.lookup(location);
    const uintptr_t page = existing ? existing : m_Cache.insert(location);
    if (!page)
      return false;
    ByteSet(reinterpret_cast<void*>(page), 0, ScsiCachePageBytes);
    if (!existing)
      m_Cache.markNoLongerEditing(location);
    m_Cache.markDirty(location);
    if (existing)
      m_Cache.release(location);
    location += ScsiCachePageBytes;
    length -= ScsiCachePageBytes;
  }
  return true;
}

bool ScsiDisk::writeFrom(uint64_t location, const void* buffer, size_t length) {
#if CRIPPLE_HDD
  return !length && location <= getSize();
#else
  return transferBufferRange(location, const_cast<void*>(buffer), length, true);
#endif
}

bool ScsiDisk::transferBuffer(uint64_t location, void* buffer, size_t length, bool writing) {
  return false;
}

bool ScsiDisk::transferBufferRange(uint64_t location, void* buffer, size_t length, bool writing) {
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;
  auto* controller = static_cast<ScsiController*>(m_pParent);
  OperationBarrier::Lease operation;
  if (!controller || !controller->acquireDiskOperation(operation))
    return false;

  if ((!buffer && length) || location > getSize() || length > getSize() - location)
    return false;
  if (!length)
    return true;

  const size_t native = getNativeBlockSize();
  // A shifted origin can leave overlapping cache keys even before the origin.
  // Keep the whole device on its legacy path once that alignment policy exists.
  if (!supportsBufferTransfers() || !native || ScsiCachePageBytes % native ||
      hasShiftedCacheAlignment()) {
    return writing ? Disk::writeFrom(location, buffer, length)
                   : Disk::readInto(location, buffer, length);
  }

  auto* bytes = static_cast<uint8_t*>(buffer);
  while (length) {
    const uint64_t alignment = getAlignmentPoint(location);
    const uint64_t key = location - ((location - alignment) % ScsiCachePageBytes);
    const size_t offset = location - key;
    const size_t validLength = getCachePageValidLength(key);
    if (key > ~uintptr_t{0} || offset >= validLength || key % native || validLength % native)
      return false;
    const size_t available = validLength - offset;
    const size_t chunk = length < available ? length : available;

    // Readers may retain writable aliases indefinitely, especially when small
    // filesystem blocks share a page with metadata. Preserve those aliases and
    // update their page; only an absent page can bypass the block cache.
    bool complete = false;
    for (size_t attempt = 0; attempt < 8 && !complete; ++attempt) {
      {
        CacheRangeAdmission admission(*this, key, ScsiCachePageBytes, true);
        // align() joins admitted ranges before publishing a shifted origin.
        // A change between chunks leaves any completed prefix retryable.
        if (hasShiftedCacheAlignment())
          return false;
        uintptr_t page = 0;
        bool ready = m_Cache.lookupStable(key, page);
        // A partial native sector needs a complete cached sector before merging;
        // all other absent ranges go directly to the caller's storage.
        if (ready && !page && (location % native || chunk % native)) {
          if (doRead(key) < validLength)
            return false;
          ready = m_Cache.lookupStable(key, page);
        }
        if (ready) {
          if (page) {
            CachePageGuard guard(m_Cache, key);
            auto* cached = reinterpret_cast<uint8_t*>(page) + offset;
            if (writing) {
              MemoryCopy(cached, bytes, chunk);
              m_Cache.markDirty(key);
              const uintptr_t cacheKey = key;
              if (!m_Cache.syncBatch(
                      &cacheKey, 1,
                      [](const Cache::WritebackPage* pages, size_t count, void* context) {
                        auto* disk = static_cast<ScsiDisk*>(context);
                        for (size_t i = 0; i < count; ++i) {
                          if (disk->doWriteDirect(pages[i].key, pages[i].location) !=
                              disk->getCachePageValidLength(pages[i].key))
                            return false;
                        }
                        return true;
                      },
                      this))
                return false;
            } else {
              MemoryCopy(bytes, cached, chunk);
            }
          } else if (!transferBuffer(location, bytes, chunk, writing)) {
            return false;
          }
          complete = true;
        }
      }
      if (!complete) {
        // A cache callback may itself need range admission. Join it without
        // that gate or a borrowed pin, then recheck identity after re-entry.
        uintptr_t retryPage = 0;
        if (attempt == 7 || !m_Cache.lookupStable(key, retryPage, true))
          return false;
        if (retryPage)
          m_Cache.release(key);
      }
    }
    bytes += chunk;
    location += chunk;
    length -= chunk;
  }
  return true;
}

bool ScsiDisk::writeFromBatch(WriteBuffer* buffers, size_t count) {
  if (count > MaxWriteBuffers || (count && !buffers))
    return false;
  for (size_t i = 0; i < count; ++i)
    buffers[i].complete = false;
  if (!count)
    return true;
#if CRIPPLE_HDD
  return false;
#else
  TerminationDeferral lifetime;
  DiskUse use;
  OperationBarrier::Lease operation;
  auto* controller = static_cast<ScsiController*>(m_pParent);
  if (!acquireUse(use) || !controller || !controller->acquireDiskOperation(operation))
    return false;
  const size_t native = getNativeBlockSize();
  bool eligible = supportsBufferTransfers() && native && !(ScsiCachePageBytes % native) &&
                  !hasShiftedCacheAlignment();
  uint64_t first = ~uint64_t{0}, end = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto& b = buffers[i];
    if (!b.buffer || !b.length || b.location >= getSize() || b.length > getSize() - b.location)
      return false;
    eligible &= b.location % ScsiCachePageBytes == 0 && b.length == ScsiCachePageBytes;
    if (b.location < first)
      first = b.location;
    if (b.location + b.length > end)
      end = b.location + b.length;
    for (size_t j = 0; j < i; ++j)
      if (b.location < buffers[j].location + buffers[j].length &&
          buffers[j].location < b.location + b.length)
        eligible = false;
  }
  if (eligible && end - first <= ~size_t{0}) {
    CacheRangeAdmission admission(*this, first, end - first, true);
    eligible = !hasShiftedCacheAlignment();
    // Existing aliases must be merged by the ordinary path, not bypassed.
    for (size_t i = 0; eligible && i < count; ++i) {
      uintptr_t page = 0;
      eligible = m_Cache.lookupStable(buffers[i].location, page);
      if (page) {
        m_Cache.release(buffers[i].location);
        eligible = false;
      }
    }
    if (eligible)
      return transferWriteBuffers(buffers, count);
  }
  return Disk::writeFromBatch(buffers, count);
#endif
}

bool ScsiDisk::transferWriteBuffers(WriteBuffer* buffers, size_t count) {
  bool success = true;
  for (size_t i = 0; i < count; ++i) {
    auto& b = buffers[i];
    b.complete = transferBuffer(b.location, const_cast<void*>(b.buffer), b.length, true);
    success = b.complete && success;
  }
  return success;
}

bool ScsiDisk::syncData() {
#if CRIPPLE_HDD
  return false;
#else
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;
  auto* controller = static_cast<ScsiController*>(m_pParent);
  OperationBarrier::Lease operation;
  if (!controller || !controller->acquireDiskOperation(operation))
    return false;
  return controller->addRequest(0, RequestQueue::NewRequest, SCSI_REQUEST_SYNC,
                                reinterpret_cast<uint64_t>(this), SyncWholeDevice) != 0;
#endif
}

void ScsiDisk::write(uint64_t location) {
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return;

  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent) {
    return;
  }

  OperationBarrier::Lease operation;
  if (!pParent->acquireDiskOperation(operation)) {
    return;
  }

#if !CRIPPLE_HDD
  const size_t nativeBlockSize = getNativeBlockSize();
  if (!nativeBlockSize || (ScsiCachePageBytes % nativeBlockSize)) {
    ERROR("ScsiDisk::write - incompatible cache and native block sizes.");
    return;
  }

  if (location >= getSize()) {
    ERROR("ScsiDisk::write - location too high");
    ERROR(" -> " << location << " vs " << getSize());
    return;
  }

  if ((location / getNativeBlockSize()) >= getBlockCount()) {
    ERROR("ScsiDisk::write - location too high");
    ERROR(" -> block " << (location / getNativeBlockSize()) << " vs " << getBlockCount());
    return;
  }

  const uint64_t alignPoint = getAlignmentPoint(location);

  const uint64_t pageLocation = location - ((location - alignPoint) % ScsiCachePageBytes);
  const size_t validLength = getCachePageValidLength(pageLocation);
  if (!validLength || (pageLocation % nativeBlockSize) || (validLength % nativeBlockSize)) {
    ERROR("ScsiDisk::write - invalid terminal cache page geometry.");
    return;
  }

  uintptr_t buffer;
  if (!(buffer = m_Cache.lookup(pageLocation))) {
    ERROR("ScsiDisk::write - no buffer!");
    return;
  }

  // The cache owns deferred writeback and retry, so repeated changes share one
  // pending write instead of competing with a second controller queue.
  m_Cache.markDirty(pageLocation);
  m_Cache.release(pageLocation);
#endif
}

void ScsiDisk::flush(uint64_t location) {
  if (!sync(location, false)) {
    WARNING("ScsiDisk::flush - writeback failed");
  }
}

bool ScsiDisk::sync(uint64_t location, bool async) {
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;

  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent) {
    return false;
  }

  OperationBarrier::Lease operation;
  if (!pParent->acquireDiskOperation(operation)) {
    return false;
  }

  if (location >= getSize()) {
    return false;
  }
  const uint64_t alignPoint = getAlignmentPoint(location);
  const uint64_t pageLocation = location - ((location - alignPoint) % ScsiCachePageBytes);
  if (async) {
    return m_Cache.sync(pageLocation, true);
  }

  // A filesystem cache callback can synchronously flush this lower cache.
  // Re-entering the shared CacheManager queue would reject that nested request.
  const uintptr_t page = m_Cache.lookup(pageLocation);
  if (!page) {
    return false;
  }
  CachePageGuard pageGuard(m_Cache, pageLocation);
  const bool succeeded = flushCachePage(pageLocation, page);
  if (!succeeded) {
    m_Cache.markDirty(pageLocation);
  }
  return succeeded;
}

bool ScsiDisk::syncPages(const uint64_t* locations, size_t count) {
  static_assert(Disk::MaxSyncPages <= Cache::MaxWritebackPages);
  if (count > MaxSyncPages || (count && !locations))
    return false;
  if (!count)
    return true;
#if CRIPPLE_HDD
  return false;
#else
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;
  auto* controller = static_cast<ScsiController*>(m_pParent);
  OperationBarrier::Lease operation;
  if (!controller || !controller->acquireDiskOperation(operation))
    return false;
  const size_t nativeBlockSize = getNativeBlockSize();
  if (!nativeBlockSize || ScsiCachePageBytes % nativeBlockSize)
    return false;

  uintptr_t keys[MaxSyncPages] = {};
  size_t pageCount = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint64_t location = locations[i];
    if (location >= getSize() || location % 512)
      return false;
    const uint64_t alignment = getAlignmentPoint(location);
    const uint64_t key = location - ((location - alignment) % ScsiCachePageBytes);
    const size_t length = getCachePageValidLength(key);
    if (key > ~uintptr_t{0} || !length || key % nativeBlockSize || length % nativeBlockSize)
      return false;
    bool duplicate = false;
    for (size_t j = 0; j < pageCount; ++j)
      duplicate |= keys[j] == key;
    if (!duplicate)
      keys[pageCount++] = key;
  }
  return m_Cache.syncBatch(keys, pageCount, syncCacheBatch, this);
#endif
}

bool ScsiDisk::syncCacheBatch(const Cache::WritebackPage* pages, size_t count, void* context) {
  auto* disk = static_cast<ScsiDisk*>(context);
  bool succeeded = true;
  for (size_t first = 0; first < count; first += MaxWriteBuffers) {
    const size_t n = count - first < MaxWriteBuffers ? count - first : MaxWriteBuffers;
    WriteBuffer buffers[MaxWriteBuffers];
    for (size_t i = 0; i < n; ++i) {
      const auto& page = pages[first + i];
      buffers[i] = {page.key, reinterpret_cast<const void*>(page.location),
                    disk->getCachePageValidLength(page.key), false};
    }
    if (disk->supportsBufferTransfers()) {
      succeeded = disk->transferWriteBuffers(buffers, n) && succeeded;
    } else {
      auto* controller = static_cast<ScsiController*>(disk->m_pParent);
      for (size_t i = 0; i < n; ++i) {
        const auto& b = buffers[i];
        const uint64_t written = controller->addRequest(
            0, RequestQueue::NewRequest, SCSI_REQUEST_WRITE_DIRECT,
            reinterpret_cast<uint64_t>(disk), b.location, reinterpret_cast<uintptr_t>(b.buffer));
        succeeded = written == b.length && succeeded;
      }
    }
  }
  // A partial wave still needs its successful writes made durable.
  const bool durable = disk->syncData();
  return succeeded && durable;
}

bool ScsiDisk::syncAll() {
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;

#if CRIPPLE_HDD
  return false;
#else
  ScsiController* controller = static_cast<ScsiController*>(m_pParent);
  if (!controller) {
    return false;
  }
  OperationBarrier::Lease operation;
  if (!controller->acquireDiskOperation(operation)) {
    return false;
  }

  const bool cacheSucceeded = m_Cache.syncAll(syncCacheBatch, this);
  // Previously submitted writes may still reside in the device even if the
  // cache is empty, or another page failed during this drain.
  const uint64_t flushed =
      controller->addRequest(0, RequestQueue::NewRequest, SCSI_REQUEST_SYNC,
                             reinterpret_cast<uint64_t>(this), SyncWholeDevice);
  return cacheSucceeded && flushed != 0;
#endif
}

bool ScsiDisk::retireCachePage(uint64_t location) {
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;

  ScsiController* controller = static_cast<ScsiController*>(m_pParent);
  if (!controller) {
    return false;
  }

  OperationBarrier::Lease operation;
  if (!controller->acquireDiskOperation(operation)) {
    return false;
  }

  const size_t nativeBlockSize = getNativeBlockSize();
  if (!nativeBlockSize || (ScsiCachePageBytes % nativeBlockSize)) {
    return false;
  }

  const uint64_t alignPoint = getAlignmentPoint(location);
  const uint64_t pageLocation = location - ((location - alignPoint) % ScsiCachePageBytes);
  const size_t validLength = getCachePageValidLength(pageLocation);
  if (!validLength || (pageLocation % nativeBlockSize) || (validLength % nativeBlockSize)) {
    return false;
  }

  CacheRangeAdmission admission(*this, pageLocation, ScsiCachePageBytes, true);
  return m_Cache.retireWriteback(pageLocation, retireCachePageCallback, this);
}

bool ScsiDisk::flushCachePage(uint64_t location, uintptr_t page) {
#if !CRIPPLE_HDD
  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent || !page) {
    return false;
  }

  const size_t nativeBlockSize = getNativeBlockSize();
  if (!nativeBlockSize || (ScsiCachePageBytes % nativeBlockSize)) {
    ERROR("ScsiDisk::flush - incompatible cache and native block sizes.");
    return false;
  }

  if (location >= getSize()) {
    ERROR("ScsiDisk::flush - location too high");
    return false;
  }

  if ((location / getNativeBlockSize()) >= getBlockCount()) {
    ERROR("ScsiDisk::flush - location too high");
    return false;
  }

  const uint64_t alignPoint = getAlignmentPoint(location);

  const uint64_t pageLocation = location - ((location - alignPoint) % ScsiCachePageBytes);
  const size_t validLength = getCachePageValidLength(pageLocation);
  if (!validLength || (pageLocation % nativeBlockSize) || (validLength % nativeBlockSize)) {
    ERROR("ScsiDisk::flush - invalid terminal cache page geometry.");
    return false;
  }

  // The caller owns a pin even if retirement closes admission meanwhile.
  // Direct writes borrow it without another lookup or transferred reference.
  const uint64_t writeResult =
      pParent->addRequest(0, RequestQueue::NewRequest, SCSI_REQUEST_WRITE_DIRECT,
                          reinterpret_cast<uint64_t>(this), pageLocation, page);
  const uint64_t syncResult =
      pParent->addRequest(0, SCSI_REQUEST_SYNC, reinterpret_cast<uint64_t>(this), pageLocation);
  const bool success = writeResult == validLength && syncResult != 0;
  if (!success) {
    WARNING("ScsiDisk::flush - write or synchronise request failed");
  }

  return success;
#else
  return false;
#endif
}

void ScsiDisk::align(uint64_t location) {
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return;

  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent) {
    return;
  }

  OperationBarrier::Lease operation;
  if (!pParent->acquireDiskOperation(operation)) {
    return;
  }

  CacheRangeAdmission admission(*this, 0, getSize(), true);
  LockGuard<Mutex> guard(m_AlignmentLock);
  for (size_t i = 0; i < m_AlignPoints.count(); ++i) {
    if (m_AlignPoints[i] == location) {
      return;
    }
  }
  m_HasShiftedCacheAlignment |= location % ScsiCachePageBytes != 0;
  m_AlignPoints.pushBack(location);
}

uint64_t ScsiDisk::doRead(uint64_t location) {
  const size_t fillSize = getCacheFillLength(location);
  const size_t cacheLength = cacheExtentLength(fillSize);
  if (!fillSize || !cacheLength || fillSize > 0xffff || !getNativeBlockSize() ||
      (cacheLength % ScsiCachePageBytes) || (fillSize % getNativeBlockSize())) {
    return 0;
  }

  // Wait for the unit to be ready before reading
  bool bReady = false;
  for (int i = 0; i < 3; i++) {
    if ((bReady = unitReady()))
      break;
  }

  if (!bReady) {
    ERROR("ScsiDisk::doRead - unit not ready");
    return 0;
  }

  // Handle the case where a read took place while we were waiting in the
  // RequestQueue - don't double up the cache.
  uintptr_t buffer = m_Cache.lookup(location);
  if (buffer) {
    WARNING("ScsiDisk::doRead(" << location << ") - buffer was already in cache");
    m_Cache.release(location);
    return fillSize;
  }
  bool didExist = false;
  buffer = m_Cache.insert(location, cacheLength, &didExist);
  if (!buffer) {
    WARNING("ScsiDisk::doRead - could not allocate a complete cache extent");
    return 0;
  }
  if (didExist) {
    return fillSize;
  }
  CacheFillGuard fillGuard(m_Cache, location, cacheLength);
  ByteSet(reinterpret_cast<void*>(buffer), 0, cacheLength);

  size_t blockNum = location / getNativeBlockSize();
  size_t blockCount = fillSize / getNativeBlockSize();

  bool bOk = false;
  ScsiCommand* pCommand;

  // TOC?
  if (m_DeviceType == CdDvdDevice) {
    /// \todo Cache this somewhere.
    pCommand = new ScsiCommands::ReadTocCommand(getNativeBlockSize());
    uint8_t* toc = new uint8_t[getNativeBlockSize()];
    PointerGuard<uint8_t> tmpBuffGuard(toc, true);
    const bool tocOk =
        sendCommand(pCommand, reinterpret_cast<uintptr_t>(toc), getNativeBlockSize());
    delete pCommand;
    if (!tocOk) {
      WARNING(
          "ScsiDisk::doRead - could not find data track (READ TOC "
          "failed)");
      return 0;
    }

    uint16_t i;
    bool bHaveTrack = false;
    const size_t responseLength = static_cast<size_t>((toc[0] << 8) | toc[1]) + 2;
    if (responseLength < 4 || responseLength > getNativeBlockSize()) {
      WARNING("ScsiDisk::doRead - malformed READ TOC response");
      return 0;
    }
    ScsiCommands::ReadTocCommand::TocEntry* Toc =
        reinterpret_cast<ScsiCommands::ReadTocCommand::TocEntry*>(toc + 4);
    const size_t descriptorCount = (responseLength - 4) / sizeof(*Toc);
    for (i = 0; i < descriptorCount; i++) {
      if (Toc[i].Flags & 0x04) {
        bHaveTrack = true;
        break;
      }
    }

    if (!bHaveTrack) {
      WARNING("ScsiDisk::doRead - could not find data track (no data track)");
      return 0;
    }

    uint32_t trackStart = BIG_TO_HOST32(Toc[i].TrackStart);
    if ((blockNum + trackStart) < blockNum) {
      WARNING("ScsiDisk::doRead - TOC overflow");
      return 0;
    }

    blockNum += trackStart;
  }

  for (int i = 0; i < 3 && !bOk; i++) {
    SCSI_DEBUG_LOG("SCSI: trying read(10)");
    pCommand = new ScsiCommands::Read10(blockNum, blockCount);
    bOk = sendCommand(pCommand, buffer, fillSize);
    delete pCommand;
  }
  for (int i = 0; i < 3 && !bOk; i++) {
    SCSI_DEBUG_LOG("SCSI: trying read(12)");
    pCommand = new ScsiCommands::Read12(blockNum, blockCount);
    bOk = sendCommand(pCommand, buffer, fillSize);
    delete pCommand;
  }
  for (int i = 0; i < 3 && !bOk; i++) {
    SCSI_DEBUG_LOG("SCSI: trying read(16)");
    pCommand = new ScsiCommands::Read16(blockNum, blockCount);
    bOk = sendCommand(pCommand, buffer, fillSize);
    delete pCommand;
  }

  if (bOk) {
    fillGuard.publish();
  } else {
    ERROR("SCSI: reading failed?");
    return 0;
  }

  return fillSize;
}

size_t ScsiDisk::getCacheFillLength(uint64_t location) const {
  const size_t preferred = getCacheFillSize();
  const size_t native = getNativeBlockSize();
  if (!preferred || !native || (preferred % ScsiCachePageBytes) || (preferred % native) ||
      location >= getSize() || (location % native)) {
    return 0;
  }

  size_t length = preferred;
  const uint64_t remaining = getSize() - location;
  if (length > remaining) {
    length = static_cast<size_t>(remaining);
  }

  length -= length % native;
  return length;
}

uint64_t ScsiDisk::doWrite(uint64_t location) {
  // Wait for the unit to be ready before writing
  bool bReady = false;
  for (int i = 0; i < 3; i++) {
    if ((bReady = unitReady()))
      break;
  }

  if (!bReady) {
    ERROR("ScsiDisk::doWrite - unit not ready");
    return 0;
  }

  // Handle the case where a read took place while we were waiting in the
  // RequestQueue - don't double up the cache.
  uintptr_t buffer = m_Cache.lookup(location);
  if (!buffer) {
    WARNING("ScsiDisk::doWrite(" << location << ") - buffer was not in cache");
    return 0;
  }

  // Make sure we don't hold the refcnt once we exit this method.
  CachePageGuard guard(m_Cache, location);

  const size_t validLength = getCachePageValidLength(location);
  return writePageBuffer(location, buffer) ? validLength : 0;
}

uint64_t ScsiDisk::doWriteDirect(uint64_t location, uintptr_t page) {
  if (!page) {
    return 0;
  }

  bool ready = false;
  for (int i = 0; i < 3; ++i) {
    if ((ready = unitReady())) {
      break;
    }
  }

  if (!ready) {
    ERROR("ScsiDisk::doWriteDirect - unit not ready");
    return 0;
  }

  const size_t validLength = getCachePageValidLength(location);
  return writePageBuffer(location, page) ? validLength : 0;
}

bool ScsiDisk::writePageBuffer(uint64_t location, uintptr_t page) {
  const size_t nativeBlockSize = getNativeBlockSize();
  const size_t validLength = getCachePageValidLength(location);
  if (!page || !nativeBlockSize || !validLength || validLength > 0xffff ||
      (location % nativeBlockSize) || (validLength % nativeBlockSize)) {
    return false;
  }

  size_t block = location / nativeBlockSize;
  size_t count = validLength / nativeBlockSize;

  bool bOk = false;
  ScsiCommand* pCommand;

  for (int i = 0; i < 3; i++) {
    SCSI_DEBUG_LOG("SCSI: trying write(10)");
    pCommand = new ScsiCommands::Write10(block, count);
    bOk = sendCommand(pCommand, page, validLength, true);
    delete pCommand;
    if (bOk)
      break;
  }
  if (!bOk) {
    for (int i = 0; i < 3; i++) {
      SCSI_DEBUG_LOG("SCSI: trying write(12)");
      pCommand = new ScsiCommands::Write12(block, count);
      bOk = sendCommand(pCommand, page, validLength, true);
      delete pCommand;
      if (bOk)
        break;
    }
  }
  if (!bOk) {
    for (int i = 0; i < 3; i++) {
      SCSI_DEBUG_LOG("SCSI: trying write(16)");
      pCommand = new ScsiCommands::Write16(block, count);
      bOk = sendCommand(pCommand, page, validLength, true);
      delete pCommand;
      if (bOk)
        break;
    }
  }

  if (!bOk) {
    ERROR("SCSI: writing failed?");
  }

  return bOk;
}

uint64_t ScsiDisk::doSync(uint64_t location) {
  // Wait for the unit to be ready before writing
  bool bReady = false;
  for (int i = 0; i < 3; i++) {
    if ((bReady = unitReady()))
      break;
  }

  if (!bReady) {
    ERROR("ScsiDisk::doSync - unit not ready");
    return 0;
  }

  const bool wholeDevice = location == SyncWholeDevice;
  const size_t nativeBlockSize = getNativeBlockSize();
  const size_t validLength = wholeDevice ? 1 : getCachePageValidLength(location);
  if (!nativeBlockSize || !getSize() || !validLength ||
      (!wholeDevice && ((location % nativeBlockSize) || (validLength % nativeBlockSize)))) {
    return 0;
  }

  // SBC defines zero blocks as the entire remaining medium, from LBA zero.
  const size_t block = wholeDevice ? 0 : location / nativeBlockSize;
  const size_t count = wholeDevice ? 0 : validLength / nativeBlockSize;

  bool bOk = false;
  ScsiCommand* pCommand;

  // Kick off a synchronise (this will be slow, but will ensure the data is on
  // disk)
  for (int i = 0; i < 3; i++) {
    SCSI_DEBUG_LOG("SCSI: trying synchronise(10)");
    pCommand = new ScsiCommands::Synchronise10(block, count);
    bOk = sendCommand(pCommand, 0, 0);
    delete pCommand;
    if (bOk)
      break;
  }

  if (!bOk) {
    for (int i = 0; i < 3; i++) {
      SCSI_DEBUG_LOG("SCSI: trying synchronise(16)");
      pCommand = new ScsiCommands::Synchronise16(block, count);
      bOk = sendCommand(pCommand, 0, 0);
      delete pCommand;
      if (bOk)
        break;
    }
  }

  return bOk ? validLength : 0;
}

bool ScsiDisk::pin(uint64_t location) {
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;

  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent) {
    return false;
  }

  OperationBarrier::Lease operation;
  if (!pParent->acquireDiskOperation(operation)) {
    return false;
  }
  if (location >= getSize()) {
    return false;
  }

  const uint64_t alignPoint = getAlignmentPoint(location);

  const uint64_t cacheLocation = location - ((location - alignPoint) % ScsiCachePageBytes);
  if (!m_Cache.pin(cacheLocation))
    return false;
  if (m_Cache.beginMutableLoan(cacheLocation))
    return true;
  m_Cache.release(cacheLocation);
  return false;
}

void ScsiDisk::unpin(uint64_t location) {
  if (location >= getSize()) {
    return;
  }
  const uint64_t alignPoint = getAlignmentPoint(location);

  const uint64_t cacheLocation = location - ((location - alignPoint) % ScsiCachePageBytes);
  releaseView(cacheLocation, true);
}

void ScsiDisk::releaseView(uint64_t cacheLocation, bool writable) {
  // A new partition may change alignment while an older view still owns its pin.
  if (writable)
    m_Cache.endMutableLoan(cacheLocation);
  m_Cache.release(cacheLocation);
}

size_t ScsiDisk::getCachePageValidLength(uint64_t location) const {
  if (location >= getSize()) {
    return 0;
  }

  const uint64_t remaining = getSize() - location;
  return remaining < ScsiCachePageBytes ? static_cast<size_t>(remaining) : ScsiCachePageBytes;
}

uint64_t ScsiDisk::getAlignmentPoint(uint64_t location) const {
  LockGuard<Mutex> guard(m_AlignmentLock);
  uint64_t alignPoint = 0;
  for (size_t i = 0; i < m_AlignPoints.count(); ++i) {
    if (m_AlignPoints[i] <= location && m_AlignPoints[i] > alignPoint) {
      alignPoint = m_AlignPoints[i];
    }
  }
  return alignPoint;
}

bool ScsiDisk::hasShiftedCacheAlignment() const {
  LockGuard<Mutex> guard(m_AlignmentLock);
  return m_HasShiftedCacheAlignment;
}
