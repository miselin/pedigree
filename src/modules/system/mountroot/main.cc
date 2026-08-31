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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/core/BootIO.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/Module.h"
#include "modules/system/lodisk/LoDisk.h"
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/Filesystem.h"
#include "modules/system/vfs/VFS.h"

class File;

static bool bRootMounted = false;

static List<Filesystem*> g_MountedFilesystems;
static FileDisk* g_pLiveDisk = nullptr;

static void error(const char* s) {
  extern BootIO bootIO;
  static HugeStaticString str;
  str += s;
  str += "\n";
  bootIO.write(str, BootIO::Red, BootIO::Black);
  str.clear();
}

static Device* probeDisk(Device* diskDevice) {
  if (diskDevice->getType() != Device::Disk) {
    return diskDevice;
  }

  Disk* pDisk = static_cast<Disk*>(diskDevice);
  String stableName;
  Filesystem* pFs = nullptr;
  if (VFS::instance().mount(pDisk, stableName, &pFs)) {
    // For mount message
    bool didMountAsRoot = false;

    // Search for the root specifier, if we haven't already mounted root
    if (!bRootMounted) {
      File* f = pFs->find(String("/.pedigree-root"));
      if (f && !bRootMounted) {
        NOTICE("Mounted " << stableName << " successfully as root.");
        VFS::instance().setRootFilesystem(pFs);
        bRootMounted = didMountAsRoot = true;
      }
    }

    if (!didMountAsRoot) {
      NOTICE("Mounted " << stableName << " at /media/" << stableName << ".");
    }

    g_MountedFilesystems.pushBack(pFs);
  }

  return diskDevice;
}

static bool init() {
  // Mount scratch filesystem (ie, pure ram filesystem, for POSIX /tmp etc)
  RamFs* pRamFs = new RamFs;
  pRamFs->initialise(0);
  VFS::instance().registerFilesystem(pRamFs, String("scratch"));
  g_MountedFilesystems.pushBack(pRamFs);

  // Mount runtime filesystem.
  // The runtime filesystem assigns a Process ownership to each file, only
  // that process can modify/remove it. If the Process terminates without
  // removing the file, the file is not removed.
  RamFs* pRuntimeFs = new RamFs;
  pRuntimeFs->initialise(0);
  pRuntimeFs->setProcessOwnership(true);
  VFS::instance().registerFilesystem(pRuntimeFs, String("runtime"));
  g_MountedFilesystems.pushBack(pRuntimeFs);

  // Mount all available filesystems.
  Device::foreach (probeDisk);

  if (VFS::instance().getFilesystemAt(String("/media/raw")) == 0) {
    error("/media/raw does not exist - cannot continue startup.");
    return false;
  }

  // Are we running a live CD?
  /// \todo Use the configuration manager to determine if we're running a live
  /// CD or
  ///       not, to avoid the potential for conflicts here.
  if (VFS::instance().find(String("/livedisk.img"))) {
    NOTICE("trying to find live disk");
    FileDisk* pRamDisk = new FileDisk(String("/livedisk.img"), FileDisk::RamOnly);
    if (pRamDisk && pRamDisk->initialise()) {
      NOTICE("have a live disk");
      Device::addToRoot(pRamDisk);
      g_pLiveDisk = pRamDisk;

      // Mount it in the VFS
      VFS::instance().setRootFilesystem(nullptr);
      bRootMounted = false;
      NOTICE("probing ram disk for partitions");
      Device::foreach (probeDisk, pRamDisk);
    } else
      delete pRamDisk;
  }

  // Is there a root disk mounted?
  if (VFS::instance().find(String("/.pedigree-root")) == 0) {
    error("No root disk on this system (no /.pedigree-root found).");
    if (!HOSTED)  // hosted builds don't mount disks
    {
      return false;
    }
  }

  // All done, nothing more to do here.
  return true;
}

static Device* removeLiveDisk(Device* device) {
  if (device == g_pLiveDisk) {
    g_pLiveDisk = nullptr;
    return nullptr;
  }
  return device;
}

static bool isLiveDiskFilesystem(Filesystem* filesystem) {
  Device* device = filesystem->getDisk();
  while (device) {
    if (device == g_pLiveDisk) {
      return true;
    }
    device = device->getParent();
  }
  return false;
}

static void destroy() {
  NOTICE("Unmounting all filesystems...");

  List<Filesystem*> liveDiskFilesystems;
  List<Filesystem*> backingFilesystems;

  for (auto filesystem : g_MountedFilesystems) {
    if (g_pLiveDisk && isLiveDiskFilesystem(filesystem)) {
      liveDiskFilesystems.pushBack(filesystem);
    } else {
      backingFilesystems.pushBack(filesystem);
    }
  }

  // Filesystems on the live disk retain its partitions, while FileDisk
  // retains a File in the original backing filesystem.
  while (liveDiskFilesystems.count()) {
    Filesystem* filesystem = liveDiskFilesystems.popFront();
    NOTICE("Unmounting " << filesystem->getVolumeLabel() << " [" << Hex << filesystem << "]...");
    VFS::instance().unregisterFilesystem(filesystem);
    NOTICE("unmount done");
  }

  if (g_pLiveDisk) {
    Device::foreach (removeLiveDisk);
    if (g_pLiveDisk) {
      FATAL("mountroot could not retire its live-disk device");
      return;
    }
  }

  while (backingFilesystems.count()) {
    Filesystem* filesystem = backingFilesystems.popFront();
    NOTICE("Unmounting " << filesystem->getVolumeLabel() << " [" << Hex << filesystem << "]...");
    VFS::instance().unregisterFilesystem(filesystem);
    NOTICE("unmount done");
  }

  g_MountedFilesystems.clear();
  bRootMounted = false;

  NOTICE("Unmounting all filesystems has completed.");
}

MODULE_INFO_RUNTIME_PINNED("mountroot", &init, &destroy, "vfs", "partition", "rawfs", "ramfs");

// We expect the filesystems metamodule to fail, but by the time it does and
// we are allowed to continue, all the filesystems are loaded.
MODULE_OPTIONAL_DEPENDS("filesystems", "fat", "ext2", "iso9660", "lodisk");
