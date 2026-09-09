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

#include "Ps2Controller.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Controller.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Trace.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"

static constexpr size_t Ps2IoPollLimit = 1000000;

Ps2Controller::Ps2Controller(Controller* pDev)
    : Controller(pDev),
      IrqHandler(),
      m_pBase(nullptr),
      m_bHasSecondPort(false),
      m_FirstPortBuffer(16384),
      m_SecondPortBuffer(16384),
      m_FirstIrqId(0),
      m_SecondIrqId(0),
      m_DebugState(),
      m_ConfigByte(0),
      m_IoGate(),
      m_DrainLock(),
      m_ReadMode(PollingReadMode),
      m_RejectedThreadIo(0),
      m_FirstPortDrops(0),
      m_SecondPortDrops(0),
      m_EmptyIrqs(0) {}

Ps2Controller::Ps2Controller()
    : Controller(),
      IrqHandler(),
      m_pBase(nullptr),
      m_bHasSecondPort(false),
      m_FirstPortBuffer(16384),
      m_SecondPortBuffer(16384),
      m_FirstIrqId(0),
      m_SecondIrqId(0),
      m_DebugState(),
      m_ConfigByte(0),
      m_IoGate(),
      m_DrainLock(),
      m_ReadMode(PollingReadMode),
      m_RejectedThreadIo(0),
      m_FirstPortDrops(0),
      m_SecondPortDrops(0),
      m_EmptyIrqs(0) {}

Ps2Controller::~Ps2Controller() {
  if (m_FirstIrqId || m_SecondIrqId) {
    uninitialise();
  }
}

void Ps2Controller::initialise() {
  TRACE("PS2 Controller startup");
  m_pBase = addresses()[0]->m_Io;
  assert(m_pBase);

  TRACE("PS2: disabling devices");
  sendCommand(0xAD);  // disable all devices
  sendCommand(0xA7);
  readByteNonBlock();  // clear output buffer

  TRACE("PS2: disabling IRQs");
  m_ConfigByte = sendCommandWithResponse(0x20);
  m_ConfigByte = (m_ConfigByte & ~0x3) | 0x40;  // disable IRQs, leave translation enabled
  sendCommand(0x60, m_ConfigByte);

  m_bHasSecondPort = (m_ConfigByte & (1 << 5)) != 0;

  TRACE("PS2: performing self-test");
  uint8_t selfTestResponse = sendCommandWithResponse(0xAA);
  NOTICE("PS/2: self-test response: " << Hex << selfTestResponse);

  // Some 8042 implementations reset their config during self-test. Restore
  // our cached IRQ-disabled/translation state before either port is enabled.
  sendCommand(0x60, m_ConfigByte);

  // Enable both ports.
  TRACE("PS2: enabling ports");
  sendCommand(0xAE);
  sendCommand(0xA8);
  // Keep the cached command byte in sync with the port-enable commands so a
  // later IRQ configuration write does not disable the ports again.
  m_ConfigByte &= static_cast<uint8_t>(~((1 << 4) | (1 << 5)));

  // Reset all devices.
  TRACE("PS2: resetting first device");
  writeFirstPort(0xFF);
  uint8_t firstAck = 0;
  uint8_t firstStatus = 0;
  readFirstPort(firstAck);
  readFirstPort(firstStatus);
  NOTICE("PS/2: first port reset result: " << Hex << firstAck << ", " << firstStatus);

  TRACE("PS2: resetting second device");
  writeSecondPort(0xFF);
  uint8_t secondAck = 0;
  uint8_t secondStatus = 0;
  uint8_t secondExtra = 0;
  readSecondPort(secondAck);
  readSecondPort(secondStatus);
  readSecondPort(secondExtra);
  NOTICE("PS/2: second port reset result: " << Hex << secondAck << ", " << secondStatus << ", "
                                            << secondExtra);

  TRACE("PS2: startup complete");
}

bool Ps2Controller::initialise3() {
  if (m_FirstIrqId || m_SecondIrqId || !m_pBase) {
    return false;
  }

  m_FirstPortBuffer.wipe();
  m_SecondPortBuffer.wipe();
  m_FirstPortBuffer.enableWrites();
  m_SecondPortBuffer.enableWrites();
  m_RejectedThreadIo = 0;
  m_FirstPortDrops = 0;
  m_SecondPortDrops = 0;
  m_EmptyIrqs = 0;
  m_ReadMode = BufferedReadMode;

  IrqManager& irqManager = *Machine::instance().getIrqManager();
  m_FirstIrqId = irqManager.registerIsaIrqHandler(1, this, IrqPolicy::edgeThreaded());
  if (m_FirstIrqId && m_bHasSecondPort) {
    m_SecondIrqId = irqManager.registerIsaIrqHandler(12, this, IrqPolicy::edgeThreaded());
  }
  if (!m_FirstIrqId || (m_bHasSecondPort && !m_SecondIrqId)) {
    uninitialise();
    return false;
  }

  return true;
}

void Ps2Controller::uninitialise() {
  if (m_FirstIrqId || m_SecondIrqId) {
    if (!configureIrqEnable(false, false)) {
      FATAL("PS/2 teardown could not disable its interrupt sources");
    }

    // Device streaming can continue after its IRQ bits are cleared. Stop the
    // drain loop before waiting for callbacks, even under continuous traffic.
    m_ReadMode = StoppingReadMode;
    IrqManager& irqManager = *Machine::instance().getIrqManager();
    if (m_FirstIrqId) {
      if (!irqManager.unregisterHandler(m_FirstIrqId, this)) {
        FATAL("PS/2 teardown could not drain its first port callback");
      }
      m_FirstIrqId = 0;
    }
    if (m_SecondIrqId) {
      if (!irqManager.unregisterHandler(m_SecondIrqId, this)) {
        FATAL("PS/2 teardown could not drain its second port callback");
      }
      m_SecondIrqId = 0;
    }
  }

  // No producer remains. Publish the terminal read mode before waking blocked
  // readers so none can fall back to polling during teardown.
  m_ReadMode = StoppingReadMode;
  m_FirstPortBuffer.disableWrites();
  m_SecondPortBuffer.disableWrites();
}

bool Ps2Controller::acquireIoForThread() {
  if (Processor::inDeviceHardIrq()) {
    m_RejectedThreadIo += 1;
    return false;
  }

  while (!m_IoGate.tryAcquire()) {
#if THREADS
    Thread* current = Processor::information().getCurrentThread();
    if (!current || !Processor::getInterrupts()) {
      m_RejectedThreadIo += 1;
      return false;
    }
#if HOSTED
    if (current->getHostedSignalDepth()) {
      m_RejectedThreadIo += 1;
      return false;
    }
#endif
    // The current owner may be a preempted thread. Yielding lets it finish
    // without turning controller configuration into a same-core spin.
    Scheduler::instance().yield();
#else
    m_RejectedThreadIo += 1;
    return false;
#endif
  }
  return true;
}

void Ps2Controller::releaseIo() {
  m_IoGate.release();
}

bool Ps2Controller::sendCommandLocked(uint8_t command) {
  if (!waitForWritingLocked())
    return false;
  m_pBase->write8(command, 4);
  return true;
}

bool Ps2Controller::sendCommandLocked(uint8_t command, uint8_t data) {
  if (!sendCommandLocked(command) || !waitForWritingLocked())
    return false;
  m_pBase->write8(data, 0);
  return true;
}

bool Ps2Controller::sendCommandWithResponseLocked(uint8_t command, uint8_t& response) {
  if (!sendCommandLocked(command) || !waitForReadingLocked())
    return false;
  response = m_pBase->read8(0);
  return true;
}

bool Ps2Controller::sendCommandWithResponseLocked(uint8_t command, uint8_t data,
                                                  uint8_t& response) {
  if (!sendCommandLocked(command, data) || !waitForReadingLocked())
    return false;
  response = m_pBase->read8(0);
  return true;
}

bool Ps2Controller::writeFirstPortLocked(uint8_t byte) {
  if (!waitForWritingLocked())
    return false;
  m_pBase->write8(byte, 0);
  return true;
}

void Ps2Controller::sendCommand(uint8_t command) {
  if (!acquireIoForThread()) {
    return;
  }
  (void)sendCommandLocked(command);
  releaseIo();
}

void Ps2Controller::sendCommand(uint8_t command, uint8_t data) {
  if (!acquireIoForThread()) {
    return;
  }
  (void)sendCommandLocked(command, data);
  releaseIo();
}

uint8_t Ps2Controller::sendCommandWithResponse(uint8_t command) {
  if (!acquireIoForThread()) {
    return 0;
  }
  uint8_t response = 0;
  (void)sendCommandWithResponseLocked(command, response);
  releaseIo();
  return response;
}

uint8_t Ps2Controller::sendCommandWithResponse(uint8_t command, uint8_t data) {
  if (!acquireIoForThread()) {
    return 0;
  }
  uint8_t response = 0;
  (void)sendCommandWithResponseLocked(command, data, response);
  releaseIo();
  return response;
}

void Ps2Controller::writeFirstPort(uint8_t byte) {
  if (!acquireIoForThread()) {
    return;
  }
  (void)writeFirstPortLocked(byte);
  releaseIo();
}

void Ps2Controller::writeSecondPort(uint8_t byte) {
  if (!acquireIoForThread()) {
    return;
  }
  (void)sendCommandLocked(0xD4, byte);
  releaseIo();
}

bool Ps2Controller::hasSecondPort() const {
  return m_bHasSecondPort;
}

void Ps2Controller::setIrqEnable(bool firstEnabled, bool secondEnabled) {
  if (m_DebugState.active()) {
    return;
  }

  configureIrqEnable(firstEnabled, secondEnabled);
}

bool Ps2Controller::configureIrqEnable(bool firstEnabled, bool secondEnabled) {
  if (!acquireIoForThread()) {
    return false;
  }

  IrqManager& irqManager = *Machine::instance().getIrqManager();

  // Keep controller configuration atomic with respect to both port workers.
  secondEnabled = secondEnabled && m_bHasSecondPort;
  irqManager.enable(1, false);
  irqManager.enable(12, false);

  // Never accidentally remove translation
  uint8_t flagAdd = 0x40, flagRemove = ~0;
  if (firstEnabled) {
    flagAdd |= 1;
  } else {
    flagRemove &= ~1;
  }
  if (secondEnabled) {
    flagAdd |= 2;
  } else {
    flagRemove &= ~2;
  }

  NOTICE("Old config byte: " << Hex << m_ConfigByte);
  m_ConfigByte |= flagAdd;
  m_ConfigByte &= flagRemove;
  NOTICE("New config byte: " << Hex << m_ConfigByte);
  if (!sendCommandLocked(0x60, m_ConfigByte)) {
    releaseIo();
    return false;
  }
  NOTICE("completed!");

  // re-enable now that we're done here
  irqManager.enable(1, firstEnabled);
  irqManager.enable(12, secondEnabled);
  releaseIo();
  return true;
}

uint8_t Ps2Controller::readByte() {
  if (m_DebugState.active()) {
    // KDB may have interrupted the current gate owner and may still carry
    // the hard-IRQ marker. A single probe is safe; waiting is not.
    return readByteNonBlock();
  }

  if (!acquireIoForThread()) {
    return 0;
  }
  if (!waitForReadingLocked()) {
    releaseIo();
    return 0;
  }
  const uint8_t result = m_pBase->read8();
  releaseIo();
  return result;
}

uint8_t Ps2Controller::readByteNonBlock() {
  if (!m_IoGate.tryAcquire()) {
    return 0;
  }
  if ((m_pBase->read8(4) & 1) == 0) {
    releaseIo();
    return 0;
  }
  const uint8_t result = m_pBase->read8();
  releaseIo();
  return result;
}

bool Ps2Controller::readFirstPort(uint8_t& byte, bool block) {
  if (m_DebugState.active()) {
    byte = readByteNonBlock();
    return byte != 0;
  }

  const size_t readMode = m_ReadMode.value();
  if (readMode == StoppingReadMode) {
    return false;
  }
  if (readMode == PollingReadMode) {
    byte = readByte();
    return true;
  }

  size_t numRead = m_FirstPortBuffer.read(&byte, 1, block);
  return numRead > 0;
}

bool Ps2Controller::readSecondPort(uint8_t& byte, bool block) {
  if (m_DebugState.active()) {
    byte = readByteNonBlock();
    return byte != 0;
  }

  const size_t readMode = m_ReadMode.value();
  if (readMode == StoppingReadMode) {
    return false;
  }
  if (readMode == PollingReadMode) {
    byte = readByte();
    return true;
  }

  size_t numRead = m_SecondPortBuffer.read(&byte, 1, block);
  return numRead > 0;
}

void Ps2Controller::setDebugState(bool debugState) {
  m_DebugState.set(debugState);
}

IrqDisposition Ps2Controller::irq(irq_id_t number) {
  (void)number;
  // Both IRQ workers share port 0x60. Keep capture and delivery ordered even
  // when a worker for the other port has already drained this occurrence.
  LockGuard<Mutex> drain(m_DrainLock);
  size_t batch = 0;
  bool received = false;
  while (m_ReadMode.value() != StoppingReadMode && !m_DebugState.active()) {
    if (!acquireIoForThread()) {
      return IrqDisposition::NotHandled;
    }
    const uint8_t status = m_pBase->read8(4);
    if (!(status & OutputBufferFull)) {
      releaseIo();
      break;
    }
    const uint8_t value = m_pBase->read8(0);
    const bool secondPort = (status & SecondPortData) != 0;
    releaseIo();
    received = true;

    Buffer<uint8_t>& destination = secondPort ? m_SecondPortBuffer : m_FirstPortBuffer;
    // Reader lock contention must not discard a release, but an unconsumed
    // auxiliary port must not stop this shared worker by filling its buffer.
    if (destination.writeAvailable(&value, 1) != 1) {
      if (secondPort) {
        m_SecondPortDrops += 1;
      } else {
        m_FirstPortDrops += 1;
      }
    }

    if (++batch == 256) {
      // Retain the drain obligation across a scheduling turn: returning with
      // OBF set could strand data if its edge was already coalesced.
      Scheduler::instance().yield();
      batch = 0;
    }
  }
  if (!received) {
    m_EmptyIrqs += 1;
  }
  return m_ReadMode.value() == StoppingReadMode ? IrqDisposition::Quiesced
                                                : IrqDisposition::Handled;
}

bool Ps2Controller::waitForReadingLocked() {
  // Wait for the controller's output buffer to fill.
  for (size_t poll = 0; poll < Ps2IoPollLimit; ++poll) {
    if (m_pBase->read8(4) & OutputBufferFull)
      return true;
    Processor::pause();
  }
  if (m_pBase->read8(4) & OutputBufferFull)
    return true;
  ERROR("PS/2 controller output buffer did not fill before timeout");
  return false;
}

bool Ps2Controller::waitForWritingLocked() {
  // Wait for the controller's input buffer to empty.
  for (size_t poll = 0; poll < Ps2IoPollLimit; ++poll) {
    if (!(m_pBase->read8(4) & InputBufferFull))
      return true;
    Processor::pause();
  }
  if (!(m_pBase->read8(4) & InputBufferFull))
    return true;
  ERROR("PS/2 controller input buffer did not empty before timeout");
  return false;
}
