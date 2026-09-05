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

#ifndef SIGNAL_EVENT_H
#define SIGNAL_EVENT_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

class EXPORTED_PUBLIC SignalEventState {
 public:
  virtual ~SignalEventState() = default;
  virtual bool active() const = 0;
  virtual bool timer() const = 0;
  virtual void timerInfo(int32_t& timerId, int32_t& overrun) const = 0;
  virtual void complete(bool delivered, int32_t overrun) = 0;
  virtual const void* source() const {
    return nullptr;
  }
};

class EXPORTED_PUBLIC SignalEvent : public Event {
 public:
  enum class DeliveryDisposition {
    DefaultAction,
    CaughtHandler,
  };

  SignalEvent(uintptr_t handlerAddress, size_t signalNum, size_t specificNestingLevel = ~0UL,
              uint64_t signalMask = 0, bool deferSignal = true, bool isDeletable = false,
              HandlerPrivilege handlerPrivilege = HandlerPrivilege::Kernel,
              DeliveryDisposition disposition = DeliveryDisposition::CaughtHandler,
              bool useAlternateUserStack = false, size_t continuationEpoch = 0);

  SignalEvent(const SignalEvent& other);
  ~SignalEvent() override;

  uint64_t rebindGeneration() const {
    return m_RebindGeneration;
  }
  void setRebindGeneration(uint64_t value) {
    m_RebindGeneration = value;
  }
  uint64_t queueSequence() const {
    return m_QueueSequence;
  }
  void setQueueSequence(uint64_t value) {
    m_QueueSequence = value;
  }
  bool deliveryActive() const {
    return !m_DeliveryState || m_DeliveryState->active();
  }
  bool queuedIndividually() const {
    return m_SignalNumber >= 32 || (m_DeliveryState && m_DeliveryState->timer());
  }
  void setDeliveryState(const SharedPointer<SignalEventState>& state) {
    m_DeliveryState = state;
  }
  void transferDeliveryStateTo(SignalEvent& event) {
    event.m_DeliveryState = pedigree_std::move(m_DeliveryState);
  }
  const void* deliverySource() const {
    return m_DeliveryState ? m_DeliveryState->source() : nullptr;
  }
  void timerInfo(int32_t& timerId, int32_t& overrun) const {
    timerId = 0;
    overrun = 0;
    if (m_DeliveryState) {
      m_DeliveryState->timerInfo(timerId, overrun);
    }
  }
  void rejectSignalDelivery() const {
    if (m_DeliveryState)
      m_DeliveryState->complete(false, -1);
  }
  void completeSignalDelivery(int32_t overrun = 0) const {
    if (m_DeliveryState) {
      m_DeliveryState->complete(true, overrun);
    }
  }

  virtual size_t serialize(uint8_t* pBuffer);
  static bool unserialize(uint8_t* pBuffer, Event& event);

  virtual bool isSignalEvent() const {
    return true;
  }

  virtual bool prefersAlternateUserStack() const {
    return m_UseAlternateUserStack;
  }

  virtual bool isDeliverableWhileProcessSuspended() const {
    // Signal 9 is SIGKILL on every ABI which uses SignalEvent.
    return m_SignalNumber == 9;
  }

  virtual Event* cloneForDelivery();

  /** Copies a stable signal-disposition event without slicing subclasses. */
  virtual SignalEvent* cloneForDisposition();

  void setProcessDirected(bool processDirected) {
    m_ProcessDirected = processDirected;
  }

  bool isProcessDirected() const {
    return m_ProcessDirected;
  }

  /** Records the origin fields exposed through Linux siginfo_t. */
  void setSignalOrigin(int32_t signalCode, int32_t senderProcess, uint32_t senderUser) {
    m_SignalCode = signalCode;
    m_SenderProcess = senderProcess;
    m_SenderUser = senderUser;
  }

  int32_t getSignalCode() const {
    return m_SignalCode;
  }

  int32_t getSenderProcess() const {
    return m_SenderProcess;
  }

  void setSignalValue(uint64_t value) {
    m_SignalValue = value;
  }
  uint64_t getSignalValue() const {
    return m_SignalValue;
  }

  uint32_t getSenderUser() const {
    return m_SenderUser;
  }

  uint64_t getDeliverySignalMask() const {
    return m_SignalMask;
  }

  bool defersDeliveredSignal() const {
    return m_DeferSignal;
  }

  /** Stamps the process continuation generation captured for this delivery. */
  void setContinuationEpoch(size_t continuationEpoch) {
    m_ContinuationEpoch = continuationEpoch;
  }

  size_t getContinuationEpoch() const {
    return m_ContinuationEpoch;
  }

  virtual size_t getNumber() {
    return m_SignalNumber;
  }

 protected:
  SignalEvent(const SignalEvent& other, bool isDeletable);

 private:
  /** This keeps track of the actual signal this SignalEvent is linked to */
  size_t m_SignalNumber;

  /** Signals additionally blocked while the handler runs. */
  uint64_t m_SignalMask;

  /** Whether the delivered signal itself is blocked while the handler runs. */
  bool m_DeferSignal;

  /** Process-pending signals survive removal of their selected recipient. */
  bool m_ProcessDirected;

  /** Whether this delivery represents a caught signal handler. */
  DeliveryDisposition m_Disposition;

  /** Whether this delivery should enter an available alternate signal stack. */
  bool m_UseAlternateUserStack;

  /** Process continuation generation captured when this signal was queued. */
  size_t m_ContinuationEpoch;

  /** Linux-compatible signal origin metadata. */
  int32_t m_SignalCode;
  int32_t m_SenderProcess;
  uint32_t m_SenderUser;
  uint64_t m_SignalValue;
  SharedPointer<SignalEventState> m_DeliveryState;
  uint64_t m_RebindGeneration = 0;
  uint64_t m_QueueSequence = 0;
};

#endif
