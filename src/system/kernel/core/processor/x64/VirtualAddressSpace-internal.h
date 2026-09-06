#ifndef KERNEL_X64_VIRTUALADDRESSSPACE_INTERNAL_H
#define KERNEL_X64_VIRTUALADDRESSSPACE_INTERNAL_H
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/Processor.h"

#include "utils.h"

//
// Page Table/Directory entry flags
//
#define PAGE_PRESENT 0x01
#define PAGE_WRITE 0x02
#define PAGE_USER 0x04
#define PAGE_WRITE_COMBINE 0x08
#define PAGE_CACHE_DISABLE 0x10
#define PAGE_ACCESSED 0x20
#define PAGE_DIRTY 0x40
#define PAGE_2MB 0x80
#define PAGE_PAT 0x80
#define PAGE_GLOBAL 0x100
#define PAGE_SWAPPED 0x200
#define PAGE_COPY_ON_WRITE 0x400
#define PAGE_SHARED 0x800
// Software bits outside the physical address and protection-key fields.
#define PAGE_RUNTIME (1ULL << 55)
#define PAGE_BORROWED (1ULL << 56)
#define PAGE_NO_ACCESS (1ULL << 57)
#define PAGE_WRITE_PROTECTED (1ULL << 58)
#define PAGE_NX 0x8000000000000000
#define PAGE_WRITE_THROUGH (PAGE_PAT | PAGE_WRITE_COMBINE)

//
// Macros
//
#define PML4_INDEX(x) ((reinterpret_cast<uintptr_t>(x) >> 39) & 0x1FF)
#define PAGE_DIRECTORY_POINTER_INDEX(x) ((reinterpret_cast<uintptr_t>(x) >> 30) & 0x1FF)
#define PAGE_DIRECTORY_INDEX(x) ((reinterpret_cast<uintptr_t>(x) >> 21) & 0x1FF)
#define PAGE_TABLE_INDEX(x) ((reinterpret_cast<uintptr_t>(x) >> 12) & 0x1FF)

#define TABLE_ENTRY(table, index) (&physicalAddress(reinterpret_cast<uint64_t*>(table))[index])

#define PAGE_GET_FLAGS(x) (*x & 0x8780000000000FFFULL)
#define PAGE_SET_FLAGS(x, f) *x = (*x & ~0x8780000000000FFFULL) | f
#define PAGE_GET_PHYSICAL_ADDRESS(x) (*x & ~0x8780000000000FFFULL)

static void beginMappingInvalidation(TlbInvalidationGuard& invalidation) {
  switch (Processor::beginTlbInvalidation(invalidation)) {
    case TlbInvalidationResult::Success:
      return;
    case TlbInvalidationResult::InvalidContext:
      panic("Mapping mutation started from an invalid TLB-shootdown context");
    case TlbInvalidationResult::UnsupportedTopology:
      panic("Mapping mutation has no safe all-processor TLB route");
    case TlbInvalidationResult::SerialisationTimedOut:
      if (Processor::tlbInvalidationFailureActive()) {
        Processor::setInterrupts(false);
        while (true) {
          Processor::pause();
        }
      }
      panic("Mapping mutation admission timed out");
    case TlbInvalidationResult::SubmissionFailed:
    case TlbInvalidationResult::AcknowledgementTimedOut:
    case TlbInvalidationResult::DrainTimedOut:
      panic("Mapping mutation admission returned an invalid phase result");
  }
  panic("Mapping mutation admission returned an unknown result");
}

static const char* mappingInvalidationFailureMessage(TlbInvalidationResult result) {
  switch (result) {
    case TlbInvalidationResult::InvalidContext:
      return "Mapping changed from an invalid TLB-shootdown context";
    case TlbInvalidationResult::UnsupportedTopology:
      return "Mapping has no safe all-processor TLB route";
    case TlbInvalidationResult::SerialisationTimedOut:
      return "Cross-processor TLB shootdown serialisation timed out";
    case TlbInvalidationResult::SubmissionFailed:
      return "Cross-processor TLB shootdown IPI submission failed";
    case TlbInvalidationResult::AcknowledgementTimedOut:
      return "Cross-processor TLB shootdown acknowledgement timed out";
    case TlbInvalidationResult::DrainTimedOut:
      return "Cross-processor TLB shootdown service drain timed out";
    case TlbInvalidationResult::Success:
      break;
  }
  return "Cross-processor TLB shootdown returned an unknown result";
}

/**
 * Keeps interrupts suppressed until every VAS lock and the mutation lease are
 * retired. This ordering prevents terminal processor control from racing a
 * stale PTE with either an IRQ callback or another mapper.
 */
class X64MappingMutationScope {
 public:
  X64MappingMutationScope()
      : m_Invalidation(),
        m_RestoreInterrupts(Processor::getInterrupts()),
        m_Locks(),
        m_LockCount(0),
        m_Result(TlbInvalidationResult::Success),
        m_Coordinator(false),
        m_Finished(false) {
    beginMappingInvalidation(m_Invalidation);
  }

  ~X64MappingMutationScope() {
    finish(true);
  }

  void lock(Spinlock& lock) {
    lock.acquire();
    m_Locks[m_LockCount].lock = &lock;
    m_Locks[m_LockCount].owned = true;
    ++m_LockCount;
  }

  void unlock(Spinlock& lock) {
    for (size_t i = m_LockCount; i > 0; --i) {
      LockState& state = m_Locks[i - 1];
      if (state.lock == &lock && state.owned) {
        lock.exit();
        state.owned = false;
        return;
      }
    }
    panicWithoutRestoringInterrupts("Mapping mutation released an unowned VAS lock");
  }

  void relock(Spinlock& lock) {
    for (size_t i = 0; i < m_LockCount; ++i) {
      LockState& state = m_Locks[i];
      if (state.lock == &lock && !state.owned) {
        lock.acquire();
        state.owned = true;
        return;
      }
    }
    panicWithoutRestoringInterrupts("Mapping mutation reacquired an unknown VAS lock");
  }

  bool invalidate(void* virtualAddress) {
    m_Result = Processor::invalidateAll(virtualAddress, m_Invalidation);
    if (m_Result == TlbInvalidationResult::Success) {
      return true;
    }

    m_Coordinator = m_Invalidation.closeAdmissionForTerminalFailure(m_Result);
    return false;
  }

  bool failed() const {
    return m_Result != TlbInvalidationResult::Success;
  }

  void panicInvalidationFailure() NORETURN {
    const TlbInvalidationResult result = m_Result;
    const bool coordinator = m_Coordinator;
    finish(false);

    if (!coordinator) {
      while (true) {
        Processor::pause();
      }
    }
    panic(mappingInvalidationFailureMessage(result));
  }

  void panicWithoutRestoringInterrupts(const char* message) NORETURN {
    finish(false);
    panic(message);
  }

 private:
  struct LockState {
    Spinlock* lock;
    bool owned;
  };

  void finish(bool restoreInterrupts) {
    if (m_Finished) {
      return;
    }

    Processor::setInterrupts(false);
    for (size_t i = m_LockCount; i > 0; --i) {
      LockState& state = m_Locks[i - 1];
      if (state.owned) {
        state.lock->exit();
        state.owned = false;
      }
    }
    m_Invalidation.retire();
    m_Finished = true;

    if (restoreInterrupts && m_RestoreInterrupts) {
      Processor::setInterrupts(true);
    }
  }

  TlbInvalidationGuard m_Invalidation;
  bool m_RestoreInterrupts;
  LockState m_Locks[2];
  size_t m_LockCount;
  TlbInvalidationResult m_Result;
  bool m_Coordinator;
  bool m_Finished;
};

#endif
