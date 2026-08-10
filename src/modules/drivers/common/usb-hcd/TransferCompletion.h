/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef USB_HCD_TRANSFER_COMPLETION_H
#define USB_HCD_TRANSFER_COMPLETION_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/assert.h"

namespace UsbHcd {
/**
 * Owns the exactly-once callback obligation for one accepted transfer.
 *
 * Every transition and callback publication must occur under the HCD's
 * completion lock. Capturing a hardware result does not itself transfer
 * callback ownership: normal reclamation, cancellation, and teardown still
 * race for the one publication claim. The winner must establish that DMA has
 * stopped and publish its callback record before releasing that lock.
 */
class TransferCompletion {
 public:
  using Callback = void (*)(uintptr_t, ssize_t);

  enum class State : size_t {
    Idle,
    Active,
    Captured,
    PublicationClaimed,
  };

  enum class Reason {
    Natural,
    Cancelled,
    Teardown,
  };

  enum class CancellationDisposition {
    NoMatch,
    Claimed,
    DrainPublished,
  };

  struct Claim {
    Claim() : callback(nullptr), parameter(0), generation(0), result(0), reason(Reason::Teardown) {}

    Callback callback;
    uintptr_t parameter;
    size_t generation;
    ssize_t result;
    Reason reason;
  };

  TransferCompletion()
      : m_State(static_cast<size_t>(State::Idle)),
        m_Callback(nullptr),
        m_Parameter(0),
        m_Generation(0),
        m_Result(0) {}

  /** Arms the obligation as the final step before hardware publication. */
  void arm(Callback callback, uintptr_t parameter, size_t generation) {
    assert(generation);
    assert(state() == State::Idle);
    m_Callback = callback;
    m_Parameter = parameter;
    m_Generation = generation;
    m_State = static_cast<size_t>(State::Active);
  }

  /** Captures a hardware result without yet owning callback publication. */
  MUST_USE_RESULT bool captureNatural(ssize_t result) {
    m_Result = result;
    return transition(State::Active, State::Captured);
  }

  /** Transfers a captured result to the ordinary reclamation path. */
  MUST_USE_RESULT bool claimCaptured(Claim& claim) {
    if (!transition(State::Captured, State::PublicationClaimed))
      return false;

    fillClaim(claim, Reason::Natural, m_Result);
    return true;
  }

  /**
   * Claims cancellation or identifies the exact record already published.
   *
   * Cancellation preserves a hardware result which won first. A
   * PublicationClaimed result is safe to drain because the caller must hold
   * the same completion lock used by the publisher.
   */
  MUST_USE_RESULT CancellationDisposition claimCancellation(Callback callback, uintptr_t parameter,
                                                            ssize_t cancellationResult,
                                                            Claim& claim) {
    if (!matches(callback, parameter))
      return CancellationDisposition::NoMatch;

    while (true) {
      const State observed = state();
      if (observed == State::Active) {
        if (transition(State::Active, State::PublicationClaimed)) {
          fillClaim(claim, Reason::Cancelled, cancellationResult);
          return CancellationDisposition::Claimed;
        }
        continue;
      }
      if (observed == State::Captured) {
        if (claimCaptured(claim))
          return CancellationDisposition::Claimed;
        continue;
      }
      if (observed == State::PublicationClaimed) {
        fillClaim(claim, Reason::Natural, 0);
        return CancellationDisposition::DrainPublished;
      }
      return CancellationDisposition::NoMatch;
    }
  }

  /**
   * Claims an accepted transfer after the controller is quiescent.
   *
   * An active transfer is failed because hardware produced no result. A
   * captured transfer retains its hardware result. A claimed transfer
   * already has a publisher and is never duplicated.
   */
  MUST_USE_RESULT bool claimForTeardown(ssize_t cancellationResult, Claim& claim) {
    while (true) {
      const State observed = state();
      if (observed == State::Active) {
        if (transition(State::Active, State::PublicationClaimed)) {
          fillClaim(claim, Reason::Teardown, cancellationResult);
          return true;
        }
        continue;
      }
      if (observed == State::Captured)
        return claimCaptured(claim);
      return false;
    }
  }

  State state() const {
    return static_cast<State>(static_cast<size_t>(m_State));
  }

  size_t generation() const {
    return m_Generation;
  }

 private:
  bool matches(Callback callback, uintptr_t parameter) const {
    return state() != State::Idle && m_Callback == callback && m_Parameter == parameter;
  }

  bool transition(State from, State to) {
    return m_State.compareAndSwap(static_cast<size_t>(from), static_cast<size_t>(to));
  }

  void fillClaim(Claim& claim, Reason reason, ssize_t result) const {
    claim.callback = m_Callback;
    claim.parameter = m_Parameter;
    claim.generation = m_Generation;
    claim.result = result;
    claim.reason = reason;
  }

  Atomic<size_t> m_State;
  Callback m_Callback;
  uintptr_t m_Parameter;
  size_t m_Generation;
  ssize_t m_Result;

  TransferCompletion(const TransferCompletion&) = delete;
  TransferCompletion& operator=(const TransferCompletion&) = delete;
};
}  // namespace UsbHcd

#endif
