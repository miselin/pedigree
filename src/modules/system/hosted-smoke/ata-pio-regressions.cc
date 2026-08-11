/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/time/Time.h"

#include "modules/drivers/common/ata/ata-common.h"

namespace {
class IoEventTrace {
 public:
  IoEventTrace()
      : m_EventCount(0), m_AlternateRun(0), m_PhaseCount(0), m_LastEvent(0), m_Valid(true) {}

  void alternateStatus() {
    ++m_AlternateRun;
    if (m_AlternateRun > 4) {
      m_Valid = false;
    }
    record('A');
  }

  void commandStatus() {
    if (m_AlternateRun) {
      m_Valid &= m_AlternateRun == 4;
      ++m_PhaseCount;
    } else if (m_LastEvent != 'S') {
      m_Valid = false;
    }
    m_AlternateRun = 0;
    record('S');
  }

  void dataWord() {
    if (m_AlternateRun) {
      m_Valid = false;
    }
    record('W');
  }

  bool valid(size_t expectedPhases) const {
    return m_Valid && !m_AlternateRun && m_PhaseCount == expectedPhases;
  }

  bool finalEventWasStatus() const {
    return m_EventCount && m_LastEvent == 'S';
  }

 private:
  void record(char event) {
    if (m_EventCount < sizeof(m_Events)) {
      m_Events[m_EventCount++] = event;
    } else {
      m_Valid = false;
    }
    m_LastEvent = event;
  }

  size_t m_EventCount;
  size_t m_AlternateRun;
  size_t m_PhaseCount;
  char m_LastEvent;
  bool m_Valid;
  char m_Events[2048];
};

class ScriptedAtaIo final : public IoBase {
 public:
  ScriptedAtaIo(bool command, IoEventTrace* trace, const uint8_t* statuses = nullptr,
                size_t statusCount = 0, const uint16_t* expectedWords = nullptr,
                size_t expectedWordCount = 0)
      : m_Command(command),
        m_Trace(trace),
        m_Statuses(statuses),
        m_StatusCount(statusCount),
        m_StatusIndex(0),
        m_ExpectedWords(expectedWords),
        m_ExpectedWordCount(expectedWordCount),
        m_StatusReads(0),
        m_AlternateReads(0),
        m_DataWrites(0),
        m_WritesMatch(true),
        m_UnexpectedAccess(false) {}

  size_t size() const override {
    return 8;
  }

  uint8_t read8(size_t offset = 0) override {
    if (m_Command && offset == 7 && m_StatusCount) {
      const size_t index = m_StatusIndex < m_StatusCount ? m_StatusIndex++ : m_StatusCount - 1;
      ++m_StatusReads;
      m_Trace->commandStatus();
      return m_Statuses[index];
    }
    if (!m_Command && offset == 2) {
      ++m_AlternateReads;
      m_Trace->alternateStatus();
      return 0;
    }

    m_UnexpectedAccess = true;
    return 0;
  }

  uint16_t read16(size_t offset = 0) override {
    (void)offset;
    m_UnexpectedAccess = true;
    return 0;
  }

  uint32_t read32(size_t offset = 0) override {
    (void)offset;
    m_UnexpectedAccess = true;
    return 0;
  }

#if BITS_64
  uint64_t read64(size_t offset = 0) override {
    (void)offset;
    m_UnexpectedAccess = true;
    return 0;
  }
#endif

  void write8(uint8_t value, size_t offset = 0) override {
    (void)value;
    (void)offset;
    m_UnexpectedAccess = true;
  }

  void write16(uint16_t value, size_t offset = 0) override {
    if (!m_Command || offset || !m_ExpectedWords || m_DataWrites >= m_ExpectedWordCount ||
        value != m_ExpectedWords[m_DataWrites]) {
      m_WritesMatch = false;
    }
    ++m_DataWrites;
    m_Trace->dataWord();
  }

  void write32(uint32_t value, size_t offset = 0) override {
    (void)value;
    (void)offset;
    m_UnexpectedAccess = true;
  }

#if BITS_64
  void write64(uint64_t value, size_t offset = 0) override {
    (void)value;
    (void)offset;
    m_UnexpectedAccess = true;
  }
#endif

  operator bool() const override {
    return true;
  }

  size_t statusReads() const {
    return m_StatusReads;
  }

  size_t alternateReads() const {
    return m_AlternateReads;
  }

  size_t dataWrites() const {
    return m_DataWrites;
  }

  bool writesMatch() const {
    return m_WritesMatch;
  }

  bool unexpectedAccess() const {
    return m_UnexpectedAccess;
  }

 private:
  bool m_Command;
  IoEventTrace* m_Trace;
  const uint8_t* m_Statuses;
  size_t m_StatusCount;
  size_t m_StatusIndex;
  const uint16_t* m_ExpectedWords;
  size_t m_ExpectedWordCount;
  size_t m_StatusReads;
  size_t m_AlternateReads;
  size_t m_DataWrites;
  bool m_WritesMatch;
  bool m_UnexpectedAccess;
};

bool check(bool condition, const char* test, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
  return false;
}

void makeFixtureData(uint16_t* words, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    words[i] = static_cast<uint16_t>(0xA500U ^ i);
  }
}

AtaPioPollBudget makeBudget(size_t maximumPolls) {
  AtaPioPollBudget budget = {Time::getTicks(), 30 * Time::Multiplier::Second, 0, maximumPolls};
  return budget;
}

bool successfulTwoSectorTransfer() {
  constexpr const char* Test = "ata-pio-success";
  const uint8_t statuses[] = {0x48, 0x48, 0x50};
  uint16_t words[512];
  makeFixtureData(words, 512);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 3, words, 512);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = makeBudget(3);
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 2, budget, finalStatus);
  const bool passed = check(
      result && finalStatus.__reg_contents == 0x50 && budget.polls == 3 &&
          command.statusReads() == 3 && control.alternateReads() == 12 &&
          command.dataWrites() == 512 && command.writesMatch() && trace.valid(3) &&
          trace.finalEventWasStatus() && !command.unexpectedAccess() && !control.unexpectedAccess(),
      Test, "the helper did not complete two ordered data phases and a terminal status phase");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-success");
  }
  return passed;
}

bool busyProgressesWithinOneBudget() {
  constexpr const char* Test = "ata-pio-busy-progress";
  const uint8_t statuses[] = {0x80, 0x48, 0x80, 0x50};
  uint16_t words[256];
  makeFixtureData(words, 256);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 4, words, 256);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = makeBudget(4);
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 1, budget, finalStatus);
  const bool passed =
      check(result && finalStatus.__reg_contents == 0x50 && budget.polls == 4 &&
                command.statusReads() == 4 && control.alternateReads() == 8 &&
                command.dataWrites() == 256 && command.writesMatch() && trace.valid(2) &&
                trace.finalEventWasStatus(),
            Test, "BSY progress did not stay within the cumulative data-out budget");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-busy-progress");
  }
  return passed;
}

bool ignoresStaleStatusWhileBusy() {
  constexpr const char* Test = "ata-pio-busy-stale-status";
  const uint8_t statuses[] = {0x81, 0xA0, 0x48, 0x50};
  uint16_t words[256];
  makeFixtureData(words, 256);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 4, words, 256);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = makeBudget(4);
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 1, budget, finalStatus);
  const bool passed =
      check(result && finalStatus.__reg_contents == 0x50 && budget.polls == 4 &&
                command.statusReads() == 4 && control.alternateReads() == 8 &&
                command.dataWrites() == 256 && command.writesMatch() && trace.valid(2) &&
                trace.finalEventWasStatus(),
            Test, "ERR or device-fault bits were interpreted while BSY made them stale");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-busy-stale-status");
  }
  return passed;
}

bool rejectsLateError() {
  constexpr const char* Test = "ata-pio-late-error";
  const uint8_t statuses[] = {0x08, 0x41};
  uint16_t words[256];
  makeFixtureData(words, 256);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 2, words, 256);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = makeBudget(2);
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 1, budget, finalStatus);
  const bool passed =
      check(!result && finalStatus.__reg_contents == 0x41 && budget.polls == 2 &&
                command.dataWrites() == 256 && command.writesMatch() && trace.valid(2) &&
                trace.finalEventWasStatus(),
            Test, "an ERR status after the final data word was reported as success");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-late-error");
  }
  return passed;
}

bool rejectsDeviceFault() {
  constexpr const char* Test = "ata-pio-device-fault";
  const uint8_t statuses[] = {0x08, 0x60};
  uint16_t words[256];
  makeFixtureData(words, 256);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 2, words, 256);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = makeBudget(2);
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 1, budget, finalStatus);
  const bool passed =
      check(!result && finalStatus.__reg_contents == 0x60 && budget.polls == 2 &&
                command.dataWrites() == 256 && command.writesMatch() && trace.valid(2) &&
                trace.finalEventWasStatus(),
            Test, "a device fault after the final data word was reported as success");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-device-fault");
  }
  return passed;
}

bool requiresDrqForData() {
  constexpr const char* Test = "ata-pio-requires-drq";
  const uint8_t statuses[] = {0x40, 0x08};
  uint16_t words[256];
  makeFixtureData(words, 256);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 2, words, 256);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = makeBudget(2);
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 1, budget, finalStatus);
  const bool passed =
      check(!result && finalStatus.__reg_contents == 0x40 && budget.polls == 1 &&
                command.statusReads() == 1 && control.alternateReads() == 4 &&
                command.dataWrites() == 0 && trace.valid(1) && trace.finalEventWasStatus(),
            Test, "DRDY without DRQ was treated as progress into a later data phase");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-requires-drq");
  }
  return passed;
}

bool rejectsUnexpectedExtraDataPhase() {
  constexpr const char* Test = "ata-pio-extra-data-phase";
  const uint8_t statuses[] = {0x08, 0x08, 0x40};
  uint16_t words[256];
  makeFixtureData(words, 256);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 3, words, 256);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = makeBudget(3);
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 1, budget, finalStatus);
  const bool passed =
      check(!result && finalStatus.__reg_contents == 0x08 && budget.polls == 2 &&
                command.statusReads() == 2 && control.alternateReads() == 8 &&
                command.dataWrites() == 256 && command.writesMatch() && trace.valid(2) &&
                trace.finalEventWasStatus(),
            Test, "an extra data phase was treated as progress into a later completion state");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-extra-data-phase");
  }
  return passed;
}

bool requiresDrdyForCompletion() {
  constexpr const char* Test = "ata-pio-terminal-requires-drdy";
  const uint8_t statuses[] = {0x08, 0x10};
  uint16_t words[256];
  makeFixtureData(words, 256);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 2, words, 256);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = makeBudget(2);
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 1, budget, finalStatus);
  const bool passed =
      check(!result && finalStatus.__reg_contents == 0x10 && budget.polls == 2 &&
                command.statusReads() == 2 && control.alternateReads() == 8 &&
                command.dataWrites() == 256 && command.writesMatch() && trace.valid(2) &&
                trace.finalEventWasStatus(),
            Test, "a nonzero terminal status without DRDY was reported as completion");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-terminal-requires-drdy");
  }
  return passed;
}

bool rejectsSecondSectorErrorBeforeData() {
  constexpr const char* Test = "ata-pio-second-sector-error";
  const uint8_t statuses[] = {0x08, 0x41};
  uint16_t words[512];
  makeFixtureData(words, 512);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 2, words, 512);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = makeBudget(2);
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 2, budget, finalStatus);
  const bool passed =
      check(!result && finalStatus.__reg_contents == 0x41 && budget.polls == 2 &&
                command.statusReads() == 2 && control.alternateReads() == 8 &&
                command.dataWrites() == 256 && command.writesMatch() && trace.valid(2) &&
                trace.finalEventWasStatus(),
            Test, "a second-sector error allowed another sector of data to be written");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-second-sector-error");
  }
  return passed;
}

bool rejectsAlreadyExpiredBudget() {
  constexpr const char* Test = "ata-pio-expired-timeout";
  const uint8_t statuses[] = {0x08};
  uint16_t words[256];
  makeFixtureData(words, 256);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 1, words, 256);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = {Time::getTicks(), 0, 0, 3};
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 1, budget, finalStatus);
  const bool passed =
      check(!result && finalStatus.__reg_contents == 0 && budget.polls == 0 &&
                command.statusReads() == 0 && control.alternateReads() == 0 &&
                command.dataWrites() == 0 && trace.valid(0) && !trace.finalEventWasStatus(),
            Test, "an expired command budget touched status, control, or data registers");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-expired-timeout");
  }
  return passed;
}

bool rejectsZeroStatus() {
  constexpr const char* Test = "ata-pio-zero-status";
  const uint8_t statuses[] = {0x00};
  uint16_t words[256];
  makeFixtureData(words, 256);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 1, words, 256);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = makeBudget(3);
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 1, budget, finalStatus);
  const bool passed = check(!result && finalStatus.__reg_contents == 0 && budget.polls == 1 &&
                                command.statusReads() == 1 && command.dataWrites() == 0 &&
                                trace.valid(1) && trace.finalEventWasStatus(),
                            Test, "an absent-device status was retried or reported as success");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-zero-status");
  }
  return passed;
}

bool stopsAtExactPollBoundary() {
  constexpr const char* Test = "ata-pio-poll-boundary";
  const uint8_t statuses[] = {0x80};
  uint16_t words[256];
  makeFixtureData(words, 256);
  IoEventTrace trace;
  ScriptedAtaIo command(true, &trace, statuses, 1, words, 256);
  ScriptedAtaIo control(false, &trace);
  AtaPioPollBudget budget = makeBudget(3);
  AtaStatus finalStatus = {};

  const bool result = ataPioWrite512ByteSectors(&command, &control, words, 1, budget, finalStatus);
  const bool passed =
      check(!result && finalStatus.__reg_contents == 0x80 && budget.polls == 3 &&
                command.statusReads() == 3 && control.alternateReads() == 4 &&
                command.dataWrites() == 0 && trace.valid(1) && trace.finalEventWasStatus(),
            Test, "sticky BSY sampled beyond or escaped the exact command poll budget");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ata-pio-poll-boundary");
  }
  return passed;
}
}  // namespace

bool runHostedAtaPioRegressions() {
  bool passed = true;
  passed &= successfulTwoSectorTransfer();
  passed &= busyProgressesWithinOneBudget();
  passed &= ignoresStaleStatusWhileBusy();
  passed &= rejectsLateError();
  passed &= rejectsDeviceFault();
  passed &= requiresDrqForData();
  passed &= rejectsUnexpectedExtraDataPhase();
  passed &= requiresDrdyForCompletion();
  passed &= rejectsSecondSectorErrorBeforeData();
  passed &= rejectsAlreadyExpiredBudget();
  passed &= rejectsZeroStatus();
  passed &= stopsAtExactPollBoundary();
  return passed;
}
