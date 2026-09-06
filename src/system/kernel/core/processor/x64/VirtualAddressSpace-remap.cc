#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include "VirtualAddressSpace-internal.h"
#include "VirtualAddressSpace.h"

namespace {
constexpr size_t PageSize = 4096;
constexpr uint64_t LeafFlags = PAGE_PRESENT | PAGE_NO_ACCESS | PAGE_SWAPPED;

size_t tableCapacity(uintptr_t base, size_t length) {
  if (!length) {
    return 0;
  }
  const uintptr_t last = base + length - 1;
  return ((last >> 21) - (base >> 21) + 1) + ((last >> 30) - (base >> 30) + 1) +
         ((last >> 39) - (base >> 39) + 1);
}

bool permitAllocation(ssize_t& remaining) {
  if (remaining < 0) {
    return true;
  }
  if (!remaining) {
    return false;
  }
  --remaining;
  return true;
}
}  // namespace

class X64PreparedPageRemap final : public VirtualAddressSpace::PreparedPageRemap {
 public:
  using Status = VirtualAddressSpace::RemapStatus;
  using Request = VirtualAddressSpace::PageRemapRequest;
  using Detached = VirtualAddressSpace::DetachedPage;

  X64PreparedPageRemap(X64VirtualAddressSpace& space, const Request& request)
      : m_Space(space),
        m_Request(request),
        m_Moved(request.source != request.destination),
        m_Preserved(request.oldLength < request.newLength ? request.oldLength : request.newLength),
        m_Detached(),
        m_AllowedVictim(),
        m_Spares(),
        m_RetiredTables(),
        m_SpareCount(0),
        m_UsedSpares(0),
        m_RetiredCount(0),
        m_Committed(false) {}

  ~X64PreparedPageRemap() override {
    for (size_t i = m_UsedSpares; i < m_SpareCount; ++i) {
      PhysicalMemoryManager::instance().freePage(m_Spares.get()[i]);
    }
  }

  Status prepare(ssize_t& remaining) {
    const size_t destinationPages = m_Moved ? m_Request.newLength / PageSize : 0;
    const size_t count = (m_Request.oldLength - m_Preserved) / PageSize + destinationPages;
    if (count) {
      if (!permitAllocation(remaining)) {
        return Status::NoMemory;
      }
      m_Detached = UniqueArray<Detached>::allocate(count);
      if (!m_Detached) {
        return Status::NoMemory;
      }
    }
    if (destinationPages) {
      if (!permitAllocation(remaining)) {
        return Status::NoMemory;
      }
      m_AllowedVictim = UniqueArray<unsigned char>::allocate(destinationPages);
      if (!m_AllowedVictim) {
        return Status::NoMemory;
      }
      ByteSet(m_AllowedVictim.get(), 0, destinationPages);
    }
    for (size_t i = 0; i < m_Request.victimCount; ++i) {
      const auto& range = m_Request.victims[i];
      if (!m_Moved || !m_Request.replace || !range.length || (range.base % PageSize) ||
          (range.length % PageSize) || range.base < m_Request.destination ||
          range.base - m_Request.destination > m_Request.newLength ||
          range.length > m_Request.newLength - (range.base - m_Request.destination)) {
        return Status::InvalidRange;
      }
      const size_t first = (range.base - m_Request.destination) / PageSize;
      for (size_t j = 0; j < range.length / PageSize; ++j) {
        if (m_AllowedVictim.get()[first + j]) {
          return Status::InvalidRange;
        }
        m_AllowedVictim.get()[first + j] = 1;
      }
    }
    m_Request.victims = nullptr;
    m_Request.victimCount = 0;
    const size_t spares = m_Moved ? tableCapacity(m_Request.destination, m_Preserved) : 0;
    const size_t retireCapacity =
        tableCapacity(m_Request.source, m_Request.oldLength) +
        (m_Moved ? tableCapacity(m_Request.destination, m_Request.newLength) : 0);
    if (retireCapacity) {
      if (!permitAllocation(remaining)) {
        return Status::NoMemory;
      }
      m_RetiredTables = UniqueArray<physical_uintptr_t>::allocate(retireCapacity);
      if (!m_RetiredTables) {
        return Status::NoMemory;
      }
    }
    if (spares) {
      if (!permitAllocation(remaining)) {
        return Status::NoMemory;
      }
      m_Spares = UniqueArray<physical_uintptr_t>::allocate(spares);
      if (!m_Spares) {
        return Status::NoMemory;
      }
      for (size_t i = 0; i < spares; ++i) {
        if (!permitAllocation(remaining)) {
          return Status::NoMemory;
        }
        const auto page = PhysicalMemoryManager::instance().allocatePage();
        if (!page) {
          return Status::NoMemory;
        }
        m_Spares.get()[m_SpareCount++] = page;
        ByteSet(physicalAddress(reinterpret_cast<void*>(page)), 0, PageSize);
      }
    }
    return Status::Success;
  }

  Status commit(VirtualAddressSpace::RemapAdmission admission, void* context) override {
    if (m_Committed || !admission) {
      return Status::InvalidRange;
    }
    size_t detachedCount = 0, removedPages = 0;
    {
      X64MappingMutationScope mutation;
      mutation.lock(m_Space.m_Lock);
      for (size_t offset = 0; offset < m_Request.oldLength; offset += PageSize) {
        uint64_t* leaf = nullptr;
        const auto status = lookup(m_Request.source + offset, leaf);
        if (status != Status::Success) {
          return status;
        }
      }
      for (size_t offset = 0; offset < m_Request.newLength; offset += PageSize) {
        uint64_t* leaf = nullptr;
        const auto status = lookup(m_Request.destination + offset, leaf);
        if (status != Status::Success) {
          return status;
        }
        if (!m_Moved && offset < m_Request.oldLength) {
          continue;
        }
        if (leaf && (*leaf & LeafFlags) &&
            (!m_Moved || !m_Request.replace || !m_AllowedVictim.get()[offset / PageSize])) {
          return Status::InvalidRange;
        }
      }
      // Nothing below can allocate or fail. In particular, a concurrent CoW
      // replacement is now excluded and its latest leaf is the transferred one.
      if (!admission(context)) {
        return Status::Retry;
      }
      for (size_t offset = m_Preserved; offset < m_Request.oldLength; offset += PageSize) {
        removedPages += detach(m_Request.source + offset, m_Detached.get()[detachedCount++]);
      }
      if (m_Moved) {
        for (size_t offset = 0; offset < m_Request.newLength; offset += PageSize) {
          removedPages += detach(m_Request.destination + offset, m_Detached.get()[detachedCount++]);
        }
        for (size_t offset = 0; offset < m_Preserved; offset += PageSize) {
          uint64_t* source = nullptr;
          lookup(m_Request.source + offset, source);
          if (!source || !(*source & LeafFlags)) {
            continue;
          }
          uint64_t* destination = ensureLeaf(m_Request.destination + offset);
          const uint64_t value = __atomic_exchange_n(source, uint64_t{0}, __ATOMIC_ACQ_REL);
          __atomic_store_n(destination, value, __ATOMIC_RELEASE);
        }
      }
      retireEmptyTables(m_Request.source, m_Request.oldLength);
      if (m_Moved) {
        retireEmptyTables(m_Request.destination, m_Request.newLength);
      }
      for (size_t offset = 0; offset < m_Request.oldLength; offset += PageSize) {
        if (!m_Space.invalidateMapping(reinterpret_cast<void*>(m_Request.source + offset),
                                       mutation)) {
          mutation.panicInvalidationFailure();
        }
      }
      if (m_Moved || m_Request.newLength > m_Request.oldLength) {
        const size_t begin = m_Moved ? 0 : m_Request.oldLength;
        for (size_t offset = begin; offset < m_Request.newLength; offset += PageSize) {
          if (!m_Space.invalidateMapping(reinterpret_cast<void*>(m_Request.destination + offset),
                                         mutation)) {
            mutation.panicInvalidationFailure();
          }
        }
      }
      m_Committed = true;
    }
    for (size_t i = 0; i < m_RetiredCount; ++i) {
      PhysicalMemoryManager::instance().freePage(m_RetiredTables.get()[i]);
    }
    m_RetiredCount = 0;
    Thread* thread = Processor::information().getCurrentThread();
    if (thread && thread->getParent()) {
      thread->getParent()->trackPages(-static_cast<ssize_t>(removedPages), 0, 0);
    }
    return Status::Success;
  }

  const Detached* detachedPages() const override {
    return m_Committed ? m_Detached.get() : nullptr;
  }
  size_t detachedPageCount() const override {
    return m_Committed ? (m_Request.oldLength - m_Preserved) / PageSize +
                             (m_Moved ? m_Request.newLength / PageSize : 0)
                       : 0;
  }

 private:
  Status lookup(uintptr_t address, uint64_t*& leaf) {
    leaf = nullptr;
    uint64_t table = m_Space.m_PhysicalPML4;
    for (size_t shift = 39; shift > 12; shift -= 9) {
      uint64_t* entry = TABLE_ENTRY(table, (address >> shift) & 511);
      if (!(*entry & PAGE_PRESENT)) {
        return Status::Success;
      }
      if (*entry & PAGE_2MB) {
        return Status::Unsupported;
      }
      table = PAGE_GET_PHYSICAL_ADDRESS(entry);
    }
    leaf = TABLE_ENTRY(table, (address >> 12) & 511);
    if ((*leaf & PAGE_SWAPPED) || ((*leaf & LeafFlags) && !(*leaf & PAGE_USER))) {
      return Status::Unsupported;
    }
    return Status::Success;
  }

  bool detach(uintptr_t address, Detached& result) {
    result = {address, 0, 0, false};
    uint64_t* leaf = nullptr;
    lookup(address, leaf);
    if (!leaf || !(*leaf & LeafFlags)) {
      return false;
    }
    const uint64_t value = __atomic_exchange_n(leaf, uint64_t{0}, __ATOMIC_ACQ_REL);
    result.physical = value & ~0x8780000000000FFFULL;
    result.flags = m_Space.fromFlags(value & 0x8780000000000FFFULL, true);
    result.mapped = true;
    return true;
  }

  uint64_t* ensureLeaf(uintptr_t address) {
    uint64_t table = m_Space.m_PhysicalPML4;
    for (size_t shift = 39; shift > 12; shift -= 9) {
      uint64_t* entry = TABLE_ENTRY(table, (address >> shift) & 511);
      if (!(*entry & PAGE_PRESENT)) {
        assert(m_UsedSpares < m_SpareCount);
        *entry = m_Spares.get()[m_UsedSpares++] | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
      }
      table = PAGE_GET_PHYSICAL_ADDRESS(entry);
    }
    return TABLE_ENTRY(table, (address >> 12) & 511);
  }

  void retireEmptyTables(uintptr_t base, size_t length) {
    const uintptr_t end = base + length;
    while (base < end) {
      physical_uintptr_t tables[3];
      const size_t count = m_Space.detachEmptyTables(reinterpret_cast<void*>(base), tables);
      for (size_t i = 0; i < count; ++i) {
        m_RetiredTables.get()[m_RetiredCount++] = tables[i];
      }
      base = (base | ((uintptr_t{1} << 21) - 1)) + 1;
    }
  }

  X64VirtualAddressSpace& m_Space;
  Request m_Request;
  bool m_Moved;
  size_t m_Preserved;
  UniqueArray<Detached> m_Detached;
  UniqueArray<unsigned char> m_AllowedVictim;
  UniqueArray<physical_uintptr_t> m_Spares, m_RetiredTables;
  size_t m_SpareCount, m_UsedSpares, m_RetiredCount;
  bool m_Committed;
};

VirtualAddressSpace::RemapStatus X64VirtualAddressSpace::prepareRemap(
    const PageRemapRequest& request, UniquePointer<PreparedPageRemap>& plan) {
  plan.reset();
  const auto validRange = [this](uintptr_t base, size_t length) {
    if (!length || (base % PageSize) || (length % PageSize) || length > ~uintptr_t{0} - base) {
      return false;
    }
    const uintptr_t end = base + length;
    return (base >= getUserStart() && end <= getUserReservedStart()) ||
           (base >= getDynamicStart() && end <= getDynamicEnd());
  };
  if (m_bKernelSpace || !validRange(request.source, request.oldLength) ||
      !validRange(request.destination, request.newLength) ||
      (request.victimCount && !request.victims)) {
    return RemapStatus::InvalidRange;
  }
  if (request.oldLength / PageSize > MaximumRemapPages ||
      request.newLength / PageSize > MaximumRemapPages || request.victimCount > MaximumRemapPages) {
    return RemapStatus::NoMemory;
  }
  if (request.source != request.destination &&
      request.source < request.destination + request.newLength &&
      request.destination < request.source + request.oldLength) {
    return RemapStatus::InvalidRange;
  }
  ssize_t remaining =
      __atomic_exchange_n(&m_RemapPreparationFailure, ssize_t{-1}, __ATOMIC_ACQ_REL);
  if (!permitAllocation(remaining)) {
    return RemapStatus::NoMemory;
  }
  auto* prepared = new X64PreparedPageRemap(*this, request);
  if (!prepared) {
    return RemapStatus::NoMemory;
  }
  auto owner = UniquePointer<PreparedPageRemap>::adopt(prepared);
  const auto status = prepared->prepare(remaining);
  if (status == RemapStatus::Success) {
    plan = pedigree_std::move(owner);
  }
  return status;
}
