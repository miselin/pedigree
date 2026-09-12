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

bool encodeLongFilename(const String& filename, uint16_t* characters, size_t& characterCount,
                        bool& tooLong) {
  characterCount = 0;
  tooLong = false;
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
      if (characterCount >= MaxLongFilenameCharacters) {
        tooLong = true;
        return false;
      }
      characters[characterCount++] = character;
    } else {
      if ((characterCount + 2) > MaxLongFilenameCharacters) {
        tooLong = true;
        return false;
      }
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
    : Directory(name, info.accessedTime, info.modifiedTime, info.creationTime, inode_num,
                static_cast<Filesystem*>(pFs), 0, pParent),
      m_DirClus(dirClus),
      m_DirOffset(dirOffset),
      m_Unlinked(false),
      m_Type(FAT16),
      m_BlockSize(0),
      m_bRootDir(false),
      m_Lock(),
      m_DirBlockSize(0) {
  uint32_t permissions = 0777;  /// \todo Permissions

  setPermissionsOnly(permissions);
  setUidOnly(0);
  setGidOnly(0);

  m_BlockSize = pFs->m_BlockSize;
  m_Type = pFs->m_Type;

  setInode(inode_num);
  pFs->registerNode(this);
}

bool FatDirectory::encodeEntrySet(const String& filename, const Dir& metadata,
                                  Vector<Dir>& entries) {
  const bool special = filename == "." || filename == "..";
  uint16_t characters[LongFilenameStorageCharacters];
  size_t count = 0;
  bool tooLong = false;
  for (size_t i = 0; i < filename.length(); ++i) {
    const uint8_t character = filename[i];
    if (character < 0x20 || character == '"' || character == '*' || character == '/' ||
        character == ':' || character == '<' || character == '>' || character == '?' ||
        character == '\\' || character == '|') {
      SYSCALL_ERROR(InvalidArgument);
      return false;
    }
  }
  if (!special &&
      (!filename.length() || !encodeLongFilename(filename, characters, count, tooLong))) {
    syscallError(tooLong ? Error::NameTooLong : Error::InvalidArgument);
    return false;
  }
  const size_t longCount =
      special ? 0 : (count + LongFilenameCharactersPerEntry - 1) / LongFilenameCharactersPerEntry;
  FatFilesystem* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  const String shortName = filesystem->convertFilenameTo(filename);
  if (shortName.length() < 11) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  const uint8_t checksum =
      shortFilenameChecksum(reinterpret_cast<const uint8_t*>(shortName.cstr()));
  for (size_t i = 0; i < longCount; ++i) {
    Dir raw;
    ByteSet(&raw, 0xFF, sizeof(raw));
    DirLongFilename* entry = reinterpret_cast<DirLongFilename*>(&raw);
    const size_t ordinal = longCount - i;
    entry->LDIR_Ord = ordinal | (i == 0 ? 0x40 : 0);
    entry->LDIR_Attr = ATTR_LONG_NAME;
    entry->LDIR_Type = 0;
    entry->LDIR_Chksum = checksum;
    entry->LDIR_FstClusLO = 0;
    for (size_t character = 0; character < LongFilenameCharactersPerEntry; ++character) {
      const size_t index = (ordinal - 1) * LongFilenameCharactersPerEntry + character;
      writeLongFilenameCharacter(reinterpret_cast<uint8_t*>(entry), character,
                                 index < count    ? characters[index]
                                 : index == count ? 0
                                                  : 0xFFFF);
    }
    entries.pushBack(raw);
  }
  Dir shortEntry = metadata;
  MemoryCopy(shortEntry.DIR_Name, shortName.cstr(), 11);
  entries.pushBack(shortEntry);
  return true;
}

FatDirectory::~FatDirectory() {
  static_cast<FatFilesystem*>(m_pFilesystem)->releaseNode(this);
}

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
  Time::Timestamp creationTime = pFs->getUnixTimestamp(entry.DIR_CrtTime, entry.DIR_CrtDate);
  if (creationTime && entry.DIR_CrtTimeTenth >= 100 && entry.DIR_CrtTimeTenth < 200)
    ++creationTime;

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
    LockGuard<Mutex> fileGuard(static_cast<FatFilesystem*>(m_pFilesystem)->m_FileMutationLock);
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
    LockGuard<Mutex> fileGuard(static_cast<FatFilesystem*>(m_pFilesystem)->m_FileMutationLock);
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
    FatFilesystem* filesystem;
  } adapter = {emitter, context, static_cast<FatFilesystem*>(m_pFilesystem)};

  auto scanEmitter = [](void* opaque, const ScannedEntry& entry, uint64_t currentCookie,
                        uint64_t nextCookie) -> bool {
    Context* context = reinterpret_cast<Context*>(opaque);
    const uintptr_t inode =
        entry.type == EntryType::Regular
            ? context->filesystem->fileIdentifier(entry.directoryCluster, entry.directoryOffset)
            : LITTLE_TO_HOST16(entry.entry.DIR_FstClusLO) |
                  (static_cast<uint32_t>(LITTLE_TO_HOST16(entry.entry.DIR_FstClusHI)) << 16);
    DirectoryEntryView view = {entry.name.view(), inode, entry.type, currentCookie, nextCookie};
    return context->emitter(context->context, view);
  };

  LockGuard<Mutex> guard(m_Lock);
  if (isDetached())
    return ReadStatus::Complete;
  return scanDirectory(cookie, scanEmitter, &adapter);
}
