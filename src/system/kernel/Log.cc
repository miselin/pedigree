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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/utility.h"

extern BootstrapStruct_t *g_pBootstrapInfo;

Log Log::m_Instance;
EXPORTED_PUBLIC BootProgressUpdateFn g_BootProgressUpdate = 0;
EXPORTED_PUBLIC size_t g_BootProgressTotal = 0;
EXPORTED_PUBLIC size_t g_BootProgressCurrent = 0;

static NormalStaticString getTimestamp()
{
    Time::Timestamp t = Time::getTime();

    NormalStaticString r;
    r += "[";
    r.append(t);
    r += ".";
    r.append(Processor::id());
    r += "] ";
    return r;
}

Log::Log()
    :
#ifdef THREADS
      m_Lock(),
#endif
      m_StaticEntries(0), m_StaticEntryStart(0), m_StaticEntryEnd(0),
      m_Buffer(),
#ifdef DONT_LOG_TO_SERIAL
      m_EchoToSerial(false),
#else
      m_EchoToSerial(true),
#endif
      m_nOutputCallbacks(0)
{
    for (size_t i = 0; i < LOG_CALLBACK_COUNT; ++i)
    {
        m_OutputCallbacks[i] = nullptr;
    }
}

Log::~Log()
{
    LogEntry entry;
    entry << Notice << "-- Log Terminating --";
    addEntry(entry);
}

Log &Log::instance()
{
    return m_Instance;
}

void Log::initialise1()
{
#ifndef ARM_COMMON
    char *cmdline = g_pBootstrapInfo->getCommandLine();
    if (cmdline)
    {
        List<String> cmds = String(cmdline).tokenise(' ');
        for (auto it = cmds.begin(); it != cmds.end(); it++)
        {
            auto cmd = *it;
            if (cmd == String("--disable-log-to-serial"))
            {
                m_EchoToSerial = false;
                break;
            }
            else if (cmd == String("--enable-log-to-serial"))
            {
                m_EchoToSerial = true;
                break;
            }
        }
    }
#endif
}

void Log::initialise2()
{
#ifndef DONT_LOG_TO_SERIAL
    if (m_EchoToSerial)
        installSerialLogger();
#endif
}

void Log::installCallback(LogCallback *pCallback, bool bSkipBacklog)
{
    {
#ifdef THREADS
        LockGuard<Spinlock> guard(m_Lock);
#endif
        bool ok = false;
        for (size_t i = 0; i < LOG_CALLBACK_COUNT; ++i)
        {
            if (m_OutputCallbacks[i] == nullptr)
            {
                m_OutputCallbacks[i] = pCallback;
                ++m_nOutputCallbacks;
                ok = true;
                break;
            }
        }

        if (!ok)
        {
            /// \todo installCallback should return success/failure
            return;
        }
    }

    // Some callbacks want to skip a (potentially) massive backlog
    if (bSkipBacklog)
        return;

    // Call the callback for the existing, flushed, log entries
    size_t entry = m_StaticEntryStart;
    while (1)
    {
        if (entry == m_StaticEntryEnd)
            break;
        else
        {
            HugeStaticString str;
            switch (m_StaticLog[entry].severity)
            {
                case Debug:
                    str = "(DD) ";
                    break;
                case Notice:
                    str = "(NN) ";
                    break;
                case Warning:
                    str = "(WW) ";
                    break;
                case Error:
                    str = "(EE) ";
                    break;
                case Fatal:
                    str = "(FF) ";
                    break;
            }
            str += getTimestamp();
            str += m_StaticLog[entry].str;
#ifndef SERIAL_IS_FILE
            str += "\r\n";  // Handle carriage return
#else
            str += "\n";
#endif

            /// \note This could send a massive batch of log entries on the
            ///       callback. If the callback isn't designed to handle big
            ///       buffers this may fail.
            pCallback->callback(str);
        }

        entry = (entry + 1) % LOG_ENTRIES;
    }
}
void Log::removeCallback(LogCallback *pCallback)
{
#ifdef THREADS
    LockGuard<Spinlock> guard(m_Lock);
#endif
    for (size_t i = 0; i < LOG_CALLBACK_COUNT; ++i)
    {
        if (m_OutputCallbacks[i] == nullptr)
        {
            m_OutputCallbacks[i] = pCallback;
            --m_nOutputCallbacks;
            break;
        }
    }
}

size_t Log::getStaticEntryCount() const
{
    return m_StaticEntries;
}

size_t Log::getDynamicEntryCount() const
{
    return 0;
}

const Log::StaticLogEntry &Log::getStaticEntry(size_t n) const
{
    return m_StaticLog[(m_StaticEntryStart + n) % LOG_ENTRIES];
}
/** Returns the (n - getStaticEntryCount())'th dynamic log entry */
const Log::DynamicLogEntry &Log::getDynamicEntry(size_t n) const
{
    return m_StaticLog[0];
}

bool Log::echoToSerial()
{
    return m_EchoToSerial;
}

const Log::LogEntry &Log::getLatestEntry() const
{
    return m_StaticLog[m_StaticEntries - 1];
}

Log::LogEntry::LogEntry() : timestamp(), severity(), str(), numberType(Dec) {}

Log::LogEntry &Log::LogEntry::operator<<(const char *s)
{
    str.append(s);
    return *this;
}

Log::LogEntry &Log::LogEntry::operator<<(const String &s)
{
    str.appendBytes(s, s.length());
    return *this;
}

Log::LogEntry &Log::LogEntry::operator<<(char *append_str)
{
    return (*this) << (reinterpret_cast<const char *>(append_str));
}

Log::LogEntry &Log::LogEntry::operator<<(bool b)
{
    if (b)
        return *this << "true";

    return *this << "false";
}

template <class T>
Log::LogEntry &Log::LogEntry::operator<<(T n)
{
    size_t radix = 10;
    if (numberType == Hex)
    {
        radix = 16;
        str.append("0x");
    }
    else if (numberType == Oct)
    {
        radix = 8;
        str.append("0");
    }
    str.append(n, radix);
    return *this;
}

Log::LogEntry &Log::LogEntry::operator<<(NumberType type)
{
    numberType = type;
    return *this;
}

Log::LogEntry &Log::LogEntry::operator<<(SeverityLevel level)
{
    // Zero the buffer.
    str.clear();
    severity = level;

#ifndef UTILITY_LINUX
    Machine &machine = Machine::instance();
    if (machine.isInitialised() == true && machine.getTimer() != 0)
    {
        Timer &timer = *machine.getTimer();
        timestamp = timer.getTickCount();
    }
    else
        timestamp = 0;
#endif

    return *this;
}

// NOTE: Make sure that the templated << operator gets only instantiated for
//       integer types.
template Log::LogEntry &Log::LogEntry::operator<<(char);
template Log::LogEntry &Log::LogEntry::operator<<(unsigned char);
template Log::LogEntry &Log::LogEntry::operator<<(short);
template Log::LogEntry &Log::LogEntry::operator<<(unsigned short);
template Log::LogEntry &Log::LogEntry::operator<<(int);
template Log::LogEntry &Log::LogEntry::operator<<(unsigned int);
template Log::LogEntry &Log::LogEntry::operator<<(long);
template Log::LogEntry &Log::LogEntry::operator<<(unsigned long);
// NOTE: Instantiating these for MIPS32 requires __udiv3di, but we only have
//       __udiv3ti (??) in libgcc.a for mips.
#ifndef MIPS32
template Log::LogEntry &Log::LogEntry::operator<<(long long);
template Log::LogEntry &Log::LogEntry::operator<<(unsigned long long);
#endif

Log &Log::operator<<(const LogEntry &entry)
{
    m_Buffer = entry;
    return *this;
}

Log &Log::operator<<(Modifier type)
{
    // Flush the buffer.
    if (type == Flush)
    {
        flushEntry();
    }

    return *this;
}

void Log::addEntry(const LogEntry &entry, bool lock, bool flush)
{
    m_Buffer = entry;
    if (flush)
    {
        flushEntry(lock);
    }
}

void Log::flushEntry(bool lock)
{
    static bool handlingFatal = false;

#ifdef THREADS
    if (lock) m_Lock.acquire();
#endif

    if (m_StaticEntries >= LOG_ENTRIES)
    {
        m_StaticEntryStart = (m_StaticEntryStart + 1) % LOG_ENTRIES;
    }
    else
        m_StaticEntries++;

    m_StaticLog[m_StaticEntryEnd] = m_Buffer;
    m_StaticEntryEnd = (m_StaticEntryEnd + 1) % LOG_ENTRIES;

#ifdef THREADS
    // no need for lock anymore - all tracked now
    // remaining work hits callbacks which can lock themselves
    if (lock) m_Lock.release();
#endif

    if (m_nOutputCallbacks)
    {
        // We have output callbacks installed. Build the string we'll pass
        // to each callback *now* and then send it.
        HugeStaticString str;
        switch (m_Buffer.severity)
        {
            case Debug:
                str = "(DD) ";
                break;
            case Notice:
                str = "(NN) ";
                break;
            case Warning:
                str = "(WW) ";
                break;
            case Error:
                str = "(EE) ";
                break;
            case Fatal:
                str = "(FF) ";
                break;
        }
        str += getTimestamp();
        str += m_Buffer.str;
#ifndef SERIAL_IS_FILE
        str += "\r\n";  // Handle carriage return
#else
        str += "\n";
#endif

        for (size_t i = 0; i < LOG_CALLBACK_COUNT; ++i)
        {
            if (m_OutputCallbacks[i] != nullptr)
            {
                m_OutputCallbacks[i]->callback(
                    static_cast<const char *>(str));
            }
        }
    }

    // Panic if that was a fatal error.
    if ((!handlingFatal) && m_Buffer.severity == Fatal)
    {
        handlingFatal = true;

        const char *panicstr = static_cast<const char *>(m_Buffer.str);

// Attempt to trap to debugger, panic if that fails.
#ifdef DEBUGGER
        Processor::breakpoint();
#endif
        panic(panicstr);
    }
}

Log::LogCallback::~LogCallback()
{
}
