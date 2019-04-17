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
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/StaticCord.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

extern BootstrapStruct_t *g_pBootstrapInfo;

/** Maximum number of repeated log messages to de-dupe. */
#define LOG_MAX_DEDUPE_MESSAGES     20

/** Show log timestamps in nanoseconds. */
#define LOG_TIMESTAMPS_IN_NANOS     0

Log Log::m_Instance;
EXPORTED_PUBLIC BootProgressUpdateFn g_BootProgressUpdate = 0;
EXPORTED_PUBLIC size_t g_BootProgressTotal = 0;
EXPORTED_PUBLIC size_t g_BootProgressCurrent = 0;

TinyStaticString Log::m_DebugSeverityString("(DD) ");
TinyStaticString Log::m_NoticeSeverityString("(NN) ");
TinyStaticString Log::m_WarningSeverityString("(WW) ");
TinyStaticString Log::m_ErrorSeverityString("(EE) ");
TinyStaticString Log::m_FatalSeverityString("(FF) ");

#if !SERIAL_IS_FILE
TinyStaticString Log::m_LineEnding("\r\n");
#else
TinyStaticString Log::m_LineEnding("\n");
#endif

NormalStaticString Log::m_DedupeHead("(last message+severity repeated ");
TinyStaticString Log::m_DedupeTail(" times)");

// Lookup tables to not do int->str conversions every repeated log message
static const char *g_RepeatedStrings[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20",
};

static size_t g_RepeatedLengths[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
};

static const size_t g_NumRepeatedStrings = 20;

Log::Log()
    :
      m_Lock(),
      m_StaticEntries(0), m_StaticEntryStart(0), m_StaticEntryEnd(0),
      m_Buffer(),
      m_EchoToSerial(LOG_TO_SERIAL),
      m_nOutputCallbacks(0),
      m_LastEntryHash(0),
      m_LastEntrySeverity(Fatal),
      m_HashMatchedCount(0),
      m_Timestamps(true),
      m_LastTime(0)
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
#if !ARM_COMMON
    char *cmdline = g_pBootstrapInfo->getCommandLine();
    if (cmdline)
    {
        Vector<String> cmds = String(cmdline).tokenise(' ');
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
    EMIT_IF(LOG_TO_SERIAL)
    {
        if (m_EchoToSerial)
            installSerialLogger();
    }
}

void Log::installCallback(LogCallback *pCallback, bool bSkipBacklog)
{
    {
        LockGuard<Spinlock> guard(m_Lock);
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
    LogCord msg;
    while (1)
    {
        msg.clear();

        if (entry == m_StaticEntryEnd)
        {
            break;
        }
        else if (m_StaticLog[entry].str.length())
        {
            msg.append("(backlog) ", 10);

            const TinyStaticString &severity = severityToString(m_StaticLog[entry].severity);
            msg.append(severity, severity.length());
            if (m_Timestamps)
            {
                const NormalStaticString &ts = getTimestamp();
                msg.append(ts, ts.length());
            }
            msg.append(m_StaticLog[entry].str, m_StaticLog[entry].str.length());
            msg.append(m_LineEnding, m_LineEnding.length());

            /// \note This could send a massive batch of log entries on the
            ///       callback. If the callback isn't designed to handle big
            ///       buffers this may fail.
            pCallback->callback(msg);
        }

        entry = (entry + 1) % LOG_ENTRIES;
    }
}
void Log::removeCallback(LogCallback *pCallback)
{
    LockGuard<Spinlock> guard(m_Lock);
    for (size_t i = 0; i < LOG_CALLBACK_COUNT; ++i)
    {
        if (m_OutputCallbacks[i] == pCallback)
        {
            m_OutputCallbacks[i] = nullptr;
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

Log::LogEntry::LogEntry() : timestamp(), severity(), str(), numberType(Dec)
{
    str.disableHashing();
}

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

Log::LogEntry &Log::LogEntry::operator<<(const StringView &s)
{
    str.appendBytes(s.str(), s.length());
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

    EMIT_IF(!UTILITY_LINUX)
    {
        Machine &machine = Machine::instance();
        if (machine.isInitialised() == true && machine.getTimer() != 0)
        {
            Timer &timer = *machine.getTimer();
            timestamp = timer.getTickCount();
        }
        else
            timestamp = 0;
    }

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
#if !MIPS32
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

    LogCord msg;
    msg.clear();

    if (lock)
        m_Lock.acquire();

    if (m_StaticEntries >= LOG_ENTRIES)
    {
        m_StaticEntryStart = (m_StaticEntryStart + 1) % LOG_ENTRIES;
    }
    else
        m_StaticEntries++;

    m_StaticLog[m_StaticEntryEnd] = m_Buffer;
    m_StaticEntryEnd = (m_StaticEntryEnd + 1) % LOG_ENTRIES;

    // no need for lock anymore - all tracked now
    // remaining work hits callbacks which can lock themselves
    if (lock)
        m_Lock.release();

    if (m_nOutputCallbacks)
    {
        bool wasRepeated = false;
        uint64_t repeatedTimes = 0;

        // Have we seen this message before?
        m_Buffer.str.allowHashing(true);  // calculate hash now
        uint64_t currentHash = m_Buffer.str.hash();
        m_Buffer.str.disableHashing();
        if (currentHash == m_LastEntryHash)
        {
            if (m_LastEntrySeverity == m_Buffer.severity)
            {
                ++m_HashMatchedCount;

                if (m_HashMatchedCount < LOG_MAX_DEDUPE_MESSAGES)
                {
                    return;
                }
            }
        }

        if (m_HashMatchedCount)
        {
            wasRepeated = true;
            repeatedTimes = m_HashMatchedCount;
            m_HashMatchedCount = 0;
        }

        m_LastEntryHash = currentHash;
        m_LastEntrySeverity = m_Buffer.severity;

        // We have output callbacks installed. Build the string we'll pass
        // to each callback *now* and then send it.
        if (wasRepeated)
        {
            msg.append(m_DedupeHead, m_DedupeHead.length());
            if (repeatedTimes < g_NumRepeatedStrings)
            {
                msg.append(g_RepeatedStrings[repeatedTimes], g_RepeatedLengths[repeatedTimes]);
            }
            else
            {
                TinyStaticString repeated;
                repeated.append(repeatedTimes);
                msg.append(repeated, repeated.length());
            }
            msg.append(m_DedupeTail, m_DedupeTail.length());
            msg.append(m_LineEnding, m_LineEnding.length());
        }

        const TinyStaticString &severity = severityToString(m_Buffer.severity);
        msg.append(severity, severity.length());
        if (m_Timestamps)
        {
            const NormalStaticString &ts = getTimestamp();
            msg.append(ts, ts.length());
        }
        msg.append(m_Buffer.str, m_Buffer.str.length());
        msg.append(m_LineEnding, m_LineEnding.length());

        for (size_t i = 0; i < LOG_CALLBACK_COUNT; ++i)
        {
            if (m_OutputCallbacks[i] != nullptr)
            {
                m_OutputCallbacks[i]->callback(msg);
            }
        }
    }

    // Panic if that was a fatal error.
    if ((!handlingFatal) && m_Buffer.severity == Fatal)
    {
        handlingFatal = true;

        const char *panicstr = static_cast<const char *>(m_Buffer.str);

        // Attempt to trap to debugger, panic if that fails.
        EMIT_IF(DEBUGGER)
        {
            Processor::breakpoint();
        }
        panic(panicstr);
    }
}

void Log::enableTimestamps()
{
    m_Timestamps = true;
}

void Log::disableTimestamps()
{
    m_Timestamps = false;
}

const NormalStaticString &Log::getTimestamp()
{
    Time::Timestamp tn = Time::getTimeNanoseconds();
    Time::Timestamp ts = Time::getTime();
    Time::Timestamp t;
    EMIT_IF(LOG_TIMESTAMPS_IN_NANOS)
    {
        t = tn;
    }
    else
    {
        t = ts;
    }
    if (t == m_LastTime)
    {
        return m_CachedTimestamp;
    }

    m_LastTime = t;

    NormalStaticString r;
    r += "[";
    r.append(t);
    r += ".";
    r.append(Processor::id());
    r += "] ";

    m_CachedTimestamp = r;
    return m_CachedTimestamp;
}

const TinyStaticString &Log::severityToString(SeverityLevel level) const
{
    switch (level)
    {
        case Debug:
            return m_DebugSeverityString;
        case Notice:
            return m_NoticeSeverityString;
        case Warning:
            return m_WarningSeverityString;
        case Error:
            return m_ErrorSeverityString;
        default:
            return m_FatalSeverityString;
    }
}

Log::LogCallback::~LogCallback() = default;
