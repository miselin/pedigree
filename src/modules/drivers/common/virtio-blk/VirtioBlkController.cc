/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "VirtioBlkController.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

#include <stddef.h>

#include "VirtioBlkDisk.h"
#include "modules/drivers/common/InterruptProbe.h"

namespace {
constexpr uint64_t FeatureReadOnly = uint64_t{1} << 5;
constexpr uint64_t FeatureBlockSize = uint64_t{1} << 6;
constexpr uint64_t FeatureFlush = uint64_t{1} << 9;
constexpr uint32_t RequestRead = 0;
constexpr uint32_t RequestWrite = 1;
constexpr uint32_t RequestFlush = 4;

struct VirtioBlkRequest {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
  uint8_t status;
} PACKED;
static_assert(offsetof(VirtioBlkRequest, status) == 16);
}  // namespace

VirtioBlkController::VirtioBlkController(Device* pci)
    : m_Pci(pci),
      m_Transport(pci),
      m_Control("Virtio block request"),
      m_Data("Virtio block data"),
      m_Completion(0, false),
      m_Irq(0),
      m_Bytes(0),
      m_SectorBytes(512),
      m_ExpectedUsed(0),
      m_InterruptCompletions(0),
      m_ReadOnly(false),
      m_Flush(false),
      m_TransportInitialised(false),
      m_Ready(false),
      m_CommandSeen(false),
      m_CommandValid(false),
      m_Stopping(false),
      m_Failed(false),
      m_Shutdown(false) {
  setSpecificType(String("virtio-blk-controller"));
}

VirtioBlkController::~VirtioBlkController() {
  shutdown();
}

bool VirtioBlkController::initialiseController() {
  if (!m_Pci || m_Pci->getPciVendorId() != 0x1af4 ||
      (m_Pci->getPciDeviceId() != 0x1042 && m_Pci->getPciDeviceId() != 0x1001) ||
      !m_Transport.initialise()) {
    return false;
  }
  m_TransportInitialised = true;
  if (!m_Transport.negotiate(FeatureReadOnly | FeatureBlockSize | FeatureFlush) ||
      !m_Transport.setupQueue(0, m_Queue) || m_Queue.depth() < 3) {
    return false;
  }

  const uint64_t features = m_Transport.features();
  m_ReadOnly = features & FeatureReadOnly;
  m_Flush = features & FeatureFlush;
  uint64_t sectors = 0;
  if (!m_Transport.readDeviceConfig64(0, sectors) || !sectors ||
      sectors > static_cast<uint64_t>(~size_t{0} / 512)) {
    return false;
  }
  m_Bytes = static_cast<size_t>(sectors * 512);
  if (features & FeatureBlockSize) {
    uint32_t blockSize = 0;
    if (!m_Transport.readDeviceConfig32(20, blockSize)) {
      return false;
    }
    m_SectorBytes = blockSize;
  }
  const size_t page = TargetInfo::getPageSize();
  if (m_SectorBytes < 512 || (m_SectorBytes & (m_SectorBytes - 1)) || m_SectorBytes > page ||
      page % m_SectorBytes || m_Bytes % m_SectorBytes) {
    return false;
  }

  auto& memory = PhysicalMemoryManager::instance();
  const size_t flags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;
  if (!memory.allocateRegion(m_Control, 1, PhysicalMemoryManager::continuous, flags) ||
      !memory.allocateRegion(m_Data, 1, PhysicalMemoryManager::continuous, flags)) {
    return false;
  }
  ByteSet(m_Control.virtualAddress(), 0, page);
  ByteSet(m_Data.virtualAddress(), 0, page);

  m_Irq = Machine::instance().getIrqManager()->registerPciIrqHandler(this, m_Pci,
                                                                     IrqPolicy::pciIntxThreaded());
  if (!m_Irq) {
    ERROR("Virtio block: could not register PCI INTx");
    return false;
  }
  if (!m_Transport.ready()) {
    return false;
  }
  m_Ready = true;

  if (!InterruptProbe::run([&] { return command(RequestRead, 0, nullptr, m_SectorBytes, 2, true); },
                           [&] {
                             LockGuard<Mutex> guard(m_IrqLock);
                             return m_InterruptCompletions;
                           })) {
    ERROR("Virtio block: interrupt delivery probe failed");
    return false;
  }

  auto* disk = new VirtioBlkDisk(this);
  addChild(disk);
  disk->publishEndpoint();
  NOTICE("Virtio block: " << Dec << sectors << " sectors, " << m_Bytes << " bytes, "
                          << m_SectorBytes << "-byte blocks, "
                          << (m_ReadOnly ? "read-only" : "read-write") << ", shared INTx" << Hex);
  return true;
}

void VirtioBlkController::drainCompletions(bool fromInterrupt) {
  LockGuard<Mutex> guard(m_IrqLock);
  if (m_Stopping) {
    return;
  }
  Virtio::Completion completion{};
  while (m_Queue.pop(completion)) {
    m_CommandValid = completion.cookie == this && completion.length >= m_ExpectedUsed;
    m_CommandSeen = true;
    if (fromInterrupt) {
      ++m_InterruptCompletions;
    }
    m_Completion.release();
  }
}

IrqDisposition VirtioBlkController::irq(irq_id_t) {
  LockGuard<Mutex> guard(m_IrqLock);
  if (m_Stopping) {
    return IrqDisposition::Quiesced;
  }
  const uint8_t isr = m_Transport.readIsr();
  if (!isr) {
    return IrqDisposition::NotHandled;
  }
  if (isr & 1U) {
    Virtio::Completion completion{};
    while (m_Queue.pop(completion)) {
      m_CommandValid = completion.cookie == this && completion.length >= m_ExpectedUsed;
      m_CommandSeen = true;
      ++m_InterruptCompletions;
      m_Completion.release();
    }
  }
  return IrqDisposition::Handled;
}

void VirtioBlkController::failController() {
  {
    LockGuard<Mutex> guard(m_IrqLock);
    if (m_Failed) {
      return;
    }
    m_Stopping = true;
    m_Failed = true;
  }
  if (!m_Transport.reset()) {
    panic("Virtio block: device did not stop DMA after command failure");
  }
  m_Ready = false;
  m_Queue.stop();
}

bool VirtioBlkController::command(uint32_t type, uint64_t sector, void* buffer, size_t bytes,
                                  size_t timeoutSeconds, bool interruptProbe) {
  TerminationDeferral lifetime;
  LockGuard<Mutex> serial(m_CommandLock);
  if (!m_Ready || m_Failed || (bytes && bytes > TargetInfo::getPageSize()) ||
      (type == RequestWrite && (!buffer || m_ReadOnly)) ||
      (type == RequestFlush && (bytes || buffer)) || (type != RequestFlush && !bytes)) {
    return false;
  }

  auto* request = static_cast<VirtioBlkRequest*>(m_Control.virtualAddress());
  request->type = HOST_TO_LITTLE32(type);
  request->reserved = 0;
  request->sector = HOST_TO_LITTLE64(sector);
  request->status = 0xff;
  if (type == RequestWrite) {
    MemoryCopy(m_Data.virtualAddress(), buffer, bytes);
  }

  Virtio::Buffer descriptors[3] = {
      {m_Control.physicalAddress(), 16, false},
      {m_Data.physicalAddress(), static_cast<uint32_t>(bytes), type == RequestRead},
      {m_Control.physicalAddress() + 16, 1, true},
  };
  const size_t descriptorCount = type == RequestFlush ? 2 : 3;
  if (type == RequestFlush) {
    descriptors[1] = descriptors[2];
  }
  {
    LockGuard<Mutex> guard(m_IrqLock);
    m_CommandSeen = false;
    m_CommandValid = false;
    m_ExpectedUsed = type == RequestRead ? bytes + 1 : 1;
    [[maybe_unused]] const size_t stale = m_Completion.drainAvailable();
  }
  FENCE();
  if (!m_Queue.submit(descriptors, descriptorCount, this)) {
    failController();
    return false;
  }
  m_Transport.notify(0);

  const auto deadline = Time::getTicks() + timeoutSeconds * Time::Multiplier::Second;
  bool first = true;
  for (;;) {
    const bool grace = first && interruptProbe;
    const bool signalled = m_Completion.acquireForCompletion(1, grace ? 1 : 0, grace ? 0 : 10000);
    first = false;
    if (!signalled) {
      drainCompletions(false);
    }
    bool seen = false;
    bool valid = false;
    {
      LockGuard<Mutex> guard(m_IrqLock);
      seen = m_CommandSeen;
      valid = m_CommandValid;
    }
    if (seen) {
      FENCE();
      if (!valid) {
        ERROR("Virtio block: malformed completion");
        failController();
        return false;
      }
      if (request->status != 0) {
        WARNING("Virtio block: request " << type << " failed with status " << request->status);
        return false;
      }
      if (type == RequestRead && buffer) {
        MemoryCopy(buffer, m_Data.virtualAddress(), bytes);
      }
      return true;
    }
    if (Time::getTicks() >= deadline) {
      ERROR("Virtio block: command timeout");
      failController();
      return false;
    }
  }
}

bool VirtioBlkController::readWrite(uint64_t location, void* buffer, size_t bytes, bool writing) {
  if (!buffer || !bytes || bytes > TargetInfo::getPageSize() || location >= m_Bytes ||
      bytes > m_Bytes - location || location % m_SectorBytes || bytes % m_SectorBytes ||
      (writing && m_ReadOnly)) {
    return false;
  }
  return command(writing ? RequestWrite : RequestRead, location / 512, buffer, bytes, 30, false);
}

bool VirtioBlkController::flush() {
  // With neither FLUSH nor CONFIG_WCE negotiated, virtio specifies a
  // writethrough cache; read-only media have no outstanding writes.
  return (m_ReadOnly || !m_Flush) ? m_Ready : command(RequestFlush, 0, nullptr, 0, 30, false);
}

void VirtioBlkController::shutdown() {
  if (m_Shutdown) {
    return;
  }
  shutdownDiskCaches();
  RequestQueue::destroy();
  {
    LockGuard<Mutex> serial(m_CommandLock);
    {
      LockGuard<Mutex> guard(m_IrqLock);
      m_Stopping = true;
    }
    if (m_TransportInitialised && !m_Transport.reset()) {
      panic("Virtio block: device did not stop DMA during shutdown");
    }
    m_Ready = false;
    m_Queue.stop();
  }
  if (m_Irq && !Machine::instance().getIrqManager()->unregisterHandler(m_Irq, this)) {
    panic("Virtio block: synchronous interrupt retirement failed");
  }
  m_Irq = 0;
  m_Shutdown = true;
}
