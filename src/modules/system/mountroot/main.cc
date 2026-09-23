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

#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/core/BootIO.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/Module.h"
#include "modules/system/lodisk/LoDisk.h"
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/Filesystem.h"
#include "modules/system/vfs/VFS.h"

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
#include "pedigree/kernel/processor/hosted/smoke.h"
#endif

class File;

static bool bRootMounted = false;

enum class RootSelectorKind { None, Uuid, Label };
static RootSelectorKind g_RootSelectorKind = RootSelectorKind::None;
static String g_RootSelectorValue;

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

static bool parseRootSelector() {
  char* commandLine = g_pBootstrapInfo->getCommandLine();
  if (!commandLine) {
    return false;
  }

  RootSelectorKind kind = RootSelectorKind::None;
  String value;
  Vector<String> arguments = String(commandLine).tokenise(' ');
  for (auto argument : arguments) {
    StringView view = argument.view();
    if (view.length() > 10 && view.substring(0, 10) == "root=UUID=") {
      kind = RootSelectorKind::Uuid;
      value = view.substring(10, view.length()).toString();
    } else if (view.length() > 11 && view.substring(0, 11) == "root=LABEL=") {
      if (kind != RootSelectorKind::Uuid) {
        kind = RootSelectorKind::Label;
        value = view.substring(11, view.length()).toString();
      }
    }
  }

  if (kind == RootSelectorKind::None || !value.length()) {
    return false;
  }

  value.strip();
  if (!value.length()) {
    return false;
  }

  g_RootSelectorKind = kind;
  g_RootSelectorValue = value;
  return true;
}

static bool isRootFilesystem(Filesystem* filesystem) {
  if (g_RootSelectorKind == RootSelectorKind::Uuid) {
    String uuid;
    return filesystem->getUuid(uuid) && uuid == g_RootSelectorValue;
  }
  if (g_RootSelectorKind == RootSelectorKind::Label) {
    return filesystem->getVolumeLabel() == g_RootSelectorValue;
  }
  return false;
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

    if (!bRootMounted && isRootFilesystem(pFs)) {
      NOTICE("Mounted " << stableName << " successfully as root.");
      VFS::instance().setRootFilesystem(pFs);
      bRootMounted = didMountAsRoot = true;
    }

    if (!didMountAsRoot) {
      NOTICE("Mounted " << stableName << " at /media/" << stableName << ".");
    }

    g_MountedFilesystems.pushBack(pFs);
  }

  return diskDevice;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
static bool installHostedProfileRoot() {
  auto root = UniquePointer<Filesystem>::adopt(new RamFs);
  if (!root || !root.get()->initialise(nullptr))
    return false;

  const char* directories[] = {"/etc", "/dev", "/dev/shm", "/run",  "/run/lock", "/run/sockets",
                               "/sys", "/var", "/var/run", "/proc", "/tmp"};
  for (const char* path : directories) {
    if (!root.get()->createDirectory(StringView(path), 0755))
      return false;
  }

  const struct {
    const char* path;
    const char* contents;
  } accounts[] = {{"/etc/passwd", "root:x:0:0:root:/:/bin/sh\n"},
                  {"/etc/group", "root:x:0:root\n"}};
  for (const auto& account : accounts) {
    if (!root.get()->createFile(StringView(account.path), 0644))
      return false;
    Directory::ChildLease lease;
    File* file = root.get()->findRetained(StringView(account.path), lease);
    const size_t length = StringLength(account.contents);
    if (!file || file->write(0, length, reinterpret_cast<uintptr_t>(account.contents)) != length)
      return false;
  }

  // Publish only the complete fixture so POSIX and users see the same boot namespace.
  if (!VFS::instance().setRootFilesystem(root.get()))
    return false;
  g_MountedFilesystems.pushBack(root.releaseOwnership());
  bRootMounted = true;
  return true;
}
#endif

static bool init() {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  const bool hostedProfile = hostedSyscallProfileRequested();
#else
  const bool hostedProfile = false;
#endif
  if (!hostedProfile && !parseRootSelector()) {
    error("No valid root=UUID= or root=LABEL= selector was supplied.");
    if (!HOSTED) {
      return false;
    }
  }

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

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (hostedProfile) {
    if (!installHostedProfileRoot()) {
      error("Unable to prepare the hosted syscall profiling root.");
      return false;
    }
    return true;
  }
#endif

  // Root selection must not hide later partitions, such as the UEFI ESP.
  // The first matching root wins; other filesystems remain available in /media.
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
  if (!bRootMounted) {
    error("No root disk matched the supplied root filesystem selector.");
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

  Vector<Filesystem*> ownedBackings;
  if (!VFS::instance().shutdownMountView(ownedBackings)) {
    panic("mountroot could not drain the filesystem namespace");
  }
  for (auto* filesystem : ownedBackings)
    g_MountedFilesystems.pushBack(filesystem);

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
    if (!VFS::instance().unregisterFilesystem(filesystem, true, true)) {
      panic("mountroot could not cleanly unmount a live-disk filesystem");
    }
    NOTICE("unmount done");
  }

  if (g_pLiveDisk) {
    Device::foreach (removeLiveDisk);
    if (g_pLiveDisk) {
      panic("mountroot could not retire its live-disk device");
    }
  }

  while (backingFilesystems.count()) {
    Filesystem* filesystem = backingFilesystems.popFront();
    NOTICE("Unmounting " << filesystem->getVolumeLabel() << " [" << Hex << filesystem << "]...");
    if (!VFS::instance().unregisterFilesystem(filesystem, true, true)) {
      panic("mountroot could not cleanly unmount a backing filesystem");
    }
    NOTICE("unmount done");
  }

  g_MountedFilesystems.clear();
  bRootMounted = false;

  NOTICE("Unmounting all filesystems has completed.");
}

MODULE_INFO_RUNTIME_PINNED("mountroot", &init, &destroy, "vfs", "partition", "rawfs", "ramfs");

// We expect the filesystems metamodule to fail, but by the time it does and
// we are allowed to continue, all the filesystems are loaded.
MODULE_OPTIONAL_DEPENDS("filesystems", "fat", "ext2", "iso9660", "lodisk", "diskimage");
