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

#include <vfs/VFS.h>

ProcessGroup::~ProcessGroup()
{
    // Remove all processes in the list from this group
    for(List<PosixProcess*>::Iterator it = Members.begin();
        it != Members.end();
        ++it)
    {
        if(*it)
        {
            (*it)->setGroupMembership(PosixProcess::NoGroup);
            (*it)->setProcessGroup(0, false);
        }
    }

    ProcessGroupManager::instance().returnGroupId(processGroupId);

    // All have been removed, update our list accordingly
    Members.clear();
}

PosixProcess::PosixProcess() :
    Process(), m_pSession(0), m_pProcessGroup(0), m_GroupMembership(NoGroup), m_Mask(0)
{
    registerProcess();
}

/** Copy constructor. */
PosixProcess::PosixProcess(Process *pParent, bool bCopyOnWrite) :
    Process(pParent, bCopyOnWrite), m_pSession(0), m_pProcessGroup(0),
    m_GroupMembership(NoGroup), m_Mask(0)
{
    if(pParent->getType() == Posix)
    {
        PosixProcess *pPosixParent = static_cast<PosixProcess *>(pParent);
        m_pSession = pPosixParent->m_pSession;
        setProcessGroup(pPosixParent->getProcessGroup());
        if(m_pProcessGroup)
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

void PosixProcess::setProcessGroup(ProcessGroup *newGroup, bool bRemoveFromGroup)
{
    // Remove ourselves from our existing group.
    if(m_pProcessGroup && bRemoveFromGroup)
    {
        for(List<PosixProcess*>::Iterator it = m_pProcessGroup->Members.begin();
            it != m_pProcessGroup->Members.end();
            )
        {
            if((*it) == this)
            {
                it = m_pProcessGroup->Members.erase(it);
            }
            else
                ++it;
        }
    }

    // Now join the real group.
    m_pProcessGroup = newGroup;
    if(m_pProcessGroup)
    {
        m_pProcessGroup->Members.pushBack(this);
        NOTICE(">>>>>> Adding self to the members list, new size = " << m_pProcessGroup->Members.count() << ".");

        ProcessGroupManager::instance().setGroupId(m_pProcessGroup->processGroupId);
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
