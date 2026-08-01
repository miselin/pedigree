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

#if THREADS

#include "pedigree/kernel/debugger/commands/ThreadsCommand.h"
#include "pedigree/kernel/debugger/DebuggerIO.h"
#include "pedigree/kernel/debugger/commands/ThreadWaitDiagnostic.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/demangle.h"
#include "pedigree/kernel/utilities/utility.h"

ThreadsCommand::ThreadsCommand()
    : DebuggerCommand(), Scrollable(), m_SelectedLine(0), m_nLines(0)
{
}

ThreadsCommand::~ThreadsCommand()
{
}

static const char *debugStateName(Thread::DebugState state)
{
    switch (state)
    {
        case Thread::SemWait:
            return "Sem-Wait";
        case Thread::CondWait:
            return "Cond-Wait";
        case Thread::Joining:
            return "Joining";
        case Thread::FutexWait:
            return "Futex-Wait";
        case Thread::EventWait:
            return "Event-Wait";
        case Thread::ProcessWait:
            return "Process-Wait";
        case Thread::CallbackDrain:
            return "Callback-Drain";
        case Thread::None:
            return nullptr;
    }
    return "<unknown DebugState>";
}

static void appendWaitState(Thread *thread, HugeStaticString &line)
{
    ThreadWaitDiagnostic diagnostic;
    diagnostic.sleeping = thread->getStatus() == Thread::Sleeping;

    Thread::WaitDebugInfo wait;
    if (!thread->getWaitDebugInfo(wait))
    {
        appendThreadWaitDiagnostic(diagnostic, line);
        return;
    }

    diagnostic.hasWait = true;
    diagnostic.queue = reinterpret_cast<uintptr_t>(wait.queue);
    diagnostic.channelOwner =
        reinterpret_cast<uintptr_t>(wait.channelOwner);
    diagnostic.channelValue = wait.channelValue;
    diagnostic.reason = wait.reason;
    diagnostic.stateLevel = wait.stateLevel;
    diagnostic.queued = wait.queued;

    uintptr_t debugAddress = 0;
    if (
        thread->getDebugState(debugAddress) == Thread::SemWait &&
        wait.channelOwner)
    {
        const Semaphore *semaphore =
            reinterpret_cast<const Semaphore *>(wait.channelOwner);
        const void *owner = semaphore->getDebugMutexOwner();
        if (owner)
        {
            diagnostic.mutexOwner = reinterpret_cast<uintptr_t>(owner);
        }
    }

    appendThreadWaitDiagnostic(diagnostic, line);
}

static void selectLine(
    size_t index, Scheduler::ProcessLease &selectedProcess,
    Process::ThreadLease &selectedThread)
{
    size_t line = 0;
    for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); ++i)
    {
        Scheduler::ProcessLease process;
        if (!Scheduler::instance().acquireProcess(process, i))
        {
            continue;
        }

        if (index == line)
        {
            selectedProcess = pedigree_std::move(process);
            return;
        }
        ++line;

        for (size_t j = 0; j < process->getNumThreads(); ++j)
        {
            Process::ThreadLease thread;
            const bool threadAcquired =
                process->acquireThread(thread, j);
            if (index == line)
            {
                if (threadAcquired)
                {
                    selectedProcess = pedigree_std::move(process);
                    selectedThread = pedigree_std::move(thread);
                }
                return;
            }
            ++line;
        }
    }
}

void ThreadsCommand::autocomplete(
    const HugeStaticString &input, HugeStaticString &output)
{
}

bool ThreadsCommand::execute(
    const HugeStaticString &input, HugeStaticString &output,
    InterruptState &state, DebuggerIO *pScreen)
{
    // How many lines do we have?
    m_nLines = 0;
    for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); i++)
    {
        Scheduler::ProcessLease process;
        if (!Scheduler::instance().acquireProcess(process, i))
        {
            continue;
        }
        m_nLines++;  // For the process.
        m_nLines += process->getNumThreads();
    }

    // Let's enter 'raw' screen mode.
    pScreen->disableCli();

    // Initialise the Scrollable class
    move(0, 1);
    resize(pScreen->getWidth(), pScreen->getHeight() - 2);
    setScrollKeys('j', 'k');

    // Clear the top status lines.
    pScreen->drawHorizontalLine(
        ' ', 0, 0, pScreen->getWidth() - 1, DebuggerIO::White,
        DebuggerIO::Green);

    // Write the correct text in the upper status line.
    pScreen->drawString(
        "Pedigree debugger - Thread selector", 0, 0, DebuggerIO::White,
        DebuggerIO::Green);

    // Clear the bottom status lines.
    // TODO: If we use arrow keys and page up/down keys we actually can remove
    // the status line
    //       because the interface is then intuitive enough imho.
    pScreen->drawHorizontalLine(
        ' ', pScreen->getHeight() - 1, 0, pScreen->getWidth() - 1,
        DebuggerIO::White, DebuggerIO::Green);

    // Write some helper text in the lower status line.
    // TODO FIXME: Drawing this might screw the top status bar
    pScreen->drawString(
        "backspace: Page up. space: Page down. q: Quit. enter: Switch to "
        "thread",
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
    pScreen->drawString(
        "enter", pScreen->getHeight() - 1, 47, DebuggerIO::Yellow,
        DebuggerIO::Green);

    // Main loop.
    bool bStop = false;
    bool bReturn = true;
    while (!bStop)
    {
        refresh(pScreen);

        // Wait for input.
        char c = 0;
        while (!(c = pScreen->getChar()))
            ;

        // TODO: Use arrow keys and page up/down someday
        if (c == 'j')
        {
            scroll(-1);
            if (static_cast<ssize_t>(m_SelectedLine) - 1 >= 0)
                m_SelectedLine--;
        }
        else if (c == 'k')
        {
            scroll(1);
            if (m_SelectedLine + 1 < getLineCount())
                m_SelectedLine++;
        }
        else if (c == ' ')
        {
            scroll(static_cast<ssize_t>(height()));
            if (m_SelectedLine + height() < getLineCount())
                m_SelectedLine += height();
            else
                m_SelectedLine = getLineCount() - 1;
        }
        else if (c == 0x08)
        {
            scroll(-static_cast<ssize_t>(height()));
            if (static_cast<ssize_t>(m_SelectedLine) -
                    static_cast<ssize_t>(height()) >=
                0)
                m_SelectedLine -= height();
            else
                m_SelectedLine = 0;
        }
        else if (c == '\n' || c == '\r')
        {
            //      if(swapThread(state, pScreen))
            //      {
            //        bStop = true;
            //        bReturn = false;
            //      }
        }
        else if (c == 'q')
            bStop = true;
    }

    // HACK:: Serial connections will fill the screen with the last background
    // colour used.
    //        Here we write a space with black background so the CLI screen
    //        doesn't get filled by some random colour!
    pScreen->drawString(" ", 1, 0, DebuggerIO::White, DebuggerIO::Black);
    pScreen->enableCli();
    return bReturn;
}

const char *ThreadsCommand::getLine1(
    size_t index, DebuggerIO::Colour &colour, DebuggerIO::Colour &bgColour)
{
    static NormalStaticString Line;
    Line.clear();

    Scheduler::ProcessLease process;
    Process::ThreadLease thread;
    selectLine(index, process, thread);
    Process *tehProcess = process.get();
    Thread *tehThread = thread.get();

    if (!tehProcess)
    {
        // No processes, or the index does not match.
        return Line;
    }

    // If this is just a process line.
    colour = DebuggerIO::Yellow;
    if (index == m_SelectedLine)
        bgColour = DebuggerIO::Blue;
    else
        bgColour = DebuggerIO::Black;
    if (tehThread == 0)
    {
        Line += "[";
        Line += tehProcess->getId();
        Line += "] ";
        Line += tehProcess->description();
    }
    else
    {
        Line += " | ";
    }

    return Line;
}
const char *ThreadsCommand::getLine2(
    size_t index, size_t &colOffset, DebuggerIO::Colour &colour,
    DebuggerIO::Colour &bgColour)
{
    static HugeStaticString Line;
    Line.clear();

    Scheduler::ProcessLease process;
    Process::ThreadLease thread;
    selectLine(index, process, thread);
    Thread *tehThread = thread.get();

    if (tehThread != 0 &&
        tehThread != Processor::information().getCurrentThread())
    {
        Thread::Status status = tehThread->getStatus();
        switch (status)
        {
            case Thread::Created:
                Line += "C";
                break;
            case Thread::Running:
                Line += "r";
                break;
            case Thread::Ready:
                Line += "R";
                break;
            case Thread::Sleeping:
                Line += "S";
                break;
            case Thread::Zombie:
                Line += "Z";
                break;
            case Thread::AwaitingJoin:
                Line += "J";
                break;
        }

        Line += "[";
        Line += "CPU";
        Line += tehThread->getCpuId();
        Line += ":";
        Line += tehThread->getId();
        Line += "] ";

        uintptr_t ip = 0;
        Thread::DebugState state = tehThread->getDebugState(ip);
        if (state != Thread::None)
        {
            Line += debugStateName(state);
            Line += " @ ";
            Line.append(ip, 16);

            uintptr_t symStart;
            const char *pSym =
                KernelElf::instance().globalLookupSymbol(ip, &symStart);
            if (pSym)
            {
                LargeStaticString sym;
                demangle_full(LargeStaticString(pSym), sym);
                Line += ": ";
                Line += sym;
            }
        }
        else
        {
            // Extract the last instruction pointer from the scheduler state
            const SchedulerState &state = tehThread->state();
            ip = state.getInstructionPointer();

            Line += " @ ";
            Line.append(ip, 16);

            uintptr_t symStart;
            const char *pSym =
                KernelElf::instance().globalLookupSymbol(ip, &symStart);
            if (pSym)
            {
                LargeStaticString sym;
                demangle_full(LargeStaticString(pSym), sym);
                Line += ": ";
                Line += sym;
            }
        }
        appendWaitState(tehThread, Line);
        colour = DebuggerIO::LightGrey;
    }
    else if (tehThread != 0)  // tehThread == g_pCurrentThread
    {
        Thread::Status status = tehThread->getStatus();
        switch (status)
        {
            case Thread::Created:
                Line += "C";
                break;
            case Thread::Running:
                Line += "r";
                break;
            case Thread::Ready:
                Line += "R";
                break;
            case Thread::Sleeping:
                Line += "S";
                break;
            case Thread::Zombie:
                Line += "Z";
                break;
            case Thread::AwaitingJoin:
                Line += "J";
                break;
        }
        Line += "[";
        Line += "CPU";
        Line += tehThread->getCpuId();
        Line += ":";
        Line += tehThread->getId();
        Line += "] - CURRENT ";

        uintptr_t ip = 0;
        Thread::DebugState state = tehThread->getDebugState(ip);
        if (state != Thread::None)
        {
            Line += debugStateName(state);
            Line += " @ ";
            Line.append(ip, 16);

            uintptr_t symStart;
            const char *pSym =
                KernelElf::instance().globalLookupSymbol(ip, &symStart);
            if (pSym)
            {
                LargeStaticString sym;
                demangle_full(LargeStaticString(pSym), sym);
                Line += ": ";
                Line += sym;
            }
        }
        appendWaitState(tehThread, Line);

        colour = DebuggerIO::Yellow;
    }

    if (index == m_SelectedLine)
        bgColour = DebuggerIO::Blue;
    else
        bgColour = DebuggerIO::Black;
    colOffset = 3;
    return Line;
}

size_t ThreadsCommand::getLineCount()
{
    return m_nLines;
}

bool ThreadsCommand::swapThread(InterruptState &state, DebuggerIO *pScreen)
{
    Scheduler::ProcessLease process;
    Process::ThreadLease thread;
    selectLine(m_SelectedLine, process, thread);
    Thread *tehThread = thread.get();

    // We can only swap to threads, not entire processes!
    if (tehThread == 0)
        return false;

    pScreen->destroy();
    //  Scheduler::instance().switchToAndDebug(state, tehThread);

    return true;
}

#endif
