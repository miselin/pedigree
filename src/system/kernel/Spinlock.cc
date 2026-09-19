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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#if TRACK_LOCKS
#include "pedigree/kernel/debugger/commands/LocksCommand.h"
#endif

Spinlock::Spinlock() = default;

Spinlock::Spinlock(bool bLocked, bool bAvoidTracking)
    : m_Lock(bLocked), m_bAvoidTracking(bAvoidTracking) {}

bool Spinlock::acquire(bool recurse, bool safe) {
  // Keep this local until we own the lock: another CPU (or a preempting
  // thread before CLI) must not overwrite the owner's restoration state.
  const bool interrupts = Processor::getInterrupts();
  if (interrupts)
    Processor::setInterrupts(false);

  uintptr_t ra = 0;
#if SPINLOCK_DIAGNOSTICS
  ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  if (UNLIKELY(m_Magic != 0xdeadbaba))
    badMagic(ra);
#endif

#if TRACK_LOCKS
  if (!m_bAvoidTracking) {
    g_LocksCommand.clearFatal();
    if (!g_LocksCommand.lockAttempted(this, Processor::index(), interrupts))
      panic("Lock checker disallowed spinlock acquisition");
    g_LocksCommand.setFatal();
  }
#endif

  const bool reentered = UNLIKELY(!m_Lock.tryAcquire()) && acquireContended(recurse, safe, ra);
  if (!reentered) {
    m_bInterrupts = interrupts;
    __atomic_store_n(&m_OwnedProcessor, Processor::index(), __ATOMIC_RELAXED);
    if (recurse) {
      m_pOwner = Processor::information().getCurrentThread();
      m_Level = 1;
    }
#if SPINLOCK_DIAGNOSTICS
    m_Ra = ra;
#endif
  }

#if TRACK_LOCKS
  if (!m_bAvoidTracking) {
    g_LocksCommand.clearFatal();
    if (!g_LocksCommand.lockAcquired(this, Processor::index(), interrupts))
      panic("Lock checker disallowed acquired spinlock");
    g_LocksCommand.setFatal();
  }
#endif
  return true;
}

bool Spinlock::acquireContended(bool recurse, bool safe, uintptr_t ra) {
  const size_t processorId = Processor::index();
  do {
    const size_t owner = __atomic_load_n(&m_OwnedProcessor, __ATOMIC_RELAXED);
    // Test CPU identity first: other CPUs must not inspect recursion state
    // which only its owner can access. Early boot threads may all be null.
    if (owner == processorId && recurse && m_Level &&
        m_pOwner == Processor::information().getCurrentThread()) {
      ++m_Level;
      return true;
    }

#if TRACK_LOCKS
    if (!m_bAvoidTracking) {
      g_LocksCommand.clearFatal();
      if (!g_LocksCommand.checkState(this, processorId))
        panic("Lock checker rejected contended spinlock");
      g_LocksCommand.setFatal();
    }
#endif

#if MULTIPROCESSOR
    if (Processor::getCount() > 1 && (!safe || owner != processorId)) {
      // Read while occupied instead of repeatedly issuing locked RMWs.
      Processor::pause();
      continue;
    }
#endif
    deadlock(ra, false);
  } while (m_Lock.acquired() || !m_Lock.tryAcquire());
  return false;
}

void Spinlock::trackRelease(uintptr_t ra) const {
#if TRACK_LOCKS
  if (!m_bAvoidTracking) {
    g_LocksCommand.clearFatal();
    const size_t processorId = m_OwnedProcessor == ~size_t(0) ? ~0U : m_OwnedProcessor;
    if (!g_LocksCommand.lockReleased(this, processorId))
      panic("Lock checker disallowed spinlock release");
    g_LocksCommand.setFatal();
  }
#endif
}

inline void Spinlock::unlock(uintptr_t ra) {
#if SPINLOCK_DIAGNOSTICS
  if (UNLIKELY(Processor::getInterrupts()))
    badReleaseInterrupts();
  if (UNLIKELY(m_Magic != 0xdeadbaba))
    badMagic(ra);
#endif

#if TRACK_LOCKS
  // Cross-CPU releases retire the acquisition CPU's entry before clearing it.
  trackRelease(ra);
#endif
  if (m_Level && --m_Level)
    return;

  m_pOwner = nullptr;
  __atomic_store_n(&m_OwnedProcessor, ~size_t(0), __ATOMIC_RELAXED);
#if SPINLOCK_DIAGNOSTICS
  // Save diagnostic data before publishing unlock: a new owner may immediately
  // overwrite the lock's fields. Nothing may touch them after publication.
  const uintptr_t acquiredAt = m_Ra;
  m_Ra = 0;
  if (UNLIKELY(!m_Lock.releaseChecked())) {
    deadlock(ra, true, acquiredAt);
  }
#else
  m_Lock.release();
#endif
}

void Spinlock::exit(uintptr_t ra) {
  unlock(ra);
}

void Spinlock::release() {
  // Only the outermost recursive release restores IRQs. Capture before unlock,
  // since another CPU can acquire and change the saved state immediately.
  const bool interrupts = m_bInterrupts && m_Level <= 1;
  uintptr_t ra = 0;
#if SPINLOCK_DIAGNOSTICS
  ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
  unlock(ra);
  if (interrupts)
    Processor::setInterrupts(true);
}

inline void Spinlock::unwind() {
  m_Level = 0;
  m_pOwner = nullptr;
  __atomic_store_n(&m_OwnedProcessor, ~size_t(0), __ATOMIC_RELAXED);
#if SPINLOCK_DIAGNOSTICS
  m_Ra = 0;
#endif
}

volatile processor_register_t* Spinlock::deferredReleaseWord() {
  unwind();
  return &m_Lock.m_Value;
}

void Spinlock::unlockForScheduler() {
  unwind();
  m_Lock.release();
}

bool Spinlock::acquired() {
  return m_Lock.acquired();
}

bool Spinlock::interrupts() const {
  return m_bInterrupts;
}

uintptr_t Spinlock::acquisitionAddress() const {
#if SPINLOCK_DIAGNOSTICS
  return m_Ra;
#else
  return 0;
#endif
}

void Spinlock::badMagic(uintptr_t ra) const {
#if SPINLOCK_DIAGNOSTICS
  WARNING(" --> fail: sentinels: before=" << Hex << m_Sentinel << " after=" << m_MagicAlign);
  FATAL_NOLOCK("Wrong magic in Spinlock [" << Hex << m_Magic << " should be 0xdeadbaba] [this="
                                           << reinterpret_cast<uintptr_t>(this)
                                           << "] return=" << ra);
#endif
  panic("Corrupt spinlock");
}

void Spinlock::badReleaseInterrupts() const {
  FATAL_NOLOCK("Spinlock: release() called with interrupts enabled.");
  panic("Spinlock released with interrupts enabled");
}

void Spinlock::deadlock(uintptr_t ra, bool releasing, uintptr_t acquiredAt) {
  // Logging/debugger backtraces may themselves need the deadlocked lock.
  const bool locked = m_Lock.acquired();
  if (!releasing)
    acquiredAt = acquisitionAddress();
  m_Lock.release();
  ERROR_NOLOCK("Spinlock deadlocked in " << (releasing ? "release" : "acquire"));
  ERROR_NOLOCK(" -> my return address is " << Hex << ra);
  ERROR_NOLOCK(" -> return address of other locker is " << Hex << acquiredAt);
  FATAL_NOLOCK("Spinlock has deadlocked, spinlock is " << Hex << reinterpret_cast<uintptr_t>(this)
                                                       << ", locked=" << locked << ".");
  panic("Spinlock has deadlocked");
}
