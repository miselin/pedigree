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

#include "Iso9660Directory.h"
#include "pedigree/kernel/utilities/BufferView.h"

namespace {
constexpr uint64_t IsoSectorSize = 2048;
}

Iso9660Directory::~Iso9660Directory() = default;

Directory::LookupStatus Iso9660Directory::resolveChild(const StringView& name, File*& child) {
  child = nullptr;
  if (!m_pFs) {
    return LookupStatus::IoError;
  }

  ResolveContext context = {this, name, nullptr, 0, false};
  uint64_t cookie = 0;
  const ReadStatus status = scanDirectory(cookie, resolveEntry, &context);
  if (context.child) {
    child = context.child;
    return LookupStatus::Found;
  }
  if (status == ReadStatus::IoError) {
    return LookupStatus::IoError;
  }
  return LookupStatus::NotFound;
}

Directory::LookupStatus Iso9660Directory::resolveChildAt(uint64_t cookie, const StringView& name,
                                                         File*& child) {
  child = nullptr;
  if (!m_pFs) {
    return LookupStatus::IoError;
  }

  const uint64_t expectedCookie = cookie;
  ResolveContext context = {this, name, nullptr, expectedCookie, true};
  scanDirectory(cookie, resolveEntry, &context);
  if (context.child) {
    child = context.child;
    return LookupStatus::Found;
  }
  // A cookie is only a fast path; retain ordinary lookup semantics if a
  // caller resumes with a stale location.
  return resolveChild(name, child);
}

Directory::ReadStatus Iso9660Directory::readDirectory(uint64_t& cookie,
                                                      DirectoryEntryEmitter emitter,
                                                      void* context) {
  if (!emitter) {
    return ReadStatus::IoError;
  }

  ReadContext readContext = {emitter, context};
  return scanDirectory(cookie, emitEntry, &readContext);
}

Directory::ReadStatus Iso9660Directory::scanDirectory(uint64_t& cookie, ScannedEntryEmitter emitter,
                                                      void* context) {
  if (!m_pFs || !emitter) {
    return ReadStatus::IoError;
  }

  const uint64_t directorySize = LITTLE_TO_HOST32(m_Dir.DataLen_LE);
  const uint64_t directoryLocation = LITTLE_TO_HOST32(m_Dir.ExtentLocation_LE);
  const uint64_t sectorCount = (directorySize + IsoSectorSize - 1) / IsoSectorSize;
  if (cookie > directorySize) {
    return ReadStatus::IoError;
  }

  uint64_t scanCookie = cookie;
  while (scanCookie < directorySize) {
    const uint64_t sector = scanCookie / IsoSectorSize;
    if (sector >= sectorCount) {
      return ReadStatus::IoError;
    }

    const uint64_t sectorStart = sector * IsoSectorSize;
    const size_t sectorLimit = static_cast<size_t>(min(IsoSectorSize, directorySize - sectorStart));
    size_t offset = static_cast<size_t>(scanCookie - sectorStart);

    alignas(Iso9660DirRecord) uint8_t sectorBytes[IsoSectorSize];
    const uint64_t diskLocation = (directoryLocation + sector) * IsoSectorSize;
    if (!m_pFs->readSector(diskLocation, sectorBytes)) {
      return ReadStatus::IoError;
    }
    const BufferView buffer(sectorBytes, sizeof(sectorBytes));

    while (offset < sectorLimit) {
      const uint64_t currentCookie = sectorStart + offset;
      cookie = currentCookie;

      // A zero-length record pads the remainder of this logical sector.
      if (!sectorBytes[offset]) {
        scanCookie = min(sectorStart + IsoSectorSize, directorySize);
        cookie = scanCookie;
        break;
      }

      const size_t bytesRemaining = sectorLimit - offset;
      if (bytesRemaining < sizeof(Iso9660DirRecord)) {
        return ReadStatus::IoError;
      }

      Iso9660DirRecord* record = buffer.as<Iso9660DirRecord>(offset);
      const size_t recordLength = record->RecLen;
      if (recordLength < sizeof(*record) || recordLength > bytesRemaining ||
          !record->FileIdentLen || record->FileIdentLen > (recordLength - sizeof(*record))) {
        return ReadStatus::IoError;
      }

      const uint64_t nextCookie = currentCookie + recordLength;
      uint8_t* fileIdentifier = buffer.as<uint8_t>(offset + sizeof(*record));
      offset += recordLength;
      scanCookie = nextCookie;

      // Path walking and the common enumeration wrapper provide . and ...
      const bool dotEntry =
          record->FileIdentLen == 1 && (fileIdentifier[0] == 0 || fileIdentifier[0] == 1);
      if (dotEntry || (record->FileFlags & (1 << 0))) {
        cookie = nextCookie;
        continue;
      }

      String fileName = m_pFs->parseName(*record);
      ScannedEntry entry = {&fileName, record, currentCookie, nextCookie};
      if (!emitter(context, entry)) {
        cookie = currentCookie;
        return ReadStatus::Stopped;
      }
      cookie = nextCookie;
    }
  }

  cookie = directorySize;
  return ReadStatus::Complete;
}

bool Iso9660Directory::resolveEntry(void* opaque, const ScannedEntry& entry) {
  ResolveContext* context = reinterpret_cast<ResolveContext*>(opaque);
  if (context->checkCookie && entry.currentCookie != context->expectedCookie) {
    return false;
  }
  if (!(*entry.name == context->name)) {
    return !context->checkCookie;
  }

  const bool directory = (entry.record->FileFlags & (1 << 1)) != 0;
  context->child =
      context->directory->m_pFs->fileFromDirRecord(*entry.record, 0, context->directory, directory);
  return false;
}

bool Iso9660Directory::emitEntry(void* opaque, const ScannedEntry& scanned) {
  ReadContext* context = reinterpret_cast<ReadContext*>(opaque);
  const EntryType type =
      (scanned.record->FileFlags & (1 << 1)) ? EntryType::Directory : EntryType::Regular;
  DirectoryEntryView entry = {scanned.name->view(), 0, type, scanned.currentCookie,
                              scanned.nextCookie};
  return context->emitter(context->context, entry);
}
