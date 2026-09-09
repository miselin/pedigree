/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <array>
#include <limits>
#include <vector>

#include "modules/drivers/common/usb-hcd/EhciHandoff.h"
#include <gtest/gtest.h>

namespace {
using State = EhciHandoff::State;

struct Event {
  enum class Kind { Read32, Write32, Write8, Delay };

  Kind kind;
  uint16_t offset;
  uint32_t value;
};

struct Config {
  static constexpr uint16_t Capability = 0x40;
  static constexpr uint16_t Control = Capability + 4;

  explicit Config(bool biosOwned = true) {
    registers[Capability / 4] = State::LegacyCapabilityId;
    if (biosOwned)
      registers[Capability / 4] |= State::BiosOwned;
    registers[Control / 4] = EhciLegacy::AcknowledgeStatus | EhciLegacy::SmiEnables | 0x003f0000U;
  }

  uint32_t& capability() {
    return registers[Capability / 4];
  }

  uint32_t& control() {
    return registers[Control / 4];
  }

  bool read32(uint16_t offset, uint32_t& value) {
    events.push_back({Event::Kind::Read32, offset, 0});
    if (failAllReads || (offset == Control && failControlReadOnce)) {
      if (offset == Control)
        failControlReadOnce = false;
      return false;
    }

    if (offset == Capability)
      updateOwnershipOnRead();

    value = registers[offset / 4];
    if (offset == Control && forceEnableReadbackOnce &&
        countEvents(Event::Kind::Write32, Control)) {
      value |= forceEnableReadbackOnce;
      forceEnableReadbackOnce = 0;
    }
    return true;
  }

  bool write32(uint16_t offset, uint32_t value) {
    events.push_back({Event::Kind::Write32, offset, value});
    if (offset == Control) {
      if (failControlWriteOnce) {
        failControlWriteOnce = false;
        return false;
      }

      // The high status bits are W1C. The middle status-shadow bits are
      // read-only, while the low SMI enable bits are ordinary R/W fields.
      uint32_t stored = registers[Control / 4];
      stored = (stored & 0x003f0000U) | (stored & ~value & EhciLegacy::AcknowledgeStatus) |
               (value & EhciLegacy::SmiEnables);
      registers[Control / 4] = stored;
      return true;
    }
    registers[offset / 4] = value;
    return true;
  }

  bool write8(uint16_t offset, uint8_t value) {
    events.push_back({Event::Kind::Write8, offset, value});
    if (offset != Capability + 3)
      return false;

    if (value) {
      requestActive = true;
      releaseActive = false;
      capability() |= State::OsOwned;
      if (failRequestWriteOnce) {
        failRequestWriteOnce = false;
        return false;
      }
    } else {
      requestActive = false;
      releaseActive = true;
      capability() &= ~State::OsOwned;
      if (failReleaseWriteOnce) {
        failReleaseWriteOnce = false;
        return false;
      }
    }
    return true;
  }

  void delay() {
    events.push_back({Event::Kind::Delay, 0, 0});
    ++delays;
  }

  size_t firstEvent(Event::Kind kind, uint16_t offset) const {
    for (size_t i = 0; i < events.size(); ++i) {
      if (events[i].kind == kind && events[i].offset == offset)
        return i;
    }
    return std::numeric_limits<size_t>::max();
  }

  size_t countEvents(Event::Kind kind, uint16_t offset) const {
    size_t count = 0;
    for (const auto& event : events) {
      if (event.kind == kind && event.offset == offset)
        ++count;
    }
    return count;
  }

  std::array<uint32_t, 64> registers{};
  std::vector<Event> events;
  size_t delays = 0;
  size_t handoffPolls = 0;
  size_t reclaimPolls = 0;
  size_t handoffClearAfter = std::numeric_limits<size_t>::max();
  size_t reclaimAfter = std::numeric_limits<size_t>::max();
  uint32_t forceEnableReadbackOnce = 0;
  bool requestActive = false;
  bool releaseActive = false;
  bool failAllReads = false;
  bool failControlReadOnce = false;
  bool failControlWriteOnce = false;
  bool failRequestWriteOnce = false;
  bool failReleaseWriteOnce = false;
  bool handoffLeavesOs = true;

 private:
  void updateOwnershipOnRead() {
    if (requestActive && (capability() & State::BiosOwned) && handoffPolls < handoffClearAfter) {
      ++handoffPolls;
      if (handoffPolls >= handoffClearAfter) {
        capability() &= ~State::BiosOwned;
        if (!handoffLeavesOs)
          capability() &= ~State::OsOwned;
      }
    }

    if (releaseActive && !(capability() & State::BiosOwned) && !(capability() & State::OsOwned) &&
        reclaimPolls < reclaimAfter) {
      ++reclaimPolls;
      if (reclaimPolls >= reclaimAfter)
        capability() |= State::BiosOwned;
    }
  }
};

TEST(EhciHandoff, AcquiresBiosOwnershipAndDisablesLegacySmis) {
  Config config;
  config.handoffClearAfter = 1;
  State state;

  EXPECT_EQ(state.acquire(Config::Capability, config, 3), EhciHandoff::AcquireResult::Acquired);
  EXPECT_TRUE(state.wasBiosOwned());
  EXPECT_TRUE(state.requested());
  EXPECT_TRUE(state.acquired());
  EXPECT_TRUE(state.disabled());
  EXPECT_EQ(state.savedSmiEnables(), EhciLegacy::SmiEnables);
  EXPECT_EQ(config.capability() & State::BiosOwned, 0U);
  EXPECT_NE(config.capability() & State::OsOwned, 0U);
  EXPECT_EQ(config.control() & EhciLegacy::SmiEnables, 0U);
  EXPECT_EQ(config.control() & 0x003f0000U, 0x003f0000U);
  EXPECT_LT(config.firstEvent(Event::Kind::Write8, Config::Capability + 3),
            config.firstEvent(Event::Kind::Write32, Config::Control));
}

TEST(EhciHandoff, UnownedShortcutDoesNotRequestOrRelease) {
  Config config(false);
  State state;

  EXPECT_EQ(state.acquire(Config::Capability, config), EhciHandoff::AcquireResult::AlreadyUnowned);
  EXPECT_FALSE(state.wasBiosOwned());
  EXPECT_FALSE(state.requested());
  EXPECT_FALSE(state.acquired());
  const size_t eventCount = config.events.size();
  const uint32_t originalControl = config.control();
  EXPECT_EQ(state.release(config, true), EhciHandoff::ReleaseResult::NotOwned);
  EXPECT_EQ(config.events.size(), eventCount);
  EXPECT_EQ(config.control(), originalControl);
}

TEST(EhciHandoff, InitialAccessFailureDoesNotMutateHardware) {
  Config config;
  config.failAllReads = true;
  State state;

  EXPECT_EQ(state.acquire(Config::Capability, config), EhciHandoff::AcquireResult::AccessFailure);
  EXPECT_EQ(config.countEvents(Event::Kind::Write8, Config::Capability + 3), 0U);
  EXPECT_EQ(config.countEvents(Event::Kind::Write32, Config::Control), 0U);
  EXPECT_EQ(state.withdrawRequest(config), EhciHandoff::ReleaseResult::NotOwned);
}

TEST(EhciHandoff, FailedRequestWriteCanBeWithdrawn) {
  Config config;
  config.failRequestWriteOnce = true;
  State state;

  EXPECT_EQ(state.acquire(Config::Capability, config), EhciHandoff::AcquireResult::AccessFailure);
  EXPECT_TRUE(state.requested());
  EXPECT_FALSE(state.acquired());
  EXPECT_EQ(state.withdrawRequest(config), EhciHandoff::ReleaseResult::FirmwareConfirmed);
  EXPECT_EQ(config.capability() & State::OsOwned, 0U);
  EXPECT_EQ(config.countEvents(Event::Kind::Write8, Config::Capability + 3), 2U);
}

TEST(EhciHandoff, OwnershipTimeoutIsBoundedAndDoesNotClaimAcquisition) {
  Config config;
  State state;

  EXPECT_EQ(state.acquire(Config::Capability, config, 3),
            EhciHandoff::AcquireResult::OwnershipTimeout);
  EXPECT_TRUE(state.wasBiosOwned());
  EXPECT_TRUE(state.requested());
  EXPECT_FALSE(state.acquired());
  EXPECT_EQ(config.handoffPolls, 3U);
  EXPECT_EQ(config.delays, 2U);

  // A late firmware response still cannot retroactively make this attempt an
  // acquired state; the unwind path clears the OS semaphore first.
  config.capability() &= ~State::BiosOwned;
  EXPECT_EQ(state.withdrawRequest(config, 2), EhciHandoff::ReleaseResult::Released);
  EXPECT_FALSE(state.acquired());
}

TEST(EhciHandoff, LateFirmwareAckRestoresSmisBeforeWithdrawingRequest) {
  Config config;
  State state;

  ASSERT_EQ(state.acquire(Config::Capability, config, 1),
            EhciHandoff::AcquireResult::OwnershipTimeout);
  // Firmware acknowledged after the bounded acquire poll and disabled its
  // SMI enables while doing so.
  config.capability() &= ~State::BiosOwned;
  config.control() &= ~EhciLegacy::SmiEnables;
  EXPECT_EQ(state.withdrawRequest(config, 2), EhciHandoff::ReleaseResult::Released);
  EXPECT_EQ(config.control() & EhciLegacy::SmiEnables, EhciLegacy::SmiEnables);
  EXPECT_EQ(config.capability() & State::OsOwned, 0U);
}

TEST(EhciHandoff, FirmwareStillOwningAfterFailedHandoffLeavesItsSmisAlone) {
  Config config;
  State state;

  ASSERT_EQ(state.acquire(Config::Capability, config, 1),
            EhciHandoff::AcquireResult::OwnershipTimeout);
  const size_t controlWrites = config.countEvents(Event::Kind::Write32, Config::Control);
  EXPECT_EQ(state.withdrawRequest(config), EhciHandoff::ReleaseResult::FirmwareConfirmed);
  EXPECT_EQ(config.countEvents(Event::Kind::Write32, Config::Control), controlWrites);
}

TEST(EhciHandoff, RejectsAHandshakeThatClearsBothSemaphores) {
  Config config;
  config.handoffClearAfter = 1;
  config.handoffLeavesOs = false;
  State state;

  EXPECT_EQ(state.acquire(Config::Capability, config),
            EhciHandoff::AcquireResult::OwnershipMismatch);
  EXPECT_FALSE(state.acquired());
  EXPECT_EQ(state.withdrawRequest(config, 2), EhciHandoff::ReleaseResult::Released);
}

TEST(EhciHandoff, SmiFailureIsRestoredBeforeWithdrawingRequest) {
  Config config;
  config.handoffClearAfter = 1;
  config.forceEnableReadbackOnce = 1;
  State state;

  EXPECT_EQ(state.acquire(Config::Capability, config),
            EhciHandoff::AcquireResult::SmiDisableFailure);
  EXPECT_TRUE(state.requested());
  EXPECT_FALSE(state.acquired());
  EXPECT_FALSE(state.disabled());
  EXPECT_EQ(state.withdrawRequest(config, 2), EhciHandoff::ReleaseResult::Released);
  EXPECT_EQ(config.control() & EhciLegacy::SmiEnables, EhciLegacy::SmiEnables);

  size_t restore = std::numeric_limits<size_t>::max();
  size_t clearRequest = std::numeric_limits<size_t>::max();
  for (size_t i = 0; i < config.events.size(); ++i) {
    const auto& event = config.events[i];
    if (event.kind == Event::Kind::Write32 && event.offset == Config::Control &&
        event.value == (EhciLegacy::AcknowledgeStatus | EhciLegacy::SmiEnables))
      restore = i;
    if (event.kind == Event::Kind::Write8 && event.offset == Config::Capability + 3 &&
        event.value == 0)
      clearRequest = i;
  }
  ASSERT_NE(restore, std::numeric_limits<size_t>::max());
  ASSERT_NE(clearRequest, std::numeric_limits<size_t>::max());
  EXPECT_LT(restore, clearRequest);
}

TEST(EhciHandoff, ReleaseRestoresOriginalSmisAndConfirmsFirmware) {
  Config config;
  config.handoffClearAfter = 1;
  config.reclaimAfter = 1;
  State state;

  ASSERT_EQ(state.acquire(Config::Capability, config), EhciHandoff::AcquireResult::Acquired);
  EXPECT_EQ(state.release(config, true), EhciHandoff::ReleaseResult::FirmwareConfirmed);
  EXPECT_EQ(config.control() & EhciLegacy::SmiEnables, EhciLegacy::SmiEnables);
  EXPECT_EQ(config.control() & 0x003f0000U, 0x003f0000U);
  EXPECT_EQ(config.capability() & State::OsOwned, 0U);
  EXPECT_NE(config.capability() & State::BiosOwned, 0U);
  EXPECT_EQ(state.offset(), 0U);
  EXPECT_FALSE(state.requested());
  EXPECT_FALSE(state.acquired());
}

TEST(EhciHandoff, ReleaseReportsWhenFirmwareDoesNotAcknowledge) {
  Config config;
  config.handoffClearAfter = 1;
  State state;

  ASSERT_EQ(state.acquire(Config::Capability, config), EhciHandoff::AcquireResult::Acquired);
  EXPECT_EQ(state.release(config, true, 2), EhciHandoff::ReleaseResult::Released);
  EXPECT_EQ(config.capability() & State::OsOwned, 0U);
  EXPECT_EQ(config.capability() & State::BiosOwned, 0U);
}

TEST(EhciHandoff, ReleaseRequiresQuiescenceAndCanBeRetriedAfterAccessFailure) {
  Config config;
  config.handoffClearAfter = 1;
  State state;

  ASSERT_EQ(state.acquire(Config::Capability, config), EhciHandoff::AcquireResult::Acquired);
  const size_t eventCount = config.events.size();
  EXPECT_EQ(state.release(config, false), EhciHandoff::ReleaseResult::NotQuiesced);
  EXPECT_EQ(config.events.size(), eventCount);

  config.failControlReadOnce = true;
  EXPECT_EQ(state.release(config, true), EhciHandoff::ReleaseResult::AccessFailure);
  EXPECT_TRUE(state.acquired());
  EXPECT_EQ(state.release(config, true, 2), EhciHandoff::ReleaseResult::Released);
  EXPECT_FALSE(state.acquired());
}

TEST(EhciHandoff, InvalidAndDuplicateAttemptsDoNotRestartSnapshot) {
  Config config;
  config.handoffClearAfter = 1;
  State state;

  EXPECT_EQ(state.acquire(0x41, config), EhciHandoff::AcquireResult::InvalidCapability);
  EXPECT_TRUE(config.events.empty());
  ASSERT_EQ(state.acquire(Config::Capability, config), EhciHandoff::AcquireResult::Acquired);
  const size_t eventCount = config.events.size();
  EXPECT_EQ(state.acquire(Config::Capability, config),
            EhciHandoff::AcquireResult::AlreadyAttempted);
  EXPECT_EQ(config.events.size(), eventCount);
}
}  // namespace
