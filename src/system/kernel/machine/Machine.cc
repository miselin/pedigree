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

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Serial.h"
#include "pedigree/kernel/machine/Vga.h"
#include "pedigree/kernel/processor/Processor.h"

Machine::~Machine() {}

void Machine::finalShutdown(ShutdownType type) {
  if (type == ShutdownType::Restart)
    Processor::reset();
}

void Machine::displayShutdownMessage(const char* message) {
  if (!message)
    return;
  Vga* console = getNumVga() ? getVga(0) : nullptr;
  if (console) {
    console->setLargestTextMode();
    uint16_t* cells = *console;
    const size_t rows = console->getNumRows();
    const size_t cols = console->getNumCols();
    if (cells && rows && cols) {
      for (size_t i = 0; i < rows * cols; ++i)
        cells[i] = 0x0f20;
      size_t length = 0;
      while (length < cols && message[length])
        ++length;
      const size_t start = (rows / 2) * cols + (cols - length) / 2;
      for (size_t i = 0; i < length; ++i)
        cells[start + i] = 0x0f00 | static_cast<uint8_t>(message[i]);
      console->moveCursor(cols, rows);
      console->flush();
    }
  }
  // Logging and graphics service callbacks may already have been unloaded.
  Serial* serial = getNumSerial() ? getSerial(0) : nullptr;
  if (serial) {
    serial->write_str(message);
    serial->write_str("\r\n");
  }
}

bool Machine::quiesceAllOtherProcessors() {
  EMIT_IF(MULTIPROCESSOR) {
    return false;
  }
  return true;
}

bool Machine::resumeAllOtherProcessors() {
  EMIT_IF(MULTIPROCESSOR) {
    return false;
  }
  return true;
}

bool Machine::stopAllOtherProcessors() {
  EMIT_IF(MULTIPROCESSOR) {
    return false;
  }
  return true;
}
