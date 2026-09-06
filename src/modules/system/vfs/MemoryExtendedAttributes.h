/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_VFS_MEMORYEXTENDEDATTRIBUTES_H
#define PEDIGREE_VFS_MEMORYEXTENDEDATTRIBUTES_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/StringView.h"

#include "ExtendedAttributes.h"

class EXPORTED_PUBLIC MemoryExtendedAttributes {
 public:
  static constexpr size_t MaximumEntries = 128;
  static constexpr size_t MaximumStoreBytes = 256 * 1024;
  static constexpr size_t MaximumTotalBytes = 16 * 1024 * 1024;

  class EXPORTED_PUBLIC Quota {
   public:
    explicit Quota(size_t limit = MaximumTotalBytes) : m_Limit(limit) {}
    size_t used() const;

   private:
    friend class MemoryExtendedAttributes;
    NOT_COPYABLE_OR_ASSIGNABLE(Quota);
    bool increase(size_t amount);
    void decrease(size_t amount);
    const size_t m_Limit;
    size_t m_Used = 0;
  };

  static Quota& globalQuota();
  explicit MemoryExtendedAttributes(Quota& quota = globalQuota());
  ~MemoryExtendedAttributes();

  XattrStatus get(const StringView& name, void* buffer, size_t capacity, size_t& required);
  XattrStatus list(void* buffer, size_t capacity, size_t& required);
  XattrStatus set(const StringView& name, const void* value, size_t length, unsigned flags);
  XattrStatus remove(const StringView& name);

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(MemoryExtendedAttributes);
  struct Entry;
  struct PreparedEntry;
  Entry** find(const StringView& name);
  Mutex m_Lock;
  Quota& m_Quota;
  Entry* m_Entries = nullptr;
  size_t m_Count = 0;
  size_t m_Bytes = 0;
};

#endif
