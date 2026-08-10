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

#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Cord.h"
#include "pedigree/kernel/utilities/StaticCord.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

extern BootstrapStruct_t* g_pBootstrapInfo;

/** Maximum number of repeated log messages to de-dupe. */
#define LOG_MAX_DEDUPE_MESSAGES 20

/** Show log timestamps in nanoseconds. */
#define LOG_TIMESTAMPS_IN_NANOS 0

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
static const char* g_RepeatedStrings[] = {
    "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",  "10",
    "11", "12", "13", "14", "15", "16", "17", "18", "19", "20",
};

static size_t g_RepeatedLengths[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
};

static const size_t g_NumRepeatedStrings = 20;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
Log::CallbackPinHook Log::m_CallbackPinHook = nullptr;
Log::EntrySnapshotHook Log::m_EntrySnapshotHook = nullptr;
#endif

Log::Log()
    : m_Lock(),
      m_StaticLog(),
      m_StaticEntries(0),
      m_StaticEntryStart(0),
      m_StaticEntryEnd(0),
      m_EchoToSerial(LOG_TO_SERIAL),
      m_CallbackWaiters(),
      m_OutputCallbacks(),
      m_ActiveCallbackPins(nullptr),
      m_nOutputCallbacks(0),
      m_LastEntryHash(0),
      m_LastEntrySeverity(Fatal),
      m_HashMatchedCount(0),
      m_Timestamps(true),
      m_LastTime(0) {
  for (size_t i = 0; i < LOG_CALLBACK_COUNT; ++i) {
    m_OutputCallbacks[i].callback = nullptr;
    m_OutputCallbacks[i].inFlight = 0;
    m_OutputCallbacks[i].removers = 0;
    m_OutputCallbacks[i].enabled = false;
  }
}

Log::~Log() {
  // can't render a timestamp, by the time we're shutting down here the
  // machine instance is gone
  LogEntry entry;
  entry << NoTimestamp << Notice << "-- Log Terminating --";
  addEntry(entry);
}

Log& Log::instance() {
  return m_Instance;
}

void Log::initialise1() {
  char* cmdline = g_pBootstrapInfo->getCommandLine();
  if (cmdline) {
    Vector<String> cmds = String(cmdline).tokenise(' ');
    for (auto it = cmds.begin(); it != cmds.end(); it++) {
      auto& cmd = *it;
      if (cmd == String("--disable-log-to-serial")) {
        m_EchoToSerial = false;
        break;
      } else if (cmd == String("--enable-log-to-serial")) {
        m_EchoToSerial = true;
        break;
      }
    }
  }
}

void Log::initialise2() {
  EMIT_IF(LOG_TO_SERIAL) {
    if (m_EchoToSerial)
      installSerialLogger();
  }
}

bool Log::installCallback(LogCallback* pCallback, bool bSkipBacklog) {
  if (!pCallback) {
    return false;
  }

  CallbackSlot* slot = nullptr;
  size_t entry = 0;
  size_t remaining = 0;
  {
    // Registration and its backlog boundary are one transaction with
    // entry publication. An entry is therefore delivered either through
    // the captured backlog or through normal dispatch, never neither.
    LockGuard<Spinlock> logGuard(m_Lock);
    auto callbackGuard = m_CallbackWaiters.acquire();
    for (size_t i = 0; i < LOG_CALLBACK_COUNT; ++i) {
      if (m_OutputCallbacks[i].callback == pCallback) {
        return false;
      }
    }

    for (size_t i = 0; i < LOG_CALLBACK_COUNT; ++i) {
      if (!m_OutputCallbacks[i].callback) {
        slot = &m_OutputCallbacks[i];
        slot->callback = pCallback;
        slot->inFlight = 0;
        slot->removers = 0;
        slot->enabled = true;
        ++m_nOutputCallbacks;
        break;
      }
    }

    if (!slot) {
      return false;
    }

    entry = m_StaticEntryStart;
    remaining = m_StaticEntries;
  }

  // Some callbacks want to skip a (potentially) massive backlog
  if (bSkipBacklog)
    return true;

  // Call the callback for the existing, flushed, log entries
  while (remaining--) {
    StaticLogEntry backlogEntry;
    NormalStaticString timestamp;
    bool timestamps = false;
    {
      LockGuard<Spinlock> guard(m_Lock);
      backlogEntry = m_StaticLog[entry];
      timestamps = m_Timestamps;
      if (timestamps) {
        timestamp = getTimestamp();
      }
    }

    if (backlogEntry.str.length()) {
      LogCord msg;
      msg.append("(backlog) ", 10);

      const TinyStaticString& severity = severityToString(backlogEntry.severity);
      msg.append(severity, severity.length());
      if (timestamps && timestamp.length()) {
        msg.append(timestamp, timestamp.length());
      }
      msg.append(backlogEntry.str, backlogEntry.str.length());
      msg.append(m_LineEnding, m_LineEnding.length());
      bool locked = !backlogEntry.lockfree;

      /// \note This could send a massive batch of log entries on the
      ///       callback. If the callback isn't designed to handle big
      ///       buffers this may fail.
      dispatchCallback(slot, msg, locked);
    }

    entry = (entry + 1) % LOG_ENTRIES;
  }

  return true;
}

bool Log::removeCallback(LogCallback* pCallback) {
  if (!pCallback) {
    return true;
  }

  TerminationDeferral terminationDeferral;
#if THREADS
  Thread* current = Processor::information().getCurrentThread();
  const bool canYield = current && Processor::getInterrupts();
#else
  const bool canYield = false;
#endif
  CallbackSlot* slot = nullptr;
  bool removerRegistered = false;

  while (true) {
    auto guard = m_CallbackWaiters.acquire();
    if (!slot) {
      for (size_t i = 0; i < LOG_CALLBACK_COUNT; ++i) {
        if (m_OutputCallbacks[i].callback == pCallback) {
          slot = &m_OutputCallbacks[i];
          break;
        }
      }

      if (!slot) {
        return true;
      }
    }

    if (slot->enabled) {
      slot->enabled = false;
      if (m_nOutputCallbacks) {
        --m_nOutputCallbacks;
      }
    }

    if (!slot->inFlight) {
      if (removerRegistered && slot->removers) {
        --slot->removers;
      }
      if (!slot->removers) {
        clearCallback(slot);
      }
      return true;
    }

    if (isCallbackContext(currentCallbackOwner())) {
      // Snapshot dispatch can pin callbacks which have not run yet. Treat any
      // live target as peer-owned here so false always preserves retry state.
      return false;
    }

    if (!canYield) {
      return false;
    }

    if (!removerRegistered) {
      ++slot->removers;
      removerRegistered = true;
    }

#if THREADS
    const WaitQueue::WakeReason reason = guard.waitForCompletion(
        WaitQueue::Channel(slot), Thread::CallbackDrain, reinterpret_cast<uintptr_t>(pCallback));
    (void)reason;
#else
    return false;
#endif
  }
}

void* Log::currentCallbackOwner() {
#if THREADS
  ProcessorInformation& information = Processor::information();
  Thread* thread = information.getCurrentThread();
  return thread ? static_cast<void*>(thread) : static_cast<void*>(&information);
#else
  return nullptr;
#endif
}

bool Log::isCallbackContext(void* owner) {
  for (CallbackPin* pin = m_ActiveCallbackPins; pin; pin = pin->next) {
    if (pin->owner == owner) {
      return true;
    }
  }
  return false;
}

void Log::clearCallback(CallbackSlot* slot) {
  slot->callback = nullptr;
  slot->inFlight = 0;
  slot->removers = 0;
  slot->enabled = false;
}

size_t Log::snapshotCallbacks(CallbackPin pins[LOG_CALLBACK_COUNT]) {
  auto guard = m_CallbackWaiters.acquire();
  void* owner = currentCallbackOwner();
  size_t count = 0;
  for (size_t i = 0; i < LOG_CALLBACK_COUNT; ++i) {
    CallbackSlot* slot = &m_OutputCallbacks[i];
    if (!slot->callback || !slot->enabled) {
      continue;
    }

    CallbackPin& pin = pins[count++];
    pin.slot = slot;
    pin.callback = slot->callback;
    pin.owner = owner;
    pin.next = m_ActiveCallbackPins;
    m_ActiveCallbackPins = &pin;
    ++slot->inFlight;
  }
  return count;
}

bool Log::pinCallback(CallbackSlot* slot, CallbackPin& pin) {
  auto guard = m_CallbackWaiters.acquire();
  if (!slot->callback || !slot->enabled) {
    return false;
  }

  pin.slot = slot;
  pin.callback = slot->callback;
  pin.owner = currentCallbackOwner();
  pin.next = m_ActiveCallbackPins;
  m_ActiveCallbackPins = &pin;
  ++slot->inFlight;
  return true;
}

bool Log::callbackEnabled(const CallbackPin& pin) {
  auto guard = m_CallbackWaiters.acquire();
  return pin.slot->enabled && pin.slot->callback == pin.callback;
}

void Log::releaseCallback(CallbackPin& pin) {
  auto guard = m_CallbackWaiters.acquire();

  CallbackPin** cursor = &m_ActiveCallbackPins;
  while (*cursor && *cursor != &pin) {
    cursor = &((*cursor)->next);
  }
  if (*cursor == &pin) {
    *cursor = pin.next;
  }

  CallbackSlot* slot = pin.slot;
  if (slot->inFlight) {
    --slot->inFlight;
  }

  if (!slot->enabled && !slot->inFlight) {
    if (slot->removers) {
      guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(slot));
    }
  }

  pin.slot = nullptr;
  pin.callback = nullptr;
  pin.owner = nullptr;
  pin.next = nullptr;
}

void Log::dispatchCallbacks(CallbackPin pins[LOG_CALLBACK_COUNT], size_t count,
                            const LogCord& message, bool locked) {
  for (size_t i = 0; i < count; ++i) {
    CallbackPin& pin = pins[i];
    if (callbackEnabled(pin)) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      CallbackPinHook hook = __atomic_load_n(&m_CallbackPinHook, __ATOMIC_ACQUIRE);
      if (hook) {
        hook(pin.callback);
      }
#endif
      pin.callback->callback(message, locked);
    }
    releaseCallback(pin);
  }
}

void Log::dispatchCallback(CallbackSlot* slot, const LogCord& message, bool locked) {
  CallbackPin pin = {};
  if (!pinCallback(slot, pin)) {
    return;
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  CallbackPinHook hook = __atomic_load_n(&m_CallbackPinHook, __ATOMIC_ACQUIRE);
  if (hook) {
    hook(pin.callback);
  }
#endif
  pin.callback->callback(message, locked);
  releaseCallback(pin);
}

size_t Log::getStaticEntryCount() const {
  return m_StaticEntries;
}

size_t Log::getDynamicEntryCount() const {
  return 0;
}

const Log::StaticLogEntry& Log::getStaticEntry(size_t n) const {
  return m_StaticLog[(m_StaticEntryStart + n) % LOG_ENTRIES];
}
/** Returns the (n - getStaticEntryCount())'th dynamic log entry */
const Log::DynamicLogEntry& Log::getDynamicEntry(size_t n) const {
  return m_StaticLog[0];
}

bool Log::echoToSerial() {
  return m_EchoToSerial;
}

const Log::LogEntry& Log::getLatestEntry() const {
  return m_StaticLog[m_StaticEntries - 1];
}

Log::LogEntry::LogEntry() : timestamp(), severity(), str(), numberType(Dec) {
  str.disableHashing();
}

Log::LogEntry& Log::LogEntry::operator<<(const char* s) {
  str.append(s);
  return *this;
}

Log::LogEntry& Log::LogEntry::operator<<(const String& s) {
  str.appendBytes(s.cstr(), s.length());
  return *this;
}

Log::LogEntry& Log::LogEntry::operator<<(const StringView& s) {
  str.appendBytes(s.str(), s.length());
  return *this;
}

Log::LogEntry& Log::LogEntry::operator<<(const Cord& c) {
  for (auto it = c.segbegin(); it != c.segend(); ++it) {
    str.appendBytes(it.ptr(), it.length());
  }
  return *this;
}

Log::LogEntry& Log::LogEntry::operator<<(const TinyStaticString& s) {
  str.appendBytes(s, s.length());
  return *this;
}

Log::LogEntry& Log::LogEntry::operator<<(const NormalStaticString& s) {
  str.appendBytes(s, s.length());
  return *this;
}

Log::LogEntry& Log::LogEntry::operator<<(const LargeStaticString& s) {
  str.appendBytes(s, s.length());
  return *this;
}

Log::LogEntry& Log::LogEntry::operator<<(const HugeStaticString& s) {
  str.appendBytes(s, s.length());
  return *this;
}

Log::LogEntry& Log::LogEntry::operator<<(char* append_str) {
  return (*this) << (reinterpret_cast<const char*>(append_str));
}

Log::LogEntry& Log::LogEntry::operator<<(bool b) {
  if (b)
    return *this << "true";

  return *this << "false";
}

template <class T>
Log::LogEntry& Log::LogEntry::operator<<(T n) {
  size_t radix = 10;
  if (numberType == Hex) {
    radix = 16;
    str.append("0x");
  } else if (numberType == Oct) {
    radix = 8;
    str.append("0");
  }
  str.append(n, radix);
  return *this;
}

Log::LogEntry& Log::LogEntry::operator<<(NumberType type) {
  numberType = type;
  return *this;
}

Log::LogEntry& Log::LogEntry::operator<<(SeverityLevel level) {
  // Zero the buffer.
  str.clear();
  severity = level;

  timestamp = 0;

  EMIT_IF(!UTILITY_LINUX) {
    if (showTimestamp) {
      Machine& machine = Machine::instance();
      if (machine.isInitialised() == true && machine.getTimer() != 0) {
        Timer& timer = *machine.getTimer();
        timestamp = timer.getTickCount();
      }
    }
  }

  return *this;
}

Log::LogEntry& Log::LogEntry::operator<<(LogEntryModifier modifier) {
  if (modifier == Unlocked) {
    lockfree = true;
  } else if (modifier == NoTimestamp) {
    showTimestamp = false;
  }
  return *this;
}

// NOTE: Make sure that the templated << operator gets only instantiated for
//       integer types.
template Log::LogEntry& Log::LogEntry::operator<<(char);
template Log::LogEntry& Log::LogEntry::operator<<(unsigned char);
template Log::LogEntry& Log::LogEntry::operator<<(short);
template Log::LogEntry& Log::LogEntry::operator<<(unsigned short);
template Log::LogEntry& Log::LogEntry::operator<<(int);
template Log::LogEntry& Log::LogEntry::operator<<(unsigned int);
template Log::LogEntry& Log::LogEntry::operator<<(long);
template Log::LogEntry& Log::LogEntry::operator<<(unsigned long);
template Log::LogEntry& Log::LogEntry::operator<<(long long);
template Log::LogEntry& Log::LogEntry::operator<<(unsigned long long);

void Log::addEntry(const LogEntry& source, bool lock) {
  static bool handlingFatal = false;

  StaticLogEntry entry = source;
  LogCord msg;
  TinyStaticString repeated;
  NormalStaticString timestamp;
  CallbackPin callbackPins[LOG_CALLBACK_COUNT] = {};
  msg.clear();

  size_t outputCallbackCount = 0;
  bool suppressOutput = false;
  bool wasRepeated = false;
  uint64_t repeatedTimes = 0;
  bool showTimestamp = false;
  bool shouldPanic = false;

  if (lock)
    m_Lock.acquire();

  if (m_StaticEntries >= LOG_ENTRIES) {
    m_StaticEntryStart = (m_StaticEntryStart + 1) % LOG_ENTRIES;
  } else
    m_StaticEntries++;

  m_StaticLog[m_StaticEntryEnd] = entry;
  m_StaticEntryEnd = (m_StaticEntryEnd + 1) % LOG_ENTRIES;

  outputCallbackCount = snapshotCallbacks(callbackPins);
  if (outputCallbackCount) {
    // Have we seen this message before?
    entry.str.allowHashing(true);
    uint64_t currentHash = entry.str.hash();
    entry.str.disableHashing();
    if (currentHash == m_LastEntryHash) {
      if (m_LastEntrySeverity == entry.severity) {
        ++m_HashMatchedCount;

        if (m_HashMatchedCount < LOG_MAX_DEDUPE_MESSAGES) {
          suppressOutput = true;
        }
      }
    }

    if (!suppressOutput) {
      if (m_HashMatchedCount) {
        wasRepeated = true;
        repeatedTimes = m_HashMatchedCount;
        m_HashMatchedCount = 0;
      }

      m_LastEntryHash = currentHash;
      m_LastEntrySeverity = entry.severity;
      showTimestamp = m_Timestamps;
      if (showTimestamp) {
        timestamp = getTimestamp();
      }
    }
  }

  if (!suppressOutput && !handlingFatal && entry.severity == Fatal) {
    handlingFatal = true;
    shouldPanic = true;
  }

  if (lock)
    m_Lock.release();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  EntrySnapshotHook snapshotHook = __atomic_load_n(&m_EntrySnapshotHook, __ATOMIC_ACQUIRE);
  if (snapshotHook) {
    snapshotHook(entry);
  }
#endif

  if (outputCallbackCount && !suppressOutput) {
    if (wasRepeated) {
      msg.append(m_DedupeHead, m_DedupeHead.length());
      if (repeatedTimes < g_NumRepeatedStrings) {
        msg.append(g_RepeatedStrings[repeatedTimes], g_RepeatedLengths[repeatedTimes]);
      } else {
        repeated.append(repeatedTimes);
        msg.append(repeated, repeated.length());
      }
      msg.append(m_DedupeTail, m_DedupeTail.length());
      msg.append(m_LineEnding, m_LineEnding.length());
    }

    const TinyStaticString& severity = severityToString(entry.severity);
    msg.append(severity, severity.length());
    if (showTimestamp && timestamp.length()) {
      msg.append(timestamp, timestamp.length());
    }
    msg.append(entry.str, entry.str.length());
    msg.append(m_LineEnding, m_LineEnding.length());
    dispatchCallbacks(callbackPins, outputCallbackCount, msg, !entry.lockfree);
  } else {
    for (size_t i = 0; i < outputCallbackCount; ++i) {
      releaseCallback(callbackPins[i]);
    }
  }

  // Panic if that was a fatal error.
  if (shouldPanic) {
    const char* panicstr = static_cast<const char*>(entry.str);

    // Attempt to trap to debugger, panic if that fails.
    EMIT_IF(DEBUGGER) {
      Processor::breakpoint();
    }
    panic(panicstr);
  }
}

void Log::enableTimestamps() {
  LockGuard<Spinlock> guard(m_Lock);
  m_Timestamps = true;
}

void Log::disableTimestamps() {
  LockGuard<Spinlock> guard(m_Lock);
  m_Timestamps = false;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Log::setCallbackPinHook(CallbackPinHook hook) {
  __atomic_store_n(&m_CallbackPinHook, hook, __ATOMIC_RELEASE);
}

void Log::setEntrySnapshotHook(EntrySnapshotHook hook) {
  __atomic_store_n(&m_EntrySnapshotHook, hook, __ATOMIC_RELEASE);
}
#endif

const NormalStaticString& Log::getTimestamp() {
  Time::Timestamp tn = Time::getTimeNanoseconds();
  Time::Timestamp ts = Time::getTime();
  Time::Timestamp t;
  EMIT_IF(LOG_TIMESTAMPS_IN_NANOS) {
    t = tn;
  }
  else {
    t = ts;
  }
  if (t == m_LastTime) {
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

const TinyStaticString& Log::severityToString(SeverityLevel level) const {
  switch (level) {
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
