/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_IRQDIAGNOSTICSNAPSHOTSTORE_H
#define PEDIGREE_KERNEL_MACHINE_IRQDIAGNOSTICSNAPSHOTSTORE_H
#include "pedigree/kernel/machine/IrqManager.h"

#include <config.h>

static_assert(__atomic_always_lock_free(sizeof(size_t), nullptr),
              "IRQ diagnostic publication words must be lock-free");

/**
 * Per-line immutable snapshot publication for stopped-world debuggers and
 * best-effort live readers.
 *
 * A reader claims one of three banks before copying it. A writer makes one
 * nonblocking attempt to claim an inactive, unclaimed bank; diagnostics are
 * allowed to remain stale rather than becoming an IRQ liveness dependency.
 */
template <size_t LineCount>
class IrqDiagnosticSnapshotStore {
 public:
  IrqDiagnosticSnapshotStore()
      : m_Banks(),
        m_Publications(),
        m_BankClaims(),
        m_WriterAdmissions(),
        m_MissedPublications(),
        m_Dirty() {
    for (size_t line = 0; line < LineCount; ++line) {
      for (size_t bank = 0; bank < BankCount; ++bank) {
        m_Banks[line][bank].line = static_cast<uint8_t>(line);
      }
      m_Publications[line] = static_cast<size_t>(1) << BankBits;
    }
  }

  /** Begins one nonblocking publication, returning its private bank. */
  IrqLineDiagnosticSnapshot* beginPublication(size_t line, size_t& targetBank) {
    if (line >= LineCount) {
      return nullptr;
    }

    size_t expectedAdmission = 0;
    if (!__atomic_compare_exchange_n(&m_WriterAdmissions[line], &expectedAdmission,
                                     static_cast<size_t>(1), false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)) {
      missed(line);
      return nullptr;
    }

    const size_t publication = __atomic_load_n(&m_Publications[line], __ATOMIC_ACQUIRE);
    const size_t activeBank = publication & BankMask;
    for (size_t offset = 1; offset < BankCount; ++offset) {
      const size_t candidate = (activeBank + offset) % BankCount;
      size_t expectedClaim = 0;
      if (__atomic_compare_exchange_n(&m_BankClaims[line][candidate], &expectedClaim, WriterClaim,
                                      false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        targetBank = candidate;
        return &m_Banks[line][candidate];
      }
    }

    __atomic_store_n(&m_WriterAdmissions[line], static_cast<size_t>(0), __ATOMIC_RELEASE);
    missed(line);
    return nullptr;
  }

  /** Makes a completely populated private bank visible to readers. */
  void finishPublication(size_t line, size_t targetBank) {
    if (line >= LineCount || targetBank >= BankCount) {
      return;
    }

    const size_t current = __atomic_load_n(&m_Publications[line], __ATOMIC_RELAXED);
    size_t generation = (current >> BankBits) + 1;
    if (!generation) {
      ++generation;
    }

    // Readers cannot select an inactive bank. Drop the private claim
    // before the release-store which makes this bank active.
    __atomic_store_n(&m_BankClaims[line][targetBank], static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(&m_Publications[line], (generation << BankBits) | targetBank,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&m_WriterAdmissions[line], static_cast<size_t>(0), __ATOMIC_RELEASE);
  }

  /** Copies one bank without waiting for a publisher or taking a lock. */
  bool snapshot(size_t line, IrqLineDiagnosticSnapshot& out) const {
    if (line >= LineCount) {
      return false;
    }

    for (size_t attempt = 0; attempt < SnapshotAttempts; ++attempt) {
      const size_t publication = __atomic_load_n(&m_Publications[line], __ATOMIC_ACQUIRE);
      const size_t bank = publication & BankMask;
      if (bank >= BankCount) {
        return false;
      }

      size_t claim = __atomic_load_n(&m_BankClaims[line][bank], __ATOMIC_ACQUIRE);
      if (claim & WriterClaim) {
        continue;
      }
      if (!__atomic_compare_exchange_n(&m_BankClaims[line][bank], &claim, claim + 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        continue;
      }

      if (__atomic_load_n(&m_Publications[line], __ATOMIC_ACQUIRE) != publication) {
        __atomic_fetch_sub(&m_BankClaims[line][bank], static_cast<size_t>(1), __ATOMIC_RELEASE);
        continue;
      }

      out = m_Banks[line][bank];
      out.snapshotGeneration = publication >> BankBits;
      __atomic_fetch_sub(&m_BankClaims[line][bank], static_cast<size_t>(1), __ATOMIC_RELEASE);
      return true;
    }
    return false;
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  /** Holds the currently published bank to exercise reader/writer rotation.
   */
  bool claimPublishedBankForTest(size_t line, size_t& bank) const {
    if (line >= LineCount) {
      return false;
    }

    const size_t publication = __atomic_load_n(&m_Publications[line], __ATOMIC_ACQUIRE);
    bank = publication & BankMask;
    if (bank >= BankCount) {
      return false;
    }

    size_t claim = __atomic_load_n(&m_BankClaims[line][bank], __ATOMIC_ACQUIRE);
    if ((claim & WriterClaim) ||
        !__atomic_compare_exchange_n(&m_BankClaims[line][bank], &claim, claim + 1, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      return false;
    }

    if (__atomic_load_n(&m_Publications[line], __ATOMIC_ACQUIRE) != publication) {
      __atomic_fetch_sub(&m_BankClaims[line][bank], static_cast<size_t>(1), __ATOMIC_RELEASE);
      return false;
    }
    return true;
  }

  void releasePublishedBankForTest(size_t line, size_t bank) const {
    if (line < LineCount && bank < BankCount) {
      __atomic_fetch_sub(&m_BankClaims[line][bank], static_cast<size_t>(1), __ATOMIC_RELEASE);
    }
  }
#endif

  size_t missedPublications(size_t line) const {
    return line < LineCount ? __atomic_load_n(&m_MissedPublications[line], __ATOMIC_RELAXED) : 0;
  }

  /** Records a bounded publication which could not obtain coherent input. */
  void recordMissedPublication(size_t line) {
    if (line < LineCount) {
      missed(line);
    }
  }

  /** Reports and clears a missed writer rendezvous for bounded refresh. */
  bool consumeDirty(size_t line) {
    return line < LineCount &&
           __atomic_exchange_n(&m_Dirty[line], static_cast<size_t>(0), __ATOMIC_ACQ_REL) != 0;
  }

 private:
  static constexpr size_t BankCount = 3;
  static constexpr size_t BankBits = 2;
  static constexpr size_t BankMask = (1U << BankBits) - 1;
  static constexpr size_t SnapshotAttempts = 4;
  static constexpr size_t WriterClaim = static_cast<size_t>(1) << ((sizeof(size_t) * 8) - 1);

  void missed(size_t line) {
    __atomic_store_n(&m_Dirty[line], static_cast<size_t>(1), __ATOMIC_RELEASE);
    __atomic_add_fetch(&m_MissedPublications[line], static_cast<size_t>(1), __ATOMIC_RELAXED);
  }

  IrqLineDiagnosticSnapshot m_Banks[LineCount][BankCount];
  size_t m_Publications[LineCount];
  mutable size_t m_BankClaims[LineCount][BankCount];
  size_t m_WriterAdmissions[LineCount];
  size_t m_MissedPublications[LineCount];
  size_t m_Dirty[LineCount];
};

#endif
