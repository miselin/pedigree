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
bool discardEditingRange(Cache& cache, uintptr_t key, size_t length) {
  bool discarded = true;
  for (size_t offset = 0; offset < length; offset += 4096) {
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

void ScsiDisk::cacheCallback(CacheConstants::CallbackCause cause, uintptr_t loc, uintptr_t page,
                             void* meta) {
  ScsiDisk* pDisk = reinterpret_cast<ScsiDisk*>(meta);

  switch (cause) {
    case CacheConstants::WriteBack: {
      // Cache shutdown runs after external operations have been closed,
      // so writeback must use the internal path.
      pDisk->flushCachePage(loc);
    } break;
    case CacheConstants::Eviction:
      // no-op for ScsiDisk
      break;
    default:
      WARNING(
          "ScsiDisk: unknown cache callback -- could indicate "
          "potential future I/O issues.");
      break;
  }
}

ScsiDisk::ScsiDisk()
    : Disk(),
      m_Cache(PhysicalMemoryManager::below4GB),
      m_Inquiry(0),
      m_AlignmentLock(),
      m_nAlignPoints(0),
      m_NumBlocks(0),
      m_BlockSize(0x1000),
      m_NativeBlockSize(0),
      m_DeviceType(NoDevice) {
  m_Cache.setCallback(cacheCallback, this);
}

ScsiDisk::~ScsiDisk() {
  m_Cache.shutdown();
}

void ScsiDisk::shutdownCache() {
  m_Cache.shutdown();
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
  SCSI_DEBUG_LOG("ScsiDisk: Capacity: "
                 << Dec << m_NumBlocks << " blocks, each " << m_NativeBlockSize << " bytes - "
                 << (m_NativeBlockSize * m_NumBlocks) << Hex << " bytes in total.");

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

uintptr_t ScsiDisk::read(uint64_t location) {
  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent) {
    return 0;
  }

  OperationBarrier::Lease operation;
  if (!pParent->acquireDiskOperation(operation)) {
    return 0;
  }

  const size_t fillSize = getCacheFillSize();
  if (!fillSize || !getNativeBlockSize() || (fillSize % 4096) ||
      (fillSize % getNativeBlockSize())) {
    ERROR("ScsiDisk::read - invalid cache or native block size.");
    return 0;
  }
  if (location >= getSize() || getNativeBlockSize() > (getSize() - location)) {
    ERROR("ScsiDisk::read - location too high (" << location << " of " << getSize() << ")");
    return 0;
  }
  size_t blockNum = location / getNativeBlockSize();
  if (blockNum >= getBlockCount()) {
    ERROR("ScsiDisk::read - location too high (block " << blockNum << " > " << getBlockCount()
                                                       << ")");
    return 0;
  }
  const uint64_t alignPoint = getAlignmentPoint(location);

  const uint64_t pageLocation = location - ((location - alignPoint) % 4096);
  const size_t pageOffset = location - pageLocation;

  uintptr_t buffer;
  if ((buffer = m_Cache.lookup(pageLocation))) {
    return buffer + pageOffset;
  }

  // Cache extents follow the most recent alignment point, which may not be
  // aligned to the device's cache block size.
  size_t loc = location - ((location - alignPoint) % fillSize);
  const size_t fillLength = getCacheFillLength(loc);
  if (pageLocation < loc || (pageLocation - loc) >= fillLength ||
      4096 > (fillLength - (pageLocation - loc))) {
    return 0;
  }

  uint64_t numRead =
      pParent->addRequest(0, SCSI_REQUEST_READ, reinterpret_cast<uint64_t>(this), loc);
  if (numRead < fillLength) {
    // Failed to read for some reason, expose the failure to our caller.
    WARNING("ScsiDisk::read - short read!");
    return 0;
  }
#if READAHEAD_ENABLED
  // Read the next block.
  for (size_t i = 0; i < 2; ++i) {
    loc += fillSize;
    pParent->addAsyncRequest(0, SCSI_REQUEST_READ, reinterpret_cast<uint64_t>(this), loc);
  }
#endif
  buffer = m_Cache.lookup(pageLocation);
  if (!buffer) {
    return 0;
  }
  return buffer + pageOffset;
}

void ScsiDisk::write(uint64_t location) {
  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent) {
    return;
  }

  OperationBarrier::Lease operation;
  if (!pParent->acquireDiskOperation(operation)) {
    return;
  }

#if !CRIPPLE_HDD
  if (!(getBlockSize() && getNativeBlockSize())) {
    ERROR("ScsiDisk::write - block size is zero.");
    return;
  }

  if (location >= getSize() || getNativeBlockSize() > (getSize() - location)) {
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

  // Calculate the offset to get location on a page boundary.
  ssize_t offs = -((location - alignPoint) % 4096);

  uintptr_t buffer;
  if (!(buffer = m_Cache.lookup(location + offs))) {
    ERROR("ScsiDisk::write - no buffer!");
    return;
  }

  // Implicit pin caused by lookup() must be released by the write request
  // handler, to ensure the refcount is correct.
  pParent->addAsyncRequest(0, SCSI_REQUEST_WRITE, reinterpret_cast<uint64_t>(this),
                           location + offs);
#endif
}

void ScsiDisk::flush(uint64_t location) {
  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent) {
    return;
  }

  OperationBarrier::Lease operation;
  if (!pParent->acquireDiskOperation(operation)) {
    return;
  }

  flushCachePage(location);
}

void ScsiDisk::flushCachePage(uint64_t location) {
#if !CRIPPLE_HDD
  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent) {
    return;
  }

  if (!(getBlockSize() && getNativeBlockSize())) {
    ERROR("ScsiDisk::flush - block size is zero.");
    return;
  }

  if (location >= getSize() || getNativeBlockSize() > (getSize() - location)) {
    ERROR("ScsiDisk::flush - location too high");
    return;
  }

  if ((location / getNativeBlockSize()) >= getBlockCount()) {
    ERROR("ScsiDisk::flush - location too high");
    return;
  }

  const uint64_t alignPoint = getAlignmentPoint(location);

  // Calculate the offset to get location on a page boundary.
  ssize_t offs = -((location - alignPoint) % 4096);

  uintptr_t buffer;
  if (!(buffer = m_Cache.lookup(location + offs))) {
    return;
  }

  // Make sure this page remains for both the write AND the sync.
  const bool pinned = m_Cache.pin(location + offs);
  assert(pinned);

  const uint64_t writeResult =
      pParent->addRequest(0, SCSI_REQUEST_WRITE, reinterpret_cast<uint64_t>(this), location + offs);
  const uint64_t syncResult =
      pParent->addRequest(0, SCSI_REQUEST_SYNC, reinterpret_cast<uint64_t>(this), location + offs);
  if (!writeResult || !syncResult) {
    WARNING("ScsiDisk::flush - write or synchronise request failed");
  }

  // Undo our pin for write+sync.
  m_Cache.release(location + offs);
#endif
}

void ScsiDisk::align(uint64_t location) {
  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent) {
    return;
  }

  OperationBarrier::Lease operation;
  if (!pParent->acquireDiskOperation(operation)) {
    return;
  }

  LockGuard<Mutex> guard(m_AlignmentLock);
  for (size_t i = 0; i < m_nAlignPoints; ++i) {
    if (m_AlignPoints[i] == location) {
      return;
    }
  }
  assert(m_nAlignPoints < 8);
  m_AlignPoints[m_nAlignPoints++] = location;
}

uint64_t ScsiDisk::doRead(uint64_t location) {
  const size_t fillSize = getCacheFillLength(location);
  if (!fillSize || !getNativeBlockSize() || (fillSize % 4096) ||
      (fillSize % getNativeBlockSize())) {
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
  buffer = m_Cache.insert(location, fillSize, &didExist);
  if (!buffer) {
    const bool discarded = discardEditingRange(m_Cache, location, fillSize);
    (void)discarded;
    FATAL("ScsiDisk::doRead - no buffer");
    return 0;
  }
  if (didExist) {
    return fillSize;
  }
  CacheFillGuard fillGuard(m_Cache, location, fillSize);

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
    bOk = sendCommand(pCommand, reinterpret_cast<uintptr_t>(toc), getNativeBlockSize());
    delete pCommand;
    if (!bOk) {
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
  if (!preferred || !native || (preferred % 4096) || (preferred % native) ||
      location >= getSize()) {
    return 0;
  }

  size_t length = preferred;
  const uint64_t remaining = getSize() - location;
  if (length > remaining) {
    length = static_cast<size_t>(remaining);
  }

  length -= length % 4096;
  while (length && (length % native)) {
    length -= 4096;
  }
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

  size_t block = location / getNativeBlockSize();
  size_t count = 4096 / getNativeBlockSize();

  bool bOk = false;
  ScsiCommand* pCommand;

  for (int i = 0; i < 3; i++) {
    SCSI_DEBUG_LOG("SCSI: trying write(10)");
    pCommand = new ScsiCommands::Write10(block, count);
    bOk = sendCommand(pCommand, buffer, 4096, true);
    delete pCommand;
    if (bOk)
      break;
  }
  if (!bOk) {
    for (int i = 0; i < 3; i++) {
      SCSI_DEBUG_LOG("SCSI: trying write(12)");
      pCommand = new ScsiCommands::Write12(block, count);
      bOk = sendCommand(pCommand, buffer, 4096, true);
      delete pCommand;
      if (bOk)
        break;
    }
  }
  if (!bOk) {
    for (int i = 0; i < 3; i++) {
      SCSI_DEBUG_LOG("SCSI: trying write(16)");
      pCommand = new ScsiCommands::Write16(block, count);
      bOk = sendCommand(pCommand, buffer, 4096, true);
      delete pCommand;
      if (bOk)
        break;
    }
  }

  if (!bOk) {
    ERROR("SCSI: writing failed?");
  }

  return bOk ? getBlockSize() : 0;
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

  size_t block = location / getNativeBlockSize();
  size_t count = 4096 / getNativeBlockSize();

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

  return getBlockSize();
}

bool ScsiDisk::pin(uint64_t location) {
  ScsiController* pParent = static_cast<ScsiController*>(m_pParent);
  if (!pParent) {
    return false;
  }

  OperationBarrier::Lease operation;
  if (!pParent->acquireDiskOperation(operation)) {
    return false;
  }

  const uint64_t alignPoint = getAlignmentPoint(location);

  const uint64_t cacheLocation = location - ((location - alignPoint) % 4096);
  return m_Cache.pin(cacheLocation);
}

void ScsiDisk::unpin(uint64_t location) {
  const uint64_t alignPoint = getAlignmentPoint(location);

  const uint64_t cacheLocation = location - ((location - alignPoint) % 4096);
  m_Cache.release(cacheLocation);
}

uint64_t ScsiDisk::getAlignmentPoint(uint64_t location) const {
  LockGuard<Mutex> guard(m_AlignmentLock);
  uint64_t alignPoint = 0;
  for (size_t i = 0; i < m_nAlignPoints; ++i) {
    if (m_AlignPoints[i] <= location && m_AlignPoints[i] > alignPoint) {
      alignPoint = m_AlignPoints[i];
    }
  }
  return alignPoint;
}
