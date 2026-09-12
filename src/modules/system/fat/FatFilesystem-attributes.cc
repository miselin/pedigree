/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/syscallError.h"

#include "FatFilesystem.h"
#include "FatSymlink.h"
#include "system/kernel/machine/mach_pc/RtcTimeAccounting.h"

namespace {
constexpr Time::Timestamp FatEpoch = 315532800;
constexpr Time::Timestamp FatLastSecond = 4354819199ULL;
constexpr Time::Timestamp SecondsPerDay = 86400;

Time::Timestamp fatTimestamp(Time::Timestamp timestamp) {
  if (timestamp < FatEpoch)
    return FatEpoch;
  if (timestamp > FatLastSecond)
    return FatLastSecond;
  return timestamp;
}

RtcTimeAccounting::CivilTime fatCivilTime(Time::Timestamp timestamp) {
  timestamp = fatTimestamp(timestamp);
  RtcTimeAccounting::CivilTime civil = {1980, 1, 1, 0, 0, 0, 0};
  uint64_t days = (timestamp - FatEpoch) / SecondsPerDay;
  while (days >= (RtcTimeAccounting::isLeapYear(civil.year) ? 366U : 365U)) {
    days -= RtcTimeAccounting::isLeapYear(civil.year) ? 366U : 365U;
    ++civil.year;
  }
  while (days >= RtcTimeAccounting::daysInMonth(civil.year, civil.month)) {
    days -= RtcTimeAccounting::daysInMonth(civil.year, civil.month);
    ++civil.month;
  }
  civil.day = days + 1;
  civil.hour = (timestamp % SecondsPerDay) / 3600;
  civil.minute = (timestamp % 3600) / 60;
  civil.second = timestamp % 60;
  return civil;
}
}  // namespace

bool FatFilesystem::getUuid(String& uuid) const {
  const uint8_t signature = m_Type == FAT32 ? m_Superblock32.BS_BootSig : m_Superblock16.BS_BootSig;
  if (signature != 0x28 && signature != 0x29) {
    uuid = String();
    return false;
  }
  const uint32_t volumeId =
      LITTLE_TO_HOST32(m_Type == FAT32 ? m_Superblock32.BS_VolID : m_Superblock16.BS_VolID);
  uuid.Format("%04X-%04X", volumeId >> 16, volumeId & 0xFFFF);
  return true;
}

uint16_t FatFilesystem::getFatDate(Time::Timestamp timestamp) const {
  const auto civil = fatCivilTime(timestamp);
  return static_cast<uint16_t>(((civil.year - 1980) << 9) | (civil.month << 5) | civil.day);
}

uint16_t FatFilesystem::getFatTime(Time::Timestamp timestamp) const {
  const auto civil = fatCivilTime(timestamp);
  return static_cast<uint16_t>((civil.hour << 11) | (civil.minute << 5) | (civil.second / 2));
}

Time::Timestamp FatFilesystem::getUnixTimestamp(uint16_t time, uint16_t date) const {
  time = LITTLE_TO_HOST16(time);
  date = LITTLE_TO_HOST16(date);
  const size_t year = 1980 + (date >> 9);
  const uint8_t month = (date >> 5) & 15;
  const uint8_t day = date & 31;
  const uint8_t hour = time >> 11;
  const uint8_t minute = (time >> 5) & 63;
  const uint8_t second = (time & 31) * 2;
  if (month < 1 || month > 12 || day < 1 || day > RtcTimeAccounting::daysInMonth(year, month) ||
      hour > 23 || minute > 59 || second > 59)
    return 0;

  uint64_t days = day - 1;
  for (size_t precedingYear = 1980; precedingYear < year; ++precedingYear)
    days += RtcTimeAccounting::isLeapYear(precedingYear) ? 366 : 365;
  for (uint8_t precedingMonth = 1; precedingMonth < month; ++precedingMonth)
    days += RtcTimeAccounting::daysInMonth(year, precedingMonth);
  return FatEpoch + days * SecondsPerDay + hour * 3600 + minute * 60 + second;
}

void FatFilesystem::writeEntryAttributes(File* file, Dir* entry, bool creating) {
  // Bypass the FAT snapshot wrapper: the caller already owns the filesystem lock.
  encodeEntryAttributes(file->File::getAttributes(), entry, creating);
}

void FatFilesystem::encodeEntryAttributes(const File::Attributes& attributes, Dir* entry,
                                          bool creating) {
  entry->DIR_WrtDate = HOST_TO_LITTLE16(getFatDate(attributes.modified));
  entry->DIR_WrtTime = HOST_TO_LITTLE16(getFatTime(attributes.modified));
  entry->DIR_LstAccDate = HOST_TO_LITTLE16(getFatDate(attributes.accessed));
  if (creating || !entry->DIR_CrtDate) {
    // The VFS changed field is ctime. FAT birth time must survive later changes.
    const Time::Timestamp created = attributes.changed ? attributes.changed : Time::getTime();
    entry->DIR_CrtDate = HOST_TO_LITTLE16(getFatDate(created));
    entry->DIR_CrtTime = HOST_TO_LITTLE16(getFatTime(created));
    const auto civil = fatCivilTime(created);
    entry->DIR_CrtTimeTenth = (civil.second % 2) * 100;
  }
}

bool FatFilesystem::writePendingAttributes(const PendingAttributes& pending) {
  const uint32_t cluster = pending.slot >> 32;
  const uint32_t offset = pending.slot;
  Dir* entry = getDirectoryEntry(cluster, offset);
  if (!entry)
    return false;
  if (entry->DIR_Name[0] == 0 || entry->DIR_Name[0] == 0xE5 ||
      (entry->DIR_Attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME) {
    delete entry;
    return false;
  }
  encodeEntryAttributes(pending.attributes, entry);
  const bool succeeded = writeDirectoryEntry(entry, cluster, offset);
  delete entry;
  return succeeded;
}

bool FatFilesystem::syncNodeAttributes(File* file) {
  if (m_bReadOnly)
    return !m_IoFailed;
  if (file == m_pRoot || isNodeUnlinked(file))
    return true;
  uint32_t cluster = 0, offset = 0;
  if (file->isDirectory()) {
    auto* directory = static_cast<FatDirectory*>(file);
    cluster = directory->getDirCluster();
    offset = directory->getDirOffset();
  } else if (file->isSymlink()) {
    auto* symlink = static_cast<FatSymlink*>(file);
    cluster = symlink->getDirCluster();
    offset = symlink->getDirOffset();
  } else {
    auto* regular = static_cast<FatFile*>(file);
    cluster = regular->getDirCluster();
    offset = regular->getDirOffset();
  }
  if (cluster == 0xdeadbeef || offset == 0xbeefdead)
    return true;
  const uint64_t slot = (uint64_t(cluster) << 32) | offset;
  PendingAttributes* pending = m_PendingAttributes;
  while (pending && pending->slot != slot)
    pending = pending->next;
  if (!pending) {
    pending = new PendingAttributes;
    if (!pending) {
      m_IoFailed = true;
      SYSCALL_ERROR(OutOfMemory);
      return false;
    }
    pending->slot = slot;
    pending->next = m_PendingAttributes;
    m_PendingAttributes = pending;
  }
  pending->attributes = file->File::getAttributes();
  if (!pending->attributes.changed)
    pending->attributes.changed = Time::getTime();
  if (!writePendingAttributes(*pending))
    return false;
  discardPendingAttributes(cluster, offset);
  return true;
}

bool FatFilesystem::syncPendingAttributes() {
  if (m_bReadOnly)
    return !m_IoFailed && !m_PendingAttributes;
  bool succeeded = !m_IoFailed;
  PendingAttributes** link = &m_PendingAttributes;
  while (*link) {
    PendingAttributes* pending = *link;
    if (!writePendingAttributes(*pending)) {
      succeeded = false;
      link = &pending->next;
      continue;
    }
    *link = pending->next;
    delete pending;
  }
  return succeeded;
}

void FatFilesystem::movePendingAttributes(uint32_t oldCluster, uint32_t oldOffset,
                                          uint32_t newCluster, uint32_t newOffset) {
  const uint64_t oldSlot = (uint64_t(oldCluster) << 32) | oldOffset;
  const uint64_t newSlot = (uint64_t(newCluster) << 32) | newOffset;
  if (oldSlot == newSlot)
    return;
  discardPendingAttributes(newCluster, newOffset);
  for (PendingAttributes* pending = m_PendingAttributes; pending; pending = pending->next) {
    if (pending->slot == oldSlot) {
      pending->slot = newSlot;
      return;
    }
  }
}

void FatFilesystem::discardPendingAttributes(uint32_t cluster, uint32_t offset) {
  const uint64_t slot = (uint64_t(cluster) << 32) | offset;
  PendingAttributes** link = &m_PendingAttributes;
  while (*link) {
    PendingAttributes* pending = *link;
    if (pending->slot == slot) {
      *link = pending->next;
      delete pending;
      return;
    }
    link = &pending->next;
  }
}

void FatFilesystem::clearPendingAttributes() {
  while (m_PendingAttributes) {
    PendingAttributes* pending = m_PendingAttributes;
    m_PendingAttributes = pending->next;
    delete pending;
  }
}

void FatFilesystem::fileAttributeChanged(File* file) {
  LockGuard<Mutex> guard(m_FileMutationLock);
  if (!file->isDirectory() && !file->isSymlink()) {
    auto* regular = static_cast<FatFile*>(file);
    const auto attributes = file->File::getAttributes();
    const auto* state = regular->m_State;
    const bool changed = fatTimestamp(state->accessed) / SecondsPerDay !=
                             fatTimestamp(attributes.accessed) / SecondsPerDay ||
                         fatTimestamp(state->modified) / 2 != fatTimestamp(attributes.modified) / 2;
    regular->m_State->accessed = attributes.accessed;
    regular->m_State->modified = attributes.modified;
    regular->m_State->changed = attributes.changed;
    {
      LockGuard<Mutex> registry(m_StateLock);
      for (auto* alias = regular->m_State->aliases; alias; alias = alias->m_NextAlias)
        alias->copyStateAttributes();
    }
    regular->m_MetadataDirty = regular->m_MetadataDirty || changed;
    return;
  }
  if (!syncNodeAttributes(file))
    WARNING("FAT: unable to persist file attributes");
}

uint64_t FatFile::maximumFileSize() const {
  return UINT32_MAX;
}

void FatFile::fileAttributeChanged() {
  static_cast<FatFilesystem*>(m_pFilesystem)->fileAttributeChanged(this);
}

void FatDirectory::fileAttributeChanged() {
  static_cast<FatFilesystem*>(m_pFilesystem)->fileAttributeChanged(this);
}

void FatSymlink::fileAttributeChanged() {
  static_cast<FatFilesystem*>(m_pFilesystem)->fileAttributeChanged(this);
}

bool FatFilesystem::syncNode(File* file) {
  LockGuard<Mutex> guard(m_FileMutationLock);
  if (m_bReadOnly)
    return !m_IoFailed;
  const bool metadata = syncNodeAttributes(file);
  const bool allocation = syncFat();
  return m_pDisk && m_pDisk->syncData() && metadata && allocation;
}

bool FatDirectory::sync() {
  return static_cast<FatFilesystem*>(m_pFilesystem)->syncNode(this);
}

bool FatSymlink::sync() {
  return static_cast<FatFilesystem*>(m_pFilesystem)->syncNode(this);
}
