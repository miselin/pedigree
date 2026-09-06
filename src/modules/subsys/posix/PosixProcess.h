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

#ifndef POSIX_PROCESS_H
#define POSIX_PROCESS_H

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/TimerHandler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/processor/types.h"

#include "PosixSubsystem.h"
#include "credential-state.h"

class PosixProcess;
class Timer;

class ProcessGroup {
  friend class PosixProcess;
  friend class ProcessGroupManager;

 public:
  ProcessGroup() = default;
  virtual ~ProcessGroup();

  int processGroupId = 0;
  size_t sessionId = 0;
  PosixProcess* Leader = nullptr;

 private:
  ProcessGroup(const ProcessGroup&) = delete;
  ProcessGroup& operator=(const ProcessGroup&) = delete;
  PosixProcess* firstMember = nullptr;
  size_t memberCount = 0;
  ProcessGroup* registryNext = nullptr;
  bool registered = false;
};

class IntervalTimer : public TimerHandler {
 public:
  enum Mode {
    /// Hardware-backed timer (wall time).
    Hardware = 0,
    /// CPU time in user mode only.
    Virtual,
    /// CPU time in user and system.
    Profile
  };

  /// Setting hw=true will use hardware. hw=false requires adjust() to be
  /// called to be able to trigger timers.
  IntervalTimer(PosixProcess* pProcess, Mode mode = Hardware);
  virtual ~IntervalTimer();

  /// Set the interval for the timer, which is loaded once the timer expires.
  /// Set zero to make a non-reloading timer.
  void setInterval(Time::Timestamp interval, Time::Timestamp* prevInterval = nullptr);

  /// Set the current value of the timer.
  void setTimerValue(Time::Timestamp value, Time::Timestamp* prevValue = nullptr);

  /// Set both interval and value atomically.
  void setIntervalAndValue(Time::Timestamp interval, Time::Timestamp value,
                           Time::Timestamp* prevInterval = nullptr,
                           Time::Timestamp* prevValue = nullptr);

  /** Disarms without delivering an expiry during process teardown. */
  void disarm();

  void getIntervalAndValue(Time::Timestamp& interval, Time::Timestamp& value);

  /** Advances a CPU timer to a monotonic absolute process-time snapshot. */
  void consumeCpuTime(Time::Timestamp absoluteTotal);

  Time::Timestamp getInterval() const;
  Time::Timestamp getValue() const;

 private:
  virtual void timer(uint64_t delta);

  Time::Timestamp absoluteCpuTotal() const;
  bool advanceCpuTimeLocked(Time::Timestamp absoluteTotal);
  void signal();

  PosixProcess* m_Process;
  Mode m_Mode;
  Time::Timestamp m_Value;
  Time::Timestamp m_Interval;
  Time::Timestamp m_LastCpuTotal;
  Spinlock m_Lock;
  bool m_Armed;
  Timer* m_pTimer;
};

class EXPORTED_PUBLIC PosixProcess : public Process {
  friend class ProcessGroup;

 public:
  /** Defines what status this Process has within its group */
  enum Membership {
    /** Group leader. The one who created the group, and whose PID was
     * absorbed to become the Process Group ID.
     */
    Leader = 0,

    /** Group member. These processes have a unique Process ID. */
    Member,

    /** Not in a group. */
    NoGroup
  };

  PosixProcess();

  /** Copy constructor. */
  PosixProcess(Process* pParent, bool bCopyOnWrite = true);
  virtual ~PosixProcess();

  /**
   * Publishes a fully assembled POSIX process. Call after its subsystem,
   * mappings, descriptors, and delayed initial Thread are ready.
   */
  void publish();

  void setProcessGroup(ProcessGroup* newGroup);
  void inheritProcessGroup(PosixProcess* parent);
  ProcessGroup* getProcessGroup() const;
  bool getProcessGroupId(size_t& groupId) const;
  void leaveProcessGroup();

  void setGroupMembership(Membership type);
  Membership getGroupMembership() const;

  size_t getSessionId() const;
  bool sharesSession(const PosixProcess& other) const;
  bool jobControlReady() const;
  void markExecCommitted();
  bool hasExecCommitted() const;
  int createSession();
  int changeProcessGroup(PosixProcess& caller, int groupId);

  virtual ProcessType getType();

  void setMask(uint32_t mask);
  uint32_t getMask() const;

  IntervalTimer& getRealIntervalTimer();
  IntervalTimer& getVirtualIntervalTimer();
  IntervalTimer& getProfileIntervalTimer();

  using CredentialSnapshot = PosixCredentials::Snapshot;
  using CredentialChange = PosixCredentials::Change;
  using CredentialStatus = PosixCredentials::Status;
  CredentialSnapshot snapshotCredentials() const;
  FilesystemCredentials realFilesystemCredentials() const;
  bool snapshotFilesystemCredentials(const Thread*, FilesystemCredentials&) const override;
  bool installUserIdentity(User*, Group*, const uint32_t*, size_t) override;
  CredentialStatus changeCredentials(Thread&, CredentialChange, uint32_t, uint32_t, uint32_t);
  CredentialStatus replaceGroups(Thread&, const uint32_t*, size_t);
  uint32_t changeFilesystemId(Thread&, bool group, uint32_t requested);
  void setDumpable(bool);
  void commitExecCredentials(Thread&, bool allExecutableFilesReadable);

  virtual int64_t getUserId() const;
  virtual int64_t getGroupId() const;
  virtual int64_t getEffectiveUserId() const;
  virtual int64_t getEffectiveGroupId() const;
  virtual void getSupplementalGroupIds(Vector<int64_t>& vec) const;

  void setUserId(int64_t id) override;
  void setGroupId(int64_t id) override;
  void setEffectiveUserId(int64_t id) override;
  void setEffectiveGroupId(int64_t id) override;
  void setSupplementalGroupIds(const Vector<int64_t>& vec);

  int64_t getSavedUserId() const;
  int64_t getSavedGroupId() const;
  void setSavedUserId(int64_t id);
  void setSavedGroupId(int64_t id);

 private:
  void setTrustedIdentity(uint32_t CredentialSnapshot::* field, int64_t id);

  // Register with other systems e.g. procfs
  void registerProcess();
  void unregisterProcess();

  virtual void reportTimesUpdated(Time::Timestamp userTotal, Time::Timestamp total);
  virtual void processTerminated();

  PosixProcess(const PosixProcess&);
  PosixProcess& operator=(const PosixProcess&);

  void initializeJobControl(Process* parent);
  size_t m_SessionId;
  ProcessGroup* m_pProcessGroup;
  PosixProcess* m_GroupPrevious;
  PosixProcess* m_GroupNext;
  bool m_ExecCommitted;
  Membership m_GroupMembership;
  uint32_t m_Mask;

  IntervalTimer m_RealIntervalTimer;
  IntervalTimer m_VirtualIntervalTimer;
  IntervalTimer m_ProfileIntervalTimer;

  CredentialSnapshot m_Credentials;
  bool m_bRegistered;
};

#endif
