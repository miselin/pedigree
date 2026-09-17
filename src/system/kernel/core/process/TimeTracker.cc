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

#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/TimeTracker.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

TimeTracker::TimeTracker(Process* pProcess, bool fromUserspace,
                         bool entryInterruptsAlreadyDisabled)
    : m_pProcess(pProcess),
      m_pThread(nullptr),
      m_bFromUserspace(fromUserspace)
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
      ,
      m_bSyscallAttributed(false),
      m_PreviousSyscallTimingSlot(Thread::NoSyscallTimingSlot)
#endif
{
  // Accounting baselines belong to the exact interrupted Thread. A Process
  // can execute on multiple CPUs and cannot provide one shared baseline.
  m_pThread = Processor::information().getCurrentThread();
  if (!m_pThread) {
    // We can get called early, so ensure we don't make any
    // assumptions about what's present.
    m_pProcess = nullptr;
    return;
  }

  Process* threadProcess = m_pThread->getParent();
  if (!threadProcess || (m_pProcess && m_pProcess != threadProcess)) {
    m_pProcess = nullptr;
    m_pThread = nullptr;
    return;
  }
  m_pProcess = threadProcess;

  // Track time already spent wherever we were previously.
  m_pThread->transitionTime(KernelTimeTransition::interrupted(m_bFromUserspace),
                            KernelTimeTransition::handler(),
                            entryInterruptsAlreadyDisabled);
}

TimeTracker::~TimeTracker() {
  finish();
}

void TimeTracker::attributeSyscall(size_t rawNumber) {
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
  if (!m_pProcess || !m_pThread || !m_bFromUserspace || m_bSyscallAttributed ||
      !m_pProcess->benchmarkSyscallTimingEnabled()) {
    return;
  }

  const size_t slot = Process::syscallTimingSlot(rawNumber);
  m_pProcess->recordSyscallTimingCall(slot);
  m_PreviousSyscallTimingSlot = m_pThread->installSyscallTimingSlot(slot);
  m_bSyscallAttributed = true;
#else
  (void)rawNumber;
#endif
}

void TimeTracker::finish() {
  Thread* thread = m_pThread;
  if (!m_pProcess || !thread)
    return;

  // Make completion idempotent before touching state so a no-return caller
  // can explicitly finish without the destructor double-charging later.
  m_pProcess = nullptr;
  m_pThread = nullptr;

  // Track time spent in the RAII section.
  thread->transitionTime(KernelTimeTransition::handler(),
                         KernelTimeTransition::resumed(m_bFromUserspace));
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
  if (m_bSyscallAttributed) {
    thread->restoreSyscallTimingSlot(m_PreviousSyscallTimingSlot);
    m_bSyscallAttributed = false;
  }
#endif
}

void TimeTracker::finishInKernel() {
  Thread* thread = m_pThread;
  if (!m_pProcess || !thread)
    return;

  // Make completion idempotent before touching state so a no-return caller
  // can explicitly finish without the destructor double-charging later.
  m_pProcess = nullptr;
  m_pThread = nullptr;

  // Event return restores a saved kernel frame before the outer architecture
  // tail makes the eventual Kernel -> User transition.
  thread->transitionTime(KernelTimeTransition::handler(), KernelTimeTransition::handler());
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
  if (m_bSyscallAttributed) {
    thread->restoreSyscallTimingSlot(m_PreviousSyscallTimingSlot);
    m_bSyscallAttributed = false;
  }
#endif
}

void TimeTracker::finishForUserReturn() {
  Thread* thread = m_pThread;
  if (!m_pProcess || !thread)
    return;

  // The final Kernel -> User transition owns the complete interval for a
  // clean syscall. Retire this object without publishing an intermediate
  // sample or changing the thread's accounting mode.
  m_pProcess = nullptr;
  m_pThread = nullptr;
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
  if (m_bSyscallAttributed) {
    thread->restoreSyscallTimingSlot(m_PreviousSyscallTimingSlot);
    m_bSyscallAttributed = false;
  }
#endif
}
