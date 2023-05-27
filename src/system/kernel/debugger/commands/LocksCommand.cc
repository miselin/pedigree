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

#include "pedigree/kernel/debugger/commands/LocksCommand.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/demangle.h"

#if LOCKS_COMMAND_DO_BACKTRACES && !defined(TESTSUITE)
#include "pedigree/kernel/debugger/Backtrace.h"
#include "pedigree/kernel/linker/KernelElf.h"
#endif

LocksCommand g_LocksCommand;
#ifndef TESTSUITE
extern Spinlock g_MallocLock;
#endif

// This is global because we need to rely on it before the constructor is
// called.
static bool g_bReady = false;

#define ERROR_OR_FATAL(x)    \
    do                       \
    {                        \
        if (m_bFatal)        \
            FATAL_NOLOCK(x); \
        else                 \
            ERROR_NOLOCK(x); \
    } while (0)

LocksCommand::LocksCommand()
    : DebuggerCommand(), m_pDescriptors(), m_bAcquiring(false), m_LockIndex(0),
      m_bFatal(true), m_SelectedLine(0)
{
    for (size_t i = 0; i < LOCKS_COMMAND_NUM_CPU; ++i)
    {
#if LOCKS_COMMAND_DO_BACKTRACES
        m_bTracing[i] = false;
#endif
        m_NextPosition[i] = 0;
    }
}

LocksCommand::~LocksCommand()
{
}

void LocksCommand::autocomplete(
    const HugeStaticString &input, HugeStaticString &output)
{
}

bool LocksCommand::execute(
    const HugeStaticString &input, HugeStaticString &output,
    InterruptState &state, DebuggerIO *pScreen)
{
#if !TRACK_LOCKS
    output += "Sorry, this kernel was not built with TRACK_LOCKS enabled.";
    return true;
#endif

    if (!g_bReady)
    {
        output += "Lock tracking has not yet been enabled.";
        return true;
    }

    // Let's enter 'raw' screen mode.
    pScreen->disableCli();

    // Prepare Scrollable interface.
    move(0, 1);
    resize(pScreen->getWidth(), pScreen->getHeight() - 2);
    setScrollKeys('j', 'k');

    pScreen->drawHorizontalLine(
        ' ', 0, 0, pScreen->getWidth() - 1, DebuggerIO::White,
        DebuggerIO::Green);
    pScreen->drawHorizontalLine(
        ' ', pScreen->getHeight() - 1, 0, pScreen->getWidth() - 1,
        DebuggerIO::White, DebuggerIO::Green);
    pScreen->drawString(
        "Pedigree debugger - Lock tracker", 0, 0, DebuggerIO::White,
        DebuggerIO::Green);

    pScreen->drawString(
        "backspace: Page up. space: Page down. q: Quit.",
        pScreen->getHeight() - 1, 0, DebuggerIO::White, DebuggerIO::Green);
    pScreen->drawString(
        "backspace", pScreen->getHeight() - 1, 0, DebuggerIO::Yellow,
        DebuggerIO::Green);
    pScreen->drawString(
        "space", pScreen->getHeight() - 1, 20, DebuggerIO::Yellow,
        DebuggerIO::Green);
    pScreen->drawString(
        "q", pScreen->getHeight() - 1, 38, DebuggerIO::Yellow,
        DebuggerIO::Green);

    // Main I/O loop.
    bool bStop = false;
    bool bReturn = true;
    while (!bStop)
    {
        refresh(pScreen);

        char in = 0;
        while (!(in = pScreen->getChar()))
            ;

        switch (in)
        {
            case 'j':
                scroll(-1);
                if (static_cast<ssize_t>(m_SelectedLine) - 1 >= 0)
                    --m_SelectedLine;
                break;

            case 'k':
                scroll(1);
                if (m_SelectedLine + 1 < getLineCount())
                    ++m_SelectedLine;
                break;

            case ' ':
                scroll(5);
                if (m_SelectedLine + 5 < getLineCount())
                    m_SelectedLine += 5;
                else
                    m_SelectedLine = getLineCount() - 1;
                break;

            case 0x08:  // backspace
                scroll(-5);
                if (static_cast<ssize_t>(m_SelectedLine) - 5 >= 0)
                    m_SelectedLine -= 5;
                else
                    m_SelectedLine = 0;
                break;

            case 'q':
                bStop = true;
        }
    }

    // HACK:: Serial connections will fill the screen with the last background
    // colour used.
    //        Here we write a space with black background so the CLI screen
    //        doesn't get filled by some random colour!
    pScreen->drawString(" ", 1, 0, DebuggerIO::White, DebuggerIO::Black);
    pScreen->enableCli();
    return bReturn;
}

const char *LocksCommand::getLine1(
    size_t index, DebuggerIO::Colour &colour, DebuggerIO::Colour &bgColour)
{
    static NormalStaticString Line;
    Line.clear();

    size_t nLock = 0;
    size_t nDepth = 0;
    size_t nCpu = 0;
    LockDescriptor *pD = 0;
    for (nCpu = 0; nCpu < LOCKS_COMMAND_NUM_CPU; ++nCpu)
    {
        if (nLock++ == index)
        {
            break;
        }

        if (!m_NextPosition[nCpu])
        {
            continue;
        }

        nDepth = 0;
        for (size_t j = 0; j < MAX_DESCRIPTORS; ++j)
        {
            pD = &m_pDescriptors[nCpu][j];
            if (pD->state == Inactive)
            {
                pD = 0;
                break;
            }
            else if (nLock == index)
            {
                break;
            }
#if LOCKS_COMMAND_DO_BACKTRACES
            else if ((nLock < index) && (nLock + pD->n >= index))
            {
                break;
            }

            nLock += pD->n;
#endif

            ++nDepth;
            ++nLock;
        }

        if (pD)
        {
            break;
        }
    }

    if ((nLock - 1) > index)
    {
        return Line;
    }

    if (!pD)
    {
        Line += "CPU";
        Line.append(nCpu);
        Line += " (";
        Line.append(m_NextPosition[nCpu]);
        Line += " locks):";
    }
    else
    {
        Line += " | ";
    }

    colour = DebuggerIO::White;
    if (index == m_SelectedLine)
        bgColour = DebuggerIO::Blue;
    else
        bgColour = DebuggerIO::Black;

    return Line;
}

const char *LocksCommand::getLine2(
    size_t index, size_t &colOffset, DebuggerIO::Colour &colour,
    DebuggerIO::Colour &bgColour)
{
    static HugeStaticString Line;
    Line.clear();

    size_t nLock = 0;
    size_t nDepth = 0;
    size_t nCpu = 0;
    LockDescriptor *pD = 0;
    bool doBacktrace = false;
    for (nCpu = 0; nCpu < LOCKS_COMMAND_NUM_CPU; ++nCpu)
    {
        if (!m_NextPosition[nCpu])
        {
            continue;
        }

        if (nLock++ == index)
        {
            break;
        }

        nDepth = 0;
        for (size_t j = 0; j < MAX_DESCRIPTORS; ++j)
        {
            pD = &m_pDescriptors[nCpu][j];
            if (pD->state == Inactive)
            {
                pD = 0;
                break;
            }
            if (nLock == index)
            {
                break;
            }
#if LOCKS_COMMAND_DO_BACKTRACES
            else if ((nLock < index) && (nLock + pD->n >= index))
            {
                // Backtrace frame.
                doBacktrace = true;
                break;
            }

            nLock += pD->n;
#endif

            ++nDepth;
            ++nLock;
        }

        if (pD)
        {
            break;
        }
    }

    if (!pD)
    {
        return Line;
    }

    colOffset = nDepth + 3;

#if LOCKS_COMMAND_DO_BACKTRACES
    if (doBacktrace && pD->n)
    {
        ++colOffset;

        // Not the right lock, but we do need to backtrace.
        size_t backtraceFrame = index - nLock - 1;

        if (backtraceFrame > pD->n)
        {
            ERROR_OR_FATAL("wtf");
        }

        uintptr_t addr = pD->ra[backtraceFrame];

        Line += " -> [";
        Line.append(addr, 16);
        Line += "]";

#ifndef TESTSUITE
        uintptr_t symStart = 0;
        const char *pSym =
            KernelElf::instance().globalLookupSymbol(addr, &symStart);
        if (pSym)
        {
            LargeStaticString sym(pSym);

            Line += " ";

            symbol_t symbol;
            demangle(sym, &symbol);
            Line += static_cast<const char *>(symbol.name);
        }
#endif
    }
    else if (!doBacktrace)
#endif
    {
        Line.append(reinterpret_cast<uintptr_t>(pD->pLock), 16);
        Line += " state=";
        Line += stateName(pD->state);
        Line += " caller=";
        Line.append(pD->pLock->m_Ra, 16);

#ifndef TESTSUITE
        uintptr_t symStart = 0;
        const char *pSym = KernelElf::instance().globalLookupSymbol(
            pD->pLock->m_Ra, &symStart);
        if (pSym)
        {
            LargeStaticString sym(pSym);

            Line += " ";

            symbol_t symbol;
            demangle(sym, &symbol);
            Line += static_cast<const char *>(symbol.name);
        }
#endif
    }

    colour = DebuggerIO::White;
    if (index == m_SelectedLine)
        bgColour = DebuggerIO::Blue;
    else
        bgColour = DebuggerIO::Black;

    return Line;
}

size_t LocksCommand::getLineCount()
{
    size_t numLocks = 0;
    for (size_t i = 0; i < LOCKS_COMMAND_NUM_CPU; ++i)
    {
        size_t nextPos = m_NextPosition[i];
        if (nextPos)
        {
            // For the CPU line to appear.
            ++numLocks;
        }

#if LOCKS_COMMAND_DO_BACKTRACES
        // Add backtrace frames for this lock.
        for (size_t j = 0; j < nextPos; ++j)
        {
            numLocks += m_pDescriptors[i][j].n;
        }
#endif

        numLocks += nextPos;
    }

    return numLocks;
}

void LocksCommand::setReady()
{
    g_bReady = true;
}

void LocksCommand::setFatal()
{
    m_bFatal = true;
}

void LocksCommand::clearFatal()
{
    m_bFatal = false;
}

bool LocksCommand::lockAttempted(
    const Spinlock *pLock, size_t nCpu, bool intState)
{
    if (!g_bReady)
        return true;
    if (pLock->m_bAvoidTracking)
        return true;
    if (nCpu == ~0U)
        nCpu = Processor::id();

    size_t pos = (m_NextPosition[nCpu] += 1) - 1;
    if (pos > MAX_DESCRIPTORS)
    {
        ERROR_OR_FATAL(
            "Spinlock " << Hex << pLock << " ran out of room for locks [" << Dec
                        << pos << "].");
        return false;
    }

    if (pos && intState)
    {
        // We're more than one lock deep, but interrupts are enabled!
        ERROR_OR_FATAL(
            "Spinlock " << Hex << pLock << " attempted at level " << Dec << pos
                        << Hex << " with interrupts enabled on CPU" << Dec
                        << nCpu << ".");
        return false;
    }

    LockDescriptor *pD = &m_pDescriptors[nCpu][pos];

    if (pD->state != Inactive)
    {
        ERROR_OR_FATAL("LocksCommand tracking state is corrupt.");
        return false;
    }

    pD->pLock = pLock;
    pD->state = Attempted;

#ifndef TESTSUITE
#if LOCKS_COMMAND_DO_BACKTRACES
    pD->n = 0;

    // Backtrace has to be touched carefully as it takes locks too. Also, we
    // generally don't care about the top level lock's backtrace, but rather
    // those that are nested (as they are the ones that will cause problems
    // with out-of-order release, typically).
    if (pos && Processor::isInitialised() >= 2 &&
        m_bTracing[nCpu].compareAndSwap(false, true))
    {
        Backtrace bt;
        bt.performBpBacktrace(0, 0);

        size_t numFrames = bt.numStackFrames();
        if (numFrames > NUM_BT_FRAMES)
        {
            numFrames = NUM_BT_FRAMES;
        }
        for (size_t i = 0; i < numFrames; ++i)
        {
            pD->ra[i] = bt.getReturnAddress(i);
        }
        pD->n = numFrames;

        m_bTracing[nCpu] = false;
    }
#endif
#endif

    return true;
}

bool LocksCommand::lockAcquired(
    const Spinlock *pLock, size_t nCpu, bool intState)
{
    if (!g_bReady)
        return true;
    if (pLock->m_bAvoidTracking)
        return true;
    if (nCpu == ~0U)
        nCpu = Processor::id();

    size_t back = m_NextPosition[nCpu] - 1;
    if (back > MAX_DESCRIPTORS)
    {
        ERROR_OR_FATAL(
            "Spinlock " << Hex << pLock
                        << " acquired unexpectedly (no tracked locks).");
        return false;
    }

    if (back && intState)
    {
        // We're more than one lock deep, but interrupts are enabled!
        ERROR_OR_FATAL(
            "Spinlock " << Hex << pLock << " acquired at level " << Dec << back
                        << Hex << " with interrupts enabled on CPU" << Dec
                        << nCpu << ".");
        return false;
    }

    LockDescriptor *pD = &m_pDescriptors[nCpu][back];

    if (pD->state != Attempted || pD->pLock != pLock)
    {
        ERROR_OR_FATAL(
            "Spinlock " << Hex << pLock << " acquired unexpectedly.");
        return false;
    }

    pD->state = Acquired;

    return true;
}

bool LocksCommand::lockReleased(const Spinlock *pLock, size_t nCpu)
{
    if (!g_bReady)
        return true;
    if (pLock->m_bAvoidTracking)
        return true;
    if (nCpu == ~0U)
        nCpu = Processor::id();

    size_t back = m_NextPosition[nCpu] - 1;

    LockDescriptor *pD = &m_pDescriptors[nCpu][back];

    if (pD->state != Acquired || pD->pLock != pLock)
    {
        // Maybe we need to unwind another CPU.
        /// \todo not SMP-safe...
        bool ok = false;
        for (size_t i = 0; i < LOCKS_COMMAND_NUM_CPU; ++i)
        {
            if (i == nCpu)
                continue;

            back = m_NextPosition[i] - 1;
            if (back < MAX_DESCRIPTORS)
            {
                LockDescriptor *pCheckD = &m_pDescriptors[i][back];
                if (pCheckD->state == Acquired && pCheckD->pLock == pLock)
                {
                    nCpu = i;
                    ok = true;
                    pD = pCheckD;
                    break;
                }
            }
        }

        if (!ok)
        {
            ERROR_OR_FATAL(
                "Spinlock "
                << Hex << pLock << " released out-of-order [expected lock "
                << (pD ? pD->pLock : 0) << (pD ? "" : " (no lock)")
                << ", state " << (pD ? stateName(pD->state) : "(no state)")
                << "].");
            return false;
        }
    }

    pD->pLock = 0;
    pD->state = Inactive;

    m_NextPosition[nCpu] -= 1;

    return true;
}

bool LocksCommand::checkSchedule(size_t nCpu)
{
    if (!g_bReady)
        return true;
    if (nCpu == ~0U)
        nCpu = Processor::id();

    size_t pos = m_NextPosition[nCpu];
    if (pos)
    {
        ERROR_OR_FATAL(
            "Rescheduling CPU" << nCpu << " is not allowed, as there are still "
                               << pos << " acquired locks.");
        return false;
    }

    return true;
}

bool LocksCommand::checkState(const Spinlock *pLock, size_t nCpu)
{
    if (!g_bReady)
        return true;
    if (pLock->m_bAvoidTracking)
        return true;
    if (nCpu == ~0U)
        nCpu = Processor::id();

    bool bResult = true;

    // Enter critical section for all cores.
    while (!m_bAcquiring.compareAndSwap(false, true))
        Processor::pause();

    // Check state of our lock against all other CPUs.
    for (size_t i = 0; i < LOCKS_COMMAND_NUM_CPU; ++i)
    {
        if (i == nCpu)
            continue;

        bool foundLock = false;
        LockDescriptor *pD = 0;
        for (size_t j = 0; j < m_NextPosition[i]; ++j)
        {
            pD = &m_pDescriptors[i][j];
            if (pD->state == Inactive)
            {
                pD = 0;
                break;
            }

            if (pD->pLock == pLock && pD->state == Acquired)
            {
                foundLock = true;
            }
        }

        // If the most recent lock they tried is ours, we're OK.
        if (!foundLock || !pD || pD->pLock == pLock)
        {
            continue;
        }

        if (pD->state != Attempted)
        {
            continue;
        }

        // Okay, we have an attempted lock, which we could hold.
        for (size_t j = 0; j < m_NextPosition[nCpu]; ++j)
        {
            LockDescriptor *pMyD = &m_pDescriptors[nCpu][j];
            if (pMyD->state == Inactive)
            {
                break;
            }

            if (pMyD->pLock == pD->pLock && pMyD->state == Acquired)
            {
                // We hold their attempted lock. We're waiting on them.
                // Deadlock.
                ERROR_OR_FATAL(
                    "Detected lock dependency inversion (deadlock) between "
                    << Hex << pLock << " and " << pD->pLock << "!");
                bResult = false;
                break;
            }
        }
    }

    // Done with critical section.
    m_bAcquiring = false;

    return bResult;
}
