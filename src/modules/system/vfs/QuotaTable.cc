/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "QuotaTable.h"

size_t QuotaTable::position(uint32_t id) const {
  size_t first = 0, last = m_Entries.count();
  while (first < last) {
    const size_t middle = first + (last - first) / 2;
    if (m_Entries[middle].id < id)
      first = middle + 1;
    else
      last = middle;
  }
  return first;
}

QuotaTable::Entry* QuotaTable::find(uint32_t id) {
  const size_t index = position(id);
  return index < m_Entries.count() && m_Entries[index].id == id ? &m_Entries[index] : nullptr;
}

const QuotaTable::Entry* QuotaTable::find(uint32_t id) const {
  const size_t index = position(id);
  return index < m_Entries.count() && m_Entries[index].id == id ? &m_Entries[index] : nullptr;
}

QuotaTable::Entry* QuotaTable::prepare(uint32_t id) {
  const size_t index = position(id);
  if (index < m_Entries.count() && m_Entries[index].id == id)
    return &m_Entries[index];
  if (!m_Entries.tryReserve(m_Entries.count() + 1))
    return nullptr;
  Entry entry;
  entry.id = id;
  entry.record.valid = Quota::Supported;
  m_Entries.insert(index, entry);
  return &m_Entries[index];
}

QuotaStatus QuotaTable::set(uint32_t id, const QuotaRecord& requested) {
  if (requested.valid & ~Quota::Limits)
    return QuotaStatus::Unsupported;
  if (((requested.valid & Quota::BlockLimits) && requested.blockSoftLimit) ||
      ((requested.valid & Quota::InodeLimits) && requested.inodeSoftLimit))
    return QuotaStatus::Unsupported;
  if (((requested.valid & Quota::BlockLimits) && requested.blockHardLimit > 0xffffffffULL) ||
      ((requested.valid & Quota::InodeLimits) && requested.inodeHardLimit > 0xffffffffULL))
    return QuotaStatus::Overflow;
  Entry* entry = prepare(id);
  if (!entry)
    return QuotaStatus::NoMemory;
  if (requested.valid & Quota::BlockLimits)
    entry->record.blockHardLimit = requested.blockHardLimit;
  if (requested.valid & Quota::InodeLimits)
    entry->record.inodeHardLimit = requested.inodeHardLimit;
  entry->dirty = true;
  return QuotaStatus::Success;
}

QuotaStatus QuotaTable::canCharge(const Entry& entry, uint64_t bytes, uint64_t inodes,
                                  bool enforce) {
  const auto& record = entry.record;
  constexpr uint64_t MaximumSpace = 0xffffffffULL * 1024;
  if (record.currentSpace > MaximumSpace || bytes > MaximumSpace - record.currentSpace ||
      record.currentInodes > 0xffffffffULL || inodes > 0xffffffffULL - record.currentInodes)
    return QuotaStatus::Overflow;
  if (enforce &&
      ((bytes && record.blockHardLimit &&
        record.currentSpace + bytes > record.blockHardLimit * 1024) ||
       (inodes && record.inodeHardLimit && record.currentInodes + inodes > record.inodeHardLimit)))
    return QuotaStatus::Limit;
  return QuotaStatus::Success;
}

void QuotaTable::charge(Entry& entry, uint64_t bytes, uint64_t inodes) {
  entry.record.currentSpace += bytes;
  entry.record.currentInodes += inodes;
  entry.dirty = true;
}

void QuotaTable::refund(Entry& entry, uint64_t bytes, uint64_t inodes) {
  assert(bytes <= entry.record.currentSpace && inodes <= entry.record.currentInodes);
  entry.record.currentSpace -= bytes;
  entry.record.currentInodes -= inodes;
  entry.dirty = true;
}

bool QuotaTable::sameRecord(const QuotaRecord& a, const QuotaRecord& b) {
  return a.blockHardLimit == b.blockHardLimit && a.blockSoftLimit == b.blockSoftLimit &&
         a.currentSpace == b.currentSpace && a.inodeHardLimit == b.inodeHardLimit &&
         a.inodeSoftLimit == b.inodeSoftLimit && a.currentInodes == b.currentInodes &&
         a.blockTime == b.blockTime && a.inodeTime == b.inodeTime && a.valid == b.valid;
}

void QuotaTable::clear() {
  m_Entries.clear();
}
void QuotaTable::swap(QuotaTable& other) {
  m_Entries.swap(other.m_Entries);
}

QuotaStatus QuotaOld::decode(const void* bytes, size_t length, QuotaRecord& record) {
  if (!bytes || length != RecordSize)
    return QuotaStatus::Invalid;
  uint32_t words[8];
  MemoryCopy(words, bytes, RecordSize);
  record = QuotaRecord();
  record.blockHardLimit = words[0];
  record.blockSoftLimit = words[1];
  record.currentSpace = static_cast<uint64_t>(words[2]) * 1024;
  record.inodeHardLimit = words[3];
  record.inodeSoftLimit = words[4];
  record.currentInodes = words[5];
  record.blockTime = words[6];
  record.inodeTime = words[7];
  record.valid = Quota::Supported;
  return QuotaStatus::Success;
}

QuotaStatus QuotaOld::encode(const QuotaRecord& record, void* bytes, size_t length) {
  if (!bytes || length != RecordSize)
    return QuotaStatus::Invalid;
  if (record.blockHardLimit > 0xffffffffULL || record.blockSoftLimit > 0xffffffffULL ||
      record.currentSpace > 0xffffffffULL * 1024 || record.inodeHardLimit > 0xffffffffULL ||
      record.inodeSoftLimit > 0xffffffffULL || record.currentInodes > 0xffffffffULL ||
      record.blockTime > 0xffffffffULL || record.inodeTime > 0xffffffffULL)
    return QuotaStatus::Overflow;
  const uint32_t words[8] = {static_cast<uint32_t>(record.blockHardLimit),
                             static_cast<uint32_t>(record.blockSoftLimit),
                             static_cast<uint32_t>((record.currentSpace + 1023) / 1024),
                             static_cast<uint32_t>(record.inodeHardLimit),
                             static_cast<uint32_t>(record.inodeSoftLimit),
                             static_cast<uint32_t>(record.currentInodes),
                             static_cast<uint32_t>(record.blockTime),
                             static_cast<uint32_t>(record.inodeTime)};
  MemoryCopy(bytes, words, RecordSize);
  return QuotaStatus::Success;
}

bool QuotaOld::looksLikeNewFormat(const void* bytes, size_t length) {
  if (!bytes || length < sizeof(uint32_t))
    return false;
  uint32_t magic;
  MemoryCopy(&magic, bytes, sizeof(magic));
  magic = LITTLE_TO_HOST32(magic);
  return magic == 0xd9c01f11U || magic == 0xd9c01927U;
}
