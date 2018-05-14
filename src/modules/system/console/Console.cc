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

#include "Console.h"
#include "modules/Module.h"
#include "modules/system/vfs/VFS.h"

#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

ConsoleManager ConsoleManager::m_Instance;

void ConsoleManager::newConsole(char c, size_t i)
{
    char a = 'a' + (i % 10);
    if (i <= 9)
        a = '0' + i;

    char master[] = {'p', 't', 'y', c, a, 0};
    char slave[] = {'t', 't', 'y', c, a, 0};

    String masterName(master), slaveName(slave);

    ConsoleMasterFile *pMaster = new ConsoleMasterFile(i, masterName, this);
    ConsoleSlaveFile *pSlave = new ConsoleSlaveFile(i, slaveName, this);

    pMaster->setOther(pSlave);
    pSlave->setOther(pMaster);

    {
        LockGuard<Spinlock> guard(m_Lock);
        m_Consoles.pushBack(pMaster);
        m_Consoles.pushBack(pSlave);
    }
}

ConsoleManager::ConsoleManager() : m_Consoles(), m_Lock()
{
    // Create all consoles, so we can look them up easily.
    for (size_t i = 0; i < 16; ++i)
    {
        for (char c = 'p'; c <= 'z'; ++c)
        {
            newConsole(c, i);
        }
        for (char c = 'a'; c <= 'e'; ++c)
        {
            newConsole(c, i);
        }
    }
}

ConsoleManager::~ConsoleManager()
{
    for (auto it : m_Consoles)
    {
        delete it;
    }
}

ConsoleManager &ConsoleManager::instance()
{
    return m_Instance;
}

File *ConsoleManager::getConsole(String consoleName)
{
    LockGuard<Spinlock> guard(m_Lock);
    for (size_t i = 0; i < m_Consoles.count(); i++)
    {
        ConsoleFile *pC = m_Consoles[i];
        if (pC->m_Name == consoleName)
        {
            return pC;
        }
    }
    // Error - not found.
    return 0;
}

ConsoleFile *ConsoleManager::getConsoleFile(RequestQueue *pBackend)
{
    return 0;
}

bool ConsoleManager::lockConsole(File *file)
{
    if (!isConsole(file))
        return false;

    ConsoleMasterFile *pConsole = static_cast<ConsoleMasterFile *>(file);
    if (!pConsole->isMaster())
        return false;

    if (pConsole->bLocked)
        return false;

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    pConsole->bLocked = true;
    pConsole->pLocker = pProcess;

    return true;
}

void ConsoleManager::unlockConsole(File *file)
{
    if (!isConsole(file))
        return;

    ConsoleMasterFile *pConsole = static_cast<ConsoleMasterFile *>(file);
    if (!pConsole->isMaster())
        return;

    // Make sure we are the owner of the master.
    // Forked children shouldn't be able to close() and steal a master pty.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    if (pConsole->pLocker != pProcess)
        return;
    pConsole->bLocked = false;
}

bool ConsoleManager::isConsole(File *file)
{
    if (!file)
        return false;
    return (file->getInode() == 0xdeadbeef);
}

bool ConsoleManager::isMasterConsole(File *file)
{
    if (!isConsole(file))
        return false;

    ConsoleFile *pFile = reinterpret_cast<ConsoleFile *>(file);
    return pFile->isMaster();
}

void ConsoleManager::setAttributes(File *file, size_t flags)
{
    // \todo Sanity checking of the flags.
    if (!file)
        return;
    ConsoleFile *pFile = reinterpret_cast<ConsoleFile *>(file);
    pFile->m_Flags = flags;
}

void ConsoleManager::getAttributes(File *file, size_t *flags)
{
    if (!file || !flags)
        return;
    ConsoleFile *pFile = reinterpret_cast<ConsoleFile *>(file);
    *flags = pFile->m_Flags;
}

void ConsoleManager::setControlChars(File *file, void *p)
{
    if (!file || !p)
        return;
    ConsoleFile *pFile = reinterpret_cast<ConsoleFile *>(file);
    MemoryCopy(pFile->m_ControlChars, p, MAX_CONTROL_CHAR);
}

void ConsoleManager::getControlChars(File *file, void *p)
{
    if (!file || !p)
        return;
    ConsoleFile *pFile = reinterpret_cast<ConsoleFile *>(file);
    MemoryCopy(p, pFile->m_ControlChars, MAX_CONTROL_CHAR);
}

int ConsoleManager::getWindowSize(
    File *file, unsigned short *rows, unsigned short *cols)
{
    if (!file)
        return -1;
    ConsoleFile *pFile = reinterpret_cast<ConsoleFile *>(file);
    if (!pFile->isMaster())
    {
        if (pFile->m_pOther)
        {
            pFile = pFile->m_pOther;
        }
    }

    *rows = pFile->m_Rows;
    *cols = pFile->m_Cols;
    return 0;
}

int ConsoleManager::setWindowSize(
    File *file, unsigned short rows, unsigned short cols)
{
    if (!file)
        return false;
    ConsoleFile *pFile = reinterpret_cast<ConsoleFile *>(file);
    if ((!pFile->isMaster()) && pFile->m_pOther)
    {
        // Ignore. Slave cannot change window size.
        return 0;
    }
    pFile->m_Rows = rows;
    pFile->m_Cols = cols;
    return 0;
}

bool ConsoleManager::hasDataAvailable(File *file)
{
    if (!file)
        return false;
    ConsoleFile *pFile = reinterpret_cast<ConsoleFile *>(file);
    return pFile->select(false, 0);
}

void ConsoleManager::flush(File *file)
{
}

File *ConsoleManager::getOther(File *file)
{
    if (!file)
        return 0;
    ConsoleFile *pFile = reinterpret_cast<ConsoleFile *>(file);
    if (!pFile->m_pOther)
    {
        return file;  // some consoles (e.g. physical) don't have others
    }
    return pFile->m_pOther;
}

static bool initConsole()
{
    return true;
}

static void destroyConsole()
{
}

MODULE_INFO("console", &initConsole, &destroyConsole, "vfs");
