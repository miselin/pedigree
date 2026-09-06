/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/utilities/utility.h"

#include "SwapStore.h"

SwapStatus SwapStore::validateHeader(const void* page, uint64_t bytes, size_t& pages) {
  pages = 0;
  constexpr size_t Page = PagingChannel::PageBytes;
  if (!page || PhysicalMemoryManager::getPageSize() != Page || bytes % Page || bytes < 2 * Page)
    return SwapStatus::Invalid;
  const auto* data = static_cast<const unsigned char*>(page);
  if (MemoryCompare(data + Page - 10, "SWAPSPACE2", 10))
    return SwapStatus::Invalid;
  // Linux version-one fields are little-endian on the supported x64 ABI.
  auto word = [&](size_t at) {
    return uint32_t(data[at]) | (uint32_t(data[at + 1]) << 8) | (uint32_t(data[at + 2]) << 16) |
           (uint32_t(data[at + 3]) << 24);
  };
  if (word(1024) != 1 || !word(1028) || uint64_t(word(1028)) >= bytes / Page)
    return SwapStatus::Invalid;
  if (word(1032))
    return SwapStatus::Unsupported;
  pages = word(1028) < MaximumPages ? word(1028) : MaximumPages;
  return SwapStatus::Success;
}
