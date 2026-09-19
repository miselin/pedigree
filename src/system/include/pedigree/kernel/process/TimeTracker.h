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

#ifndef _PROCESS_TIME_TRACKER_H
#define _PROCESS_TIME_TRACKER_H
#include "pedigree/kernel/compiler.h"

#include <config.h>
#include <stddef.h>

class Process;
class Thread;

/**
 * Tracks time spent and assigns it to the given process, in an RAII way.
 *
 * Mostly useful for things like syscalls where there's a definitive entry and
 * exit at which time should be tracked differently.
 */
class TimeTracker {
 public:
  // Expose member initialization so stack auto-initialization does not fill
  // the whole object before the constructor writes the same fields.
  ALWAYS_INLINE TimeTracker(Process* pProcess, bool fromUserspace,
                            bool entryInterruptsAlreadyDisabled = false)
      : m_pProcess(pProcess),
        m_pThread(nullptr),
        m_bFromUserspace(fromUserspace)
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
        ,
        m_bSyscallAttributed(false),
        m_PreviousSyscallTimingSlot(~static_cast<size_t>(0))
#endif
  {
    initialise(entryInterruptsAlreadyDisabled);
  }
  ALWAYS_INLINE ~TimeTracker() {
    if (m_pProcess && m_pThread)
      finish();
  }

  /** Completes accounting before a no-return architectural transition. */
  void finish();

  /** Completes accounting before restoring a saved kernel continuation. */
  void finishInKernel();

  /** Retires the tracker while leaving the kernel interval for user return. */
  ALWAYS_INLINE void finishForUserReturn() {
    Thread* thread = m_pThread;
    if (!m_pProcess || !thread)
      return;

    // The architecture tail owns the final accounting sample. Keep retirement
    // visible here so destruction can omit a second completion attempt.
    m_pProcess = nullptr;
    m_pThread = nullptr;
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
    restoreSyscallTiming(thread);
#endif
  }

  /** Attributes this userspace Linux syscall to the active Process. */
  void attributeSyscall(size_t rawNumber);

 private:
  void initialise(bool entryInterruptsAlreadyDisabled);
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
  void restoreSyscallTiming(Thread* thread);
#endif

  Process* m_pProcess;
  Thread* m_pThread;
  bool m_bFromUserspace;
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
  bool m_bSyscallAttributed;
  size_t m_PreviousSyscallTimingSlot;
#endif
};

#endif
