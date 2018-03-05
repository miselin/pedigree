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
#include "ProcFs.h"

#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/utilities/utility.h"

#include <signal.h>

ProcessGroup::~ProcessGroup()
{
    // Remove all processes in the list from this group
    for (List<PosixProcess *>::Iterator it = Members.begin();
         it != Members.end(); ++it)
    {
        if (*it)
        {
            (*it)->setGroupMembership(PosixProcess::NoGroup);
            (*it)->setProcessGroup(0, false);
        }
    }

    ProcessGroupManager::instance().returnGroupId(processGroupId);

    // All have been removed, update our list accordingly
    Members.clear();
}

PosixProcess::PosixProcess()
    : Process(), m_pSession(0), m_pProcessGroup(0), m_GroupMembership(NoGroup),
      m_Mask(0), m_RealIntervalTimer(this, IntervalTimer::Hardware),
      m_VirtualIntervalTimer(this, IntervalTimer::Virtual),
      m_ProfileIntervalTimer(this, IntervalTimer::Profile)
{
    registerProcess();
}

/** Copy constructor. */
PosixProcess::PosixProcess(Process *pParent, bool bCopyOnWrite)
    : Process(pParent, bCopyOnWrite), m_pSession(0), m_pProcessGroup(0),
      m_GroupMembership(NoGroup), m_Mask(0),
      m_RealIntervalTimer(this, IntervalTimer::Hardware),
      m_VirtualIntervalTimer(this, IntervalTimer::Virtual),
      m_ProfileIntervalTimer(this, IntervalTimer::Profile)
{
    if (pParent->getType() == Posix)
    {
        PosixProcess *pPosixParent = static_cast<PosixProcess *>(pParent);
        m_pSession = pPosixParent->m_pSession;
        setProcessGroup(pPosixParent->getProcessGroup());
        if (m_pProcessGroup)
        {
            setGroupMembership(Member);
        }

        // Child inherits parent's mask.
        m_Mask = pPosixParent->getMask();
    }

    registerProcess();
}

PosixProcess::~PosixProcess()
{
    unregisterProcess();
}

void PosixProcess::setProcessGroup(
    ProcessGroup *newGroup, bool bRemoveFromGroup)
{
    // Remove ourselves from our existing group.
    if (m_pProcessGroup && bRemoveFromGroup)
    {
        for (List<PosixProcess *>::Iterator it =
                 m_pProcessGroup->Members.begin();
             it != m_pProcessGroup->Members.end();)
        {
            if ((*it) == this)
            {
                it = m_pProcessGroup->Members.erase(it);
            }
            else
                ++it;
        }
    }

    // Now join the real group.
    m_pProcessGroup = newGroup;
    if (m_pProcessGroup)
    {
        m_pProcessGroup->Members.pushBack(this);
        ProcessGroupManager::instance().setGroupId(
            m_pProcessGroup->processGroupId);
    }
}

ProcessGroup *PosixProcess::getProcessGroup() const
{
    return m_pProcessGroup;
}

void PosixProcess::setGroupMembership(Membership type)
{
    m_GroupMembership = type;
}

PosixProcess::Membership PosixProcess::getGroupMembership() const
{
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
    Filesystem *pFs = VFS::instance().lookupFilesystem(String("proc"));
    if (!pFs)
    {
        return;
    }

    ProcFs *pProcFs = static_cast<ProcFs *>(pFs);
    pProcFs->addProcess(this);
}

void PosixProcess::unregisterProcess()
{
    Filesystem *pFs = VFS::instance().lookupFilesystem(String("proc"));
    if (!pFs)
    {
        return;
    }

    ProcFs *pProcFs = static_cast<ProcFs *>(pFs);
    pProcFs->removeProcess(this);
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

void PosixProcess::reportTimesUpdated(Time::Timestamp user, Time::Timestamp system)
{
    m_VirtualIntervalTimer.adjustValue(-user);
    m_ProfileIntervalTimer.adjustValue(-(user + system));
}

void PosixProcess::processTerminated()
{
    // Cancel all timers.
    m_RealIntervalTimer.setIntervalAndValue(0, 0);
    m_VirtualIntervalTimer.setIntervalAndValue(0, 0);
    m_ProfileIntervalTimer.setIntervalAndValue(0, 0);
}

IntervalTimer::IntervalTimer(PosixProcess *pProcess, Mode mode) :
    m_Process(pProcess), m_Mode(mode), m_Value(0), m_Interval(0), m_Lock(false),
    m_Armed(false)
{
    if (m_Mode == Hardware)
    {
        Timer *t = Machine::instance().getTimer();
        if (t)
        {
            t->registerHandler(this);
        }
    }
}

IntervalTimer::~IntervalTimer()
{
    if (m_Mode == Hardware)
    {
        Timer *t = Machine::instance().getTimer();
        if (t)
        {
            t->unregisterHandler(this);
        }
    }
}

void IntervalTimer::setInterval(Time::Timestamp interval, Time::Timestamp *prevInterval)
{
    LockGuard<Spinlock> guard(m_Lock);

    if (prevInterval)
    {
        *prevInterval = m_Interval;
    }
    m_Interval = interval;
}

void IntervalTimer::setTimerValue(Time::Timestamp value, Time::Timestamp *prevValue)
{
    LockGuard<Spinlock> guard(m_Lock);

    if (prevValue)
    {
        *prevValue = m_Value;
    }
    m_Value = value;
    m_Armed = m_Value > 0;
}

void IntervalTimer::setIntervalAndValue(Time::Timestamp interval, Time::Timestamp value, Time::Timestamp *prevInterval, Time::Timestamp *prevValue)
{
    LockGuard<Spinlock> guard(m_Lock);

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

void IntervalTimer::getIntervalAndValue(Time::Timestamp &interval, Time::Timestamp &value)
{
    LockGuard<Spinlock> guard(m_Lock);

    interval = m_Interval;
    value = m_Value;
}

void IntervalTimer::adjustValue(int64_t adjustment)
{
    bool needsSignal = false;
    {
        LockGuard<Spinlock> guard(m_Lock);

        // Fixup in case of potential underflow
        if ((adjustment < 0) && (static_cast<uint64_t>(adjustment * -1) > m_Value))
        {
            m_Value = 0;
        }
        else
        {
            m_Value += adjustment;
        }

        if (m_Armed && !m_Value)
        {
            m_Value = m_Interval;
            m_Armed = m_Value > 0;

            needsSignal = true;
        }
    }

    if (needsSignal)
    {
        signal();
    }
}

Time::Timestamp IntervalTimer::getInterval() const
{
    return m_Interval;
}

Time::Timestamp IntervalTimer::getValue() const
{
    return m_Value;
}

void IntervalTimer::timer(uint64_t delta, InterruptState &state)
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


        if (m_Value < delta)
        {
            m_Value = m_Interval;
            m_Armed = m_Value > 0;

            needsSignal = true;
        }
        else
        {
            m_Value -= delta;
        }
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
    PosixSubsystem *pSubsystem =
        static_cast<PosixSubsystem *>(m_Process->getSubsystem());

    // Don't yield in the middle of the timer handler
    pSubsystem->sendSignal(m_Process->getThread(0), signal, false);
}
