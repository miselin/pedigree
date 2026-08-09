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
#include "IntervalTimerState.h"
#include "ProcFs.h"

#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/utilities/utility.h"

#include <signal.h>

ProcessGroup::~ProcessGroup()
{
    RecursingLockGuard<Spinlock> guard(
        ProcessGroupManager::instance().lock());

    // Remove all processes in the list from this group
    for (List<PosixProcess *>::Iterator it = Members.begin();
         it != Members.end(); ++it)
    {
        if (*it)
        {
            if ((*it)->m_pProcessGroup == this)
            {
                (*it)->m_pProcessGroup = 0;
                (*it)->m_GroupMembership = PosixProcess::NoGroup;
            }
        }
    }

    if (registered)
    {
        ProcessGroupManager::instance().unregisterGroup(
            processGroupId, this);
    }

    // All have been removed, update our list accordingly
    Members.clear();
}

PosixProcess::PosixProcess()
    : Process(DeferredPublication()), m_pSession(0), m_pProcessGroup(0),
      m_GroupMembership(NoGroup), m_Mask(0),
      m_RobustListData(),
      m_RealIntervalTimer(this, IntervalTimer::Hardware),
      m_VirtualIntervalTimer(this, IntervalTimer::Virtual),
      m_ProfileIntervalTimer(this, IntervalTimer::Profile), m_Uid(0), m_Gid(0),
      m_Euid(0), m_Egid(0), m_Suid(0), m_Sgid(0), m_SupplementalIds(),
      m_bRegistered(false)
{
    enableTimeAccountingReports();
}

/** Copy constructor. */
PosixProcess::PosixProcess(Process *pParent, bool bCopyOnWrite)
    : Process(DeferredPublication(), pParent, bCopyOnWrite), m_pSession(0),
      m_pProcessGroup(0), m_GroupMembership(NoGroup), m_Mask(0),
      m_RobustListData(),
      m_RealIntervalTimer(this, IntervalTimer::Hardware),
      m_VirtualIntervalTimer(this, IntervalTimer::Virtual),
      m_ProfileIntervalTimer(this, IntervalTimer::Profile), m_Uid(0), m_Gid(0),
      m_Euid(0), m_Egid(0), m_Suid(0), m_Sgid(0), m_SupplementalIds(),
      m_bRegistered(false)
{
    enableTimeAccountingReports();

    if (pParent->getType() == Posix)
    {
        PosixProcess *pPosixParent = static_cast<PosixProcess *>(pParent);
        m_pSession = pPosixParent->m_pSession;

        // Child inherits parent's mask.
        m_Mask = pPosixParent->getMask();

        m_SupplementalIds.clear();
        pPosixParent->getSupplementalGroupIds(m_SupplementalIds);
        m_Suid = pPosixParent->getSavedUserId();
        m_Sgid = pPosixParent->getSavedGroupId();
    }

    m_Uid = pParent->getUserId();
    m_Gid = pParent->getGroupId();
    m_Euid = pParent->getEffectiveUserId();
    m_Egid = pParent->getEffectiveGroupId();
    m_Suid = -1;
    m_Sgid = -1;

}

PosixProcess::~PosixProcess()
{
    prepareForDestruction();
    leaveProcessGroup();
    unregisterProcess();
}

void PosixProcess::publish()
{
    // Scheduler enumeration must not observe this object until the caller has
    // installed all child-side state and a non-runnable initial Thread.
    Process::publish();
    registerProcess();
}

void PosixProcess::setProcessGroup(ProcessGroup *newGroup)
{
    RecursingLockGuard<Spinlock> guard(
        ProcessGroupManager::instance().lock());

    ProcessGroup *oldGroup = m_pProcessGroup;
    if (oldGroup == newGroup)
    {
        return;
    }

    // Remove ourselves from our existing group.
    if (oldGroup)
    {
        for (List<PosixProcess *>::Iterator it =
                 oldGroup->Members.begin();
             it != oldGroup->Members.end();)
        {
            if ((*it) == this)
            {
                it = oldGroup->Members.erase(it);
            }
            else
                ++it;
        }
        if (oldGroup->Leader == this)
        {
            oldGroup->Leader = 0;
        }
    }

    // Now join the real group.
    m_pProcessGroup = newGroup;
    if (m_pProcessGroup)
    {
        bool alreadyMember = false;
        for (List<PosixProcess *>::Iterator it =
                 m_pProcessGroup->Members.begin();
             it != m_pProcessGroup->Members.end(); ++it)
        {
            if (*it == this)
            {
                alreadyMember = true;
                break;
            }
        }
        if (!alreadyMember)
        {
            m_pProcessGroup->Members.pushBack(this);
        }
        if (!m_pProcessGroup->registered)
        {
            ProcessGroupManager::instance().registerGroup(
                m_pProcessGroup->processGroupId, m_pProcessGroup);
            m_pProcessGroup->registered = true;
        }
    }

    if (
        oldGroup && oldGroup != m_pProcessGroup &&
        !oldGroup->Members.count())
    {
        delete oldGroup;
    }
}

void PosixProcess::inheritProcessGroup(PosixProcess *parent)
{
    if (!parent)
    {
        return;
    }

    RecursingLockGuard<Spinlock> guard(
        ProcessGroupManager::instance().lock());
    setProcessGroup(parent->m_pProcessGroup);
    if (!m_pProcessGroup)
    {
        m_GroupMembership = NoGroup;
    }
    else if (parent->m_GroupMembership == Leader)
    {
        m_GroupMembership = Member;
    }
    else
    {
        m_GroupMembership = parent->m_GroupMembership;
    }
}

ProcessGroup *PosixProcess::getProcessGroup() const
{
    return m_pProcessGroup;
}

bool PosixProcess::getProcessGroupId(size_t &groupId) const
{
    RecursingLockGuard<Spinlock> guard(
        ProcessGroupManager::instance().lock());
    if (!m_pProcessGroup)
    {
        return false;
    }

    groupId = m_pProcessGroup->processGroupId;
    return true;
}

void PosixProcess::leaveProcessGroup()
{
    RecursingLockGuard<Spinlock> guard(
        ProcessGroupManager::instance().lock());
    ProcessGroup *group = m_pProcessGroup;
    if (!group)
    {
        m_GroupMembership = NoGroup;
        return;
    }

    for (List<PosixProcess *>::Iterator it = group->Members.begin();
         it != group->Members.end();)
    {
        if (*it == this)
        {
            it = group->Members.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (group->Leader == this)
    {
        group->Leader = 0;
    }

    // Clear the raw back-pointer before group destruction can run.
    m_pProcessGroup = 0;
    m_GroupMembership = NoGroup;
    if (!group->Members.count())
    {
        delete group;
    }
}

void PosixProcess::setGroupMembership(Membership type)
{
    RecursingLockGuard<Spinlock> guard(
        ProcessGroupManager::instance().lock());
    m_GroupMembership = type;
}

PosixProcess::Membership PosixProcess::getGroupMembership() const
{
    RecursingLockGuard<Spinlock> guard(
        ProcessGroupManager::instance().lock());
    return m_GroupMembership;
}

PosixSession *PosixProcess::getSession() const
{
    return m_pSession;
}

void PosixProcess::setSession(PosixSession *p)
{
    m_pSession = p;
}

Process::ProcessType PosixProcess::getType()
{
    return Posix;
}

void PosixProcess::setMask(uint32_t mask)
{
    m_Mask = mask;
}

uint32_t PosixProcess::getMask() const
{
    return m_Mask;
}

const PosixProcess::RobustListData &PosixProcess::getRobustList() const
{
    return m_RobustListData;
}

void PosixProcess::setRobustList(const RobustListData &data)
{
    m_RobustListData = data;
}

void PosixProcess::registerProcess()
{
    Filesystem *pFs =
        VFS::instance().getFilesystemAt(String("/media/proc"));
    if (!pFs)
    {
        return;
    }

    ProcFs *pProcFs = static_cast<ProcFs *>(pFs);
    pProcFs->addProcess(this);
    m_bRegistered = true;
}

void PosixProcess::unregisterProcess()
{
    if (!m_bRegistered)
    {
        return;
    }

    Filesystem *pFs =
        VFS::instance().getFilesystemAt(String("/media/proc"));
    if (!pFs)
    {
        return;
    }

    ProcFs *pProcFs = static_cast<ProcFs *>(pFs);
    pProcFs->removeProcess(this);
    m_bRegistered = false;
}

IntervalTimer &PosixProcess::getRealIntervalTimer()
{
    return m_RealIntervalTimer;
}

IntervalTimer &PosixProcess::getVirtualIntervalTimer()
{
    return m_VirtualIntervalTimer;
}

IntervalTimer &PosixProcess::getProfileIntervalTimer()
{
    return m_ProfileIntervalTimer;
}

void PosixProcess::reportTimesUpdated(
    Time::Timestamp userTotal, Time::Timestamp total)
{
    m_VirtualIntervalTimer.consumeCpuTime(userTotal);
    m_ProfileIntervalTimer.consumeCpuTime(total);
}

void PosixProcess::processTerminated()
{
    // Cancel all timers.
    PosixSubsystem *subsystem =
        static_cast<PosixSubsystem *>(getSubsystem());
    if (subsystem)
    {
        subsystem->cancelAlarm();
    }
    m_RealIntervalTimer.disarm();
    m_VirtualIntervalTimer.disarm();
    m_ProfileIntervalTimer.disarm();
}

IntervalTimer::IntervalTimer(PosixProcess *pProcess, Mode mode)
    : m_Process(pProcess), m_Mode(mode), m_Value(0), m_Interval(0),
      m_LastCpuTotal(0), m_Lock(false), m_Armed(false), m_pTimer(nullptr)
{
    if (m_Mode == Hardware)
    {
        Timer *t = Machine::instance().getTimer();
        if (t && t->registerHandler(this))
        {
            m_pTimer = t;
        }
        else
        {
            ERROR("IntervalTimer could not register its hardware callback");
        }
    }
    else
    {
        m_LastCpuTotal = absoluteCpuTotal();
    }
}

IntervalTimer::~IntervalTimer()
{
    if (m_pTimer)
    {
        if (!m_pTimer->unregisterHandler(this))
        {
            FATAL("IntervalTimer could not drain its hardware callback");
        }
        m_pTimer = nullptr;
    }
}

void IntervalTimer::setInterval(
    Time::Timestamp interval, Time::Timestamp *prevInterval)
{
    bool needsSignal = false;
    {
        LockGuard<Spinlock> guard(m_Lock);
        if (m_Mode != Hardware)
        {
            needsSignal = advanceCpuTimeLocked(absoluteCpuTotal());
        }

        if (prevInterval)
        {
            *prevInterval = m_Interval;
        }
        m_Interval = interval;
    }
    if (needsSignal)
    {
        signal();
    }
}

void IntervalTimer::setTimerValue(
    Time::Timestamp value, Time::Timestamp *prevValue)
{
    bool needsSignal = false;
    {
        LockGuard<Spinlock> guard(m_Lock);
        if (m_Mode != Hardware)
        {
            needsSignal = advanceCpuTimeLocked(absoluteCpuTotal());
        }

        if (prevValue)
        {
            *prevValue = m_Value;
        }
        m_Value = value;
        m_Armed = m_Value > 0;
    }
    if (needsSignal)
    {
        signal();
    }
}

void IntervalTimer::setIntervalAndValue(
    Time::Timestamp interval, Time::Timestamp value,
    Time::Timestamp *prevInterval, Time::Timestamp *prevValue)
{
    bool needsSignal = false;
    {
        LockGuard<Spinlock> guard(m_Lock);
        if (m_Mode != Hardware)
        {
            needsSignal = advanceCpuTimeLocked(absoluteCpuTotal());
        }

        if (prevInterval)
        {
            *prevInterval = m_Interval;
        }

        if (prevValue)
        {
            *prevValue = m_Value;
        }

        m_Interval = interval;
        m_Value = value;
        m_Armed = m_Value > 0;
    }
    if (needsSignal)
    {
        signal();
    }
}

void IntervalTimer::disarm()
{
    LockGuard<Spinlock> guard(m_Lock);
    if (m_Mode != Hardware)
    {
        const Time::Timestamp current = absoluteCpuTotal();
        if (current > m_LastCpuTotal)
        {
            m_LastCpuTotal = current;
        }
    }
    m_Value = 0;
    m_Interval = 0;
    m_Armed = false;
}

void IntervalTimer::getIntervalAndValue(
    Time::Timestamp &interval, Time::Timestamp &value)
{
    bool needsSignal = false;
    {
        LockGuard<Spinlock> guard(m_Lock);
        if (m_Mode != Hardware)
        {
            needsSignal = advanceCpuTimeLocked(absoluteCpuTotal());
        }

        interval = m_Interval;
        value = m_Value;
    }
    if (needsSignal)
    {
        signal();
    }
}

void IntervalTimer::consumeCpuTime(Time::Timestamp absoluteTotal)
{
    if (m_Mode == Hardware)
    {
        return;
    }

    bool needsSignal = false;
    {
        LockGuard<Spinlock> guard(m_Lock);
        needsSignal = advanceCpuTimeLocked(absoluteTotal);
    }

    if (needsSignal)
    {
        signal();
    }
}

Time::Timestamp IntervalTimer::absoluteCpuTotal() const
{
    if (m_Mode == Virtual)
    {
        return m_Process->getUserTime();
    }
    if (m_Mode == Profile)
    {
        return m_Process->getUserTime() + m_Process->getKernelTime();
    }
    return 0;
}

bool IntervalTimer::advanceCpuTimeLocked(Time::Timestamp absoluteTotal)
{
    const PosixIntervalTimerState::AbsoluteConsumption result =
        PosixIntervalTimerState::consumeAbsolute(
            m_Value, m_Interval, m_Armed, m_LastCpuTotal, absoluteTotal);
    m_Value = result.timer.value;
    m_Armed = result.timer.armed;
    m_LastCpuTotal = result.baseline;
    return result.timer.expired;
}

Time::Timestamp IntervalTimer::getInterval() const
{
    return m_Interval;
}

Time::Timestamp IntervalTimer::getValue() const
{
    return m_Value;
}

void IntervalTimer::timer(uint64_t delta)
{
    if (m_Mode != Hardware)
    {
        return;
    }

    bool needsSignal = false;
    {
        LockGuard<Spinlock> guard(m_Lock);

        if (!m_Armed)
        {
            // Disarmed - ignore the timer event.
            return;
        }

        const PosixIntervalTimerState::Consumption result =
            PosixIntervalTimerState::consume(
                m_Value, m_Interval, m_Armed, delta);
        m_Value = result.value;
        m_Armed = result.armed;
        needsSignal = result.expired;
    }

    if (needsSignal)
    {
        signal();
    }
}

void IntervalTimer::signal()
{
    int signal = -1;
    switch (m_Mode)
    {
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
    if (!Scheduler::instance().acquireProcess(process, m_Process))
    {
        return;
    }
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(process->getSubsystem());
    Process::ThreadLease target;
    const bool targetAcquired =
        process->acquireThread(target, static_cast<size_t>(0));
    if (!pSubsystem || !targetAcquired)
    {
        return;
    }

    // Don't yield in the middle of the timer handler
    pSubsystem->sendSignal(target.get(), signal, false);
}

int64_t PosixProcess::getUserId() const
{
    return m_Uid;
}

int64_t PosixProcess::getGroupId() const
{
    return m_Gid;
}

int64_t PosixProcess::getEffectiveUserId() const
{
    return m_Euid;
}

int64_t PosixProcess::getEffectiveGroupId() const
{
    return m_Egid;
}

void PosixProcess::getSupplementalGroupIds(Vector<int64_t> &vec) const
{
    for (auto it : m_SupplementalIds)
    {
        vec.pushBack(it);
    }
}

void PosixProcess::setUserId(int64_t id)
{
    m_Uid = id;
}

void PosixProcess::setGroupId(int64_t id)
{
    m_Gid = id;
}

void PosixProcess::setEffectiveUserId(int64_t id)
{
    m_Euid = id;
}

void PosixProcess::setEffectiveGroupId(int64_t id)
{
    m_Egid = id;
}

void PosixProcess::setSupplementalGroupIds(const Vector<int64_t> &vec)
{
    m_SupplementalIds.clear();
    m_SupplementalIds.reserve(vec.size(), false);

    for (auto it : vec)
    {
        m_SupplementalIds.pushBack(it);
    }
}

int64_t PosixProcess::getSavedUserId() const
{
    return m_Suid;
}

int64_t PosixProcess::getSavedGroupId() const
{
    return m_Sgid;
}

void PosixProcess::setSavedUserId(int64_t id)
{
    m_Suid = id;
}

void PosixProcess::setSavedGroupId(int64_t id)
{
    m_Sgid = id;
}
