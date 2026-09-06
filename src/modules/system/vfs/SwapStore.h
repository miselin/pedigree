/* Copyright (c) 2026, Pedigree Developers. */
#ifndef VFS_SWAP_STORE_H
#define VFS_SWAP_STORE_H
#include "pedigree/kernel/machine/DiskPaging.h"
#include "pedigree/kernel/processor/types.h"

struct SwapReference {
  uint64_t value = 0;
  explicit operator bool() const {
    return value != 0;
  }
};
struct SwapSnapshot {
  uint64_t totalPages = 0;
  uint64_t usedPages = 0;
  bool active = false;
};
enum class SwapStatus {
  Success,
  Unsupported,
  Busy,
  NoMemory,
  Invalid,
  IoError,
  NotActive,
  Unmapped
};

// Every operation is serialized by MemoryMapManager::OperationGuard. Slot
// references describe immutable contents, independently of any address space.
class EXPORTED_PUBLIC SwapStore {
 public:
  static constexpr size_t MaximumPages = 4096;
  static SwapStore& instance();
  bool prepareAtBoot();
  SwapStatus activate(uint32_t endpoint);
  SwapStatus finishDeactivate(uint32_t endpoint);
  SwapStatus writePage(physical_uintptr_t physical, SwapReference& result);
  SwapStatus readPage(SwapReference reference, physical_uintptr_t physical);
  bool zeroPage(physical_uintptr_t physical);
  bool retain(SwapReference reference);
  void release(SwapReference& reference);
  SwapSnapshot snapshot() const;
  uint32_t endpointId() const;
  static SwapStatus validateHeader(const void* page, uint64_t deviceBytes, size_t& pages);

 private:
  SwapStore() = default;
  struct State;
  State* m_State = nullptr;
};
#endif
