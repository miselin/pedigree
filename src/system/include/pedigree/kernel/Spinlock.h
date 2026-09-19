/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

#include "pedigree/kernel/SpinlockWord.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

class EXPORTED_PUBLIC Spinlock {
  friend class PerProcessorScheduler;
  friend class LocksCommand;

 public:
  Spinlock();
  Spinlock(bool bLocked, bool bAvoidTracking = false);

  /**
   * Enter the critical section.
   *
   * The 'safe' param disables certain deadlock checks that might fail in
   * some circumstances (especially during multiprocessor startup). It really
   * shouldn't be used for the majority of cases.
   */
  bool acquire(bool recurse = false, bool safe = true);

  /** Exit the critical section, without restoring interrupts. */
  void exit(uintptr_t ra = 0);

  /** Exit the critical section, restoring previous interrupt state. */
  void release();

  bool acquired();

  bool interrupts() const;

  static const bool allow_recursion = true;

 private:
  /** Returns true for recursive reentry, false for a newly acquired lock. */
  bool acquireContended(bool recurse, bool safe, uintptr_t ra) NEVER_INLINE;

  /** Unlocks without restoring IRQ state, leaving nested acquisitions held. */
  void unlock(uintptr_t ra) ALWAYS_INLINE;

  /** Unwind the spinlock because a thread is releasing it. */
  void unwind() ALWAYS_INLINE;

  /** Scheduler owns tracking and IRQ restoration across these handoffs. */
  volatile processor_register_t* deferredReleaseWord();
  void unlockForScheduler();

  /** Track the release of this lock. */
  void trackRelease(uintptr_t ra) const;

  uintptr_t acquisitionAddress() const;
  void badMagic(uintptr_t ra) const NEVER_INLINE;
  void badReleaseInterrupts() const NEVER_INLINE;
  void deadlock(uintptr_t ra, bool releasing, uintptr_t acquiredAt = 0) NEVER_INLINE NORETURN;

  NOT_COPYABLE_OR_ASSIGNABLE(Spinlock);

  SpinlockWord m_Lock;
  bool m_bInterrupts = false;

#if SPINLOCK_DIAGNOSTICS
  uint64_t m_Sentinel = 0;
  uint32_t m_Magic = 0xdeadbaba;
  uint32_t m_MagicAlign = 0;
  uintptr_t m_Ra = 0;
#endif

  void* m_pOwner = nullptr;
  size_t m_Level = 0;
  size_t m_OwnedProcessor = ~0;

  bool m_bAvoidTracking = false;
};

#endif
