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

#include <processor/types.h>
#include "PosixSubsystem.h"
#include <Log.h>

#include <process/Process.h>

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
        List<PosixProcess*> Members;

    private:
        ProcessGroup(const ProcessGroup&);
        ProcessGroup &operator = (ProcessGroup &);
};

class PosixProcess : public Process
{
    public:

        /** Defines what status this Process has within its group */
        enum Membership
        {
            /** Group leader. The one who created the group, and whose PID was absorbed
             *  to become the Process Group ID.
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

    private:
        // Register with other systems e.g. procfs
        void registerProcess();
        void unregisterProcess();

        PosixProcess(const PosixProcess&);
        PosixProcess& operator=(const PosixProcess&);

        PosixSession *m_pSession;
        ProcessGroup *m_pProcessGroup;
        Membership m_GroupMembership;
        uint32_t m_Mask;
        RobustListData m_RobustListData;
};

#endif
