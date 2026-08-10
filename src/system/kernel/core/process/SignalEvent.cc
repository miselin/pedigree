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

#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/types.h"

SignalEvent::SignalEvent(uintptr_t handlerAddress, size_t signalNum, size_t specificNestingLevel,
                         uint64_t signalMask, bool deferSignal, bool isDeletable)
    : Event(handlerAddress, isDeletable, specificNestingLevel),
      m_SignalNumber(signalNum),
      m_SignalMask(signalMask),
      m_DeferSignal(deferSignal) {}

Event* SignalEvent::cloneForDelivery() {
  if (isDeletable()) {
    return this;
  }

  return new SignalEvent(m_HandlerAddress, m_SignalNumber, m_NestingLevel, m_SignalMask,
                         m_DeferSignal, true);
}

/// \todo There may be a need for serialization in the future...
size_t SignalEvent::serialize(uint8_t* pBuffer) {
  Thread* pThread = Processor::information().getCurrentThread();
  if (pThread) {
    pThread->markSignalInterruptedWait();

    constexpr uint64_t UnblockableSignals =
        (static_cast<uint64_t>(1) << (9 - 1)) | (static_cast<uint64_t>(1) << (19 - 1));
    uint64_t mask = pThread->getSignalMask() | m_SignalMask;
    if (m_DeferSignal && m_SignalNumber > 0 && m_SignalNumber <= 64) {
      mask |= static_cast<uint64_t>(1) << (m_SignalNumber - 1);
    }
    mask &= ~UnblockableSignals;
    pThread->setSignalMask(mask);
  }

  pBuffer[0] = static_cast<uint8_t>(m_SignalNumber & 0xFF);
  return 1;
}

bool SignalEvent::unserialize(uint8_t* pBuffer, Event& event) {
  pBuffer[0] = static_cast<uint8_t>(event.getNumber() & 0xFF);
  return true;
}
