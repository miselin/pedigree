/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_SYSFS_H
#define POSIX_SYSFS_H

#include "pedigree/kernel/Atomic.h"

#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/Filesystem.h"

class SysFsDirectory : public Directory {
 public:
  SysFsDirectory(const String& name, uintptr_t inode, Filesystem* filesystem, File* parent)
      : Directory(name, 0, 0, 0, inode, filesystem, 0, parent) {
    setPermissionsOnly(FILE_UR | FILE_UX | FILE_GR | FILE_GX | FILE_OR | FILE_OX);
  }

  void addEntry(const String& name, File* file) {
    addDirectoryEntry(name, file);
  }
};

class SysFs final : public Filesystem {
 public:
  SysFs() : m_Root(nullptr), m_NextInode(0) {}
  ~SysFs() override;

  bool initialise(Disk*) override;
  File* getRoot() const override {
    return m_Root;
  }
  const String& getVolumeLabel() const override;
  SyncStatus sync() override {
    return SyncStatus::Success;
  }

  uintptr_t allocateInode() {
    return (m_NextInode += 1) - 1;
  }

 protected:
  bool createFile(File*, const String&, uint32_t) override {
    return false;
  }
  bool createDirectory(File*, const String&, uint32_t) override {
    return false;
  }
  bool createSymlink(File*, const String&, const String&) override {
    return false;
  }
  bool removeNode(File*, const String&, File*) override {
    return false;
  }

 private:
  SysFsDirectory* directory(SysFsDirectory* parent, const char* name);
  void attribute(SysFsDirectory* parent, const char* name, const String& contents);
  void symlink(SysFsDirectory* parent, const char* name, const String& target);
  void addPciDevices(SysFsDirectory* devices);
  void addBlockDevices(SysFsDirectory* devices);
  void addNetworkDevices(SysFsDirectory* devices);

  SysFsDirectory* m_Root;
  Atomic<uintptr_t> m_NextInode;
};

#endif
