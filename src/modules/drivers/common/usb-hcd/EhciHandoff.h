/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef EHCI_HANDOFF_H
#define EHCI_HANDOFF_H

#include <stddef.h>
#include <stdint.h>

#include "EhciLegacy.h"

namespace EhciHandoff {

/** The result of requesting ownership from firmware. */
enum class AcquireResult {
  Acquired,
  AlreadyUnowned,
  InvalidCapability,
  AccessFailure,
  OwnershipTimeout,
  OwnershipMismatch,
  SmiDisableFailure,
  AlreadyAttempted,
};

/** The result of returning a controller to firmware. */
enum class ReleaseResult {
  NotOwned,
  Released,
  FirmwareConfirmed,
  AccessFailure,
  NotQuiesced,
};

/**
 * Tracks one EHCI legacy ownership exchange.
 *
 * Config must provide:
 *
 *   bool read32(uint16_t offset, uint32_t& value);
 *   bool write32(uint16_t offset, uint32_t value);
 *   bool write8(uint16_t offset, uint8_t value);
 *   void delay();
 *
 * The caller owns all controller quiescence ordering.  In particular, release
 * may only be called after schedules, callbacks, DMA, and the PCI interrupt
 * source have been detached.  The helper never touches controller schedules.
 */
class State {
 public:
  static constexpr uint32_t BiosOwned = 1U << 16;
  static constexpr uint32_t OsOwned = 1U << 24;
  static constexpr uint8_t LegacyCapabilityId = 1;
  static constexpr size_t DefaultPollLimit = 1000;

  State()
      : m_Offset(0),
        m_SavedSmiEnables(0),
        m_WasBiosOwned(false),
        m_RequestWritten(false),
        m_Acquired(false),
        m_Disabled(false),
        m_SmiMutationAttempted(false),
        m_SnapshotValid(false) {}

  /**
   * Request OS ownership, or use the existing BIOS-unowned shortcut.
   *
   * The semaphore request is a byte write at offset +3.  A dword
   * read-modify-write is deliberately not used: the other USBLEGSUP fields
   * are semaphores, capability metadata, or reserved bits.
   */
  template <typename Config>
  AcquireResult acquire(uint16_t offset, Config& config, size_t pollLimit = DefaultPollLimit) {
    if (m_SnapshotValid)
      return AcquireResult::AlreadyAttempted;
    reset();
    if (!validOffset(offset) || !pollLimit)
      return AcquireResult::InvalidCapability;

    uint32_t legsup = 0;
    uint32_t control = 0;
    if (!config.read32(offset, legsup) || !config.read32(controlOffset(offset), control))
      return AcquireResult::AccessFailure;
    if ((legsup & 0xffU) != LegacyCapabilityId)
      return AcquireResult::InvalidCapability;

    m_Offset = offset;
    m_SavedSmiEnables = control & EhciLegacy::SmiEnables;
    m_WasBiosOwned = (legsup & BiosOwned) != 0;
    m_SnapshotValid = true;

    if (m_WasBiosOwned) {
      // A posted config write may have reached hardware even when its access
      // wrapper reports failure, so retain the request obligation before the
      // write is attempted.
      m_RequestWritten = true;
      if (!config.write8(static_cast<uint16_t>(offset + 3), 1))
        return AcquireResult::AccessFailure;

      bool sawBiosClear = false;
      for (size_t poll = 0; poll < pollLimit; ++poll) {
        if (!config.read32(offset, legsup))
          return AcquireResult::AccessFailure;
        if (!(legsup & BiosOwned)) {
          sawBiosClear = true;
          if (!(legsup & OsOwned))
            return AcquireResult::OwnershipMismatch;
          break;
        }
        if (poll + 1 < pollLimit)
          config.delay();
      }
      if (!sawBiosClear)
        return AcquireResult::OwnershipTimeout;
    }

    m_SmiMutationAttempted = true;
    if (!EhciLegacy::disableSmis(
            offset,
            [&config](uint16_t registerOffset, uint32_t& value) {
              return config.read32(registerOffset, value);
            },
            [&config](uint16_t registerOffset, uint32_t value) {
              return config.write32(registerOffset, value);
            })) {
      return AcquireResult::SmiDisableFailure;
    }

    m_Disabled = true;
    m_Acquired = m_WasBiosOwned;
    return m_WasBiosOwned ? AcquireResult::Acquired : AcquireResult::AlreadyUnowned;
  }

  /**
   * Withdraw an ownership request which did not reach acquired state.
   *
   * This path is intentionally separate from release: no controller schedule
   * or callback has been published yet.  It restores the saved SMI enables,
   * clears only the OS-owned byte when a request was written, and does not
   * invent a BIOS-owned semaphore.  A successful return means that the OS
   * semaphore was cleared; firmware confirmation is reported separately.
   */
  template <typename Config>
  ReleaseResult withdrawRequest(Config& config, size_t pollLimit = DefaultPollLimit) {
    if (!m_SnapshotValid || m_Acquired || !m_WasBiosOwned ||
        (!m_RequestWritten && !m_SmiMutationAttempted))
      return ReleaseResult::NotOwned;
    return releaseInternal(config, true, pollLimit);
  }

  /**
   * Return ownership after the caller has quiesced all controller activity.
   *
   * If the controller started BIOS-unowned, this is a no-op: it never writes
   * either ownership semaphore or re-enables firmware SMIs for a controller
   * that never granted the OS an ownership lease.
   */
  template <typename Config>
  ReleaseResult release(Config& config, bool callerQuiescedOrNeverTouched,
                        size_t pollLimit = DefaultPollLimit) {
    if (!callerQuiescedOrNeverTouched)
      return ReleaseResult::NotQuiesced;
    return releaseInternal(config, false, pollLimit);
  }

  uint16_t offset() const {
    return m_Offset;
  }

  uint32_t savedSmiEnables() const {
    return m_SavedSmiEnables;
  }

  bool wasBiosOwned() const {
    return m_WasBiosOwned;
  }

  bool requested() const {
    return m_RequestWritten;
  }

  bool acquired() const {
    return m_Acquired;
  }

  bool disabled() const {
    return m_Disabled;
  }

 private:
  static constexpr uint16_t ControlOffset = 4;

  static bool validOffset(uint16_t offset) {
    return offset >= 0x40 && offset <= 0xf8 && !(offset & 3);
  }

  static uint16_t controlOffset(uint16_t offset) {
    return static_cast<uint16_t>(offset + ControlOffset);
  }

  void reset() {
    m_Offset = 0;
    m_SavedSmiEnables = 0;
    m_WasBiosOwned = false;
    m_RequestWritten = false;
    m_Acquired = false;
    m_Disabled = false;
    m_SmiMutationAttempted = false;
    m_SnapshotValid = false;
  }

  template <typename Config>
  bool restoreSmis(Config& config) {
    if (!m_SnapshotValid ||
        !config.write32(controlOffset(m_Offset), EhciLegacy::AcknowledgeStatus | m_SavedSmiEnables))
      return false;

    uint32_t control = 0;
    if (!config.read32(controlOffset(m_Offset), control))
      return false;
    return (control & EhciLegacy::SmiEnables) == m_SavedSmiEnables;
  }

  template <typename Config>
  ReleaseResult releaseInternal(Config& config, bool allowUnacquired, size_t pollLimit) {
    if (!m_SnapshotValid || !pollLimit)
      return ReleaseResult::NotOwned;

    if (!m_WasBiosOwned) {
      // The BIOS-unowned shortcut never establishes an OS ownership lease;
      // do not synthesize a return-to-firmware exchange on teardown.
      reset();
      return ReleaseResult::NotOwned;
    }

    if (!m_RequestWritten)
      return ReleaseResult::NotOwned;

    if (!m_Acquired && !allowUnacquired)
      return ReleaseResult::NotOwned;

    // A failed handoff can receive a late firmware acknowledgement after the
    // bounded acquire poll. Firmware may clear its own SMI enables as part of
    // that acknowledgement, so inspect live ownership before deciding whether
    // the saved enables must be restored. If firmware still owns the device,
    // leave its controls untouched. Once this helper has attempted an SMI
    // mutation, restoration is required regardless of the live semaphore.
    if (m_SmiMutationAttempted) {
      if (!restoreSmis(config))
        return ReleaseResult::AccessFailure;
    } else {
      uint32_t liveLegsup = 0;
      if (!config.read32(m_Offset, liveLegsup))
        return ReleaseResult::AccessFailure;
      if (!(liveLegsup & BiosOwned) && !restoreSmis(config))
        return ReleaseResult::AccessFailure;
    }

    if (!config.write8(static_cast<uint16_t>(m_Offset + 3), 0))
      return ReleaseResult::AccessFailure;

    uint32_t legsup = 0;
    if (!config.read32(m_Offset, legsup))
      return ReleaseResult::AccessFailure;
    if (legsup & OsOwned)
      return ReleaseResult::AccessFailure;

    bool firmwareConfirmed = (legsup & BiosOwned) != 0;
    for (size_t poll = 0; !firmwareConfirmed && poll < pollLimit; ++poll) {
      if (!config.read32(m_Offset, legsup))
        return ReleaseResult::AccessFailure;
      firmwareConfirmed = (legsup & BiosOwned) != 0;
      if (!firmwareConfirmed && poll + 1 < pollLimit)
        config.delay();
    }

    reset();
    return firmwareConfirmed ? ReleaseResult::FirmwareConfirmed : ReleaseResult::Released;
  }

  uint16_t m_Offset;
  uint32_t m_SavedSmiEnables;
  bool m_WasBiosOwned;
  bool m_RequestWritten;
  bool m_Acquired;
  bool m_Disabled;
  bool m_SmiMutationAttempted;
  bool m_SnapshotValid;
};

}  // namespace EhciHandoff

#endif
