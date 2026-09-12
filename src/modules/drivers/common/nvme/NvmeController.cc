/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "NvmeController.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

#include "NvmeDisk.h"
#include "modules/drivers/common/InterruptProbe.h"
using namespace Nvme;

NvmeController::NvmeController(Device* pci)
    : m_Pci(pci),
      m_Registers(nullptr),
      m_ReadyMilliseconds(500),
      m_MaxTransfer(MaxTransfer),
      m_Irq(0),
      m_OriginalCommand(0),
      m_PciChanged(false),
      m_HardwareOwned(false),
      m_DmaInstalled(false),
      m_Interrupts(false),
      m_Failed(false),
      m_Stopping(false),
      m_Shutdown(false),
      m_Model{} {
  setSpecificType(String("nvme-controller"));
}
NvmeController::~NvmeController() {
  shutdown();
}

bool NvmeController::waitReady(bool ready) {
  const auto deadline = Time::getTicks() + m_ReadyMilliseconds * Time::Multiplier::Millisecond;
  do {
    const uint32_t status = m_Registers->read32(Status);
    if (status == 0xffffffffU || (ready && (status & 2U)))
      return false;
    if (bool(status & 1U) == ready)
      return true;
    Time::delay(Time::Multiplier::Millisecond);
  } while (Time::getTicks() < deadline);
  return false;
}
bool NvmeController::disable() {
  m_Registers->write32(0xffffffffU, InterruptMaskSet);
  m_Registers->write32(m_Registers->read32(Configuration) & ~(1U | (3U << 14)), Configuration);
  (void)m_Registers->read32(Configuration);
  return waitReady(false);
}
void NvmeController::failController() {
  LockGuard<Mutex> reset(m_ResetLock);
  if (m_Failed)
    return;
  {
    LockGuard<Mutex> irqLock(m_IrqLock);
    m_Registers->write32(0xffffffffU, InterruptMaskSet);
    (void)m_Registers->read32(InterruptMaskSet);
    m_Stopping = true;
    m_Interrupts = false;
    m_Admin.stop();
    m_Io.stop();
  }
  // Callers only lend CPU buffers. Queue and bounce pages remain ours until
  // the controller acknowledges reset, including commands with lost completions.
  if (!disable())
    panic("NVMe: controller cannot stop DMA; refusing to release memory");
  m_DmaInstalled = false;
  m_Failed = true;
}
bool NvmeController::command(NvmeQueue& queue, Command request, void* buffer, size_t bytes,
                             bool writing, uint32_t* result, bool interruptProbe) {
  bool interrupts;
  {
    LockGuard<Mutex> irqLock(m_IrqLock);
    interrupts = m_Interrupts;
  }
  const size_t timeout = (&queue == &m_Io && (request.opcode & 255U) == 0) ? 120 : 30;
  const auto status =
      queue.execute(request, buffer, bytes, writing, interrupts, timeout, result, interruptProbe);
  if (status == NvmeQueue::Result::TransportError)
    failController();
  return status == NvmeQueue::Result::Success;
}
bool NvmeController::identify(uint32_t nsid, uint8_t kind, void* buffer, bool interruptProbe) {
  OperationBarrier::Lease lease;
  if (!m_Commands.tryAcquire(lease))
    return false;
  Command request{};
  request.opcode = 6;
  request.nsid = nsid;
  request.cdw10 = kind;
  return command(m_Admin, request, buffer, PageSize, false, nullptr, interruptProbe);
}

bool NvmeController::initialiseController() {
  if (!m_Pci || m_Pci->getPciClassCode() != 1 || m_Pci->getPciSubclassCode() != 8 ||
      m_Pci->getPciProgInterface() != 2)
    return false;
  auto& pci = PciBus::instance();
  PciFunctionState::State inherited;
  if (!pci.inspectFunction(m_Pci, inherited)) {
    ERROR("NVMe: unsupported PCI state (D0, valid capabilities and PIC INTx required)");
    return false;
  }
  m_OriginalCommand = inherited.command;
  const uint32_t bar = pci.readConfigSpace(m_Pci, 4);
  if ((bar & 1U) || ((bar & 6U) != 0 && (bar & 6U) != 4))
    return false;
  uint64_t base = bar & ~15U;
  if ((bar & 6U) == 4)
    base |= static_cast<uint64_t>(pci.readConfigSpace(m_Pci, 5)) << 32;
  Device::Address* mapping = nullptr;
  for (auto* address : m_Pci->addresses()) {
    if (address->m_Name == "bar0" && base && !address->m_IsIoSpace && address->m_Address == base)
      mapping = address;
  }
  if (!mapping || mapping->m_Size < Doorbells + 16)
    return false;
  m_PciChanged = true;
  if (!pci.updateCommand(m_Pci, 0, 2U | 0x400U))
    return false;
  mapping->map();
  m_Registers = mapping->m_Io;
  if (!m_Registers || m_Registers->size() < mapping->m_Size)
    return false;
  const uint32_t version = m_Registers->read32(Version);
  const uint64_t cap =
      m_Registers->read32(Cap) | (static_cast<uint64_t>(m_Registers->read32(Cap + 4)) << 32);
  // CNS=2 active namespace discovery starts with NVMe 1.1. Use the NVM
  // command set and 4 KiB host pages; other profiles need separate handling.
  if (version < 0x00010100 || version == 0xffffffffU || !(cap & (1ULL << 37)) ||
      ((cap >> 48) & 15U))
    return false;
  const size_t stride = size_t{4} << ((cap >> 32) & 15U);
  if (mapping->m_Size < Doorbells + 3 * stride + 4)
    return false;
  m_ReadyMilliseconds = (((cap >> 24) & 255U) + 1) * 500;
  m_HardwareOwned = true;
  // Firmware may have left an enabled controller. Stop it before replacing
  // queue addresses, and never restore firmware's old DMA enable on unload.
  if (!disable())
    return false;
  if (!pci.updateCommand(m_Pci, 4U, 2U | 0x400U) ||
      !pci.disableMessageInterrupts(m_Pci, inherited) || !pci.resourcesUnchanged(m_Pci, inherited))
    return false;
  const uint16_t depth = (cap & 0xffffU) >= QueueDepth - 1 ? QueueDepth : (cap & 0xffffU) + 1;
  if (!m_Admin.initialise(m_Registers, 0, depth, stride, PageSize) ||
      !m_Io.initialise(m_Registers, 1, depth, stride, MaxTransfer))
    return false;
  m_Registers->write32((depth - 1U) | ((depth - 1U) << 16), AdminAttributes);
  m_Registers->write32(m_Admin.submissionAddress(), AdminSubmission);
  m_Registers->write32(m_Admin.submissionAddress() >> 32, AdminSubmission + 4);
  m_Registers->write32(m_Admin.completionAddress(), AdminCompletion);
  m_Registers->write32(m_Admin.completionAddress() >> 32, AdminCompletion + 4);
  FENCE();
  m_DmaInstalled = true;
  if (!pci.updateCommand(m_Pci, 0, 6U | 0x400U))
    return false;
  m_Registers->write32(1U | (6U << 16) | (4U << 20), Configuration);
  if (!waitReady(true))
    return false;
  uint8_t controller[PageSize]{};
  if (!identify(0, 1, controller))
    return false;
  if ((controller[512] & 15U) > 6 || (controller[512] >> 4) < 6 || (controller[513] & 15U) > 4 ||
      (controller[513] >> 4) < 4)
    return false;
  const uint8_t mdts = controller[77];
  if (mdts && mdts < 4)
    m_MaxTransfer = PageSize << mdts;
  for (size_t i = 0; i < 40; ++i) {
    const uint8_t ch = controller[24 + i];
    m_Model[i] = ch >= 32 && ch <= 126 ? ch : ' ';
  }
  size_t length = 40;
  while (length && m_Model[length - 1] == ' ')
    --length;
  m_Model[length] = 0;
  if (!createIoQueue())
    return false;
  const uint32_t maximumId = controller[516] | (uint32_t{controller[517]} << 8) |
                             (uint32_t{controller[518]} << 16) | (uint32_t{controller[519]} << 24);
  if (!maximumId || !discoverNamespaces(maximumId) || !getNumChildren())
    return false;
  m_Irq = Machine::instance().getIrqManager()->registerPciIrqHandler(this, m_Pci,
                                                                     IrqPolicy::pciIntxThreaded());
  if (!m_Irq) {
    ERROR("NVMe: could not register PCI INTx");
    return false;
  }
  {
    LockGuard<Mutex> irqLock(m_IrqLock);
    m_Interrupts = true;
    if (!pci.updateCommand(m_Pci, 0x400U, 6U))
      return false;
    m_Registers->write32(1, InterruptMaskClear);
    (void)m_Registers->read32(Status);
  }
  if (!InterruptProbe::run([&] { return identify(0, 1, controller, true); },
                           [&] { return m_Admin.interruptCompletions(); })) {
    ERROR("NVMe: interrupt delivery probe failed");
    return false;
  }
  for (size_t i = 0; i < getNumChildren(); ++i)
    static_cast<NvmeDisk*>(getChild(i))->publishEndpoint();
  NOTICE("NVMe: '" << m_Model << "' ready, " << Dec << getNumChildren() << " namespaces, "
                   << depth - 1U << " command slots, shared INTx" << Hex);
  return true;
}
bool NvmeController::createIoQueue() {
  Command request{};
  request.opcode = 9;
  request.cdw10 = 7;
  if (!command(m_Admin, request))
    return false;
  request = {};
  request.opcode = 5;
  request.prp1 = m_Io.completionAddress();
  request.cdw10 = 1U | ((m_Io.depth() - 1U) << 16);
  request.cdw11 = 3;
  if (!command(m_Admin, request))
    return false;
  request = {};
  request.opcode = 1;
  request.prp1 = m_Io.submissionAddress();
  request.cdw10 = 1U | ((m_Io.depth() - 1U) << 16);
  request.cdw11 = 1U | (1U << 16);
  return command(m_Admin, request);
}
bool NvmeController::discoverNamespaces(uint32_t maximumId) {
  uint32_t previous = 0;
  // Bound hostile or nonsensical firmware enumeration, while supporting
  // sparse namespace IDs and continuation pages rather than assuming NSID 1.
  for (size_t page = 0; page < 1024; ++page) {
    uint32_t ids[1024]{};
    if (!identify(previous, 2, ids))
      return false;
    for (uint32_t nsid : ids) {
      if (!nsid)
        return true;
      if (nsid <= previous || nsid > maximumId || nsid == 0xffffffffU)
        return false;
      previous = nsid;
      auto* disk = new NvmeDisk(this, nsid);
      if (disk->initialise())
        addChild(disk);
      else
        delete disk;
    }
  }
  ERROR("NVMe: active namespace list exceeds enumeration limit");
  return false;
}
NvmeDisk* NvmeController::findNamespace(uint32_t nsid) {
  for (size_t i = 0; i < getNumChildren(); ++i) {
    auto* disk = static_cast<NvmeDisk*>(getChild(i));
    if (disk->namespaceId() == nsid)
      return disk;
  }
  return nullptr;
}
bool NvmeController::readWrite(uint32_t nsid, uint64_t lba, uint32_t blocks, void* buffer,
                               size_t bytes, bool writing) {
  OperationBarrier::Lease lease;
  if (!m_Commands.tryAcquire(lease))
    return false;
  const NvmeDisk* disk = findNamespace(nsid);
  if (!disk || !blocks || blocks > 65536 || bytes > m_MaxTransfer ||
      blocks > (~size_t{0} / disk->getNativeBlockSize()) ||
      bytes != blocks * disk->getNativeBlockSize() || lba >= disk->getBlockCount() ||
      blocks > disk->getBlockCount() - lba)
    return false;
  LockGuard<Mutex> writeLock(m_WriteLock, writing);
  Command request{};
  request.opcode = writing ? 1 : 2;
  request.nsid = nsid;
  request.cdw10 = lba;
  request.cdw11 = lba >> 32;
  request.cdw12 = blocks - 1;
  return command(m_Io, request, buffer, bytes, writing);
}
bool NvmeController::flush(uint32_t nsid) {
  OperationBarrier::Lease lease;
  if (!m_Commands.tryAcquire(lease) || !findNamespace(nsid))
    return false;
  // NVMe Flush guarantees persistence for writes completed before submission.
  LockGuard<Mutex> writeLock(m_WriteLock);
  Command request{};
  request.nsid = nsid;
  return command(m_Io, request);
}
IrqDisposition NvmeController::irq(irq_id_t) {
  LockGuard<Mutex> irqLock(m_IrqLock);
  if (m_Stopping)
    return IrqDisposition::Quiesced;
  if (!m_Interrupts)
    return IrqDisposition::NotHandled;
  const bool admin = m_Admin.complete(true);
  const bool io = m_Io.complete(true);
  return admin || io ? IrqDisposition::Handled : IrqDisposition::NotHandled;
}
size_t NvmeController::interruptCompletions() const {
  return m_Admin.interruptCompletions() + m_Io.interruptCompletions();
}
void NvmeController::shutdown() {
  if (m_Shutdown)
    return;
  shutdownDiskCaches();
  RequestQueue::destroy();
  m_Commands.closeAndWait();
  {
    LockGuard<Mutex> irqLock(m_IrqLock);
    m_Stopping = true;
    m_Interrupts = false;
    if (m_HardwareOwned)
      m_Registers->write32(0xffffffffU, InterruptMaskSet);
  }
  if (m_Irq && !Machine::instance().getIrqManager()->unregisterHandler(m_Irq, this))
    panic("NVMe: synchronous interrupt retirement failed");
  m_Irq = 0;
  m_Admin.stop();
  m_Io.stop();
  if (m_DmaInstalled && !m_Failed && (m_Registers->read32(Status) & 1U)) {
    m_Registers->write32((m_Registers->read32(Configuration) & ~(3U << 14)) | (1U << 14),
                         Configuration);
    const auto deadline = Time::getTicks() + 30 * Time::Multiplier::Second;
    while ((m_Registers->read32(Status) & 12U) != 8U && Time::getTicks() < deadline)
      Time::delay(Time::Multiplier::Millisecond);
    if ((m_Registers->read32(Status) & 12U) != 8U)
      panic("NVMe: orderly shutdown notification timed out");
  }
  if (m_DmaInstalled && !disable())
    panic("NVMe: cannot stop DMA during shutdown");
  m_DmaInstalled = false;
  if (m_PciChanged) {
    const uint16_t command =
        m_HardwareOwned ? (m_OriginalCommand & ~4U) | 0x400U : m_OriginalCommand;
    if (!PciBus::instance().updateCommand(m_Pci, 0x407U, command & 0x407U))
      panic("NVMe: failed to verify PCI state during shutdown");
  }
  m_Shutdown = true;
}

size_t NvmeController::maximumOutstanding() const {
  return m_Io.maximumOutstanding();
}
