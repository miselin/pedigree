/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/String.h"

#include "modules/Module.h"
#include "modules/drivers/common/ahci/AhciController.h"
#include "modules/drivers/common/ahci/AhciDisk.h"
#include "modules/system/vfs/Filesystem.h"
#include "modules/system/vfs/VFS.h"

namespace {
constexpr uint64_t DiskBytes = 32 * 1024 * 1024;
constexpr size_t HeaderBytes = 4096;
constexpr uint8_t BaseSeed = 0x5a;
constexpr uint8_t WriteSeed = 0xa5;
constexpr uint64_t ErrorOffset = 24 * 1024 * 1024;
constexpr char Magic[] = "PEDIGREE-AHCI-SMOKE-v1";

struct Range {
  uint64_t offset;
  size_t length;
};
constexpr Range ReadRanges[] = {
    {4096, 4096}, {65536 - 512, 8192}, {1024 * 1024 - 512, 128 * 1024}, {DiskBytes - 4096, 4096}};
constexpr Range WriteRanges[] = {
    {8 * 1024 * 1024 + 512, 8192}, {16 * 1024 * 1024 - 512, 128 * 1024}, {DiskBytes - 512, 512}};

AhciDisk* scratch = nullptr;
bool duplicateScratch = false;
uint8_t fixtureFlags = 0;

bool fail(const char* reason) {
  ERROR("AHCI-SMOKE: FAIL " << reason);
  return false;
}

uint8_t pattern(uint64_t offset, uint8_t seed) {
  return ((offset * 37) ^ (offset >> 8) ^ (offset >> 16) ^ seed) & 255;
}

uint8_t expected(uint64_t offset, bool written) {
  if (offset < HeaderBytes) {
    if (offset < sizeof(Magic) - 1)
      return Magic[offset];
    if (offset == 32)
      return 1;
    if (offset == 36)
      return BaseSeed;
    if (offset >= 40 && offset < 48)
      return (DiskBytes >> ((offset - 40) * 8)) & 255;
    if (offset == 48)
      return fixtureFlags;
    return 0;
  }
  if (written) {
    for (const Range& range : WriteRanges) {
      if (offset >= range.offset && offset < range.offset + range.length)
        return pattern(offset, WriteSeed);
    }
  }
  return pattern(offset, BaseSeed);
}

Device* findScratch(Device* device) {
  if (device->getSpecificType() == String("ahci-disk")) {
    AhciDisk* disk = static_cast<AhciDisk*>(device);
    if (disk->port() == 1 && disk->getSize() == DiskBytes) {
      if (scratch)
        duplicateScratch = true;
      else
        scratch = disk;
    }
  }
  return device;
}

bool checkRange(AhciDisk& disk, const Range& range, bool written) {
  BufferView storage[40];
  BufferViewSequence views(storage, 40);
  if (!disk.readViews(range.offset, range.length, views))
    return fail("readViews");
  bool valid = true;
  uint64_t offset = range.offset;
  for (size_t viewIndex = 0; viewIndex < views.count(); ++viewIndex) {
    const BufferView view = views[viewIndex];
    for (size_t i = 0; i < view.size(); ++i) {
      if (view[i] != expected(offset + i, written)) {
        ERROR("AHCI-SMOKE: FAIL byte mismatch at " << Dec << (offset + i));
        valid = false;
        break;
      }
    }
    offset += view.size();
  }
  disk.unpinViews(range.offset, views);
  return valid;
}

bool writeRange(AhciDisk& disk, const Range& range) {
  BufferView storage[40];
  uint64_t pageOffsets[40];
  BufferViewSequence views(storage, 40);
  if (!disk.readViews(range.offset, range.length, views))
    return fail("write readViews");
  const size_t count = views.count();
  uint64_t offset = range.offset;
  bool synced = true;
  for (size_t viewIndex = 0; viewIndex < count; ++viewIndex) {
    const BufferView view = views[viewIndex];
    pageOffsets[viewIndex] = offset;
    for (size_t i = 0; i < view.size(); ++i)
      view[i] = pattern(offset + i, WriteSeed);
    // sync() retains a separate cache reference and checks write plus FLUSH.
    if (!disk.sync(offset, false))
      synced = false;
    offset += view.size();
  }
  disk.unpinViews(range.offset, views);
  if (!synced || !disk.syncAll())
    return fail("checked disk sync");
  // Retire only after dropping every pin, so the reread must reach the disk.
  for (size_t i = 0; i < count; ++i) {
    if (!disk.retireCachePage(pageOffsets[i]))
      return fail("cache retirement");
  }
  return true;
}

bool checkRejectedRanges(AhciDisk& disk) {
  const BufferView pastEnd = disk.read(DiskBytes);
  if (pastEnd) {
    disk.unpin(DiskBytes);
    return fail("end-of-disk read accepted");
  }
  BufferView storage[1];
  BufferViewSequence views(storage, 1);
  const Range invalid[] = {{512, ~size_t{0}}, {DiskBytes - 512, 1024}};
  for (const Range& range : invalid) {
    const bool accepted = disk.readViews(range.offset, range.length, views);
    const bool empty = views.empty();
    disk.unpinViews(range.offset, views);
    if (accepted || !empty)
      return fail("invalid readViews range accepted or retained views");
  }
  if (!disk.hasNoCacheLoans())
    return fail("cache pins retained after rejected ranges");
  return true;
}

bool checkInjectedError(AhciDisk& disk, AhciDisk& root) {
  DiskUse rootUse;
  if (!root.acquireUse(rootUse))
    return fail("root admission after persistence tests");
  if (root.getSize() < 512 || root.getSize() % 512)
    return fail("root tail-sector geometry");
  const uint64_t rootLba = root.getSize() / 512 - 1;
  uint8_t rootSector[512];
  if (!root.controller()->readWrite(root.port(), rootLba, 1, rootSector, sizeof(rootSector), false))
    return fail("direct root reference read");

  constexpr uint64_t RetryOffset = 28 * 1024 * 1024;
  if (!disk.retireCachePage(ErrorOffset) || !disk.retireCachePage(RetryOffset))
    return fail("error-test cache retirement");
  const auto started = Time::getTicks();
  const BufferView failed = disk.read(ErrorOffset);
  const auto elapsed = Time::getTicks() - started;
  if (failed) {
    disk.unpin(ErrorOffset);
    return fail("injected read error returned data");
  }
  if (elapsed >= 5 * Time::Multiplier::Second || !disk.hasNoCacheLoans())
    return fail("injected read error was slow or retained cache pins");
  NOTICE("AHCI-SMOKE: PASS transport-error milliseconds="
         << Dec << elapsed / Time::Multiplier::Millisecond);

  const auto retryStarted = Time::getTicks();
  const BufferView retry = disk.read(RetryOffset);
  const auto retryElapsed = Time::getTicks() - retryStarted;
  if (retry) {
    disk.unpin(RetryOffset);
    return fail("offline scratch disk accepted an uncached read");
  }
  if (retryElapsed >= 5 * Time::Multiplier::Second || !disk.hasNoCacheLoans())
    return fail("offline rejection was slow or retained cache pins");
  NOTICE("AHCI-SMOKE: PASS offline-rejection milliseconds="
         << Dec << retryElapsed / Time::Multiplier::Millisecond);

  // Mounted ext2 metadata retains cache pins. Direct transport reads prove
  // port isolation without attempting to evict any filesystem-owned page.
  uint8_t after[512];
  if (!root.controller()->readWrite(root.port(), rootLba, 1, after, sizeof(after), false))
    return fail("direct root read after scratch error");
  bool unchanged = true;
  for (size_t i = 0; unchanged && i < sizeof(rootSector); ++i)
    unchanged = after[i] == rootSector[i];
  if (!unchanged)
    return fail("root sector changed after scratch error");
  NOTICE("AHCI-SMOKE: PASS root-after-error port=0 direct-sector=" << Dec << rootLba);
  return true;
}

bool entry() {
#if CRIPPLE_HDD
  return fail("CRIPPLE_HDD must be disabled");
#else
  Filesystem* filesystem = VFS::instance().getRootFilesystem();
  Disk* root = filesystem ? filesystem->getDisk() : nullptr;
  root = root ? root->physicalDisk() : nullptr;
  if (!root || root->getSpecificType() != String("ahci-disk"))
    return fail("root filesystem is not backed by AHCI");
  AhciDisk* rootDisk = static_cast<AhciDisk*>(root);
  AhciController* controller = rootDisk->controller();
  if (rootDisk->port() != 0 || !controller ||
      controller->getSpecificType() != String("ahci-controller"))
    return fail("root AHCI controller or port");

  scratch = nullptr;
  duplicateScratch = false;
  Device::foreach (findScratch);
  if (!scratch || duplicateScratch || scratch->controller() != controller || scratch == rootDisk ||
      scratch->getNativeBlockSize() != 512)
    return fail("unique scratch disk on AHCI port 1");
  DiskUse use;
  if (!scratch->acquireUse(use))
    return fail("scratch disk admission");
  const size_t initialCompletions = controller->interruptCompletions();
  const BufferView fixtureHeader = scratch->read(0);
  if (!fixtureHeader)
    return fail("fixture header read");
  fixtureFlags = fixtureHeader.size() > 48 ? fixtureHeader[48] : 255;
  scratch->unpin(0);
  if (fixtureFlags > 1)
    return fail("unsupported fixture flags");
  // Every byte of the immutable fixture header must match before any write.
  if (!checkRange(*scratch, {0, HeaderBytes}, false))
    return false;
  NOTICE("AHCI-SMOKE: PASS fixture-identification");
  if (!checkRejectedRanges(*scratch))
    return false;
  NOTICE("AHCI-SMOKE: PASS range-rejection");
  for (const Range& range : ReadRanges) {
    if (!checkRange(*scratch, range, false))
      return false;
  }
  NOTICE("AHCI-SMOKE: PASS patterned-reads");
  for (const Range& range : WriteRanges) {
    if (!writeRange(*scratch, range))
      return false;
  }
  NOTICE("AHCI-SMOKE: PASS writes-and-sync");
  for (const Range& range : WriteRanges) {
    if (!checkRange(*scratch, range, true))
      return false;
  }
  for (const Range& range : ReadRanges) {
    if (!checkRange(*scratch, range, true))
      return false;
  }
  NOTICE("AHCI-SMOKE: PASS uncached-rereads");
  NOTICE("AHCI-SMOKE: PASS ahci-root-mount port=0");
  const size_t completions = controller->interruptCompletions() - initialCompletions;
  if (!completions)
    return fail("no interrupt completions during smoke");
  NOTICE("AHCI-SMOKE: PASS interrupt-completions count=" << Dec << completions);
  if (fixtureFlags && !checkInjectedError(*scratch, *rootDisk))
    return false;
  NOTICE("AHCI-SMOKE: PASS complete");
  return true;
#endif
}

void exit() {}
}  // namespace

MODULE_INFO("ahci-smoke", &entry, &exit, "ahci", "mountroot", "scsi");
