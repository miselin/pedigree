/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "NvmeController.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/PciVirtualFunctions.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

#include "NvmeDisk.h"
#include "modules/drivers/common/InterruptProbe.h"
using namespace Nvme;

namespace {
uint16_t readLe16(const uint8_t* data) {
  return data[0] | (uint16_t{data[1]} << 8);
}
uint32_t readLe32(const uint8_t* data) {
  return readLe16(data) | (uint32_t{readLe16(data + 2)} << 16);
}
}  // namespace

NvmeController::NvmeController(Device* pci)
    : m_Pci(pci),
      m_Registers(nullptr),
      m_Io{},
      m_IoCount(0),
      m_IoProcessor{},
      m_ReadyMilliseconds(500),
      m_MaxTransfer(MaxTransfer),
      m_Irq(0),
      m_IoIrq{},
      m_OriginalCommand(0),
      m_ControllerId(0),
      m_VirtualizationSupported(false),
      m_NamespaceManagementSupported(false),
      m_PciChanged(false),
      m_HardwareOwned(false),
      m_DmaInstalled(false),
      m_DmaIsolated(false),
      m_Interrupts(false),
      m_BatchInitialising(false),
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
  }
  m_Admin.stop();
  for (auto* queue : m_Io) {
    if (queue) {
      queue->stop();
    }
  }
  // A cache fill may use a page as a DMA target. Stop the controller before
  // the failed fill can discard that page, even if its completion was lost.
  if (!disable())
    panic("NVMe: controller cannot stop DMA; refusing to release memory");
  m_DmaInstalled = false;
  m_Failed = true;
}
bool NvmeController::command(NvmeQueue& queue, Command request, void* buffer, size_t bytes,
                             bool writing, uint32_t* result, bool interruptProbe, bool cacheFill,
                             physical_uintptr_t directWritePhysical) {
  TerminationDeferral lifetime;
  PciBus::DmaMapping directReadMapping;
  bool interrupts;
  {
    LockGuard<Mutex> irqLock(m_IrqLock);
    interrupts = m_Interrupts;
  }
  const size_t timeout = (&queue != &m_Admin && (request.opcode & 255U) == 0) ? 120 : 30;
  const auto status = queue.execute(request, buffer, bytes, writing, interrupts, timeout, result,
                                    interruptProbe, cacheFill, directWritePhysical,
                                    &directReadMapping);
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

bool NvmeController::primaryControllerCapabilities(PrimaryControllerCapabilities& capabilities) {
  capabilities = {};
  OperationBarrier::Lease lease;
  if (!m_Commands.tryAcquire(lease) || !m_VirtualizationSupported) {
    return false;
  }
  LockGuard<Mutex> management(m_VirtualizationLock);
  return primaryControllerCapabilitiesLocked(capabilities);
}

bool NvmeController::primaryControllerCapabilitiesLocked(
    PrimaryControllerCapabilities& capabilities) {
  uint8_t data[PageSize]{};
  Command request{};
  request.opcode = 6;
  request.cdw10 = 0x14U | (uint32_t{m_ControllerId} << 16);
  if (!command(m_Admin, request, data, sizeof(data)) || readLe16(data) != m_ControllerId) {
    return false;
  }
  capabilities.controllerId = readLe16(data);
  capabilities.portId = readLe16(data + 2);
  capabilities.resourceTypes = data[4] & 3U;
  VirtualResourceCapabilities* resources[2] = {&capabilities.queues, &capabilities.interrupts};
  for (size_t i = 0; i < 2; ++i) {
    const uint8_t* resource = data + 32 + (32 * i);
    *resources[i] = {readLe32(resource),      readLe32(resource + 4),  readLe16(resource + 8),
                     readLe16(resource + 10), readLe16(resource + 12), readLe16(resource + 14)};
  }
  return true;
}

bool NvmeController::secondaryControllers(uint16_t firstControllerId,
                                          SecondaryControllerList& controllers) {
  controllers = {};
  OperationBarrier::Lease lease;
  if (!m_Commands.tryAcquire(lease) || !m_VirtualizationSupported) {
    return false;
  }
  LockGuard<Mutex> management(m_VirtualizationLock);
  return secondaryControllersLocked(firstControllerId, controllers);
}

bool NvmeController::secondaryControllersLocked(uint16_t firstControllerId,
                                                SecondaryControllerList& controllers) {
  uint8_t data[PageSize]{};
  Command request{};
  request.opcode = 6;
  request.cdw10 = 0x15U | (uint32_t{firstControllerId} << 16);
  if (!command(m_Admin, request, data, sizeof(data)) || data[0] > 127) {
    return false;
  }
  for (size_t i = 0; i < data[0]; ++i) {
    const uint8_t* entry = data + 32 + (32 * i);
    const uint16_t id = readLe16(entry);
    if (id < firstControllerId || id == m_ControllerId || readLe16(entry + 2) != m_ControllerId) {
      controllers = {};
      return false;
    }
    controllers.entries[i] = {id,
                              readLe16(entry + 2),
                              bool(entry[4] & 1U),
                              readLe16(entry + 8),
                              readLe16(entry + 10),
                              readLe16(entry + 12)};
  }
  controllers.count = data[0];
  return true;
}

bool NvmeController::virtualizationManagement(uint16_t controllerId, VirtualResource resource,
                                              VirtualizationAction action, uint16_t count,
                                              uint16_t* modified) {
  if (modified) {
    *modified = 0;
  }
  if ((resource != VirtualResource::Queue && resource != VirtualResource::Interrupt) ||
      (action != VirtualizationAction::AllocatePrimary && action != VirtualizationAction::Offline &&
       action != VirtualizationAction::Assign && action != VirtualizationAction::Online) ||
      ((action == VirtualizationAction::Offline || action == VirtualizationAction::Online) &&
       count)) {
    return false;
  }
  OperationBarrier::Lease lease;
  if (!m_Commands.tryAcquire(lease) || !m_VirtualizationSupported) {
    return false;
  }
  LockGuard<Mutex> management(m_VirtualizationLock);
  if (action == VirtualizationAction::AllocatePrimary) {
    if (controllerId != m_ControllerId) {
      return false;
    }
  } else {
    SecondaryControllerList controllers{};
    if (controllerId == m_ControllerId || !secondaryControllersLocked(controllerId, controllers)) {
      return false;
    }
    const SecondaryController* target = nullptr;
    for (size_t i = 0; i < controllers.count; ++i) {
      if (controllers.entries[i].controllerId == controllerId) {
        target = &controllers.entries[i];
        break;
      }
    }
    if (!target || (action == VirtualizationAction::Assign && target->online)) {
      return false;
    }
  }
  if (action == VirtualizationAction::AllocatePrimary || action == VirtualizationAction::Assign) {
    PrimaryControllerCapabilities capabilities{};
    if (!primaryControllerCapabilitiesLocked(capabilities) ||
        !(capabilities.resourceTypes & (1U << static_cast<unsigned>(resource)))) {
      return false;
    }
    const VirtualResourceCapabilities& limits =
        resource == VirtualResource::Queue ? capabilities.queues : capabilities.interrupts;
    if (count > limits.flexibleTotal ||
        (action == VirtualizationAction::Assign && count > limits.secondaryMaximum)) {
      return false;
    }
  }
  Command request{};
  request.opcode = 0x1c;
  request.cdw10 = (uint32_t{controllerId} << 16) | (static_cast<uint32_t>(resource) << 8) |
                  static_cast<uint32_t>(action);
  request.cdw11 = count;
  uint32_t result = 0;
  if (!command(m_Admin, request, nullptr, 0, false, &result)) {
    return false;
  }
  if (modified &&
      (action == VirtualizationAction::AllocatePrimary || action == VirtualizationAction::Assign)) {
    *modified = result & 0xffffU;
  }
  return true;
}

bool NvmeController::setNamespaceAttachment(uint32_t nsid, uint16_t controllerId, bool attached) {
  if (!nsid || nsid == 0xffffffffU) {
    return false;
  }
  OperationBarrier::Lease lease;
  if (!m_Commands.tryAcquire(lease) || !m_NamespaceManagementSupported) {
    return false;
  }
  LockGuard<Mutex> management(m_VirtualizationLock);
  if (controllerId == m_ControllerId && findNamespace(nsid)) {
    return false;
  }
  uint8_t controllers[PageSize]{};
  controllers[0] = 1;
  controllers[2] = controllerId;
  controllers[3] = controllerId >> 8;
  Command request{};
  request.opcode = 0x15;
  request.nsid = nsid;
  request.cdw10 = attached ? 0 : 1;
  return command(m_Admin, request, controllers, sizeof(controllers), true);
}

bool NvmeController::initialiseController() {
  if (!m_Pci || m_Pci->getPciClassCode() != 1 || m_Pci->getPciSubclassCode() != 8 ||
      m_Pci->getPciProgInterface() != 2)
    return false;
  auto& pci = PciBus::instance();
  PciFunctionState::State inherited;
  if (!pci.inspectFunction(m_Pci, inherited, false)) {
    ERROR("NVMe: unsupported PCI state (D0 and valid capabilities required)");
    return false;
  }
  m_OriginalCommand = inherited.command;
  const bool virtualFunction = m_Pci->getPhysicalFunction() != nullptr;
  uint64_t base = 0, assignedBytes = 0;
  if (virtualFunction) {
    if (!PciVirtualFunctions::bar(m_Pci, 0, base, assignedBytes)) {
      return false;
    }
  } else {
    const uint32_t bar = pci.readConfigSpace(m_Pci, 4);
    if ((bar & 1U) || ((bar & 6U) != 0 && (bar & 6U) != 4)) {
      return false;
    }
    base = bar & ~15U;
    if ((bar & 6U) == 4) {
      base |= static_cast<uint64_t>(pci.readConfigSpace(m_Pci, 5)) << 32;
    }
  }
  Device::Address* mapping = nullptr;
  for (auto* address : m_Pci->addresses()) {
    if (address->m_Name != "bar0" || !base || !address->m_Address || address->m_IsIoSpace ||
        (virtualFunction && address->m_Size != assignedBytes)) {
      continue;
    }
    uint64_t cpuPhysical = 0;
    if (!pci.translateAddress(base, address->m_Size, false, cpuPhysical) ||
        address->m_Address != cpuPhysical) {
      continue;
    }
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
      !pci.disableMessageInterrupts(m_Pci, inherited) ||
      !pci.resourcesUnchanged(m_Pci, inherited)) {
    return false;
  }
  if (pci.attachIsolatedDma(m_Pci)) {
    // The VF group retains its isolated domains until all VF drivers are gone.
    m_DmaIsolated = !virtualFunction;
    NOTICE("NVMe: using isolated DMA domain");
  } else if (virtualFunction) {
    ERROR("NVMe: virtual function requires isolated DMA");
    return false;
  } else {
    (void)pci.attachDmaRemapping(m_Pci);
  }
  const uint16_t depth = (cap & 0xffffU) >= QueueDepth - 1 ? QueueDepth : (cap & 0xffffU) + 1;
  if (!m_Admin.initialise(m_Registers, 0, depth, stride, PageSize, m_Pci))
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
  m_ControllerId = readLe16(controller + 78);
  m_VirtualizationSupported = (readLe16(controller + 256) & (1U << 7)) != 0;
  m_NamespaceManagementSupported = (readLe16(controller + 256) & (1U << 3)) != 0;
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
  const uint32_t maximumId = controller[516] | (uint32_t{controller[517]} << 8) |
                             (uint32_t{controller[518]} << 16) | (uint32_t{controller[519]} << 24);
  bool activeNamespaces = false;
  if (maximumId && !discoverNamespaces(maximumId, activeNamespaces)) {
    return false;
  }
  if (!getNumChildren() && (activeNamespaces || virtualFunction || !m_VirtualizationSupported)) {
    return false;
  }
  size_t requestedQueues = 0;
  const CpuAffinityMask online = Scheduler::onlineAffinity();
  for (size_t cpu = 0; cpu < Processor::getCount() && requestedQueues < MaxIoQueues; ++cpu) {
    if (online.contains(cpu)) {
      m_IoProcessor[requestedQueues++] = cpu;
    }
  }
  if (!requestedQueues) {
    requestedQueues = 1;
    m_IoProcessor[0] = 0;
  }
  uint16_t msixControl = 0;
  const size_t messageCount =
      inherited.msix && pci.readConfig16(m_Pci, inherited.msix + 2, msixControl)
          ? (msixControl & 0x7ffU) + 1U
          : 0;
  if (requestedQueues > (messageCount > 1 ? messageCount - 1 : 1)) {
    requestedQueues = messageCount > 1 ? messageCount - 1 : 1;
  }
  // The last queue's CQ doorbell must also fit the assigned BAR.
  const size_t doorbellQueues = ((mapping->m_Size - Doorbells - 4) / stride - 1) / 2;
  if (requestedQueues > doorbellQueues) {
    requestedQueues = doorbellQueues;
  }
  if (getNumChildren()) {
    Command request{};
    request.opcode = 9;
    request.cdw10 = 7;
    request.cdw11 = (requestedQueues - 1) | ((requestedQueues - 1) << 16);
    uint32_t granted = 0;
    if (!command(m_Admin, request, nullptr, 0, false, &granted)) {
      return false;
    }
    m_IoCount = requestedQueues;
    const size_t submissionQueues = (granted & 0xffffU) + 1U;
    const size_t completionQueues = (granted >> 16) + 1U;
    if (m_IoCount > submissionQueues) {
      m_IoCount = submissionQueues;
    }
    if (m_IoCount > completionQueues) {
      m_IoCount = completionQueues;
    }
  }
  IrqManager* irqManager = Machine::instance().getIrqManager();
  IrqHandler* handlers[MaxIoQueues + 1];
  size_t processors[MaxIoQueues + 1] = {};
  for (size_t i = 0; i < MaxIoQueues + 1; ++i) {
    handlers[i] = this;
    if (i) {
      processors[i] = m_IoProcessor[i - 1];
    }
  }
  size_t vectorCount = m_IoCount && messageCount > 1 ? m_IoCount + 1 : 1;
  {
    LockGuard<Mutex> irqLock(m_IrqLock);
    m_BatchInitialising = true;
  }
  for (;;) {
    irq_id_t vectors[MaxIoQueues + 1] = {};
    bool fallbackSafe = true;
    if (irqManager->registerPciMsixIrqHandlers(m_Pci, handlers, vectorCount, vectors, fallbackSafe,
                                               processors)) {
      LockGuard<Mutex> irqLock(m_IrqLock);
      m_Irq = vectors[0];
      for (size_t i = 1; i < vectorCount; ++i) {
        m_IoIrq[i - 1] = vectors[i];
      }
      NOTICE("NVMe: MSI-X vectors=" << Dec << vectorCount << ", I/O queues=" << m_IoCount << Hex);
      break;
    }
    if (!fallbackSafe) {
      ERROR("NVMe: MSI-X setup failed without a safe fallback");
      return false;
    }
    if (vectorCount == 1) {
      LockGuard<Mutex> irqLock(m_IrqLock);
      m_BatchInitialising = false;
      break;
    }
    if (m_IoCount > 1) {
      --m_IoCount;
      vectorCount = m_IoCount + 1;
    } else {
      vectorCount = 1;
    }
  }
  uint16_t ioDepth = depth;
  if (m_IoCount && ioDepth > MaxIoSlots / m_IoCount + 1) {
    ioDepth = MaxIoSlots / m_IoCount + 1;
  }
  for (size_t i = 0; i < m_IoCount; ++i) {
    auto* queue = new NvmeQueue;
    if (!queue->initialise(m_Registers, i + 1, ioDepth, stride, m_MaxTransfer, m_Pci)) {
      queue->releaseDma();
      delete queue;
      if (!i) {
        return false;
      }
      LockGuard<Mutex> irqLock(m_IrqLock);
      m_IoCount = i;
      break;
    }
    {
      LockGuard<Mutex> irqLock(m_IrqLock);
      m_Io[i] = queue;
    }
    if (!createIoQueue(i, m_IoIrq[i] ? i + 1 : 0)) {
      return false;
    }
  }
  if (!m_Irq) {
    m_Irq = irqManager->registerPciMessageIrqHandler(this, m_Pci, IrqPolicy::pciIntxThreaded());
  }
  if (!m_Irq) {
    ERROR("NVMe: could not register PCI interrupt");
    return false;
  }
  {
    LockGuard<Mutex> irqLock(m_IrqLock);
    m_Interrupts = true;
    m_BatchInitialising = false;
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
  for (size_t i = 0; i < m_IoCount; ++i) {
    Command flush{};
    flush.nsid = static_cast<NvmeDisk*>(getChild(0))->namespaceId();
    if (!InterruptProbe::run(
            [&] { return command(*m_Io[i], flush, nullptr, 0, false, nullptr, true); },
            [&] { return m_Io[i]->interruptCompletions(); })) {
      ERROR("NVMe: I/O vector delivery probe failed for queue " << Dec << i + 1 << Hex);
      return false;
    }
  }
  for (size_t i = 0; i < getNumChildren(); ++i)
    static_cast<NvmeDisk*>(getChild(i))->publishEndpoint();
  NOTICE("NVMe: '" << m_Model << "' ready, " << Dec << getNumChildren() << " namespaces, "
                   << m_IoCount << " I/O queues, " << ioDepth - 1U << " slots per queue" << Hex);
  return true;
}
bool NvmeController::createIoQueue(size_t index, uint16_t interruptVector) {
  NvmeQueue& queue = *m_Io[index];
  const uint16_t id = index + 1;
  Command request{};
  request.opcode = 5;
  request.prp1 = queue.completionAddress();
  request.cdw10 = id | ((queue.depth() - 1U) << 16);
  request.cdw11 = 3U | (uint32_t(interruptVector) << 16);
  if (!command(m_Admin, request)) {
    return false;
  }
  request = {};
  request.opcode = 1;
  request.prp1 = queue.submissionAddress();
  request.cdw10 = id | ((queue.depth() - 1U) << 16);
  request.cdw11 = 1U | (uint32_t(id) << 16);
  return command(m_Admin, request);
}
NvmeQueue& NvmeController::ioQueue() {
  const size_t cpu = Processor::index();
  for (size_t i = 0; i < m_IoCount; ++i) {
    if (m_IoProcessor[i] == cpu) {
      return *m_Io[i];
    }
  }
  return *m_Io[cpu % m_IoCount];
}
bool NvmeController::discoverNamespaces(uint32_t maximumId, bool& activeNamespaces) {
  activeNamespaces = false;
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
      activeNamespaces = true;
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
  return readWrite(nsid, lba, blocks, buffer, bytes, writing, false);
}
bool NvmeController::readWrite(uint32_t nsid, uint64_t lba, uint32_t blocks, void* buffer,
                               size_t bytes, bool writing, bool cacheFill,
                               physical_uintptr_t directWritePhysical) {
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
  return command(ioQueue(), request, buffer, bytes, writing, nullptr, false, cacheFill,
                 directWritePhysical);
}
bool NvmeController::flush(uint32_t nsid) {
  OperationBarrier::Lease lease;
  if (!m_Commands.tryAcquire(lease) || !findNamespace(nsid))
    return false;
  // NVMe Flush guarantees persistence for writes completed before submission.
  LockGuard<Mutex> writeLock(m_WriteLock);
  Command request{};
  request.nsid = nsid;
  return command(ioQueue(), request);
}
IrqDisposition NvmeController::irq(irq_id_t id) {
  NvmeQueue* queues[MaxIoQueues + 1] = {};
  size_t count = 0;
  {
    LockGuard<Mutex> irqLock(m_IrqLock);
    if (m_Stopping) {
      return IrqDisposition::Quiesced;
    }
    if (!m_Interrupts && !m_BatchInitialising) {
      return IrqDisposition::NotHandled;
    }
    if (m_BatchInitialising || !m_IoIrq[0]) {
      queues[count++] = &m_Admin;
      for (auto* queue : m_Io) {
        if (queue) {
          queues[count++] = queue;
        }
      }
    } else if (id == m_Irq) {
      queues[count++] = &m_Admin;
    } else {
      for (size_t i = 0; i < MaxIoQueues; ++i) {
        if (id == m_IoIrq[i] && m_Io[i]) {
          queues[count++] = m_Io[i];
          break;
        }
      }
    }
  }
  // Synchronous IRQ retirement keeps the queues alive across this snapshot.
  // Independent vectors must not serialize their completion work together.
  bool handled = false;
  for (size_t i = 0; i < count; ++i) {
    handled |= queues[i]->complete(true);
  }
  return handled ? IrqDisposition::Handled : IrqDisposition::NotHandled;
}
size_t NvmeController::interruptCompletions() const {
  size_t completions = m_Admin.interruptCompletions();
  for (size_t i = 0; i < m_IoCount; ++i) {
    completions += m_Io[i]->interruptCompletions();
  }
  return completions;
}
size_t NvmeController::ioQueueInterruptCompletions(size_t queue) const {
  return queue < m_IoCount ? m_Io[queue]->interruptCompletions() : 0;
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
  for (auto& irq : m_IoIrq) {
    if (irq && !Machine::instance().getIrqManager()->unregisterHandler(irq, this)) {
      panic("NVMe: synchronous I/O vector retirement failed");
    }
    irq = 0;
  }
  m_Admin.stop();
  for (auto* queue : m_Io) {
    if (queue) {
      queue->stop();
    }
  }
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
  m_Admin.releaseDma();
  for (auto*& queue : m_Io) {
    if (queue) {
      queue->releaseDma();
      delete queue;
      queue = nullptr;
    }
  }
  m_IoCount = 0;
  if (m_DmaIsolated) {
    if (!PciBus::instance().detachIsolatedDma(m_Pci)) {
      panic("NVMe: failed to retire isolated DMA domain");
    }
    m_DmaIsolated = false;
  }
  m_Shutdown = true;
}

size_t NvmeController::maximumOutstanding() const {
  size_t maximum = 0;
  for (size_t i = 0; i < m_IoCount; ++i) {
    const size_t outstanding = m_Io[i]->maximumOutstanding();
    if (outstanding > maximum) {
      maximum = outstanding;
    }
  }
  return maximum;
}
