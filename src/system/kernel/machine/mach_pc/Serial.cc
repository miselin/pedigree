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

#include "Serial.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/Processor.h"

static constexpr size_t SerialStatusPollLimit = 100000;

X86Serial::X86Serial() : m_Port("COM") {}

X86Serial::~X86Serial() {}

void X86Serial::setBase(uintptr_t nBaseAddr) {
  m_Port.allocate(nBaseAddr, 8);

  m_Port.write8(0x00, serial::inten);  // Disable all interrupts
  m_Port.write8(0x80, serial::lctrl);  // Enable DLAB (set baud rate divisor)
  m_Port.write8(0x01, serial::rxtx);   // Set divisor to 3 (lo byte) 115200 baud
  m_Port.write8(0x00, serial::inten);  //                  (hi byte)
  m_Port.write8(0x03, serial::lctrl);  // 8 bits, no parity, one stop bit
  m_Port.write8(0xC7,
                serial::iififo);  // Enable FIFO, clear them, with 14-byte threshold
  // This driver is polling-only: keep UART interrupt sources and OUT2 off.
  m_Port.write8(0x03, serial::mctrl);  // DTR and RTS asserted
  m_Port.write8(0x00, serial::inten);

  NOTICE("Modem status: " << Hex << m_Port.read8(serial::mstat));
  NOTICE("Line status: " << Hex << m_Port.read8(serial::lstat));
}

char X86Serial::read() {
  if (!isConnected())
    return 0;
  if (!waitForStatus(0x1))
    return 0;

  return m_Port.read8(serial::rxtx);
}

char X86Serial::readNonBlock() {
  if (!isConnected())
    return 0;
  if (m_Port.read8(serial::lstat) & 0x1)
    return m_Port.read8(serial::rxtx);
  else
    return '\0';
}

void X86Serial::write(char c) {
  if (!isConnected())
    return;
  if (!waitForStatus(0x20))
    return;

  m_Port.write8(static_cast<unsigned char>(c), serial::rxtx);
}

bool X86Serial::waitForStatus(uint8_t mask) {
  for (size_t poll = 0; poll < SerialStatusPollLimit; ++poll) {
    if (m_Port.read8(serial::lstat) & mask)
      return true;
    Processor::pause();
  }
  return (m_Port.read8(serial::lstat) & mask) != 0;
}

bool X86Serial::isConnected() {
  return true;
  /*
  uint8_t nStatus = m_Port.read8(serial::mstat);
  // Bits 0x30 = Clear to send & Data set ready.
  // Mstat seems to be 0xFF when the device isn't present.
  if ((nStatus & 0x30) && nStatus != 0xFF)
      return true;
  else
      return false;
  */
}
