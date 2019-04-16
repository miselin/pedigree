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

#ifndef KERNEL_LOG_H
#define KERNEL_LOG_H

#include "pedigree/kernel/compiler.h"
#ifdef THREADS
#include "pedigree/kernel/Spinlock.h"
#endif
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/StaticCord.h"
#include "pedigree/kernel/time/Time.h"

class String;
class StringView;

/** @addtogroup kernel
 * @{ */

#define SHOW_FILE_IN_LOGS 0

typedef StaticCord<8> LogCord;

#if SHOW_FILE_IN_LOGS
#define FILE_LOG(entry, level)                                             \
    do                                                                     \
    {                                                                      \
        entry << level << __FILE__ << ":" << Dec << __LINE__ << Hex << " " \
              << __FUNCTION__ << " -- ";                                   \
    } while (0)
#else
#define FILE_LOG(entry, level)
#endif

#define LOG_AT_LEVEL(level, text, lock)                       \
    do                                                        \
    {                                                         \
        Log::LogEntry __log_macro_logentry;                   \
        FILE_LOG(__log_macro_logentry, level);                \
        __log_macro_logentry << level << text;                \
        Log::instance().addEntry(__log_macro_logentry, lock); \
    } while (0)

#ifndef NO_LOGGING

/** Add a debug item to the log */
#if DEBUG_LOGGING
#define DEBUG_LOG(text) LOG_AT_LEVEL(Log::Debug, text, 1)
#define DEBUG_LOG_NOLOCK(text) LOG_AT_LEVEL(Log::Debug, text, 0)
#else
#define DEBUG_LOG(text)
#define DEBUG_LOG_NOLOCK(text)
#endif

/** Add a notice to the log */
#define NOTICE(text) LOG_AT_LEVEL(Log::Notice, text, 1)
#define NOTICE_NOLOCK(text) LOG_AT_LEVEL(Log::Notice, text, 0)

/** Add a warning message to the log */
#define WARNING(text) LOG_AT_LEVEL(Log::Warning, text, 1)
#define WARNING_NOLOCK(text) LOG_AT_LEVEL(Log::Warning, text, 0)

/** Add a error message to the log */
#define ERROR(text) LOG_AT_LEVEL(Log::Error, text, 1)
#define ERROR_NOLOCK(text) LOG_AT_LEVEL(Log::Error, text, 0)

/** Add a fatal message to the log
 *  Breaks into debugger and panics if the debugger isn't around, or the user
 *  exits it.
 */
#define FATAL(text)                        \
    do                                     \
    {                                      \
        LOG_AT_LEVEL(Log::Fatal, text, 1); \
        while (1)                          \
            ;                              \
    } while (0)
#define FATAL_NOLOCK(text)                 \
    do                                     \
    {                                      \
        LOG_AT_LEVEL(Log::Fatal, text, 0); \
        while (1)                          \
            ;                              \
    } while (0)

#ifdef PEDANTIC_PEDIGREE
#define PEDANTRY FATAL
#else
#define PEDANTRY WARNING
#endif

#else  // NO_LOGGING

#define DEBUG_LOG(text)
#define DEBUG_LOG_NOLOCK(text)
#define NOTICE(text)
#define NOTICE_NOLOCK(text)
#define WARNING(text)
#define WARNING_NOLOCK(text)
#define ERROR(text)
#define ERROR_NOLOCK(text)
#define FATAL(text)
#define FATAL_NOLOCK(text)
#define PEDANTRY(text)

#endif

/** The maximum length of an individual static log entry. */
#define LOG_LENGTH 128
/** The maximum number of static entries in the log. */
#ifdef HUGE_STATIC_LOG
// 2MB static log buffer
#define LOG_ENTRIES ((1 << 21) / sizeof(LogEntry))
#else
// 64K static log buffer
#define LOG_ENTRIES ((1 << 16) / sizeof(LogEntry))
#endif
/** Maximum number of output callbacks that can be registered. */
#define LOG_CALLBACK_COUNT 16

/** Radix for Log's integer output */
enum NumberType
{
    /** Hexadecimal */
    Hex,
    /** Decimal */
    Dec,
    /** Octal */
    Oct
};

/** Modifiers for Log */
enum Modifier
{
    /** Flush this log entry */
    Flush
};

// Function pointer to update boot progress -
// Description.
typedef void (*BootProgressUpdateFn)(const char *);

extern size_t g_BootProgressCurrent;
extern size_t g_BootProgressTotal;
extern BootProgressUpdateFn g_BootProgressUpdate;

extern void installSerialLogger();

/** Implements a kernel log that can be used to debug problems.
 *\brief the kernel's log
 *\note You should use the NOTICE, WARNING, ERROR and FATAL macros to write
 *something into the log. Direct access to the log should only be needed to
 *retrieve the entries from the log (within the debugger's log viewer for
 *example). */
class Log
{
  public:
    struct LogEntry;

    /** Output callback function type. Inherit and implement callback to use. */
    class EXPORTED_PUBLIC LogCallback
    {
      public:
        virtual void callback(const LogCord &cord) = 0;
        virtual ~LogCallback();
    };

    /** Severity level of the log entry */
    enum SeverityLevel
    {
        Debug = 0,
        Notice,
        Warning,
        Error,
        Fatal
    };

/** The lock
 *\note this should only be acquired by the NOTICE, WARNING, ERROR and FATAL
 *macros */
#ifdef THREADS
    Spinlock m_Lock;
#endif

    /** Retrieves the static Log instance.
     *\return instance of the log class */
    EXPORTED_PUBLIC static Log &instance();

    /** Initialises the Log */
    void initialise1();

    /** Initialises the default Log callback (to a serial port) */
    void initialise2();

    /** Installs an output callback */
    EXPORTED_PUBLIC void
    installCallback(LogCallback *pCallback, bool bSkipBacklog = false);

    /** Removes an output callback */
    EXPORTED_PUBLIC void removeCallback(LogCallback *pCallback);

    /** Adds an entry to the log. */
    EXPORTED_PUBLIC Log &operator<<(const LogEntry &entry);

    /** Modifier */
    EXPORTED_PUBLIC Log &operator<<(Modifier type);

    /** Adds an entry to the log and immediately flushes. */
    EXPORTED_PUBLIC void
    addEntry(const LogEntry &entry, bool lock = true, bool flush = true);

    /** Perform a flush. */
    void flushEntry(bool lock = true);

    /** Get the number of static entries in the log.
     *\return the number of static entries in the log */
    size_t getStaticEntryCount() const;
    /** Get the number of dynamic entries in the log
     *\return the number of dynamic entries in the log */
    size_t getDynamicEntryCount() const;

    /** Stores an entry in the log.
     *\param[in] T type of the log's text */
    struct EXPORTED_PUBLIC LogEntry
    {
        /** Constructor does nothing */
        LogEntry();

        /** The time (since boot) that this log entry was added, in ticks. */
        unsigned int timestamp;
        /** The severity level of this entry. */
        SeverityLevel severity;
        /** The actual entry text. */
        StaticString<LOG_LENGTH> str;
        /** The number type mode that we are in. */
        NumberType numberType;

        /** Adds an entry to the log.
         *\param[in] str the null-terminated ASCII string that should be added
         */
        LogEntry &operator<<(const char *);
        LogEntry &operator<<(const String &);
        LogEntry &operator<<(const StringView &);
        /** Adds an entry to the log
         *\param[in] str the null-terminated ASCII string that should be added
         */
        LogEntry &operator<<(char *append_str);
        /** Adds an entry to the log
         *\param[in] b boolean value */
        LogEntry &operator<<(bool b);
        /** Adds an entry to the log
         *\param[in] p pointer value */
        template <class T>
        LogEntry &operator<<(T *p)
        {
            // Preserve the current number type but always print pointers as
            // hex.
            NumberType currentNumberType = numberType;
            return (*this) << Hex << (reinterpret_cast<uintptr_t>(p))
                           << currentNumberType;
        }
        /** Adds an entry to the log (integer type)
         *\param[in] n the number */
        template <class T>
        LogEntry &operator<<(T n);

        /** Starts an entry in the log. */
        LogEntry &operator<<(SeverityLevel level);
        /** Changes the number type between hex and decimal. */
        LogEntry &operator<<(NumberType type);
    };

    /** Type of a static log entry (no memory-management involved) */
    typedef LogEntry StaticLogEntry;
    typedef LogEntry DynamicLogEntry;

    /** Returns the n'th static log entry, counting from the start. */
    const StaticLogEntry &getStaticEntry(size_t n) const;
    /** Returns the (n - getStaticEntryCount())'th dynamic log entry */
    const DynamicLogEntry &getDynamicEntry(size_t n) const;

    bool echoToSerial();

    const LogEntry &getLatestEntry() const;

    void enableTimestamps();
    void disableTimestamps();

  private:
    /** Default constructor - does nothing. */
    Log();
    /** Default destructor - does nothing */
    ~Log();
    /** Copy-constructor
     *\note NOT implemented */
    Log(const Log &);
    /** Assignment operator
     *\note NOT implemented */
    Log &operator=(const Log &);

    const NormalStaticString &getTimestamp();

    const TinyStaticString &severityToString(SeverityLevel level) const;

    /** Static buffer of log messages. */
    StaticLogEntry m_StaticLog[LOG_ENTRIES];
    /** Dynamic buffer of log messages */
    //  Vector<DynamicLogEntry*> m_DynamicLog;
    /** Number of entries in the static log */
    size_t m_StaticEntries;

    size_t m_StaticEntryStart, m_StaticEntryEnd;

    /** Temporary buffer which gets filled by calls to operator<<, and flushed
     * by << Flush. */
    StaticLogEntry m_Buffer;

    /** If we should output to serial */
    bool m_EchoToSerial;

    /** Output callback list */
    LogCallback *m_OutputCallbacks[LOG_CALLBACK_COUNT];
    size_t m_nOutputCallbacks;

    /** The Log instance (singleton class) */
    EXPORTED_PUBLIC static Log m_Instance;

    /** Last seen message hash (for cleaning up dupes). */
    uint64_t m_LastEntryHash;

    /** Last seen message severity (for cleaning up dupes). */
    SeverityLevel m_LastEntrySeverity;

    /** Number of entries that matched the last entry hash. */
    size_t m_HashMatchedCount;

    /** Are timestamps enabled? */
    bool m_Timestamps;

    /** Last timestamp seen in getTimestamp(). */
    Time::Timestamp m_LastTime;

    /** Cached timestamp string. */
    NormalStaticString m_CachedTimestamp;

    /** Log severity tag strings. */
    static TinyStaticString m_DebugSeverityString;
    static TinyStaticString m_NoticeSeverityString;
    static TinyStaticString m_WarningSeverityString;
    static TinyStaticString m_ErrorSeverityString;
    static TinyStaticString m_FatalSeverityString;

    /** Log line ending string. */
    static TinyStaticString m_LineEnding;

    /** Dedupe information strings. */
    static NormalStaticString m_DedupeHead;
    static TinyStaticString m_DedupeTail;
};

/** @} */

#endif
