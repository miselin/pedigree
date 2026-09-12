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

#include "FatDirectory.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/PointerGuard.h"
#include "pedigree/kernel/utilities/utility.h"

#include "FatFile.h"
#include "FatFilesystem.h"
#include "FatSymlink.h"
#include "fat.h"
#include "modules/system/vfs/File.h"

class Filesystem;

namespace {
constexpr size_t MaxLongFilenameEntries = 20;
constexpr size_t LongFilenameCharactersPerEntry = 13;
constexpr size_t LongFilenameStorageCharacters =
    MaxLongFilenameEntries * LongFilenameCharactersPerEntry;
constexpr size_t MaxLongFilenameCharacters = 255;

uint8_t shortFilenameChecksum(const uint8_t* name) {
  uint8_t checksum = 0;
  for (size_t i = 0; i < 11; ++i)
    checksum = static_cast<uint8_t>(((checksum & 1) ? 0x80 : 0) + (checksum >> 1) + name[i]);
  return checksum;
}

void writeLongFilenameCharacter(uint8_t* entry, size_t character, uint16_t value) {
  static const uint8_t offsets[LongFilenameCharactersPerEntry] = {1,  3,  5,  7,  9,  14, 16,
                                                                  18, 20, 22, 24, 28, 30};
  const size_t offset = offsets[character];
  entry[offset] = value & 0xFF;
  entry[offset + 1] = value >> 8;
}

bool encodeLongFilename(const String& filename, uint16_t* characters, size_t& characterCount) {
  characterCount = 0;
  for (size_t i = 0; i < filename.length();) {
    const uint8_t first = static_cast<uint8_t>(filename[i]);
    uint32_t character = 0;
    size_t sequenceLength = 0;
    if (first < 0x80) {
      character = first;
      sequenceLength = 1;
    } else if (first >= 0xC2 && first <= 0xDF) {
      character = first & 0x1F;
      sequenceLength = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
      character = first & 0x0F;
      sequenceLength = 3;
    } else if (first >= 0xF0 && first <= 0xF4) {
      character = first & 0x07;
      sequenceLength = 4;
    } else {
      return false;
    }

    if ((i + sequenceLength) > filename.length())
      return false;
    for (size_t continuation = 1; continuation < sequenceLength; ++continuation) {
      const uint8_t value = static_cast<uint8_t>(filename[i + continuation]);
      if ((value & 0xC0) != 0x80)
        return false;
      character = (character << 6) | (value & 0x3F);
    }

    if ((sequenceLength == 3 && character < 0x800) ||
        (sequenceLength == 4 && character < 0x10000) || character > 0x10FFFF ||
        (character >= 0xD800 && character <= 0xDFFF)) {
      return false;
    }

    if (character <= 0xFFFF) {
      if (characterCount >= MaxLongFilenameCharacters)
        return false;
      characters[characterCount++] = character;
    } else {
      if ((characterCount + 2) > MaxLongFilenameCharacters)
        return false;
      character -= 0x10000;
      characters[characterCount++] = 0xD800 | (character >> 10);
      characters[characterCount++] = 0xDC00 | (character & 0x3FF);
    }
    i += sequenceLength;
  }
  return true;
}
}  // namespace

FatDirectory::FatDirectory(String name, uintptr_t inode_num, FatFilesystem* pFs, File* pParent,
                           FatFileInfo& info, uint32_t dirClus, uint32_t dirOffset)
    : Directory(name, LITTLE_TO_HOST32(info.accessedTime), LITTLE_TO_HOST32(info.modifiedTime),
                LITTLE_TO_HOST32(info.creationTime), inode_num, static_cast<Filesystem*>(pFs),
                LITTLE_TO_HOST32(0), pParent),
      m_DirClus(dirClus),
      m_DirOffset(dirOffset),
      m_Type(FAT16),
      m_BlockSize(0),
      m_bRootDir(false),
      m_Lock(),
      m_DirBlockSize(0) {
  uint32_t permissions = 0777;  /// \todo Permissions

  setPermissions(permissions);
  setUid(LITTLE_TO_HOST16(0));  /// \todo Ownership of files
  setGid(LITTLE_TO_HOST16(0));

  m_BlockSize = pFs->m_BlockSize;
  m_Type = pFs->m_Type;

  setInode(inode_num);
}

FatDirectory::~FatDirectory() {}

File::Attributes FatDirectory::getAttributes() const {
  FatFilesystem* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  LockGuard<Mutex> guard(filesystem->m_FileMutationLock);
  Attributes attributes = File::getAttributes();
  attributes.blocks = filesystem->allocatedBlocks(const_cast<FatDirectory*>(this));
  return attributes;
}

void FatDirectory::setInode(uintptr_t inode) {
  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);
  m_Inode = inode;
  uintptr_t clus = m_Inode;

  m_DirBlockSize = m_BlockSize;

  m_bRootDir = false;
  if (clus == 0 && m_Type != FAT32) {
    m_DirBlockSize = pFs->m_RootDirCount * pFs->m_Superblock.BPB_BytsPerSec;
    m_bRootDir = true;
  } else if (pFs->m_Type == FAT32)
    if (clus == pFs->m_Superblock32.BPB_RootClus)
      m_bRootDir = true;
}

bool FatDirectory::addEntry(String filename, File* pFile, size_t type, bool publish) {
  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);

#if SUPERDEBUG
  NOTICE("FatDirectory::addEntry(" << filename << ")");
#endif
  NameReservation reservation;
  if (!reserveDirectoryEntry(HashedStringView(filename), reservation)) {
    return false;
  }
  LockGuard<Mutex> guard(m_Lock);
  LockGuard<Mutex> fileGuard(pFs->m_FileMutationLock);

  struct ExistingEntryContext {
    StringView name;
    bool found;
  };
  const String entryName = filename;
  ExistingEntryContext existing = {entryName.view(), false};
  auto findExisting = [](void* opaque, const ScannedEntry& entry, uint64_t, uint64_t) -> bool {
    ExistingEntryContext* context = reinterpret_cast<ExistingEntryContext*>(opaque);
    if (entry.name == context->name) {
      context->found = true;
      return false;
    }
    return true;
  };
  uint64_t scanCookie = 0;
  const ReadStatus scanStatus = scanDirectory(scanCookie, findExisting, &existing);
  if (existing.found || scanStatus == ReadStatus::IoError)
    return false;

  // grab the first cluster of the parent directory
  uint32_t clus = m_Inode;
  uint8_t* buffer = reinterpret_cast<uint8_t*>(pFs->readDirectoryPortion(clus));
  PointerGuard<uint8_t> bufferGuard(buffer, true);
  if (!buffer)
    return false;
  const size_t firstPortionSize = clus == 0 && m_Type != FAT32 ? m_DirBlockSize : m_BlockSize;

  // how many long filename entries does the filename require?
  size_t numRequired = 1;  // need *at least* one for the short filename entry
  size_t numLongEntries = 0;
  uint16_t longFilenameCharacters[LongFilenameStorageCharacters];
  size_t longFilenameCharacterCount = 0;

  // Dot and DotDot entries?
  if (!StringCompareN(static_cast<const char*>(filename), ".", filename.length()) ||
      !StringCompareN(static_cast<const char*>(filename), "..", filename.length())) {
    // Only one entry required for the dot/dotdot entries, all else get
    // a free long filename entry.
  } else {
    if (!encodeLongFilename(filename, longFilenameCharacters, longFilenameCharacterCount))
      return false;
    numLongEntries = (longFilenameCharacterCount + LongFilenameCharactersPerEntry - 1) /
                     LongFilenameCharactersPerEntry;
    if (numLongEntries > MaxLongFilenameEntries)
      return false;
    numRequired += numLongEntries;
  }

  const String shortFilename = pFs->convertFilenameTo(filename);
  if (shortFilename.length() < 11)
    return false;
  uint8_t shortName[11];
  MemoryCopy(shortName, shortFilename.cstr(), sizeof(shortName));
  const uint8_t checksum = shortFilenameChecksum(shortName);

  // find the first free element
  bool spaceFound = false;
  size_t offset = 0;
  size_t consecutiveFree = 0;
  while (true) {
    // Look for space in the directory
    /// \todo Some (sets of) entries will need to cross a cluster boundary
    while (!spaceFound) {
      consecutiveFree = 0;
      const size_t portionSize = clus == 0 && m_Type != FAT32 ? firstPortionSize : m_BlockSize;
      for (offset = 0; offset < portionSize; offset += sizeof(Dir)) {
        if (buffer[offset] == 0 || buffer[offset] == 0xE5) {
          consecutiveFree++;
        } else
          consecutiveFree = 0;

        if (consecutiveFree == numRequired) {
          spaceFound = true;
          break;
        }
      }

      // If space was found, quit
      if (spaceFound)
        break;

      // Root Directory check:
      // If no space found for our file, and if not FAT32, the root
      // directory is not resizeable so we have to fail
      if (m_Type != FAT32 && clus == 0)
        return false;

      // check the next cluster, add a new cluster if needed
      uint32_t prev = clus;
      clus = pFs->getClusterEntry(clus);
      if (!clus)
        return false;

      if (pFs->isEof(clus)) {
        uint32_t newClus = pFs->findFreeCluster();
        if (!newClus)
          return false;

        if (!pFs->setClusterEntry(prev, newClus))
          return false;

        clus = newClus;
      }

      if (!pFs->readCluster(clus, reinterpret_cast<uintptr_t>(buffer)))
        return false;
    }

    {
      // long filename entries first
      if (numLongEntries) {
        size_t currOffset = offset - (numLongEntries * sizeof(Dir));
        for (size_t i = 0; i < numLongEntries; ++i) {
          // grab a pointer to the data
          DirLongFilename* lfn = reinterpret_cast<DirLongFilename*>(&buffer[currOffset]);
          ByteSet(lfn, 0xFF, sizeof(DirLongFilename));

          const size_t ordinal = numLongEntries - i;
          lfn->LDIR_Ord = ordinal | (i == 0 ? 0x40 : 0);
          lfn->LDIR_Attr = ATTR_LONG_NAME;
          lfn->LDIR_Type = 0;
          lfn->LDIR_Chksum = checksum;
          lfn->LDIR_FstClusLO = 0;

          const size_t filenameOffset = (ordinal - 1) * LongFilenameCharactersPerEntry;
          for (size_t character = 0; character < LongFilenameCharactersPerEntry; ++character) {
            const size_t index = filenameOffset + character;
            uint16_t value = 0xFFFF;
            if (index < longFilenameCharacterCount)
              value = longFilenameCharacters[index];
            else if (index == longFilenameCharacterCount)
              value = 0;
            writeLongFilenameCharacter(reinterpret_cast<uint8_t*>(lfn), character, value);
          }

          currOffset += sizeof(Dir);
        }
      }

      // get a Dir struct for it so we can manipulate the data
      Dir* ent = reinterpret_cast<Dir*>(&buffer[offset]);
      ByteSet(ent, 0, sizeof(Dir));
      ent->DIR_Attr = type ? ATTR_DIRECTORY : 0;

      MemoryCopy(ent->DIR_Name, shortName, sizeof(shortName));
      ent->DIR_FstClusLO = pFile->getInode() & 0xFFFF;
      ent->DIR_FstClusHI = (pFile->getInode() >> 16) & 0xFFFF;
      ent->DIR_FileSize = HOST_TO_LITTLE32(pFile->getSize());
      /// \todo Fill in other fields (eg, timestamps)

      if (!pFs->writeDirectoryPortion(clus, buffer))
        return false;

      if (pFile->isDirectory()) {
        FatDirectory* fatDir = static_cast<FatDirectory*>(pFile);

        fatDir->setDirCluster(clus);
        fatDir->setDirOffset(offset);
      } else if (pFile->isSymlink()) {
        FatSymlink* fatLink = static_cast<FatSymlink*>(pFile);

        fatLink->setDirCluster(clus);
        fatLink->setDirOffset(offset);
      } else {
        FatFile* fatFile = static_cast<FatFile*>(pFile);

        fatFile->setDirCluster(clus);
        fatFile->setDirOffset(offset);
      }

      // The on-disk name can differ for FAT symlinks, so cache the VFS name.
      const bool special = filename.compare(".") || filename.compare("..");
      if (publish && !special) {
        const bool published = addCachedDirectoryEntry(reservation, pFile);
        assert(published);
        publishEvent(FileEvents::Created, pFile->getName().view(), pFile->isDirectory());
        reservation.complete(LookupStatus::Found);
      }

#if SUPERDEBUG
      NOTICE("  -> FatFilesystem::addEntry(" << filename << ") is successful");
#endif

      return true;
    }
  }
#if SUPERDEBUG
  NOTICE("  -> FatFilesystem::addEntry(" << filename << ") is not successful");
#endif

  return false;
}

bool FatDirectory::removeEntry(const String& namespaceName, File* pFile) {
  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);
  FatDirectory* fatDir = static_cast<FatDirectory*>(pFile);
  FatFile* fatFile = static_cast<FatFile*>(pFile);
  FatSymlink* fatLink = static_cast<FatSymlink*>(pFile);
  String filename = namespaceName;
  const String real_filename(namespaceName);

  // Adjust filename if we must.
  if (pFile->isSymlink())
    filename += symlinkSuffix();

  uint32_t dirClus, dirOffset;
  if (pFile->isDirectory()) {
    dirClus = fatDir->getDirCluster();
    dirOffset = fatDir->getDirOffset();
  } else if (pFile->isSymlink()) {
    dirClus = fatLink->getDirCluster();
    dirOffset = fatLink->getDirOffset();
  } else {
    dirClus = fatFile->getDirCluster();
    dirOffset = fatFile->getDirOffset();
  }

  LockGuard<Mutex> guard(m_Lock);
  LockGuard<Mutex> fileGuard(pFs->m_FileMutationLock);

  struct RemovalIdentity {
    StringView name;
    uint32_t directoryCluster;
    uint32_t directoryOffset;
    uintptr_t inode;
    bool matched;
  } identity = {real_filename.view(), dirClus, dirOffset, pFile->getInode(), false};
  auto matchIdentity = [](void* opaque, const ScannedEntry& entry, uint64_t, uint64_t) -> bool {
    RemovalIdentity* identity = reinterpret_cast<RemovalIdentity*>(opaque);
    if (entry.name != identity->name) {
      return true;
    }
    const uintptr_t inode =
        LITTLE_TO_HOST16(entry.entry.DIR_FstClusLO) |
        (static_cast<uintptr_t>(LITTLE_TO_HOST16(entry.entry.DIR_FstClusHI)) << 16);
    identity->matched = entry.directoryCluster == identity->directoryCluster &&
                        entry.directoryOffset == identity->directoryOffset &&
                        inode == identity->inode;
    return false;
  };
  uint64_t cookie = 0;
  const ReadStatus identityStatus = scanDirectory(cookie, matchIdentity, &identity);
  if (!identity.matched) {
    if (identityStatus == ReadStatus::IoError) {
      SYSCALL_ERROR(IoError);
    } else {
      SYSCALL_ERROR(DoesNotExist);
    }
    return false;
  }

  // First byte = 0xE5 means the file's been deleted.
  Dir* dir = pFs->getDirectoryEntry(dirClus, dirOffset);
  PointerGuard<Dir> dirGuard(dir);
  if (!dir)
    return false;
  const uint8_t lfnChecksum = shortFilenameChecksum(dir->DIR_Name);
  dir->DIR_Name[0] = 0xE5;

  // The short entry is the namespace commit point. Once it is deleted, stale
  // or partially corrupt LFN metadata must not make unlink report that the
  // still-cached name survived.
  if (!pFs->writeDirectoryEntry(dir, dirClus, dirOffset))
    return false;
  invalidateDirectoryEntry(HashedStringView(real_filename));

  // Check that we can actually use the previous directory entry!
  if (dirOffset >= sizeof(Dir)) {
    // The main entry is fixed, but there may be one or more long filename
    // entries for this file...
    uint16_t longFilenameCharacters[LongFilenameStorageCharacters];
    size_t longFilenameCharacterCount = 0;
    if (!encodeLongFilename(filename, longFilenameCharacters, longFilenameCharacterCount)) {
      ERROR("Unable to identify FAT LFN entries after deleting the short entry");
      return true;
    }
    const size_t numLfnEntries = (longFilenameCharacterCount + LongFilenameCharactersPerEntry - 1) /
                                 LongFilenameCharactersPerEntry;

    // Grab the first entry behind this one - check that it is in fact a LFN
    // entry
    Dir* dir_prev = pFs->getDirectoryEntry(dirClus, dirOffset - sizeof(Dir));
    PointerGuard<Dir> prevGuard(dir_prev);
    if (!dir_prev) {
      ERROR("Unable to inspect FAT LFN entries after deleting the short entry");
      return true;
    }

    if ((dir_prev->DIR_Attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME) {
      // Previous entry is a long filename entry, so delete each entry
      // This goes backwards up the list of LFN entries - if it finds an
      // entry that does *not* match a standard LFN entry it'll dump a
      // warning and break out.
      for (size_t ent = 0; ent < numLfnEntries; ent++) {
        uint32_t bytesBack = sizeof(Dir) * (ent + 1);
        if (bytesBack > dirOffset) {
          /// \todo Save the previous cluster in FatFile too
          ERROR("LFN set crosses a cluster boundary!");
          break;
        }

        uint32_t newOffset = dirOffset - bytesBack;
        Dir* lfn = pFs->getDirectoryEntry(dirClus, newOffset);
        PointerGuard<Dir> lfnGuard(lfn);
        const uint8_t expectedOrdinal = static_cast<uint8_t>(ent + 1);
        const uint8_t expectedFlags =
            static_cast<uint8_t>(expectedOrdinal | ((ent + 1 == numLfnEntries) ? 0x40 : 0));
        const DirLongFilename* longEntry = reinterpret_cast<const DirLongFilename*>(lfn);
        if ((!lfn) || ((lfn->DIR_Attr & ATTR_LONG_NAME_MASK) != ATTR_LONG_NAME) ||
            longEntry->LDIR_Ord != expectedFlags || longEntry->LDIR_Chksum != lfnChecksum ||
            longEntry->LDIR_Type || longEntry->LDIR_FstClusLO) {
          ERROR("Invalid FAT LFN chain left behind after deleting the short entry");
          break;
        }

        lfn->DIR_Name[0] = 0xE5;

        if (!pFs->writeDirectoryEntry(lfn, dirClus, newOffset)) {
          ERROR("Unable to reclaim a FAT LFN entry after deleting the short entry");
          break;
        }
      }
    }
  }

  return true;
}

bool FatDirectory::renameEntry(const String& oldName, File* pFile, const String& newName) {
  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);
  uint32_t dirClus;
  uint32_t dirOffset;
  if (pFile->isDirectory()) {
    FatDirectory* fatDir = static_cast<FatDirectory*>(pFile);
    dirClus = fatDir->getDirCluster();
    dirOffset = fatDir->getDirOffset();
  } else if (pFile->isSymlink()) {
    FatSymlink* fatLink = static_cast<FatSymlink*>(pFile);
    dirClus = fatLink->getDirCluster();
    dirOffset = fatLink->getDirOffset();
  } else {
    FatFile* fatFile = static_cast<FatFile*>(pFile);
    dirClus = fatFile->getDirCluster();
    dirOffset = fatFile->getDirOffset();
  }
  if (dirOffset < sizeof(Dir)) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }

  uint16_t characters[LongFilenameStorageCharacters];
  size_t characterCount = 0;
  if (!encodeLongFilename(newName, characters, characterCount))
    return false;
  const size_t newLongEntries =
      (characterCount + LongFilenameCharactersPerEntry - 1) / LongFilenameCharactersPerEntry;
  if (!newLongEntries || newLongEntries > MaxLongFilenameEntries ||
      newLongEntries * sizeof(Dir) > dirOffset) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }

  LockGuard<Mutex> guard(m_Lock);
  LockGuard<Mutex> fileGuard(pFs->m_FileMutationLock);
  uint8_t* buffer = reinterpret_cast<uint8_t*>(pFs->readDirectoryPortion(dirClus));
  PointerGuard<uint8_t> bufferGuard(buffer, true);
  if (!buffer)
    return false;

  Dir* shortEntry = reinterpret_cast<Dir*>(buffer + dirOffset);
  const uint8_t oldChecksum = shortFilenameChecksum(shortEntry->DIR_Name);
  size_t oldLongEntries = 0;
  while (oldLongEntries < MaxLongFilenameEntries &&
         (oldLongEntries + 1) * sizeof(Dir) <= dirOffset) {
    DirLongFilename* entry =
        reinterpret_cast<DirLongFilename*>(buffer + dirOffset - (oldLongEntries + 1) * sizeof(Dir));
    if ((entry->LDIR_Attr & ATTR_LONG_NAME_MASK) != ATTR_LONG_NAME ||
        entry->LDIR_Chksum != oldChecksum)
      break;
    ++oldLongEntries;
  }
  if (oldLongEntries != newLongEntries) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }

  const String shortFilename = pFs->convertFilenameTo(newName);
  if (shortFilename.length() < 11)
    return false;
  uint8_t shortName[11];
  MemoryCopy(shortName, shortFilename.cstr(), sizeof(shortName));
  const uint8_t checksum = shortFilenameChecksum(shortName);
  for (size_t i = 0; i < newLongEntries; ++i) {
    DirLongFilename* entry =
        reinterpret_cast<DirLongFilename*>(buffer + dirOffset - (newLongEntries - i) * sizeof(Dir));
    ByteSet(entry, 0xFF, sizeof(DirLongFilename));
    const size_t ordinal = newLongEntries - i;
    entry->LDIR_Ord = ordinal | (i == 0 ? 0x40 : 0);
    entry->LDIR_Attr = ATTR_LONG_NAME;
    entry->LDIR_Chksum = checksum;
    const size_t filenameOffset = (ordinal - 1) * LongFilenameCharactersPerEntry;
    for (size_t character = 0; character < LongFilenameCharactersPerEntry; ++character) {
      const size_t index = filenameOffset + character;
      const uint16_t value = index < characterCount    ? characters[index]
                             : index == characterCount ? 0
                                                       : 0xFFFF;
      writeLongFilenameCharacter(reinterpret_cast<uint8_t*>(entry), character, value);
    }
  }
  MemoryCopy(shortEntry->DIR_Name, shortName, sizeof(shortName));
  if (!pFs->writeDirectoryPortion(dirClus, buffer))
    return false;
  return true;
}

namespace {
struct LongFilenameState {
  uint16_t characters[LongFilenameStorageCharacters];
  uint8_t expectedOrdinal;
  uint8_t entryCount;
  uint8_t checksum;
  bool valid;
};

void resetLongFilename(LongFilenameState& state) {
  ByteSet(state.characters, 0xFF, sizeof(state.characters));
  state.expectedOrdinal = 0;
  state.entryCount = 0;
  state.checksum = 0;
  state.valid = false;
}

uint16_t readLongFilenameCharacter(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

bool consumeLongFilenameEntry(LongFilenameState& state, const DirLongFilename& entry) {
  const uint8_t ordinal = entry.LDIR_Ord & 0x1F;
  const bool last = (entry.LDIR_Ord & 0x40) != 0;
  if (entry.LDIR_Ord & 0xA0 || !ordinal || ordinal > MaxLongFilenameEntries || entry.LDIR_Type ||
      entry.LDIR_FstClusLO) {
    resetLongFilename(state);
    return false;
  }

  if (last) {
    resetLongFilename(state);
    state.valid = true;
    state.expectedOrdinal = ordinal;
    state.entryCount = ordinal;
    state.checksum = entry.LDIR_Chksum;
  }

  if (!state.valid || state.expectedOrdinal != ordinal || state.checksum != entry.LDIR_Chksum) {
    resetLongFilename(state);
    return false;
  }

  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&entry);
  size_t character = (ordinal - 1) * LongFilenameCharactersPerEntry;
  for (size_t offset = 1; offset < 11; offset += 2)
    state.characters[character++] = readLongFilenameCharacter(raw + offset);
  for (size_t offset = 14; offset < 26; offset += 2)
    state.characters[character++] = readLongFilenameCharacter(raw + offset);
  for (size_t offset = 28; offset < 32; offset += 2)
    state.characters[character++] = readLongFilenameCharacter(raw + offset);

  state.expectedOrdinal = ordinal - 1;
  return true;
}

String longFilename(const LongFilenameState& state) {
  String result;
  result.reserve((state.entryCount * LongFilenameCharactersPerEntry * 3) + 1);
  const size_t characterCount =
      pedigree_std::min(static_cast<size_t>(state.entryCount) * LongFilenameCharactersPerEntry,
                        MaxLongFilenameCharacters);
  for (size_t i = 0; i < characterCount; ++i) {
    uint32_t character = state.characters[i];
    if (!character || character == 0xFFFF)
      break;

    if (character >= 0xD800 && character <= 0xDBFF) {
      if ((i + 1) < characterCount && state.characters[i + 1] >= 0xDC00 &&
          state.characters[i + 1] <= 0xDFFF) {
        character = 0x10000 + ((character - 0xD800) << 10) + (state.characters[++i] - 0xDC00);
      } else {
        character = '?';
      }
    } else if (character >= 0xDC00 && character <= 0xDFFF) {
      character = '?';
    }

    char utf8[5] = {};
    size_t length = String::Utf32ToUtf8(character, utf8);
    if (!length) {
      utf8[0] = '?';
      length = 1;
    }
    result += String(utf8, length, true);
  }
  return result;
}

uint64_t directoryCookie(uint32_t cluster, uint32_t offset) {
  return ((static_cast<uint64_t>(cluster) << 32) | offset) + 1;
}
}  // namespace

void FatDirectory::cacheDirectoryContents() {}

Directory::ReadStatus FatDirectory::scanDirectory(uint64_t& cookie, ScanEmitter emitter,
                                                  void* context) {
  if (!emitter)
    return ReadStatus::IoError;

  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);
  const uint64_t startingCookie = cookie;
  uint32_t requestedCluster = static_cast<uint32_t>(m_Inode);
  uint32_t offset = 0;
  if (cookie) {
    const uint64_t packed = cookie - 1;
    requestedCluster = static_cast<uint32_t>(packed >> 32);
    offset = static_cast<uint32_t>(packed);
  }

  const uint32_t portionSize = m_bRootDir && m_Type != FAT32 ? m_DirBlockSize : m_BlockSize;
  if (!portionSize || (portionSize % sizeof(Dir)) || (offset % sizeof(Dir)) || offset > portionSize)
    return ReadStatus::IoError;

  uint32_t cluster = static_cast<uint32_t>(m_Inode);
  size_t visitedClusters = 1;
  if (m_bRootDir && m_Type != FAT32) {
    if (requestedCluster)
      return ReadStatus::IoError;
  } else {
    if (cluster < 2 || requestedCluster < 2 || cluster >= (pFs->m_ClusterCount + 2) ||
        requestedCluster >= (pFs->m_ClusterCount + 2)) {
      return ReadStatus::IoError;
    }

    while (cluster != requestedCluster) {
      if (visitedClusters++ >= pFs->m_ClusterCount)
        return ReadStatus::IoError;
      cluster = pFs->getClusterEntry(cluster);
      if (!cluster || pFs->isEof(cluster) || cluster < 2 || cluster >= (pFs->m_ClusterCount + 2)) {
        return ReadStatus::IoError;
      }
    }
  }

  uint8_t* buffer = new uint8_t[portionSize];
  PointerGuard<uint8_t> bufferGuard(buffer, true);
  if (!pFs->readDirectoryPortion(cluster, reinterpret_cast<uintptr_t>(buffer)))
    return ReadStatus::IoError;

  LongFilenameState lfn;
  resetLongFilename(lfn);
  uint64_t recordCookie = startingCookie;
  bool initialSlot = true;

  while (true) {
    if (offset == portionSize) {
      if (m_bRootDir && m_Type != FAT32)
        return ReadStatus::Complete;

      const uint32_t nextCluster = pFs->getClusterEntry(cluster);
      if (!nextCluster)
        return ReadStatus::IoError;
      if (pFs->isEof(nextCluster))
        return ReadStatus::Complete;
      if (nextCluster < 2 || nextCluster >= (pFs->m_ClusterCount + 2) ||
          visitedClusters++ >= pFs->m_ClusterCount) {
        return ReadStatus::IoError;
      }

      cluster = nextCluster;
      offset = 0;
      if (!pFs->readCluster(cluster, reinterpret_cast<uintptr_t>(buffer)))
        return ReadStatus::IoError;
      if (!lfn.valid)
        recordCookie = directoryCookie(cluster, 0);
      initialSlot = false;
    }

    const uint64_t slotCookie =
        initialSlot && !startingCookie ? 0 : directoryCookie(cluster, offset);
    initialSlot = false;
    const uint64_t nextCookie = directoryCookie(cluster, offset + sizeof(Dir));
    const Dir* entry = reinterpret_cast<const Dir*>(buffer + offset);
    offset += sizeof(Dir);

    const uint8_t firstCharacter = entry->DIR_Name[0];
    if (!firstCharacter)
      return ReadStatus::Complete;

    if (firstCharacter == 0xE5) {
      resetLongFilename(lfn);
      recordCookie = nextCookie;
      continue;
    }

    if ((entry->DIR_Attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME) {
      if (entry->DIR_Name[0] & 0x40)
        recordCookie = slotCookie;
      if (!consumeLongFilenameEntry(lfn, *reinterpret_cast<const DirLongFilename*>(entry)))
        recordCookie = nextCookie;
      continue;
    }

    String filename;
    if (lfn.valid && !lfn.expectedOrdinal &&
        lfn.checksum == shortFilenameChecksum(entry->DIR_Name)) {
      filename = longFilename(lfn);
    }
    if (!filename.length()) {
      uint8_t shortNameBytes[11];
      MemoryCopy(shortNameBytes, entry->DIR_Name, sizeof(shortNameBytes));
      if (shortNameBytes[0] == 0x05)
        shortNameBytes[0] = 0xE5;
      const String shortName(reinterpret_cast<const char*>(shortNameBytes), 11, true);
      filename = pFs->convertFilenameFrom(shortName);
    }

    const uint64_t currentCookie = recordCookie;
    resetLongFilename(lfn);
    recordCookie = nextCookie;

    if ((entry->DIR_Attr & ATTR_VOLUME_ID) || filename.compare(".", 1) ||
        filename.compare("..", 2)) {
      continue;
    }

    EntryType type = EntryType::Regular;
    if (entry->DIR_Attr & ATTR_DIRECTORY) {
      type = EntryType::Directory;
    } else if (filename.endswith(symlinkSuffix())) {
      filename.rtrim(symlinkSuffix().length());
      type = EntryType::Symlink;
    }

    ScannedEntry scanned;
    scanned.name = pedigree_std::move(filename);
    MemoryCopy(&scanned.entry, entry, sizeof(Dir));
    scanned.directoryCluster = cluster;
    scanned.directoryOffset = offset - sizeof(Dir);
    scanned.type = type;

    if (!emitter(context, scanned, currentCookie, nextCookie))
      return ReadStatus::Stopped;
    cookie = nextCookie;
  }
}

File* FatDirectory::materialize(const ScannedEntry& scanned) {
  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);
  const Dir& entry = scanned.entry;
  const uint32_t fileCluster = LITTLE_TO_HOST16(entry.DIR_FstClusLO) |
                               (static_cast<uint32_t>(LITTLE_TO_HOST16(entry.DIR_FstClusHI)) << 16);
  const Time::Timestamp writeTime = pFs->getUnixTimestamp(entry.DIR_WrtTime, entry.DIR_WrtDate);
  const Time::Timestamp accessTime = pFs->getUnixTimestamp(0, entry.DIR_LstAccDate);
  const Time::Timestamp creationTime = pFs->getUnixTimestamp(entry.DIR_CrtTime, entry.DIR_CrtDate);

  if (scanned.type == EntryType::Directory) {
    FatFileInfo info;
    info.accessedTime = accessTime;
    info.modifiedTime = writeTime;
    info.creationTime = creationTime;
    return new FatDirectory(scanned.name, fileCluster, pFs, this, info, scanned.directoryCluster,
                            scanned.directoryOffset);
  }

  const uint32_t size = LITTLE_TO_HOST32(entry.DIR_FileSize);
  if (scanned.type == EntryType::Symlink) {
    return new FatSymlink(scanned.name, accessTime, writeTime, creationTime, fileCluster, pFs, size,
                          scanned.directoryCluster, scanned.directoryOffset, this);
  }
  return new FatFile(scanned.name, accessTime, writeTime, creationTime, fileCluster, pFs, size,
                     scanned.directoryCluster, scanned.directoryOffset, this);
}

Directory::LookupStatus FatDirectory::resolveChild(const StringView& name, File*& child) {
  child = nullptr;
  struct Context {
    StringView name;
    ScannedEntry entry;
    bool found;
  } context = {name, ScannedEntry(), false};

  auto emitter = [](void* opaque, const ScannedEntry& entry, uint64_t, uint64_t) -> bool {
    Context* context = reinterpret_cast<Context*>(opaque);
    if (entry.name == context->name) {
      context->entry = entry;
      context->found = true;
      return false;
    }
    return true;
  };

  uint64_t cookie = 0;
  ReadStatus status;
  {
    LockGuard<Mutex> guard(m_Lock);
    if (isDetached())
      return LookupStatus::NotFound;
    status = scanDirectory(cookie, emitter, &context);
    if (context.found)
      child = materialize(context.entry);
  }

  if (context.found)
    return child ? LookupStatus::Found : LookupStatus::IoError;
  return status == ReadStatus::IoError ? LookupStatus::IoError : LookupStatus::NotFound;
}

Directory::LookupStatus FatDirectory::resolveChildAt(uint64_t cookie, const StringView& name,
                                                     File*& child) {
  child = nullptr;
  struct Context {
    StringView name;
    ScannedEntry entry;
    bool found;
  } context = {name, ScannedEntry(), false};

  auto emitter = [](void* opaque, const ScannedEntry& entry, uint64_t, uint64_t) -> bool {
    Context* context = reinterpret_cast<Context*>(opaque);
    if (entry.name == context->name) {
      context->entry = entry;
      context->found = true;
    }
    return false;
  };

  {
    LockGuard<Mutex> guard(m_Lock);
    if (isDetached())
      return LookupStatus::NotFound;
    scanDirectory(cookie, emitter, &context);
    if (context.found)
      child = materialize(context.entry);
  }

  if (context.found)
    return child ? LookupStatus::Found : LookupStatus::IoError;
  // A cookie is only a fast path; retain ordinary lookup semantics if a
  // caller resumes with a stale location.
  return resolveChild(name, child);
}

Directory::ReadStatus FatDirectory::readDirectory(uint64_t& cookie, DirectoryEntryEmitter emitter,
                                                  void* context) {
  if (!emitter)
    return ReadStatus::IoError;

  struct Context {
    DirectoryEntryEmitter emitter;
    void* context;
  } adapter = {emitter, context};

  auto scanEmitter = [](void* opaque, const ScannedEntry& entry, uint64_t currentCookie,
                        uint64_t nextCookie) -> bool {
    Context* context = reinterpret_cast<Context*>(opaque);
    const uint32_t inode =
        LITTLE_TO_HOST16(entry.entry.DIR_FstClusLO) |
        (static_cast<uint32_t>(LITTLE_TO_HOST16(entry.entry.DIR_FstClusHI)) << 16);
    DirectoryEntryView view = {entry.name.view(), inode, entry.type, currentCookie, nextCookie};
    return context->emitter(context->context, view);
  };

  LockGuard<Mutex> guard(m_Lock);
  if (isDetached())
    return ReadStatus::Complete;
  return scanDirectory(cookie, scanEmitter, &adapter);
}

void FatDirectory::fileAttributeChanged() {}
