/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/PciExpress.h"
#include "pedigree/kernel/machine/PciVirtualFunctions.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/new"

#include "modules/Module.h"
#include "modules/drivers/common/nvme/NvmeController.h"
#include "modules/drivers/common/nvme/NvmeDisk.h"
#include "modules/system/vfs/Filesystem.h"
#include "modules/system/vfs/VFS.h"

namespace {
constexpr uint64_t DiskBytes = 32 * 1024 * 1024;
constexpr char Magic[] = "PEDIGREE-NVME-SMOKE-v1";
constexpr size_t Workers = 20;
Disk* rootDisk;
bool success = true;
size_t tested = 0;
NvmeController* virtualizationFixture = nullptr;
bool hotplugFixture = false;
Semaphore start(0, false);
uint8_t pattern(uint64_t offset, uint8_t seed = 0x5a) {
  return ((offset * 37) ^ (offset >> 8) ^ (offset >> 16) ^ seed) & 255;
}
bool fail(const char* reason) {
  ERROR("NVME-SMOKE: FAIL " << reason);
  return false;
}
bool checkPattern(const uint8_t* data, uint64_t offset, size_t bytes, uint8_t seed = 0x5a) {
  for (size_t i = 0; i < bytes; ++i) {
    if (data[i] != pattern(offset + i, seed))
      return fail("data mismatch");
  }
  return true;
}
struct Worker {
  NvmeDisk* disk;
  size_t index;
  bool passed;
};
int readWorker(void* parameter) {
  auto* worker = static_cast<Worker*>(parameter);
  worker->passed = start.acquireForCompletion(1, 30);
  DiskUse lease;
  worker->passed &= worker->disk->acquireUse(lease);
  for (size_t i = 0; worker->passed && i < 16; ++i) {
    const uint64_t offset =
        !i ? 4 * 1024 * 1024 : 2 * 1024 * 1024 + worker->index * 65536 + i * 4096;
    const BufferView view = worker->disk->read(offset);
    if (!view) {
      worker->passed = false;
      break;
    }
    worker->passed = view.size() == 4096 &&
                     checkPattern(static_cast<const uint8_t*>(view.data()), offset, view.size());
    worker->disk->unpin(offset);
  }
  return worker->passed ? 0 : 1;
}
bool concurrency(NvmeDisk& disk) {
  Worker workers[Workers]{};
  Thread* threads[Workers]{};
  auto* controller = disk.controller();
  const size_t queueCount = controller->ioQueueCount();
  const CpuAffinityMask online = Scheduler::onlineAffinity();
  size_t processors[Nvme::MaxIoQueues] = {};
  size_t before[Nvme::MaxIoQueues] = {};
  size_t processorCount = 0;
  for (size_t cpu = 0; cpu < CpuAffinityMask::MaximumCpus && processorCount < queueCount; ++cpu) {
    if (online.contains(cpu)) {
      processors[processorCount++] = cpu;
    }
  }
  if (!queueCount || processorCount != queueCount) {
    return fail("I/O queue processor placement");
  }
  for (size_t i = 0; i < queueCount; ++i) {
    before[i] = controller->ioQueueInterruptCompletions(i);
  }
  for (size_t i = 0; i < Workers; ++i) {
    workers[i] = {&disk, i, false};
    ThreadPlacement placement;
    placement.allowed.set(processors[i % processorCount]);
    threads[i] = new Thread(Scheduler::instance().getKernelProcess(), readWorker, &workers[i],
                            nullptr, false, false, true, &placement);
    threads[i]->setName("NVMe smoke read");
    if (!threads[i]->start())
      FATAL("NVME-SMOKE: worker start failed");
  }
  start.release(Workers);
  bool passed = true;
  for (size_t i = 0; i < Workers; ++i) {
    if (!threads[i]->joinForCompletion())
      FATAL("NVME-SMOKE: worker join failed");
    passed &= workers[i].passed;
  }
  for (size_t i = 0; i < queueCount; ++i) {
    const size_t completed = controller->ioQueueInterruptCompletions(i) - before[i];
    NOTICE("NVME-SMOKE: I/O queue=" << Dec << i + 1 << " interrupt-completions=" << completed);
    passed &= completed != 0;
  }
  NOTICE("NVME-SMOKE: concurrent-read maximum-per-queue-outstanding="
         << Dec << controller->maximumOutstanding() << " queues=" << queueCount);
  return (passed && controller->maximumOutstanding() > 1) ||
         fail("concurrent cached reads, inactive queue, or no overlapping commands");
}
bool run(NvmeDisk& disk) {
  DiskUse lease;
  if (!disk.acquireUse(lease))
    return fail("scratch admission");
  auto* controller = disk.controller();
  const size_t blockBytes = disk.getNativeBlockSize();
  const uint32_t nsid = disk.namespaceId();
  const size_t initialInterrupts = controller->interruptCompletions();
  uint8_t* buffer = new uint8_t[Nvme::MaxTransfer];
  bool passed = controller->readWrite(nsid, 0, 4096 / blockBytes, buffer, 4096, false);
  for (size_t i = 0; passed && i < 4096; ++i) {
    uint8_t expected = 0;
    if (i < sizeof(Magic) - 1)
      expected = Magic[i];
    else if (i == 32)
      expected = 1;
    else if (i >= 40 && i < 48)
      expected = DiskBytes >> ((i - 40) * 8);
    else if (i >= 48 && i < 52)
      expected = blockBytes >> ((i - 48) * 8);
    passed = buffer[i] == expected;
  }
  if (!passed) {
    delete[] buffer;
    return fail("exact disposable fixture header");
  }
  NOTICE("NVME-SMOKE: PASS fixture nsid=" << Dec << nsid << " block-bytes=" << blockBytes);
  const size_t transfer = controller->maxTransfer();
  passed = controller->readWrite(nsid, (1024 * 1024 + 4096) / blockBytes, transfer / blockBytes,
                                 buffer, transfer, false) &&
           checkPattern(buffer, 1024 * 1024 + 4096, transfer);
  passed &= !controller->readWrite(nsid, disk.getBlockCount(), 1, buffer, blockBytes, false);
  passed &=
      !controller->readWrite(nsid, disk.getBlockCount() - 1, 2, buffer, 2 * blockBytes, false);
  passed &= !controller->readWrite(nsid, 0, 0, buffer, 0, false);
  const BufferView end = disk.read(DiskBytes);
  if (end) {
    disk.unpin(DiskBytes);
    passed = false;
  }
  if (!passed || !concurrency(disk)) {
    delete[] buffer;
    return fail("read, PRP list, or range rejection");
  }
  const BufferView subBlock = disk.read(1024);
  passed = bool(subBlock) && subBlock.size() == 3072;
  if (subBlock) {
    for (size_t i = 0; passed && i < subBlock.size(); ++i)
      passed = subBlock[i] == 0;
    disk.unpin(1024);
  }
  if (!passed) {
    delete[] buffer;
    return fail("sub-block byte-offset read");
  }
  NOTICE("NVME-SMOKE: PASS reads-prps-ranges-concurrency nsid=" << Dec << nsid);
  const uint64_t rawOffsets[] = {8 * 1024 * 1024, DiskBytes - blockBytes};
  for (uint64_t offset : rawOffsets) {
    const size_t bytes = offset == rawOffsets[0] ? transfer : blockBytes;
    for (size_t i = 0; i < bytes; ++i)
      buffer[i] = pattern(offset + i, 0xa5);
    passed =
        controller->readWrite(nsid, offset / blockBytes, bytes / blockBytes, buffer, bytes, true) &&
        controller->flush(nsid);
    if (passed)
      passed = controller->readWrite(nsid, offset / blockBytes, bytes / blockBytes, buffer, bytes,
                                     false) &&
               checkPattern(buffer, offset, bytes, 0xa5);
    if (!passed)
      break;
  }
  constexpr uint64_t CachedOffset = 16 * 1024 * 1024;
  if (passed) {
    const BufferView view = disk.read(CachedOffset);
    passed = bool(view) && view.size() == 4096;
    if (view) {
      for (size_t i = 0; i < view.size(); ++i)
        view[i] = pattern(CachedOffset + i, 0xa5);
      disk.unpin(CachedOffset);
      passed &= disk.syncPages(&CachedOffset, 1) && disk.sync(CachedOffset, false);
    }
    passed &= disk.syncAll() && disk.retireCachePage(CachedOffset);
    if (passed) {
      const BufferView reread = disk.read(CachedOffset);
      passed = bool(reread) && checkPattern(static_cast<const uint8_t*>(reread.data()),
                                            CachedOffset, reread.size(), 0xa5);
      if (reread)
        disk.unpin(CachedOffset);
    }
  }
  delete[] buffer;
  const size_t interrupts = controller->interruptCompletions() - initialInterrupts;
  if (!passed || !interrupts || !disk.hasNoCacheLoans())
    return fail("write/flush/reread, interrupts, or leaked cache pins");
  NOTICE("NVME-SMOKE: PASS writes-flush-reread nsid=" << Dec << nsid
                                                      << " interrupt-completions=" << interrupts);
  return true;
}
Device* visit(Device* device) {
  if (device->getSpecificType() == "nvme-controller") {
    auto* controller = static_cast<NvmeController*>(device);
    if (String(controller->model()) == "PEDIGREE-SRIOV-SMOKE") {
      virtualizationFixture = controller;
    } else if (String(controller->model()) == "PEDIGREE-HOTPLUG-SMOKE") {
      hotplugFixture = true;
    }
  }
  if (device->getSpecificType() == String("nvme-disk") && device != rootDisk &&
      static_cast<NvmeDisk*>(device)->getSize() == DiskBytes) {
    const String model(static_cast<NvmeDisk*>(device)->controller()->model());
    if (model == "PEDIGREE-SRIOV-SMOKE" || model == "PEDIGREE-HOTPLUG-SMOKE") {
      return device;
    }
    if (success)
      success = run(*static_cast<NvmeDisk*>(device));
    ++tested;
  }
  return device;
}

bool sriov(NvmeController& original) {
  using namespace Nvme;
  if (!original.supportsVirtualizationManagement() || original.getNumChildren() ||
      !original.prepareDiskRemoval()) {
    return fail("SR-IOV fixture PF must have no attached namespaces");
  }
  uint16_t changed = 0;
  bool passed =
      original.virtualizationManagement(original.controllerId(), VirtualResource::Queue,
                                        VirtualizationAction::AllocatePrimary, 0, &changed) &&
      !changed &&
      original.virtualizationManagement(original.controllerId(), VirtualResource::Interrupt,
                                        VirtualizationAction::AllocatePrimary, 0, &changed) &&
      !changed;
  Device* pf = original.pciDevice();
  original.shutdown();
  PciFunctionState::State resources;
  if (!PciBus::instance().inspectFunction(pf, resources, false) || (resources.command & 4U)) {
    return fail("SR-IOV stopped primary resources");
  }
  struct Config {
    Device* device;
    bool read8(uint16_t offset, uint8_t& value) {
      return PciBus::instance().readConfig8(device, offset, value);
    }
    bool read16(uint16_t offset, uint16_t& value) {
      return PciBus::instance().readConfig16(device, offset, value);
    }
    bool read32(uint16_t offset, uint32_t& value) {
      return PciBus::instance().readConfig32(device, offset, value);
    }
    bool write8(uint16_t offset, uint8_t value) {
      return PciBus::instance().writeConfig8(device, offset, value);
    }
    bool write16(uint16_t offset, uint16_t value) {
      return PciBus::instance().writeConfig16(device, offset, value);
    }
    bool write32(uint16_t offset, uint32_t value) {
      return PciBus::instance().writeConfig32(device, offset, value);
    }
  } config{pf};
  auto delay = [](size_t milliseconds) {
    const auto deadline = Time::getTicks() + milliseconds * Time::Multiplier::Millisecond;
    while (Time::getTicks() < deadline) {
      Time::delay(Time::Multiplier::Millisecond);
    }
    return true;
  };
  if (!passed || PciExpress::resetFunction(config, delay) != PciExpress::FlrResult::Complete) {
    return fail("SR-IOV primary resource reset");
  }
  if (!PciBus::instance().updateCommand(pf, 7U, 0)) {
    return fail("SR-IOV primary decoding after reset");
  }
  // Restore assigned resources after FLR, with each 64-bit BAR's high half first.
  for (size_t i = resources.barCount; i; --i) {
    if (!PciFunctionState::writeVerified32(config, 0x10 + 4 * (i - 1), resources.bars[i - 1])) {
      return fail("SR-IOV primary BAR restore");
    }
  }
  if (!config.write8(0x3c, resources.interruptLine) ||
      !PciFunctionState::resourcesUnchanged(config, resources)) {
    return fail("SR-IOV primary resource restore");
  }
  PciVirtualFunctions group(pf);
  for (size_t cycle = 0; cycle < 2; ++cycle) {
    if (!group.enable(1)) {
      return fail("SR-IOV enable");
    }
    auto* primary = new NvmeController(pf);
    passed = primary->initialiseController();
    SecondaryControllerList controllers{};
    uint16_t secondaryId = 0xffff;
    if (passed && primary->secondaryControllers(0, controllers)) {
      for (size_t i = 0; i < controllers.count; ++i) {
        if (controllers.entries[i].virtualFunction == 1 &&
            controllers.entries[i].primaryControllerId == primary->controllerId()) {
          secondaryId = controllers.entries[i].controllerId;
        }
      }
    }
    passed = passed && secondaryId != 0xffff &&
             primary->virtualizationManagement(secondaryId, VirtualResource::Queue,
                                               VirtualizationAction::Offline, 0) &&
             primary->virtualizationManagement(secondaryId, VirtualResource::Queue,
                                               VirtualizationAction::Assign, 2, &changed) &&
             changed == 2 &&
             primary->virtualizationManagement(secondaryId, VirtualResource::Interrupt,
                                               VirtualizationAction::Assign, 2, &changed) &&
             changed == 2 &&
             primary->virtualizationManagement(secondaryId, VirtualResource::Queue,
                                               VirtualizationAction::Online, 0) &&
             primary->setNamespaceAttachment(1, secondaryId, true);
    Device* vf = group.function(0);
    auto* secondary = new NvmeController(vf);
    if (passed && secondary->initialiseController()) {
      vf->addChild(secondary);
      secondary->setParent(vf);
      passed = !group.disable() && secondary->getNumChildren() == 1 &&
               static_cast<NvmeDisk*>(secondary->getChild(0))->getSize() == DiskBytes &&
               run(*static_cast<NvmeDisk*>(secondary->getChild(0)));
    } else {
      passed = false;
    }
    secondary->shutdown();
    vf->removeChild(secondary);
    secondary->setParent(nullptr);
    delete secondary;
    if (secondaryId != 0xffff) {
      passed &= primary->setNamespaceAttachment(1, secondaryId, false);
      passed &= primary->virtualizationManagement(secondaryId, VirtualResource::Queue,
                                                  VirtualizationAction::Offline, 0);
    }
    primary->shutdown();
    delete primary;
    passed &= group.disable() && !group.count();
    if (!passed) {
      return fail("SR-IOV secondary I/O or retirement");
    }
    NOTICE("NVME-SMOKE: PASS sriov-cycle=" << Dec << cycle + 1);
  }
  return true;
}

bool hotplugPresent(DiskUse* use = nullptr) {
  bool present = false;
  auto find = [&](Device* device) -> Device* {
    if (device->getSpecificType() == "nvme-controller") {
      auto* controller = static_cast<NvmeController*>(device);
      present |= String(controller->model()) == "PEDIGREE-HOTPLUG-SMOKE";
    } else if (use && !*use && device->getSpecificType() == "nvme-disk") {
      auto* disk = static_cast<NvmeDisk*>(device);
      if (device != rootDisk && disk->getSize() == DiskBytes && disk->getNativeBlockSize() == 512 &&
          String(disk->controller()->model()) == "PEDIGREE-HOTPLUG-SMOKE") {
        disk->acquireUse(*use);
      }
    }
    return device;
  };
  auto callback = pedigree_std::make_callable(find);
  Device::foreach (callback, nullptr);
  return present;
}

void hotplugDelay(size_t seconds) {
  const auto deadline = Time::getTicks() + seconds * Time::Multiplier::Second;
  while (Time::getTicks() < deadline) {
    Time::delay(50 * Time::Multiplier::Millisecond);
  }
}

bool waitHotplug(bool present) {
  const auto deadline = Time::getTicks() + 30 * Time::Multiplier::Second;
  do {
    if (hotplugPresent() == present) {
      return true;
    }
    Time::delay(100 * Time::Multiplier::Millisecond);
  } while (Time::getTicks() < deadline);
  return false;
}

bool hotplug() {
  for (size_t cycle = 1; cycle <= 2; ++cycle) {
    DiskUse use;
    if (!hotplugPresent(&use) || !use) {
      return fail("hotplug fixture admission");
    }
    auto* disk = static_cast<NvmeDisk*>(use.get());
    if (!run(*disk)) {
      return false;
    }
    NOTICE("NVME-SMOKE: READY hotplug-busy cycle=" << Dec << cycle);
    hotplugDelay(8);
    constexpr uint64_t offset = 5 * 1024 * 1024;
    uint8_t buffer[4096];
    if (!disk->controller()->readWrite(disk->namespaceId(), offset / 512, 8, buffer, sizeof(buffer),
                                       false) ||
        !checkPattern(buffer, offset, sizeof(buffer))) {
      return fail("hotplug busy device lost I/O");
    }
    use.reset();
    NOTICE("NVME-SMOKE: PASS hotplug-busy cycle=" << Dec << cycle);
    NOTICE("NVME-SMOKE: READY hotplug-remove cycle=" << Dec << cycle);
    if (!waitHotplug(false)) {
      return fail("hotplug removal timed out");
    }
    NOTICE("NVME-SMOKE: PASS hotplug-removed cycle=" << Dec << cycle);
    // Node retirement precedes the slot worker's power-off completion.
    hotplugDelay(2);
    if (cycle == 1) {
      NOTICE("NVME-SMOKE: READY hotplug-insert");
      if (!waitHotplug(true)) {
        return fail("hotplug insertion timed out");
      }
    }
  }
  return true;
}
bool entry() {
#if CRIPPLE_HDD
  return fail("CRIPPLE_HDD must be disabled");
#else
  auto* filesystem = VFS::instance().getRootFilesystem();
  rootDisk = filesystem ? filesystem->getDisk() : nullptr;
  rootDisk = rootDisk ? rootDisk->physicalDisk() : nullptr;
  if (rootDisk && rootDisk->getSpecificType() == String("nvme-disk"))
    NOTICE("NVME-SMOKE: root namespace=" << Dec << static_cast<NvmeDisk*>(rootDisk)->namespaceId());
  Device::foreach (visit);
  if (success && virtualizationFixture) {
    success = sriov(*virtualizationFixture);
  }
  if (success && hotplugFixture) {
    success = hotplug();
  }
  if (!tested || !success)
    return fail("scratch namespace checks");
  NOTICE("NVME-SMOKE: PASS complete namespaces=" << Dec << tested);
  return true;
#endif
}
void exit() {}
}  // namespace
MODULE_INFO("nvme-smoke", &entry, &exit, "nvme", "mountroot", "scsi");
