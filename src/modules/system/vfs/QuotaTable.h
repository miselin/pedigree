/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_VFS_QUOTATABLE_H
#define PEDIGREE_VFS_QUOTATABLE_H

#include "pedigree/kernel/utilities/Vector.h"

#include "Quota.h"

/** Storage and arithmetic only; the owning filesystem serializes transactions. */
class EXPORTED_PUBLIC QuotaTable {
 public:
  struct Entry {
    uint32_t id = 0;
    QuotaRecord record;
    bool dirty = false;
  };

  Entry* find(uint32_t id);
  const Entry* find(uint32_t id) const;
  Entry* prepare(uint32_t id);
  QuotaStatus set(uint32_t id, const QuotaRecord& requested);
  static QuotaStatus canCharge(const Entry&, uint64_t bytes, uint64_t inodes, bool enforce);
  static void charge(Entry&, uint64_t bytes, uint64_t inodes);
  static void refund(Entry&, uint64_t bytes, uint64_t inodes);
  static bool sameRecord(const QuotaRecord&, const QuotaRecord&);
  void clear();
  void swap(QuotaTable& other);
  Vector<Entry>& entries() {
    return m_Entries;
  }
  const Vector<Entry>& entries() const {
    return m_Entries;
  }

 private:
  size_t position(uint32_t id) const;
  Vector<Entry> m_Entries;
};

/** QFMT_VFS_OLD is a native-endian, 32-byte record array indexed by ID. */
namespace QuotaOld {
constexpr size_t RecordSize = 32;
QuotaStatus EXPORTED_PUBLIC decode(const void* bytes, size_t length, QuotaRecord& record);
QuotaStatus EXPORTED_PUBLIC encode(const QuotaRecord& record, void* bytes, size_t length);
bool EXPORTED_PUBLIC looksLikeNewFormat(const void* bytes, size_t length);
}  // namespace QuotaOld

#endif
