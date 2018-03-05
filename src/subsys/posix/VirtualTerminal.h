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

#ifndef POSIX_VIRTUALTERMINAL_H
#define POSIX_VIRTUALTERMINAL_H

#include "modules/system/console/TextIO.h"
#include "modules/system/vfs/File.h"
#include "subsys/posix/console-syscalls.h"

#define MAX_VT  64

class DevFsDirectory;
class Process;

class VirtualTerminalManager
{
    public:
        VirtualTerminalManager(DevFsDirectory *parentDir);
        virtual ~VirtualTerminalManager();

        enum SwitchPermission
        {
            Allowed,
            Disallowed
        };

        enum SystemMode
        {
            Text,
            Graphics
        };

        bool initialise();

        /**
         * \brief Starts the process of activating the given tty.
         *
         * If we are currently using a VT with VT_AUTO mode, this just directly
         * switches over. However, if the current VT is in VT_PROCESS mode, this
         * tracks the pending switchover and then signals the current VT. That
         * owning process will respond with an ioctl that leads to a call to
         * reportPermission() that may or may not block the transition.
         */
        void activate(size_t n);

        /** Report permission to switch. */
        void reportPermission(SwitchPermission perm);

        /** Find an inactive VT and open it, returning its number. */
        size_t openInactive();

        /** Lock switching altogether. */
        void lockSwitching(bool locked);

        size_t getCurrentTerminalNumber() const;
        TextIO *getCurrentTerminal() const;
        File *getCurrentTerminalFile() const;

        struct vt_mode getTerminalMode(size_t n) const;
        void setTerminalMode(size_t n, struct vt_mode mode);

        struct vt_stat getState() const;

        void setSystemMode(SystemMode mode);
        SystemMode getSystemMode() const;

    private:
        void sendSignal(size_t n, bool acq);

        struct VirtualTerminal
        {
            TextIO *textio;
            File *file;
            struct vt_mode mode;

#ifdef THREADS
            Process *owner;
#endif
        };

        VirtualTerminal m_Terminals[MAX_VT];

        TextIO *m_pTty;

        TextIO *m_pTtys[MAX_VT];
        File *m_pTtyFiles[MAX_VT];
        struct vt_mode m_Modes[MAX_VT];

        size_t m_CurrentTty;
        size_t m_WantedTty;
        size_t m_NumTtys;

        DevFsDirectory *m_ParentDir;

        bool m_bSwitchingLocked;

        SystemMode m_SystemMode;
};

#endif
