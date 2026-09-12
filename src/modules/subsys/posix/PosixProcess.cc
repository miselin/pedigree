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

#include "PosixProcess.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/utilities/utility.h"

#include <signal.h>

#include "IntervalTimerState.h"
#include "ProcFs.h"
#include "TerminalControl.h"
#include "modules/system/vfs/VFS.h"

PosixProcess::PosixProcess()
    : Process(DeferredPublication()),
      m_AccountingLifetime(false),
      m_SessionId(0),
      m_pProcessGroup(nullptr),
      m_GroupPrevious(nullptr),
      m_GroupNext(nullptr),
      m_ExecCommitted(false),
      m_GroupMembership(NoGroup),
      m_Mask(0),
      m_RealIntervalTimer(this, IntervalTimer::Hardware),
      m_VirtualIntervalTimer(this, IntervalTimer::Virtual),
      m_ProfileIntervalTimer(this, IntervalTimer::Profile),
      m_Credentials(),
      m_bRegistered(false) {
  initializeJobControl(nullptr);
  enableTimeAccountingReports();
}

/** Copy constructor. */
PosixProcess::PosixProcess(Process* pParent, bool bCopyOnWrite,
                           FilesystemContextMode filesystemContext, bool emptyAddressSpace)
    : Process(DeferredPublication(), pParent, bCopyOnWrite, filesystemContext, emptyAddressSpace),
      m_AccountingLifetime(true),
      m_SessionId(0),
      m_pProcessGroup(nullptr),
      m_GroupPrevious(nullptr),
      m_GroupNext(nullptr),
      m_ExecCommitted(false),
      m_GroupMembership(NoGroup),
      m_Mask(0),
      m_RealIntervalTimer(this, IntervalTimer::Hardware),
      m_VirtualIntervalTimer(this, IntervalTimer::Virtual),
      m_ProfileIntervalTimer(this, IntervalTimer::Profile),
      m_Credentials(),
      m_bRegistered(false) {
  initializeJobControl(pParent);
  enableTimeAccountingReports();

  if (pParent->getType() == Posix) {
    PosixProcess* pPosixParent = static_cast<PosixProcess*>(pParent);

    // Child inherits parent's mask.
    m_Mask = pPosixParent->getMask();

    m_Credentials = pPosixParent->snapshotCredentials();
  } else {
    FilesystemCredentials inherited;
    if (pParent->snapshotFilesystemCredentials(nullptr, inherited)) {
      m_Credentials.ruid = m_Credentials.euid = m_Credentials.suid = inherited.uid;
      m_Credentials.rgid = m_Credentials.egid = m_Credentials.sgid = inherited.gid;
      m_Credentials.groupCount = inherited.groupCount;
      for (size_t i = 0; i < inherited.groupCount; ++i)
        m_Credentials.groups[i] = inherited.groups[i];
    }
  }
}

PosixProcess::~PosixProcess() {
  prepareForDestruction();
  leaveProcessGroup();
  unregisterProcess();
}

void PosixProcess::publish() {
  // Scheduler enumeration must not observe this object until the caller has
  // installed all child-side state and a non-runnable initial Thread.
  assert(jobControlReady());
  Process::publish();
  registerProcess();
}

Process::ProcessType PosixProcess::getType() {
  return Posix;
}

void PosixProcess::setMask(uint32_t mask) {
  m_Mask = mask;
}

uint32_t PosixProcess::getMask() const {
  return m_Mask;
}

void PosixProcess::registerProcess() {
  Filesystem* pFs = VFS::instance().getFilesystemAt(String("/media/proc"));
  if (!pFs) {
    return;
  }

  ProcFs* pProcFs = static_cast<ProcFs*>(pFs);
  pProcFs->addProcess(this);
  m_bRegistered = true;
}

void PosixProcess::unregisterProcess() {
  if (!m_bRegistered) {
    return;
  }

  Filesystem* pFs = VFS::instance().getFilesystemAt(String("/media/proc"));
  if (!pFs) {
    return;
  }

  ProcFs* pProcFs = static_cast<ProcFs*>(pFs);
  pProcFs->removeProcess(this);
  m_bRegistered = false;
}

IntervalTimer& PosixProcess::getRealIntervalTimer() {
  return m_RealIntervalTimer;
}

IntervalTimer& PosixProcess::getVirtualIntervalTimer() {
  return m_VirtualIntervalTimer;
}

IntervalTimer& PosixProcess::getProfileIntervalTimer() {
  return m_ProfileIntervalTimer;
}

void PosixProcess::reportTimesUpdated(Time::Timestamp userTotal, Time::Timestamp total) {
  m_VirtualIntervalTimer.consumeCpuTime(userTotal);
  m_ProfileIntervalTimer.consumeCpuTime(total);
}

void PosixProcess::processTerminated() {
  // Cancel all timers.
  m_RealIntervalTimer.disarm();
  m_VirtualIntervalTimer.disarm();
  m_ProfileIntervalTimer.disarm();
  posix_account_process_exit(*this, m_AccountingLifetime);
  TerminalControl::processTerminated(*this);
}

IntervalTimer::IntervalTimer(PosixProcess* pProcess, Mode mode)
    : m_Process(pProcess),
      m_Mode(mode),
      m_Value(0),
      m_Interval(0),
      m_LastCpuTotal(0),
      m_Lock(false),
      m_Armed(false),
      m_pTimer(nullptr) {
  if (m_Mode == Hardware) {
    Timer* t = Machine::instance().getTimer();
    if (t && t->registerHandler(this)) {
      m_pTimer = t;
    } else {
      ERROR("IntervalTimer could not register its hardware callback");
    }
  } else {
    m_LastCpuTotal = absoluteCpuTotal();
  }
}

IntervalTimer::~IntervalTimer() {
  if (m_pTimer) {
    if (!m_pTimer->unregisterHandler(this)) {
      FATAL("IntervalTimer could not drain its hardware callback");
    }
    m_pTimer = nullptr;
  }
}

void IntervalTimer::setInterval(Time::Timestamp interval, Time::Timestamp* prevInterval) {
  bool needsSignal = false;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_Mode != Hardware) {
      needsSignal = advanceCpuTimeLocked(absoluteCpuTotal());
    }

    if (prevInterval) {
      *prevInterval = m_Interval;
    }
    m_Interval = interval;
  }
  if (needsSignal) {
    signal();
  }
}

void IntervalTimer::setTimerValue(Time::Timestamp value, Time::Timestamp* prevValue) {
  bool needsSignal = false;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_Mode != Hardware) {
      needsSignal = advanceCpuTimeLocked(absoluteCpuTotal());
    }

    if (prevValue) {
      *prevValue = m_Value;
    }
    m_Value = value;
    m_Armed = m_Value > 0;
  }
  if (needsSignal) {
    signal();
  }
}

void IntervalTimer::setIntervalAndValue(Time::Timestamp interval, Time::Timestamp value,
                                        Time::Timestamp* prevInterval, Time::Timestamp* prevValue) {
  bool needsSignal = false;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_Mode != Hardware) {
      needsSignal = advanceCpuTimeLocked(absoluteCpuTotal());
    }

    if (prevInterval) {
      *prevInterval = m_Interval;
    }

    if (prevValue) {
      *prevValue = m_Value;
    }

    m_Interval = interval;
    m_Value = value;
    m_Armed = m_Value > 0;
  }
  if (needsSignal) {
    signal();
  }
}

void IntervalTimer::disarm() {
  LockGuard<Spinlock> guard(m_Lock);
  if (m_Mode != Hardware) {
    const Time::Timestamp current = absoluteCpuTotal();
    if (current > m_LastCpuTotal) {
      m_LastCpuTotal = current;
    }
  }
  m_Value = 0;
  m_Interval = 0;
  m_Armed = false;
}

void IntervalTimer::getIntervalAndValue(Time::Timestamp& interval, Time::Timestamp& value) {
  bool needsSignal = false;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_Mode != Hardware) {
      needsSignal = advanceCpuTimeLocked(absoluteCpuTotal());
    }

    interval = m_Interval;
    value = m_Value;
  }
  if (needsSignal) {
    signal();
  }
}

void IntervalTimer::consumeCpuTime(Time::Timestamp absoluteTotal) {
  if (m_Mode == Hardware) {
    return;
  }

  bool needsSignal = false;
  {
    LockGuard<Spinlock> guard(m_Lock);
    needsSignal = advanceCpuTimeLocked(absoluteTotal);
  }

  if (needsSignal) {
    signal();
  }
}

Time::Timestamp IntervalTimer::absoluteCpuTotal() const {
  if (m_Mode == Virtual) {
    return m_Process->getUserTime();
  }
  if (m_Mode == Profile) {
    return m_Process->getUserTime() + m_Process->getKernelTime();
  }
  return 0;
}

bool IntervalTimer::advanceCpuTimeLocked(Time::Timestamp absoluteTotal) {
  const PosixIntervalTimerState::AbsoluteConsumption result =
      PosixIntervalTimerState::consumeAbsolute(m_Value, m_Interval, m_Armed, m_LastCpuTotal,
                                               absoluteTotal);
  m_Value = result.timer.value;
  m_Armed = result.timer.armed;
  m_LastCpuTotal = result.baseline;
  return result.timer.expired;
}

Time::Timestamp IntervalTimer::getInterval() const {
  return m_Interval;
}

Time::Timestamp IntervalTimer::getValue() const {
  return m_Value;
}

void IntervalTimer::timer(uint64_t delta) {
  if (m_Mode != Hardware) {
    return;
  }

  bool needsSignal = false;
  {
    LockGuard<Spinlock> guard(m_Lock);

    if (!m_Armed) {
      // Disarmed - ignore the timer event.
      return;
    }

    const PosixIntervalTimerState::Consumption result =
        PosixIntervalTimerState::consume(m_Value, m_Interval, m_Armed, delta);
    m_Value = result.value;
    m_Armed = result.armed;
    needsSignal = result.expired;
  }

  if (needsSignal) {
    signal();
  }
}

void IntervalTimer::signal() {
  int signal = -1;
  switch (m_Mode) {
    case Hardware:
      signal = SIGALRM;
      break;
    case Virtual:
      signal = SIGVTALRM;
      break;
    case Profile:
      signal = SIGPROF;
      break;
  }

  /// \todo sanity check that this is absolutely a PosixSubsystem
  Scheduler::ProcessLease process;
  if (!Scheduler::instance().acquireProcess(process, m_Process)) {
    return;
  }
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  Process::ThreadLease target;
  const bool targetAcquired = process->acquireProcessSignalThread(target);
  if (!pSubsystem || !targetAcquired) {
    return;
  }

  // Don't yield in the middle of the timer handler
  pSubsystem->sendSignal(target.get(), signal, false, true);
}
