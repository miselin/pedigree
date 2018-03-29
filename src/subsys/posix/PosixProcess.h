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

#include "PosixSubsystem.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/machine/TimerHandler.h"

class PosixProcess;

class PosixSession
{
  public:
    PosixSession() : Leader(0)
    {
    }

    virtual ~PosixSession()
    {
    }

    /** Session leader. */
    PosixProcess *Leader;
};

class ProcessGroup
{
  public:
    ProcessGroup() : processGroupId(0), Leader(0), Members()
    {
        Members.clear();
    }

    virtual ~ProcessGroup();

    /** The process group ID of this process group. */
    int processGroupId;

    /** The group leader of the process group. */
    PosixProcess *Leader;

    /** List of each Process that is in this process group.
     *  Includes the Leader, iterate over this in order to
     *  obtain every Process in the process group.
     */
    List<PosixProcess *> Members;

  private:
    ProcessGroup(const ProcessGroup &);
    ProcessGroup &operator=(ProcessGroup &);
};

class IntervalTimer : public TimerHandler
{
   public:
    enum Mode
    {
        /// Hardware-backed timer (wall time).
        Hardware = 0,
        /// CPU time in user mode only.
        Virtual,
        /// CPU time in user and system.
        Profile
    };

    /// Setting hw=true will use hardware. hw=false requires adjust() to be
    /// called to be able to trigger timers.
    IntervalTimer(PosixProcess *pProcess, Mode mode = Hardware);
    virtual ~IntervalTimer();

    /// Set the interval for the timer, which is loaded once the timer expires.
    /// Set zero to make a non-reloading timer.
    void setInterval(Time::Timestamp interval, Time::Timestamp *prevInterval = nullptr);

    /// Set the current value of the timer.
    void setTimerValue(Time::Timestamp value, Time::Timestamp *prevValue = nullptr);

    /// Set both interval and value atomically.
    void setIntervalAndValue(Time::Timestamp interval, Time::Timestamp value, Time::Timestamp *prevInterval = nullptr, Time::Timestamp *prevValue = nullptr);

    void getIntervalAndValue(Time::Timestamp &interval, Time::Timestamp &value);

    /// Adjust the current value directly.
    void adjustValue(int64_t adjustment);

    Time::Timestamp getInterval() const;
    Time::Timestamp getValue() const;

   private:
    virtual void timer(uint64_t delta, InterruptState &state);

    void signal();

    PosixProcess *m_Process;
    Mode m_Mode;
    Time::Timestamp m_Value;
    Time::Timestamp m_Interval;
    Spinlock m_Lock;
    bool m_Armed;
};

class EXPORTED_PUBLIC PosixProcess : public Process
{
  public:
    /** Defines what status this Process has within its group */
    enum Membership
    {
        /** Group leader. The one who created the group, and whose PID was
         * absorbed to become the Process Group ID.
         */
        Leader = 0,

        /** Group member. These processes have a unique Process ID. */
        Member,

        /** Not in a group. */
        NoGroup
    };

    /** Information about a robust list. */
    struct RobustListData
    {
        void *head;
        size_t head_len;
    };

    PosixProcess();

    /** Copy constructor. */
    PosixProcess(Process *pParent, bool bCopyOnWrite = true);
    virtual ~PosixProcess();

    void setProcessGroup(ProcessGroup *newGroup, bool bRemoveFromGroup = true);
    ProcessGroup *getProcessGroup() const;

    void setGroupMembership(Membership type);
    Membership getGroupMembership() const;

    PosixSession *getSession() const;
    void setSession(PosixSession *p);

    virtual ProcessType getType();

    void setMask(uint32_t mask);
    uint32_t getMask() const;

    const RobustListData &getRobustList() const;
    void setRobustList(const RobustListData &data);

    IntervalTimer &getRealIntervalTimer();
    IntervalTimer &getVirtualIntervalTimer();
    IntervalTimer &getProfileIntervalTimer();

    virtual int64_t getUserId() const;
    virtual int64_t getGroupId() const;
    virtual int64_t getEffectiveUserId() const;
    virtual int64_t getEffectiveGroupId() const;
    virtual void getSupplementalGroupIds(Vector<int64_t> &vec) const;

    void setUserId(int64_t id);
    void setGroupId(int64_t id);
    void setEffectiveUserId(int64_t id);
    void setEffectiveGroupId(int64_t id);
    void setSupplementalGroupIds(const Vector<int64_t> &vec);

    int64_t getSavedUserId() const;
    int64_t getSavedGroupId() const;
    void setSavedUserId(int64_t id);
    void setSavedGroupId(int64_t id);

  private:
    // Register with other systems e.g. procfs
    void registerProcess();
    void unregisterProcess();

    virtual void reportTimesUpdated(Time::Timestamp user, Time::Timestamp system);
    virtual void processTerminated();

    PosixProcess(const PosixProcess &);
    PosixProcess &operator=(const PosixProcess &);

    PosixSession *m_pSession;
    ProcessGroup *m_pProcessGroup;
    Membership m_GroupMembership;
    uint32_t m_Mask;
    RobustListData m_RobustListData;

    IntervalTimer m_RealIntervalTimer;
    IntervalTimer m_VirtualIntervalTimer;
    IntervalTimer m_ProfileIntervalTimer;

    int64_t m_Uid;
    int64_t m_Gid;
    int64_t m_Euid;
    int64_t m_Egid;
    int64_t m_Suid;
    int64_t m_Sgid;
    Vector<int64_t> m_SupplementalIds;
};

#endif
