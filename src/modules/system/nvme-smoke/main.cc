/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
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
  for (size_t i = 0; i < Workers; ++i) {
    workers[i] = {&disk, i, false};
    threads[i] = new Thread(Scheduler::instance().getKernelProcess(), readWorker, &workers[i],
                            nullptr, false, false, true);
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
  NOTICE("NVME-SMOKE: concurrent-read maximum-outstanding="
         << Dec << disk.controller()->maximumOutstanding());
  return (passed && disk.controller()->maximumOutstanding() > 1) ||
         fail("concurrent cached reads or no overlapping device commands");
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
      passed &= disk.sync(CachedOffset, false);
      disk.unpin(CachedOffset);
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
  if (device->getSpecificType() == String("nvme-disk") && device != rootDisk &&
      static_cast<NvmeDisk*>(device)->getSize() == DiskBytes) {
    if (success)
      success = run(*static_cast<NvmeDisk*>(device));
    ++tested;
  }
  return device;
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
  if (!tested || !success)
    return fail("scratch namespace checks");
  NOTICE("NVME-SMOKE: PASS complete namespaces=" << Dec << tested);
  return true;
#endif
}
void exit() {}
}  // namespace
MODULE_INFO("nvme-smoke", &entry, &exit, "nvme", "mountroot", "scsi");
