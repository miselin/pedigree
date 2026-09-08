/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/time/Time.h"

#include "modules/Module.h"
#include "modules/drivers/common/scsi/ScsiDisk.h"
#include "modules/drivers/common/usb-mass-storage/UsbMassStorageDevice.h"
#include "modules/system/usb/UsbDevice.h"
#include "modules/system/vfs/Filesystem.h"
#include "modules/system/vfs/VFS.h"

extern bool runHostedUsbBotRegressions();
extern bool runHostedHidReportRegressions();

namespace {
constexpr uint64_t DiskBytes = 32 * 1024 * 1024;
constexpr char Magic[] = "PEDIGREE-USB-SMOKE-v1";
constexpr uint64_t WriteOffsets[] = {8 * 1024 * 1024, 8 * 1024 * 1024 + 4096, DiskBytes - 4096};
ScsiDisk* scratch = nullptr;
UsbSpeed storageSpeed = LowSpeed;
bool duplicate = false;
Mutex eventLock;
Semaphore events(0, false);
uint8_t observed = 0;
size_t hidDevices = 0;
uint8_t pattern(uint64_t offset, uint8_t seed = 0x5a) {
  return ((offset * 37) ^ (offset >> 8) ^ (offset >> 16) ^ seed) & 255;
}
bool fail(const char* reason) {
  ERROR("USB-SMOKE: FAIL " << reason);
  return false;
}
Device* findScratch(Device* device) {
  if (device->getType() == Device::UsbContainer) {
    auto* usb = static_cast<UsbDeviceContainer*>(device)->getUsbDevice();
    if (usb && usb->getUsbState() == UsbDevice::HasDriver && usb->getInterface() &&
        usb->getInterface()->nClass == 3)
      ++hidDevices;
  }
  if (device->getSpecificType() == String("usb-msd-controller")) {
    for (size_t i = 0; i < device->getNumChildren(); ++i) {
      auto* disk = static_cast<ScsiDisk*>(device->getChild(i));
      if (disk->getSize() == DiskBytes) {
        duplicate |= scratch != nullptr;
        scratch = disk;
        storageSpeed = static_cast<UsbMassStorageDevice*>(device)->getSpeed();
      }
    }
  }
  return device;
}
bool storage() {
  const auto deadline = Time::getTicks() + 30 * Time::Multiplier::Second;
  do {
    scratch = nullptr;
    duplicate = false;
    hidDevices = 0;
    Device::foreach (findScratch);
    if ((scratch && hidDevices >= 2) || duplicate)
      break;
    Time::delay(10 * Time::Multiplier::Millisecond);
  } while (Time::getTicks() < deadline);
  auto* filesystem = VFS::instance().getRootFilesystem();
  Disk* root = filesystem ? filesystem->getDisk() : nullptr;
  Disk* physicalRoot = root ? root->physicalDisk() : nullptr;
  if (physicalRoot && physicalRoot->getSpecificType() == String("nvme-disk") &&
      physicalRoot->getNativeBlockSize() == 4096)
    NOTICE("USB-SMOKE: root=NVMe-4Kn");
  if (!scratch || duplicate || hidDevices < 2 || (root && scratch == root->physicalDisk()) ||
      (scratch->getNativeBlockSize() != 512 && scratch->getNativeBlockSize() != 4096))
    return fail("unique disposable USB namespace");
  DiskUse use;
  if (!scratch->acquireUse(use))
    return fail("scratch admission");
  const BufferView header = scratch->read(0);
  bool valid = bool(header) && header.size() == 4096;
  for (size_t i = 0; valid && i < header.size(); ++i) {
    uint8_t expected = 0;
    if (i < sizeof(Magic) - 1)
      expected = Magic[i];
    else if (i == 32)
      expected = 1;
    else if (i >= 40 && i < 48)
      expected = DiskBytes >> ((i - 40) * 8);
    else if (i >= 48 && i < 52)
      expected = scratch->getNativeBlockSize() >> ((i - 48) * 8);
    valid = header[i] == expected;
  }
  if (header)
    scratch->unpin(0);
  if (!valid)
    return fail("exact disposable fixture header");
  NOTICE("USB-SMOKE: PASS fixture-identification");
  NOTICE("USB-SMOKE: storage-link=" << (storageSpeed >= SuperSpeed ? "SuperSpeed" : "USB1/2"));
  if (storageSpeed >= SuperSpeed) {
    // Exercise persistent endpoint and event rings beyond their first cycle.
    for (size_t page = 0; page < 320; ++page) {
      const uint64_t offset = 1024 * 1024 + page * 4096;
      const BufferView view = scratch->read(offset);
      valid = bool(view) && view.size() == 4096;
      for (size_t i = 0; valid && i < view.size(); ++i)
        valid = view[i] == pattern(offset + i);
      if (view)
        scratch->unpin(offset);
      if (!valid || !scratch->retireCachePage(offset))
        return fail("uncached transfer-ring reads");
    }
    NOTICE("USB-SMOKE: PASS sustained-reads");
  }
  for (uint64_t offset : WriteOffsets) {
    const BufferView view = scratch->read(offset);
    valid = bool(view) && view.size() == 4096;
    for (size_t i = 0; valid && i < view.size(); ++i)
      valid = view[i] == pattern(offset + i);
    if (view) {
      if (valid) {
        for (size_t i = 0; i < view.size(); ++i)
          view[i] = pattern(offset + i, 0xa5);
        valid = scratch->sync(offset, false);
      }
      scratch->unpin(offset);
    }
    if (!valid || !scratch->syncAll() || !scratch->retireCachePage(offset))
      return fail("patterned write, flush or cache retirement");
    const BufferView reread = scratch->read(offset);
    valid = bool(reread) && reread.size() == 4096;
    for (size_t i = 0; valid && i < reread.size(); ++i)
      valid = reread[i] == pattern(offset + i, 0xa5);
    if (reread)
      scratch->unpin(offset);
    if (!valid)
      return fail("uncached reread");
  }
  if (!scratch->hasNoCacheLoans())
    return fail("cache pins retained");
  NOTICE("USB-SMOKE: PASS msd-write-flush-reread");
  return true;
}
void input(InputManager::InputNotification& event) {
  LockGuard<Mutex> lock(eventLock);
  uint8_t bits = 0;
  if (event.type == InputManager::RawKey && event.data.rawkey.scancode == 4)
    bits |= event.data.rawkey.keyUp ? 2 : 1;
  if (event.type == InputManager::Mouse) {
    if (event.data.pointy.relx == 17)
      bits |= 4;
    if (event.data.pointy.rely == -9)
      bits |= 8;
    if (event.data.pointy.buttons[0])
      bits |= 16;
    else if (observed & 16)
      bits |= 32;
  }
  bits &= ~observed;
  observed |= bits;
  if (bits) {
    NOTICE("USB-SMOKE: PASS input-bits=" << Hex << static_cast<size_t>(bits)
                                         << " observed=" << static_cast<size_t>(observed));
    events.release();
  }
}
bool entry() {
#if CRIPPLE_HDD
  return fail("CRIPPLE_HDD must be disabled");
#else
  if (!runHostedUsbBotRegressions() || !runHostedHidReportRegressions())
    return fail("BOT or HID contract regressions");
  NOTICE("USB-SMOKE: PASS bot-hid-contracts");
  if (!storage())
    return false;
  InputManager::instance().installCallback(InputManager::RawKey | InputManager::Mouse, input);
  NOTICE("USB-SMOKE: READY input");
  bool complete = false;
  while (events.acquireForCompletion(1, 30)) {
    LockGuard<Mutex> lock(eventLock);
    if (observed == 63) {
      complete = true;
      break;
    }
  }
  InputManager::instance().removeCallback(input);
  if (!complete)
    return fail("keyboard or mouse events missing");
  NOTICE("USB-SMOKE: PASS complete");
  return true;
#endif
}
void exit() {}
}  // namespace
MODULE_INFO("usb-smoke", &entry, &exit, "usb-hid", "usb-mass-storage", "mountroot", "scsi");
