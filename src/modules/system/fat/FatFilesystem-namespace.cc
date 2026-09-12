/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/syscallError.h"

#include "FatFilesystem.h"
#include "FatSymlink.h"

class FatDirectory::NamespaceEdit {
 public:
  struct Portion {
    uint32_t cluster;
    size_t size;
    uint8_t* before;
    uint8_t* after;
  };
  struct Slot {
    Portion* portion;
    uint32_t offset;
    Dir* entry() const {
      return reinterpret_cast<Dir*>(portion->after + offset);
    }
  };
  struct Contents {
    FatDirectory* directory;
    Vector<Slot> slots;
    uint32_t tail = 0;
  };

  explicit NamespaceEdit(FatFilesystem* filesystem) : m_Filesystem(filesystem) {}
  ~NamespaceEdit() {
    for (Portion* portion : m_Portions) {
      delete[] portion->before;
      delete[] portion->after;
      delete portion;
    }
  }

  bool load(FatDirectory* directory, Contents& contents) {
    contents.directory = directory;
    uint32_t cluster = directory->getInode();
    for (size_t visited = 0; visited <= m_Filesystem->m_ClusterCount; ++visited) {
      if ((cluster == 0 && m_Filesystem->m_Type == FAT32) ||
          (cluster && (cluster < 2 || cluster >= m_Filesystem->m_ClusterCount + 2)))
        return fail();
      for (const Slot& slot : contents.slots) {
        if (!slot.offset && slot.portion->cluster == cluster)
          return fail();
      }
      Portion* portion = read(cluster);
      if (!portion)
        return false;
      append(contents, portion);
      if (!cluster && m_Filesystem->m_Type != FAT32)
        return true;
      const uint32_t next = m_Filesystem->getClusterEntry(cluster);
      if (m_Filesystem->isEof(next))
        return true;
      cluster = next;
    }
    return fail();
  }

  bool locate(Contents& contents, const String& name, File* file, size_t& index) {
    struct Identity {
      StringView name;
      File* file;
      uint32_t cluster = 0;
      uint32_t offset = 0;
      bool found = false;
    } identity{name.view(), file};
    auto match = [](void* opaque, const ScannedEntry& entry, uint64_t, uint64_t) {
      Identity& identity = *static_cast<Identity*>(opaque);
      if (entry.name != identity.name)
        return true;
      const uint32_t cluster = LITTLE_TO_HOST16(entry.entry.DIR_FstClusLO) |
                               (uint32_t(LITTLE_TO_HOST16(entry.entry.DIR_FstClusHI)) << 16);
      uint32_t expectedCluster = 0, expectedOffset = 0;
      if (identity.file->isDirectory()) {
        auto* file = static_cast<FatDirectory*>(identity.file);
        expectedCluster = file->getDirCluster();
        expectedOffset = file->getDirOffset();
      } else if (identity.file->isSymlink()) {
        auto* file = static_cast<FatSymlink*>(identity.file);
        expectedCluster = file->getDirCluster();
        expectedOffset = file->getDirOffset();
      } else {
        auto* file = static_cast<FatFile*>(identity.file);
        expectedCluster = file->getDirCluster();
        expectedOffset = file->getDirOffset();
      }
      identity.found = cluster == identity.file->getInode() &&
                       entry.directoryCluster == expectedCluster &&
                       entry.directoryOffset == expectedOffset;
      identity.cluster = entry.directoryCluster;
      identity.offset = entry.directoryOffset;
      return false;
    };
    uint64_t cookie = 0;
    const ReadStatus status = contents.directory->scanDirectory(cookie, match, &identity);
    if (!identity.found) {
      syscallError(status == ReadStatus::IoError ? Error::IoError : Error::DoesNotExist);
      return false;
    }
    for (index = 0; index < contents.slots.count(); ++index) {
      const Slot& slot = contents.slots[index];
      if (slot.portion->cluster == identity.cluster && slot.offset == identity.offset)
        return true;
    }
    return fail();
  }

  void erase(Contents& contents, size_t index) {
    Dir* entry = contents.slots[index].entry();
    const uint8_t checksum = nameChecksum(entry->DIR_Name);
    entry->DIR_Name[0] = 0xE5;
    for (size_t ordinal = 1; index && ordinal <= 20; ++ordinal) {
      DirLongFilename* previous =
          reinterpret_cast<DirLongFilename*>(contents.slots[--index].entry());
      if ((previous->LDIR_Attr & ATTR_LONG_NAME_MASK) != ATTR_LONG_NAME ||
          previous->LDIR_Chksum != checksum || (previous->LDIR_Ord & 0x1F) != ordinal ||
          previous->LDIR_Type || previous->LDIR_FstClusLO)
        break;
      const bool last = previous->LDIR_Ord & 0x40;
      previous->LDIR_Ord = 0xE5;
      if (last)
        break;
    }
  }

  bool insert(Contents& contents, const String& name, const Dir& metadata, Slot& result) {
    Vector<Dir> entries;
    if (!contents.directory->encodeEntrySet(name, metadata, entries))
      return false;
    if (!uniqueShortName(contents, entries))
      return false;
    size_t index = 0, freeCount = 0;
    bool pastEnd = false;
    while (true) {
      for (; index < contents.slots.count(); ++index) {
        const uint8_t first = contents.slots[index].entry()->DIR_Name[0];
        pastEnd = pastEnd || !first;
        freeCount = pastEnd || first == 0xE5 ? freeCount + 1 : 0;
        if (freeCount == entries.count()) {
          const size_t begin = index + 1 - entries.count();
          for (size_t i = 0; i < entries.count(); ++i)
            *contents.slots[begin + i].entry() = entries[i];
          if (pastEnd && index + 1 < contents.slots.count())
            contents.slots[index + 1].entry()->DIR_Name[0] = 0;
          result = contents.slots[index];
          return true;
        }
      }
      if (!grow(contents))
        return false;
    }
  }

  bool updateParent(Contents& contents, uint32_t oldParent, uint32_t newParent) {
    if (contents.slots.count() < 2)
      return fail();
    Dir* entry = contents.slots[1].entry();
    if (MemoryCompare(entry->DIR_Name, "..         ", 11) || !(entry->DIR_Attr & ATTR_DIRECTORY))
      return fail();
    const uint32_t recorded = LITTLE_TO_HOST16(entry->DIR_FstClusLO) |
                              (uint32_t(LITTLE_TO_HOST16(entry->DIR_FstClusHI)) << 16);
    const uint32_t root = m_Filesystem->m_pRoot->getInode();
    if (recorded != oldParent && !(oldParent == root && !recorded))
      return fail();
    if (newParent == root)
      newParent = 0;
    entry->DIR_FstClusLO = HOST_TO_LITTLE16(newParent & 0xFFFF);
    entry->DIR_FstClusHI = HOST_TO_LITTLE16(newParent >> 16);
    return true;
  }

  bool commit(const Contents* destination = nullptr) {
    Vector<Portion*> ordered;
    if (destination) {
      for (const Slot& slot : destination->slots) {
        if (!slot.offset)
          ordered.pushBack(slot.portion);
      }
    }
    for (Portion* portion : m_Portions) {
      bool found = false;
      for (Portion* existing : ordered)
        found = found || existing == portion;
      if (!found)
        ordered.pushBack(portion);
    }
    for (size_t i = 0; i < ordered.count(); ++i) {
      Portion* portion = ordered[i];
      if (!MemoryCompare(portion->before, portion->after, portion->size))
        continue;
      if (!m_Filesystem->writeDirectoryPortion(portion->cluster, portion->after)) {
        // Disk writeback may have accepted a prefix before reporting failure.
        // Restore every attempted portion, including that failed write's cache.
        for (size_t rollback = i + 1; rollback; --rollback) {
          Portion* original = ordered[rollback - 1];
          if (!m_Filesystem->writeDirectoryPortion(original->cluster, original->before)) {
            m_Filesystem->m_IoFailed = true;
            m_Filesystem->m_bReadOnly = true;
            ERROR("FAT namespace rollback failed; filesystem is now read-only");
          }
        }
        return fail();
      }
    }
    return true;
  }

 private:
  static bool fail() {
    SYSCALL_ERROR(IoError);
    return false;
  }
  static uint8_t nameChecksum(const uint8_t* name) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < 11; ++i)
      checksum = uint8_t(((checksum & 1) ? 0x80 : 0) + (checksum >> 1) + name[i]);
    return checksum;
  }
  Portion* read(uint32_t cluster) {
    for (Portion* portion : m_Portions) {
      if (portion->cluster == cluster)
        return portion;
    }
    const size_t size =
        cluster ? m_Filesystem->m_BlockSize
                : m_Filesystem->m_RootDirCount * m_Filesystem->m_Superblock.BPB_BytsPerSec;
    if (!size || size % sizeof(Dir)) {
      fail();
      return nullptr;
    }
    auto* before = static_cast<uint8_t*>(m_Filesystem->readDirectoryPortion(cluster));
    if (!before) {
      fail();
      return nullptr;
    }
    auto* after = new uint8_t[size];
    MemoryCopy(after, before, size);
    Portion* portion = new Portion{cluster, size, before, after};
    m_Portions.pushBack(portion);
    return portion;
  }
  void append(Contents& contents, Portion* portion) {
    for (size_t offset = 0; offset < portion->size; offset += sizeof(Dir))
      contents.slots.pushBack(Slot{portion, uint32_t(offset)});
    contents.tail = portion->cluster;
  }
  bool grow(Contents& contents) {
    if (!contents.tail) {
      SYSCALL_ERROR(NoSpaceLeftOnDevice);
      return false;
    }
    const uint32_t cluster = m_Filesystem->findFreeCluster();
    if (!cluster)
      return false;
    auto* zero = new uint8_t[m_Filesystem->m_BlockSize];
    ByteSet(zero, 0, m_Filesystem->m_BlockSize);
    const bool cleared = m_Filesystem->writeCluster(cluster, reinterpret_cast<uintptr_t>(zero));
    delete[] zero;
    if (!cleared) {
      m_Filesystem->releaseClusterChain(cluster, false);
      return fail();
    }
    const uint32_t oldTail = m_Filesystem->getClusterEntry(contents.tail);
    if (!m_Filesystem->setClusterEntry(contents.tail, cluster)) {
      if (m_Filesystem->setClusterEntry(contents.tail, oldTail)) {
        m_Filesystem->releaseClusterChain(cluster, false);
      } else {
        m_Filesystem->m_IoFailed = true;
        m_Filesystem->m_bReadOnly = true;
        ERROR("FAT directory extension rollback failed; filesystem is now read-only");
      }
      return fail();
    }
    // An empty extension is safe to retain if a later namespace write fails.
    Portion* portion = read(cluster);
    if (!portion)
      return false;
    append(contents, portion);
    return true;
  }
  bool uniqueShortName(const Contents& contents, Vector<Dir>& entries) {
    Dir& target = entries[entries.count() - 1];
    uint8_t original[11];
    MemoryCopy(original, target.DIR_Name, 11);
    for (uint32_t suffix = 0; suffix < 1000000; ++suffix) {
      bool occupied = false;
      for (const Slot& slot : contents.slots) {
        const Dir* entry = slot.entry();
        if (!entry->DIR_Name[0])
          break;
        if (entry->DIR_Name[0] != 0xE5 &&
            (entry->DIR_Attr & ATTR_LONG_NAME_MASK) != ATTR_LONG_NAME &&
            !MemoryCompare(entry->DIR_Name, target.DIR_Name, 11)) {
          occupied = true;
          break;
        }
      }
      if (!occupied) {
        const uint8_t checksum = nameChecksum(target.DIR_Name);
        for (size_t i = 0; i + 1 < entries.count(); ++i)
          reinterpret_cast<DirLongFilename*>(&entries[i])->LDIR_Chksum = checksum;
        return true;
      }
      char digits[7];
      size_t length = 0;
      for (uint32_t number = suffix + 1; number; number /= 10)
        digits[length++] = '0' + number % 10;
      MemoryCopy(target.DIR_Name, original, 11);
      const size_t prefix = 7 - length;
      for (size_t i = 0; i < prefix; ++i) {
        if (target.DIR_Name[i] == ' ')
          target.DIR_Name[i] = '_';
      }
      target.DIR_Name[prefix] = '~';
      for (size_t i = 0; i < length; ++i)
        target.DIR_Name[prefix + 1 + i] = digits[length - 1 - i];
    }
    SYSCALL_ERROR(NoSpaceLeftOnDevice);
    return false;
  }

  FatFilesystem* m_Filesystem;
  Vector<Portion*> m_Portions;
};

bool FatDirectory::addEntry(String filename, File* file, size_t type, bool publish) {
  const bool special = filename == "." || filename == "..";
  NameReservation reservation;
  if (!special && publish && !reserveDirectoryEntry(HashedStringView(file->getName()), reservation))
    return false;
  LockGuard<Mutex> guard(m_Lock);
  auto* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  LockGuard<Mutex> fileGuard(filesystem->m_FileMutationLock);
  if (filesystem->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  if (isDetached()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  struct ExistingName {
    String name;
    bool found = false;
  } existing{file->getName()};
  auto match = [](void* opaque, const ScannedEntry& entry, uint64_t, uint64_t) {
    auto& existing = *static_cast<ExistingName*>(opaque);
    existing.found = entry.name == existing.name;
    return !existing.found;
  };
  uint64_t cookie = 0;
  if (!special) {
    const ReadStatus status = scanDirectory(cookie, match, &existing);
    if (existing.found || status == ReadStatus::IoError) {
      syscallError(existing.found ? Error::FileExists : Error::IoError);
      return false;
    }
  }
  NamespaceEdit edit(filesystem);
  NamespaceEdit::Contents contents;
  if (!edit.load(this, contents))
    return false;
  Dir metadata = {};
  metadata.DIR_Attr = type ? ATTR_DIRECTORY : 0;
  uint32_t cluster = file->getInode();
  if (filename == ".." && filesystem->m_pRoot && cluster == filesystem->m_pRoot->getInode())
    cluster = 0;
  metadata.DIR_FstClusLO = HOST_TO_LITTLE16(cluster & 0xFFFF);
  metadata.DIR_FstClusHI = HOST_TO_LITTLE16(cluster >> 16);
  metadata.DIR_FileSize = HOST_TO_LITTLE32(type ? 0 : file->getSize());
  filesystem->writeEntryAttributes(file, &metadata, true);
  NamespaceEdit::Slot location;
  if (!edit.insert(contents, filename, metadata, location) || !edit.commit())
    return false;
  filesystem->moveNode(file, location.portion->cluster, location.offset);
  if (publish && !special) {
    const bool published = addCachedDirectoryEntry(reservation, file);
    assert(published);
    publishEvent(FileEvents::Created, file->getName().view(), file->isDirectory());
    reservation.complete(LookupStatus::Found);
  }
  return true;
}

bool FatDirectory::removeEntry(const String& name, File* file) {
  LockGuard<Mutex> guard(m_Lock);
  auto* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  {
    LockGuard<Mutex> fileGuard(filesystem->m_FileMutationLock);
    if (filesystem->isReadOnly()) {
      SYSCALL_ERROR(ReadOnlyFilesystem);
      return false;
    }
    NamespaceEdit edit(filesystem);
    NamespaceEdit::Contents contents;
    size_t index = 0;
    if (!edit.load(this, contents) || !edit.locate(contents, name, file, index))
      return false;
    edit.erase(contents, index);
    if (!edit.commit())
      return false;
    filesystem->unlinkNode(file);
  }
  invalidateDirectoryEntry(HashedStringView(name));
  return true;
}

bool FatFilesystem::renameNode(Directory* oldParent, const String& oldName, File* source,
                               Directory* newParent, const String& newName, File* replaced) {
  auto* oldDirectory = static_cast<FatDirectory*>(oldParent);
  auto* newDirectory = static_cast<FatDirectory*>(newParent);
  auto* moved = source->isDirectory() ? static_cast<FatDirectory*>(source) : nullptr;
  auto* victim =
      replaced && replaced->isDirectory() ? static_cast<FatDirectory*>(replaced) : nullptr;
  const bool oldFirst =
      reinterpret_cast<uintptr_t>(oldDirectory) < reinterpret_cast<uintptr_t>(newDirectory);
  FatDirectory* first = oldFirst ? oldDirectory : newDirectory;
  FatDirectory* second = oldFirst ? newDirectory : oldDirectory;
  LockGuard<Mutex> firstGuard(first->m_Lock);
  LockGuard<Mutex> secondGuard(second->m_Lock, second != first);
  LockGuard<Mutex> movedGuard(moved ? moved->m_Lock : first->m_Lock, moved != nullptr);
  LockGuard<Mutex> victimGuard(victim ? victim->m_Lock : first->m_Lock, victim != nullptr);
  LockGuard<Mutex> fileGuard(m_FileMutationLock);
  if (oldDirectory->isDetached() || newDirectory->isDetached() || (moved && moved->isDetached()) ||
      (victim && victim->isDetached())) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  using Edit = FatDirectory::NamespaceEdit;
  Edit edit(this);
  Edit::Contents oldContents, newContents, movedContents;
  Edit::Contents* destination = oldDirectory == newDirectory ? &oldContents : &newContents;
  if (!edit.load(oldDirectory, oldContents) ||
      (destination == &newContents && !edit.load(newDirectory, newContents)))
    return false;
  size_t oldIndex = 0, replacedIndex = 0;
  if (!edit.locate(oldContents, oldName, source, oldIndex) ||
      (replaced && !edit.locate(*destination, newName, replaced, replacedIndex)))
    return false;
  Dir metadata = *oldContents.slots[oldIndex].entry();
  metadata.DIR_FileSize = HOST_TO_LITTLE32(moved ? 0 : source->getSize());
  metadata.DIR_FstClusLO = HOST_TO_LITTLE16(source->getInode() & 0xFFFF);
  metadata.DIR_FstClusHI = HOST_TO_LITTLE16(source->getInode() >> 16);
  edit.erase(oldContents, oldIndex);
  if (replaced)
    edit.erase(*destination, replacedIndex);
  String diskName = newName;
  if (source->isSymlink())
    diskName += FatDirectory::symlinkSuffix();
  Edit::Slot location;
  if (!edit.insert(*destination, diskName, metadata, location))
    return false;
  if (moved && oldDirectory != newDirectory &&
      (!edit.load(moved, movedContents) ||
       !edit.updateParent(movedContents, oldDirectory->getInode(), newDirectory->getInode())))
    return false;
  if (!edit.commit(destination))
    return false;
  if (replaced)
    unlinkNode(replaced);
  moveNode(source, location.portion->cluster, location.offset);
  return true;
}
