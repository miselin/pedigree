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

#include "subsys/posix/VirtualTerminal.h"
#include "subsys/posix/PosixSubsystem.h"
#include "subsys/posix/DevFs.h"
#include "pedigree/kernel/process/Process.h"
#include "modules/system/console/Console.h"

extern DevFs *g_pDevFs;

VirtualTerminalManager::VirtualTerminalManager(DevFsDirectory *parentDir) :
    m_pTty(nullptr), m_CurrentTty(0), m_WantedTty(~0U), m_NumTtys(0),
    m_ParentDir(parentDir), m_bSwitchingLocked(false), m_SystemMode(Text)
{
    for (size_t i = 0; i < MAX_VT; ++i)
    {
        m_Terminals[i].textio = nullptr;
        m_Terminals[i].file = nullptr;
#ifdef THREADS
        m_Terminals[i].owner = nullptr;
#endif

        ByteSet(&m_Terminals[i].mode, 0, sizeof(m_Terminals[i].mode));
        m_Terminals[i].mode.mode = VT_AUTO;
    }
}

VirtualTerminalManager::~VirtualTerminalManager()
{
    //
}

bool VirtualTerminalManager::initialise()
{
    // Create /dev/textui for the text-only UI device.
    m_pTty = new TextIO(String("textui"), g_pDevFs->getNextInode(), g_pDevFs, m_ParentDir);
    m_pTty->markPrimary();
    if (m_pTty->initialise(false))
    {
        m_ParentDir->addEntry(m_pTty->getName(), m_pTty);
    }
    else
    {
        WARNING("POSIX: no /dev/tty - VirtualTerminalManager failed to initialise.");
        g_pDevFs->revertInode();
        delete m_pTty;
        m_pTty = nullptr;

        return false;
    }

    // set up tty1
    ConsolePhysicalFile *pTty1 =
        new ConsolePhysicalFile(0, m_pTty, String("tty1"), g_pDevFs);
    m_ParentDir->addEntry(pTty1->getName(), pTty1);

    m_Terminals[0].textio = m_pTty;
    m_Terminals[0].file = pTty1;

    // create tty2-6 as non-overloaded TextIO instances
    for (size_t i = 1; i < 8; ++i)
    {
        String ttyname;
        ttyname.Format("tty%u", i + 1);

        TextIO *tio = new TextIO(ttyname, g_pDevFs->getNextInode(), g_pDevFs, m_ParentDir);
        if (tio->initialise(true))
        {
            ConsolePhysicalFile *file =
                new ConsolePhysicalFile(i + 1, tio, ttyname, g_pDevFs);
            m_ParentDir->addEntry(tio->getName(), file);

            m_Terminals[i].textio = tio;
            m_Terminals[i].file = file;

            // activate the terminal by performing an empty write, which will
            // ensure users switching to the terminal see a blank screen if
            // nothing has actually opened it - this is better than seeing the
            // previous tty's output...
            tio->write("", 0);
        }
        else
        {
            WARNING("POSIX: failed to create " << ttyname);
            g_pDevFs->revertInode();
            delete tio;
        }
    }

    return true;
}

void VirtualTerminalManager::activate(size_t n)
{
    if (n >= MAX_VT)
    {
        ERROR("VirtualTerminalManager: trying to activate invalid VT #" << n);
        return;
    }
    else if (n == m_CurrentTty)
    {
        ERROR("VirtualTerminalManager: trying to activate current VT");
        return;
    }

    if (m_bSwitchingLocked)
    {
        ERROR("VirtualTerminalManager: switching is currently locked");
        return;
    }

    struct vt_mode currentMode = getTerminalMode(m_CurrentTty);

    if (currentMode.mode == VT_AUTO)
    {
        NOTICE("VirtualTerminalManager: switching from auto VT");

        // Easy transfer.
        m_Terminals[m_CurrentTty].textio->unmarkPrimary();
        m_CurrentTty = n;
        m_Terminals[m_CurrentTty].textio->markPrimary();

        // acquiring the new terminal
        sendSignal(m_CurrentTty, true);
    }
    else
    {
        NOTICE("VirtualTerminalManager: switching from owned VT");

        m_WantedTty = n;

        // we want to release the current terminal
        sendSignal(m_CurrentTty, false);
    }
}

void VirtualTerminalManager::reportPermission(SwitchPermission perm)
{
    if (m_WantedTty == static_cast<size_t>(~0))
    {
        // No switch in progress.
        NOTICE("VirtualTerminalManager: can't acknowledge as no switch in progress");
        return;
    }

    if (perm == Disallowed)
    {
        // abort the switch
        NOTICE("VirtualTerminalManager: VT switch disallowed");
        m_WantedTty = static_cast<size_t>(~0U);
        return;
    }

    NOTICE("VirtualTerminalManager: VT switch allowed");

    // OK to switch!
    m_Terminals[m_CurrentTty].textio->unmarkPrimary();
    m_CurrentTty = m_WantedTty;
    m_Terminals[m_CurrentTty].textio->markPrimary();

    // acquiring the new terminal (acquire signal)
    sendSignal(m_CurrentTty, true);
}

size_t VirtualTerminalManager::openInactive()
{
    for (size_t i = 0; i < MAX_VT; ++i)
    {
        if (m_Terminals[i].textio == nullptr)
        {
            NOTICE("VirtualTerminalManager: opening inactive VT #" << i);

            String ttyname;
            ttyname.Format("tty%u", i + 1);

            TextIO *tio = new TextIO(ttyname, g_pDevFs->getNextInode(), g_pDevFs, m_ParentDir);
            if (tio->initialise(true))
            {
                ConsolePhysicalFile *file =
                    new ConsolePhysicalFile(i, tio, ttyname, g_pDevFs);
                m_ParentDir->addEntry(tio->getName(), file);

                m_Terminals[i].textio = tio;
                m_Terminals[i].file = file;

                // activate the terminal by performing an empty write, which will
                // ensure users switching to the terminal see a blank screen if
                // nothing has actually opened it - this is better than seeing the
                // previous tty's output...
                tio->write("", 0);

                return i;
            }
            else
            {
                WARNING("POSIX: failed to create " << ttyname);
                g_pDevFs->revertInode();
                delete tio;
            }
        }
    }

    return ~0;
}

void VirtualTerminalManager::lockSwitching(bool locked)
{
    m_bSwitchingLocked = locked;
}

size_t VirtualTerminalManager::getCurrentTerminalNumber() const
{
    return m_CurrentTty;
}

TextIO *VirtualTerminalManager::getCurrentTerminal() const
{
    return m_Terminals[m_CurrentTty].textio;
}

File *VirtualTerminalManager::getCurrentTerminalFile() const
{
    return m_Terminals[m_CurrentTty].file;
}

struct vt_mode VirtualTerminalManager::getTerminalMode(size_t n) const
{
    /// \todo validate n
    NOTICE("getTerminalMode #" << n);
    return m_Terminals[n].mode;
}

void VirtualTerminalManager::setTerminalMode(size_t n, struct vt_mode mode)
{
    /// \todo validate n
    NOTICE("setTerminalMode #" << n);
    m_Terminals[n].mode = mode;

#ifdef THREADS
    if (mode.mode == VT_PROCESS)
    {
        m_Terminals[n].owner = Processor::information().getCurrentThread()->getParent();
    }
    else
    {
        m_Terminals[n].owner = nullptr;
    }
#endif
}

struct vt_stat VirtualTerminalManager::getState() const
{
    struct vt_stat state;

    state.v_active = m_CurrentTty + 1;
    state.v_signal = 0;
    state.v_state = 1;  // VT 0 == current, always
    for (size_t i = 0; i < MAX_VT; ++i)
    {
        if (m_Terminals[i].textio != nullptr)
        {
            state.v_state |= (1 << (i + 1));
        }
    }

    NOTICE("getState:");
    NOTICE(" -> active = " << state.v_active);
    NOTICE(" -> state = " << Hex << state.v_state);

    return state;
}

void VirtualTerminalManager::setSystemMode(SystemMode mode)
{
    m_SystemMode = mode;
}

VirtualTerminalManager::SystemMode VirtualTerminalManager::getSystemMode() const
{
    return m_SystemMode;
}

void VirtualTerminalManager::setInputMode(size_t n, TextIO::InputMode newMode)
{
    if (!m_Terminals[n].textio)
    {
        NOTICE("VirtualTerminalManager: can't set mode of VT #" << n << " as it is inactive");
        return;
    }

    m_Terminals[n].textio->setMode(newMode);
}

TextIO::InputMode VirtualTerminalManager::getInputMode(size_t n) const
{
    if (!m_Terminals[n].textio)
    {
        NOTICE("VirtualTerminalManager: can't get mode of VT #" << n << " as it is inactive");
        return TextIO::Standard;
    }

    return m_Terminals[n].textio->getMode();
}

void VirtualTerminalManager::sendSignal(size_t n, bool acq)
{
    if (!m_Terminals[n].textio)
    {
        NOTICE("VirtualTerminalManager: can't send signal to VT #" << n << " as it is inactive");
        return;
    }
    
    auto mode = getTerminalMode(n);
    if (mode.mode != VT_PROCESS)
    {
        NOTICE("VirtualTerminalManager: can't send signal to VT #" << n << " as it is not owned");
        return;
    }

#ifdef THREADS
    Process *pProcess = m_Terminals[n].owner;
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("VirtualTerminal::sendSignal: no subsystem");
        return;
    }

    NOTICE("VirtualTerminalManager: signaling VT #" << n);
    pSubsystem->sendSignal(pProcess->getThread(0), acq ? mode.acqsig : mode.relsig);
#endif
}
