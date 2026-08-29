/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/fat/FatFilesystem.h"
#include "modules/system/vfs/VFS.h"

namespace {
class TrackingDisk final : public Disk {
 public:
  static constexpr size_t PageSize = TargetInfo::getPageSize();
  static constexpr size_t DataSize = 2 * PageSize;

  TrackingDisk()
      : m_ReadCount(0),
        m_WriteCount(0),
        m_UnpinCount(0),
        m_Data(new uint8_t[DataSize]),
        m_BalanceError(false) {
    ByteSet(m_Data, 0xA5, DataSize);
    ByteSet(m_ReadLocations, 0, sizeof(m_ReadLocations));
    ByteSet(m_WriteLocations, 0, sizeof(m_WriteLocations));
    ByteSet(m_UnpinLocations, 0, sizeof(m_UnpinLocations));
    ByteSet(m_PageReferences, 0, sizeof(m_PageReferences));
  }

  ~TrackingDisk() override {
    delete[] m_Data;
  }

  BufferView read(uint64_t location) override {
    if (location >= DataSize) {
      return BufferView();
    }

    if (m_ReadCount < 4) {
      m_ReadLocations[m_ReadCount] = location;
    }
    ++m_ReadCount;
    ++m_PageReferences[location / PageSize];
    return BufferView(m_Data + location, PageSize - (location % PageSize));
  }

  void write(uint64_t location) override {
    if (m_WriteCount < 4) {
      m_WriteLocations[m_WriteCount] = location;
    }
    ++m_WriteCount;
  }

  size_t getSize() const override {
    return DataSize;
  }

  size_t getBlockSize() const override {
    // Device I/O extent size must not change native-page ownership.
    return 65536;
  }

  bool pin(uint64_t location) override {
    if (location >= DataSize) {
      return false;
    }
    ++m_PageReferences[location / PageSize];
    return true;
  }

  void unpin(uint64_t location) override {
    if (m_UnpinCount < 4) {
      m_UnpinLocations[m_UnpinCount] = location;
    }
    ++m_UnpinCount;

    size_t& references = m_PageReferences[location / PageSize];
    if (!references) {
      m_BalanceError = true;
      return;
    }
    --references;
  }

  bool balanced() const {
    return !m_BalanceError && !m_PageReferences[0] && !m_PageReferences[1];
  }

  uint8_t* data() {
    return m_Data;
  }

  size_t m_ReadCount;
  size_t m_WriteCount;
  size_t m_UnpinCount;
  uint64_t m_ReadLocations[4];
  uint64_t m_WriteLocations[4];
  uint64_t m_UnpinLocations[4];

 private:
  uint8_t* m_Data;
  size_t m_PageReferences[2];
  bool m_BalanceError;
};

class FatFilesystemHarness : public FatFilesystem {
 public:
  void configure(Disk* disk) {
    m_pDisk = disk;
    m_Superblock.BPB_BytsPerSec = 512;
  }

  bool writeSectors(uint32_t sector, size_t size, uintptr_t source) {
    return writeSectorBlock(sector, size, source);
  }

  void configureRemoval(Disk* disk, uintptr_t fatTable) {
    m_pDisk = disk;
    m_Type = FAT16;
    m_BlockSize = 512;
    m_DataAreaStart = 8;
    m_FatSector = 4;
    m_Superblock.BPB_BytsPerSec = 512;
    m_Superblock.BPB_SecPerClus = 1;
    m_FatCache.insert(0, fatTable);
  }

  bool removeForTest(File* parent, File* file) {
    return remove(parent, file);
  }
};

class ShortReadFatFilesystem final : public FatFilesystem {
 public:
  ShortReadFatFilesystem() : m_ReadCalls(0) {}

  uint64_t read(File*, uint64_t, uint64_t size, uintptr_t buffer, bool = true) override {
    ++m_ReadCalls;
    ByteSet(reinterpret_cast<void*>(buffer), 0x5A, size);
    return m_ReadCalls == 1 ? size - 1 : size;
  }

  size_t m_ReadCalls;
};

class RemovalProbeFile final : public File {
 public:
  RemovalProbeFile(const String& name, Filesystem* filesystem, File* parent, size_t& destructions)
      : File(name, 0, 0, 0, 0, filesystem, 0, parent), m_Destructions(destructions) {}

  ~RemovalProbeFile() override {
    ++m_Destructions;
  }

 private:
  size_t& m_Destructions;
};

class RemovalDirectory final : public Directory {
 public:
  explicit RemovalDirectory(Filesystem* filesystem)
      : Directory(String("removal-root"), 0, 0, 0, 0, filesystem, 0, nullptr) {}

  void publish(const String& alias, File* file) {
    addDirectoryEntry(alias, file);
  }

  bool contains(const String& alias) const {
    return lookup(HashedStringView(alias)) != nullptr;
  }
};

class RemovalFilesystem final : public Filesystem {
 public:
  RemovalFilesystem(size_t& destructions, bool selfRemoving)
      : m_Root(nullptr),
        m_First(nullptr),
        m_Second(nullptr),
        m_Destructions(destructions),
        m_SelfRemoving(selfRemoving),
        m_SecondObservedPriorRetirement(true),
        m_RemoveCalls(0),
        m_FirstAlias("cache-first"),
        m_SecondAlias("cache-second"),
        m_Label("directory-removal-test") {}

  void configure(RemovalDirectory* root, File* first, File* second) {
    m_Root = root;
    m_First = first;
    m_Second = second;
  }

  bool initialise(Disk*) override {
    return true;
  }

  File* getRoot() const override {
    return m_Root;
  }

  const String& getVolumeLabel() const override {
    return m_Label;
  }

  bool remove(File* parent, File* file) override {
    ++m_RemoveCalls;
    if (m_RemoveCalls == 2 && m_Destructions != 1) {
      m_SecondObservedPriorRetirement = false;
    }

    if (m_SelfRemoving) {
      const String& alias = file == m_First ? m_FirstAlias : m_SecondAlias;
      Directory::fromFile(parent)->remove(HashedStringView(alias));
    }
    return true;
  }

  bool secondObservedPriorRetirement() const {
    return m_SecondObservedPriorRetirement;
  }

  size_t removeCalls() const {
    return m_RemoveCalls;
  }

  const String& firstAlias() const {
    return m_FirstAlias;
  }

  const String& secondAlias() const {
    return m_SecondAlias;
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

 private:
  RemovalDirectory* m_Root;
  File* m_First;
  File* m_Second;
  size_t& m_Destructions;
  bool m_SelfRemoving;
  bool m_SecondObservedPriorRetirement;
  size_t m_RemoveCalls;
  String m_FirstAlias;
  String m_SecondAlias;
  String m_Label;
};

class RemovalFatDirectory final : public FatDirectory {
 public:
  RemovalFatDirectory(FatFilesystem* filesystem, FatFileInfo& info)
      : FatDirectory(String("fat-removal-root"), 3, filesystem, nullptr, info),
        m_Alias("fat-cache-alias"),
        m_RemoveCalls(0) {}

  void publish(File* file) {
    addDirectoryEntry(m_Alias, file);
  }

  bool removeEntry(File* file) override {
    ++m_RemoveCalls;
    Directory::remove(HashedStringView(m_Alias));
    file->setInode(0);
    return true;
  }

  void removeAlias() {
    Directory::remove(HashedStringView(m_Alias));
  }

  bool containsAlias() const {
    return lookup(HashedStringView(m_Alias)) != nullptr;
  }

  size_t removeCalls() const {
    return m_RemoveCalls;
  }

 private:
  String m_Alias;
  size_t m_RemoveCalls;
};

class RetainedFatFile final : public FatFile {
 public:
  RetainedFatFile(FatFilesystem* filesystem, File* parent, size_t& destructions)
      : FatFile(String("fat-intrinsic-name"), 0, 0, 0, 2, filesystem, 0, 3, 0, parent),
        m_Destructions(destructions) {}

  ~RetainedFatFile() override {
    ++m_Destructions;
  }

 private:
  size_t& m_Destructions;
};

bool fatSectorPageBoundary() {
  constexpr size_t TransferSize = TrackingDisk::PageSize;
  uint8_t* source = new uint8_t[TransferSize];
  for (size_t i = 0; i < TransferSize; ++i) {
    source[i] = static_cast<uint8_t>((i * 37) ^ (i >> 3));
  }

  TrackingDisk disk;
  FatFilesystemHarness filesystem;
  filesystem.configure(&disk);

  const bool wrote = filesystem.writeSectors(1, TransferSize, reinterpret_cast<uintptr_t>(source));
  const bool bytesMatch = !MemoryCompare(disk.data() + 512, source, TransferSize);
  delete[] source;

  const bool splitAtPage =
      disk.m_ReadCount == 2 && disk.m_WriteCount == 2 && disk.m_ReadLocations[0] == 512 &&
      disk.m_ReadLocations[1] == TrackingDisk::PageSize && disk.m_WriteLocations[0] == 512 &&
      disk.m_WriteLocations[1] == TrackingDisk::PageSize;
  const bool balanced = disk.m_UnpinCount == 2 && disk.m_UnpinLocations[0] == 512 &&
                        disk.m_UnpinLocations[1] == TrackingDisk::PageSize && disk.balanced();

  const bool passed = wrote && bytesMatch && splitAtPage && balanced;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS fat-sector-page-boundary");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL fat-sector-page-boundary: "
        "unaligned transfer crossed an unowned cache page or leaked a "
        "read reference");
  }
  return passed;
}

bool fatShortReadPublication() {
  ShortReadFatFilesystem filesystem;
  FatFile file(String("short-read"), 0, 0, 0, 1, &filesystem, 4096);

  const uintptr_t failed = file.readBlock(0);
  const uintptr_t retried = file.readBlock(0);
  if (retried) {
    file.unpinBlock(0);
  }

  const bool passed = !failed && retried && filesystem.m_ReadCalls == 2;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS fat-short-read-publication");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL fat-short-read-publication: "
        "a short read was published or its failed Editing page blocked "
        "retry");
  }
  return passed;
}

bool directoryEmptyRemovalOwnership() {
  size_t destructions = 0;
  RemovalFilesystem filesystem(destructions, false);
  RemovalDirectory directory(&filesystem);
  RemovalProbeFile* first =
      new RemovalProbeFile(String("intrinsic-first"), &filesystem, &directory, destructions);
  RemovalProbeFile* second =
      new RemovalProbeFile(String("intrinsic-second"), &filesystem, &directory, destructions);
  filesystem.configure(&directory, first, second);
  directory.publish(filesystem.firstAlias(), first);
  directory.publish(filesystem.secondAlias(), second);

  const bool emptied = directory.empty();
  const bool retiresAsItGoes = emptied && filesystem.removeCalls() == 2 &&
                               filesystem.secondObservedPriorRetirement() && destructions == 2 &&
                               !directory.contains(filesystem.firstAlias()) &&
                               !directory.contains(filesystem.secondAlias());

  bool selfRemovingSafe = false;
  if (retiresAsItGoes) {
    size_t selfRemovingDestructions = 0;
    RemovalFilesystem selfRemovingFilesystem(selfRemovingDestructions, true);
    RemovalDirectory selfRemovingDirectory(&selfRemovingFilesystem);
    RemovalProbeFile* selfFirst =
        new RemovalProbeFile(String("self-intrinsic-first"), &selfRemovingFilesystem,
                             &selfRemovingDirectory, selfRemovingDestructions);
    RemovalProbeFile* selfSecond =
        new RemovalProbeFile(String("self-intrinsic-second"), &selfRemovingFilesystem,
                             &selfRemovingDirectory, selfRemovingDestructions);
    selfRemovingFilesystem.configure(&selfRemovingDirectory, selfFirst, selfSecond);
    selfRemovingDirectory.publish(selfRemovingFilesystem.firstAlias(), selfFirst);
    selfRemovingDirectory.publish(selfRemovingFilesystem.secondAlias(), selfSecond);

    selfRemovingSafe = selfRemovingDirectory.empty() && selfRemovingFilesystem.removeCalls() == 2 &&
                       selfRemovingDestructions == 2 &&
                       !selfRemovingDirectory.contains(selfRemovingFilesystem.firstAlias()) &&
                       !selfRemovingDirectory.contains(selfRemovingFilesystem.secondAlias());
  }

  const bool passed = retiresAsItGoes && selfRemovingSafe;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS directory-empty-removal-ownership");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL directory-empty-removal-ownership: "
        "removed entries remained alive or were retired twice");
  }
  return passed;
}

bool fatRemoveRetirementOrder() {
  TrackingDisk disk;
  uint32_t fatTable[256] = {};
  fatTable[1] = 0xFFFF;

  FatFilesystemHarness filesystem;
  filesystem.configureRemoval(&disk, reinterpret_cast<uintptr_t>(fatTable));

  FatFileInfo info = {};
  RemovalFatDirectory parent(&filesystem, info);
  size_t destructions = 0;
  RetainedFatFile* child = new RetainedFatFile(&filesystem, &parent, destructions);
  parent.publish(child);

  const bool retained = VFS::instance().retainTrackedFile(child);
  const bool removed = retained && filesystem.removeForTest(&parent, child);
  const bool operationPassed =
      removed && parent.removeCalls() == 1 && !destructions && !parent.containsAlias() &&
      fatTable[1] == 0 && disk.m_ReadCount == 1 && disk.m_WriteCount == 1 &&
      disk.m_UnpinCount == 1 && disk.m_ReadLocations[0] == 2048 &&
      disk.m_WriteLocations[0] == 2048 && disk.m_UnpinLocations[0] == 2048 && disk.balanced();

  bool emergencyWasFinal = false;
  if (retained) {
    emergencyWasFinal = VFS::instance().untrackFile(child, false);
    if (emergencyWasFinal) {
      delete child;
    } else if (parent.containsAlias()) {
      parent.removeAlias();
    }
  } else {
    parent.removeAlias();
  }

  const bool passed = operationPassed && emergencyWasFinal && destructions == 1;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS fat-remove-retirement-order");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL fat-remove-retirement-order: "
        "FAT removal read a child after retiring its final cache owner");
  }
  return passed;
}
}  // namespace

EXPORTED_PUBLIC bool runHostedFatSectorRegressions() {
  bool passed = true;
  passed &= fatSectorPageBoundary();
  passed &= fatShortReadPublication();
  passed &= directoryEmptyRemovalOwnership();
  passed &= fatRemoveRetirementOrder();
  return passed;
}
