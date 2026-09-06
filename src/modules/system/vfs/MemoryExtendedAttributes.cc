/* Copyright (c) 2026, Pedigree Developers. */
#include "MemoryExtendedAttributes.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

namespace {
MemoryExtendedAttributes::Quota sharedQuota;

XattrStatus validateName(const StringView& name) {
  if (!name.length() || name.length() > Xattr::MaximumNameLength)
    return XattrStatus::Range;
  if (!name.str())
    return XattrStatus::Invalid;
  for (size_t i = 0; i < name.length(); ++i)
    if (!name[i])
      return XattrStatus::Invalid;
  if (name.length() < 5 || !name.substring(0, 5).compare("user.", 5))
    return XattrStatus::Unsupported;
  return name.length() == 5 ? XattrStatus::Invalid : XattrStatus::Success;
}
}  // namespace

struct MemoryExtendedAttributes::Entry {
  Entry* next;
  size_t nameLength, valueLength, bytes;
  char* name() {
    return reinterpret_cast<char*>(this + 1);
  }
  void* value() {
    return name() + nameLength + 1;
  }
  static Entry* create(const StringView& name, const void* value, size_t length) {
    const size_t bytes = sizeof(Entry) + name.length() + 1 + length;
    auto* storage = new uint8_t[bytes];
    if (!storage)
      return nullptr;
    auto* entry = new (storage) Entry{nullptr, name.length(), length, bytes};
    MemoryCopy(entry->name(), name.str(), name.length());
    entry->name()[name.length()] = 0;
    if (length)
      MemoryCopy(entry->value(), value, length);
    return entry;
  }
  static void destroy(Entry* entry) {
    if (!entry)
      return;
    entry->~Entry();
    delete[] reinterpret_cast<uint8_t*>(entry);
  }
};

struct MemoryExtendedAttributes::PreparedEntry {
  Entry* entry;
  ~PreparedEntry() {
    Entry::destroy(entry);
  }
};

size_t MemoryExtendedAttributes::Quota::used() const {
  return __atomic_load_n(&m_Used, __ATOMIC_ACQUIRE);
}

bool MemoryExtendedAttributes::Quota::increase(size_t amount) {
  size_t previous = used();
  do {
    if (previous > m_Limit || amount > m_Limit - previous)
      return false;
  } while (!__atomic_compare_exchange_n(&m_Used, &previous, previous + amount, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
  return true;
}

void MemoryExtendedAttributes::Quota::decrease(size_t amount) {
  const size_t previous = __atomic_fetch_sub(&m_Used, amount, __ATOMIC_ACQ_REL);
  assert(previous >= amount);
}

MemoryExtendedAttributes::Quota& MemoryExtendedAttributes::globalQuota() {
  return sharedQuota;
}

MemoryExtendedAttributes::MemoryExtendedAttributes(Quota& quota) : m_Lock(), m_Quota(quota) {}

MemoryExtendedAttributes::~MemoryExtendedAttributes() {
  // The inode's final owner has already excluded calls into this store.
  while (m_Entries) {
    Entry* next = m_Entries->next;
    Entry::destroy(m_Entries);
    m_Entries = next;
  }
  m_Quota.decrease(m_Bytes);
}

MemoryExtendedAttributes::Entry** MemoryExtendedAttributes::find(const StringView& name) {
  Entry** slot = &m_Entries;
  while (*slot && !name.compare((*slot)->name(), (*slot)->nameLength))
    slot = &(*slot)->next;
  return slot;
}

XattrStatus MemoryExtendedAttributes::get(const StringView& name, void* buffer, size_t capacity,
                                          size_t& required) {
  TerminationDeferral lifetime;
  required = 0;
  const auto valid = validateName(name);
  if (valid != XattrStatus::Success)
    return valid;
  LockGuard<Mutex> guard(m_Lock);
  Entry* entry = *find(name);
  if (!entry)
    return XattrStatus::Missing;
  required = entry->valueLength;
  if (!capacity)
    return XattrStatus::Success;
  if (capacity < required)
    return XattrStatus::Range;
  if (required) {
    if (!buffer)
      return XattrStatus::Invalid;
    MemoryCopy(buffer, entry->value(), required);
  }
  return XattrStatus::Success;
}

XattrStatus MemoryExtendedAttributes::list(void* buffer, size_t capacity, size_t& required) {
  TerminationDeferral lifetime;
  LockGuard<Mutex> guard(m_Lock);
  required = 0;
  for (Entry* entry = m_Entries; entry; entry = entry->next)
    required += entry->nameLength + 1;
  if (!capacity)
    return XattrStatus::Success;
  if (capacity < required)
    return XattrStatus::Range;
  if (required && !buffer)
    return XattrStatus::Invalid;
  auto* output = static_cast<char*>(buffer);
  for (Entry* entry = m_Entries; entry; entry = entry->next) {
    MemoryCopy(output, entry->name(), entry->nameLength + 1);
    output += entry->nameLength + 1;
  }
  return XattrStatus::Success;
}

XattrStatus MemoryExtendedAttributes::set(const StringView& name, const void* value, size_t length,
                                          unsigned flags) {
  TerminationDeferral lifetime;
  if ((flags & ~(Xattr::Create | Xattr::Replace)) || (length && !value))
    return XattrStatus::Invalid;
  const auto valid = validateName(name);
  if (valid != XattrStatus::Success)
    return valid;
  if (length > Xattr::MaximumValueLength)
    return XattrStatus::Range;
  PreparedEntry prepared{Entry::create(name, value, length)};
  if (!prepared.entry)
    return XattrStatus::NoMemory;
  Entry* retired = nullptr;
  {
    LockGuard<Mutex> guard(m_Lock);
    Entry** slot = find(name);
    retired = *slot;
    if (retired && (flags & Xattr::Create))
      return XattrStatus::Exists;
    if (!retired && (flags & Xattr::Replace))
      return XattrStatus::Missing;
    const size_t oldBytes = retired ? retired->bytes : 0;
    const size_t newBytes = prepared.entry->bytes;
    const size_t retainedBytes = m_Bytes - oldBytes;
    if ((!retired && m_Count == MaximumEntries) || newBytes > MaximumStoreBytes - retainedBytes)
      return XattrStatus::NoSpace;
    if (newBytes > oldBytes && !m_Quota.increase(newBytes - oldBytes))
      return XattrStatus::NoSpace;
    prepared.entry->next = retired ? retired->next : nullptr;
    *slot = prepared.entry;
    prepared.entry = nullptr;
    m_Bytes = retainedBytes + newBytes;
    if (!retired)
      ++m_Count;
    if (oldBytes > newBytes)
      m_Quota.decrease(oldBytes - newBytes);
  }
  Entry::destroy(retired);
  return XattrStatus::Success;
}

XattrStatus MemoryExtendedAttributes::remove(const StringView& name) {
  TerminationDeferral lifetime;
  const auto valid = validateName(name);
  if (valid != XattrStatus::Success)
    return valid;
  Entry* retired;
  {
    LockGuard<Mutex> guard(m_Lock);
    Entry** slot = find(name);
    retired = *slot;
    if (!retired)
      return XattrStatus::Missing;
    *slot = retired->next;
    --m_Count;
    m_Bytes -= retired->bytes;
    m_Quota.decrease(retired->bytes);
  }
  Entry::destroy(retired);
  return XattrStatus::Success;
}
