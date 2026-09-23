/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "VirtioScsiController.h"
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
#include "pedigree/kernel/utilities/utility.h"

#include <stddef.h>

#include "modules/drivers/common/InterruptProbe.h"

namespace {
constexpr size_t MaxCdbBytes = 32;
constexpr size_t MaxSenseBytes = 96;
constexpr size_t ResponseOffset = 128;

struct VirtioScsiRequest {
  uint8_t lun[8];
  uint64_t id;
  uint8_t taskAttr;
  uint8_t priority;
  uint8_t crn;
  uint8_t cdb[MaxCdbBytes];
} PACKED;
static_assert(offsetof(VirtioScsiRequest, cdb) == 19);

struct VirtioScsiResponse {
  uint32_t senseLength;
  uint32_t residual;
  uint16_t statusQualifier;
  uint8_t status;
  uint8_t response;
  uint8_t sense[MaxSenseBytes];
} PACKED;
static_assert(offsetof(VirtioScsiResponse, sense) == 12);
static_assert(ResponseOffset >= sizeof(VirtioScsiRequest));

constexpr size_t ResponseHeaderBytes = offsetof(VirtioScsiResponse, sense);
}  // namespace

VirtioScsiController::VirtioScsiController(Device* pci)
    : m_Pci(pci),
      m_Transport(pci),
      m_Control("Virtio SCSI request"),
      m_Data("Virtio SCSI data"),
      m_Completion(0, false),
      m_Irq(0),
      m_Targets{},
      m_UnitCount(0),
      m_CdbBytes(0),
      m_SenseBytes(0),
      m_CommandUsed(0),
      m_InterruptCompletions(0),
      m_TransportInitialised(false),
      m_Ready(false),
      m_CommandSeen(false),
      m_CommandValid(false),
      m_Stopping(false),
      m_Failed(false),
      m_Shutdown(false) {
  setSpecificType(String("virtio-scsi-controller"));
}

VirtioScsiController::~VirtioScsiController() {
  shutdown();
}

bool VirtioScsiController::initialiseController() {
  if (!m_Pci || m_Pci->getPciVendorId() != 0x1af4 ||
      (m_Pci->getPciDeviceId() != 0x1048 && m_Pci->getPciDeviceId() != 0x1004) ||
      !m_Transport.initialise()) {
    return false;
  }
  m_TransportInitialised = true;
  if (!m_Transport.negotiate(0) || !m_Transport.setupQueue(2, m_RequestQueue) ||
      m_RequestQueue.depth() < 3) {
    return false;
  }

  uint32_t cdbBytes = 0, senseBytes = 0;
  uint16_t maxTarget = 0;
  if (!m_Transport.readDeviceConfig32(24, cdbBytes) ||
      !m_Transport.readDeviceConfig32(20, senseBytes) ||
      !m_Transport.readDeviceConfig16(30, maxTarget) || cdbBytes < 16 || cdbBytes > MaxCdbBytes ||
      !senseBytes || senseBytes > MaxSenseBytes) {
    ERROR("Virtio SCSI: unsupported command or sense size");
    return false;
  }
  m_CdbBytes = cdbBytes;
  m_SenseBytes = senseBytes;

  auto& memory = PhysicalMemoryManager::instance();
  const size_t flags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;
  if (!memory.allocateRegion(m_Control, 1, PhysicalMemoryManager::continuous, flags) ||
      !memory.allocateRegion(m_Data, 1, PhysicalMemoryManager::continuous, flags)) {
    return false;
  }
  ByteSet(m_Control.virtualAddress(), 0, TargetInfo::getPageSize());
  ByteSet(m_Data.virtualAddress(), 0, TargetInfo::getPageSize());

  m_Irq = Machine::instance().getIrqManager()->registerPciIrqHandler(this, m_Pci,
                                                                     IrqPolicy::pciIntxThreaded());
  if (!m_Irq) {
    ERROR("Virtio SCSI: could not register PCI INTx");
    return false;
  }
  if (!m_Transport.ready()) {
    return false;
  }
  m_Ready = true;

  uint8_t inquiry[36]{};
  const uint8_t inquiryCdb[6] = {0x12, 0, 0, 0, sizeof(inquiry), 0};
  if (!InterruptProbe::run(
          [&] {
            return command(0, inquiryCdb, sizeof(inquiryCdb), inquiry, sizeof(inquiry), false,
                           true) != Result::TransportError;
          },
          [&] {
            LockGuard<Mutex> guard(m_IrqLock);
            return m_InterruptCompletions;
          })) {
    ERROR("Virtio SCSI: interrupt delivery probe failed");
    return false;
  }

  // A missing target returns a normal virtio completion, so probe without
  // creating ScsiDisk instances that would log each absent target as an error.
  const unsigned lastTarget = maxTarget > 255 ? 255 : maxTarget;
  for (unsigned target = 0; target <= lastTarget; ++target) {
    if (command(static_cast<uint8_t>(target), inquiryCdb, sizeof(inquiryCdb), inquiry,
                sizeof(inquiry), false, false) == Result::Success) {
      m_Targets[m_UnitCount++] = static_cast<uint8_t>(target);
    }
    if (m_Failed) {
      return false;
    }
  }
  if (!m_UnitCount) {
    return false;
  }
  searchDisks();
  NOTICE("Virtio SCSI: " << Dec << getNumChildren() << " disks, shared INTx" << Hex);
  return getNumChildren() != 0;
}

void VirtioScsiController::drainCompletions(bool fromInterrupt) {
  LockGuard<Mutex> guard(m_IrqLock);
  if (m_Stopping) {
    return;
  }
  Virtio::Completion completion{};
  while (m_RequestQueue.pop(completion)) {
    m_CommandValid = completion.cookie == this && completion.length >= ResponseHeaderBytes;
    m_CommandUsed = completion.length;
    m_CommandSeen = true;
    if (fromInterrupt) {
      ++m_InterruptCompletions;
    }
    m_Completion.release();
  }
}

IrqDisposition VirtioScsiController::irq(irq_id_t) {
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
    while (m_RequestQueue.pop(completion)) {
      m_CommandValid = completion.cookie == this && completion.length >= ResponseHeaderBytes;
      m_CommandUsed = completion.length;
      m_CommandSeen = true;
      ++m_InterruptCompletions;
      m_Completion.release();
    }
  }
  return IrqDisposition::Handled;
}

void VirtioScsiController::failController() {
  {
    LockGuard<Mutex> guard(m_IrqLock);
    if (m_Failed) {
      return;
    }
    m_Stopping = true;
    m_Failed = true;
  }
  if (!m_Transport.reset()) {
    panic("Virtio SCSI: device did not stop DMA after command failure");
  }
  m_Ready = false;
  m_RequestQueue.stop();
}

VirtioScsiController::Result VirtioScsiController::command(uint8_t target, const void* cdb,
                                                           size_t cdbBytes, void* data,
                                                           size_t dataBytes, bool writing,
                                                           bool interruptProbe) {
  TerminationDeferral lifetime;
  LockGuard<Mutex> serial(m_CommandLock);
  if (!m_Ready || m_Failed || !cdb || !cdbBytes || cdbBytes > m_CdbBytes ||
      dataBytes > TargetInfo::getPageSize() || (dataBytes && !data)) {
    return Result::TransportError;
  }

  auto* request = static_cast<VirtioScsiRequest*>(m_Control.virtualAddress());
  auto* response = reinterpret_cast<VirtioScsiResponse*>(
      static_cast<uint8_t*>(m_Control.virtualAddress()) + ResponseOffset);
  ByteSet(request, 0, sizeof(VirtioScsiRequest));
  ByteSet(response, 0, sizeof(VirtioScsiResponse));
  request->lun[0] = 1;
  request->lun[1] = target;
  request->lun[2] = 0x40;
  request->id = HOST_TO_LITTLE64(1);
  MemoryCopy(request->cdb, cdb, cdbBytes);
  if (writing && dataBytes) {
    MemoryCopy(m_Data.virtualAddress(), data, dataBytes);
  }

  const uint32_t responseBytes = ResponseHeaderBytes + m_SenseBytes;
  Virtio::Buffer descriptors[3] = {
      {m_Control.physicalAddress(),
       static_cast<uint32_t>(offsetof(VirtioScsiRequest, cdb) + m_CdbBytes), false},
      {m_Control.physicalAddress() + ResponseOffset, responseBytes, true},
      {m_Data.physicalAddress(), static_cast<uint32_t>(dataBytes), !writing},
  };
  size_t descriptorCount = dataBytes ? 3 : 2;
  if (writing && dataBytes) {
    descriptors[1] = descriptors[2];
    descriptors[1].deviceWrites = false;
    descriptors[2] = {m_Control.physicalAddress() + ResponseOffset, responseBytes, true};
  }

  {
    LockGuard<Mutex> guard(m_IrqLock);
    m_CommandSeen = false;
    m_CommandValid = false;
    m_CommandUsed = 0;
    [[maybe_unused]] const size_t stale = m_Completion.drainAvailable();
  }
  FENCE();
  if (!m_RequestQueue.submit(descriptors, descriptorCount, this)) {
    failController();
    return Result::TransportError;
  }
  m_Transport.notify(2);

  const auto deadline = Time::getTicks() + 30 * Time::Multiplier::Second;
  bool first = true;
  for (;;) {
    const bool grace = first && interruptProbe;
    const bool signalled = m_Completion.acquireForCompletion(1, grace ? 1 : 0, grace ? 0 : 10000);
    first = false;
    if (!signalled) {
      drainCompletions(false);
    }
    bool seen = false, valid = false;
    size_t used = 0;
    {
      LockGuard<Mutex> guard(m_IrqLock);
      seen = m_CommandSeen;
      valid = m_CommandValid;
      used = m_CommandUsed;
    }
    if (seen) {
      FENCE();
      if (!valid) {
        ERROR("Virtio SCSI: malformed completion");
        failController();
        return Result::TransportError;
      }
      if (response->response || response->status || LITTLE_TO_HOST32(response->residual) ||
          LITTLE_TO_HOST32(response->senseLength) > m_SenseBytes) {
        return Result::DeviceError;
      }
      if (!writing && dataBytes) {
        if (used < ResponseHeaderBytes + dataBytes) {
          ERROR("Virtio SCSI: short read completion");
          failController();
          return Result::TransportError;
        }
        MemoryCopy(data, m_Data.virtualAddress(), dataBytes);
      }
      return Result::Success;
    }
    if (Time::getTicks() >= deadline) {
      ERROR("Virtio SCSI: command timeout");
      failController();
      return Result::TransportError;
    }
  }
}

bool VirtioScsiController::sendCommand(size_t unit, uintptr_t cdb, uint8_t cdbBytes, uintptr_t data,
                                       uint16_t dataBytes, bool writing) {
  if (unit >= m_UnitCount) {
    return false;
  }
  return command(m_Targets[unit], reinterpret_cast<const void*>(cdb), cdbBytes,
                 reinterpret_cast<void*>(data), dataBytes, writing, false) == Result::Success;
}

void VirtioScsiController::shutdown() {
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
      panic("Virtio SCSI: device did not stop DMA during shutdown");
    }
    m_Ready = false;
    m_RequestQueue.stop();
  }
  if (m_Irq && !Machine::instance().getIrqManager()->unregisterHandler(m_Irq, this)) {
    panic("Virtio SCSI: synchronous interrupt retirement failed");
  }
  m_Irq = 0;
  m_Shutdown = true;
}
