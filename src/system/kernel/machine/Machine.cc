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

namespace {
Machine::ShutdownPhase shutdownPhase = Machine::ShutdownPhase::NotStarted;
}

void Machine::setShutdownPhase(ShutdownPhase phase) {
  __atomic_store(&shutdownPhase, &phase, __ATOMIC_RELAXED);
}

const char* Machine::shutdownPhaseName() {
  // Module callers publish an enum, never a pointer into unloadable text.
  ShutdownPhase phase;
  __atomic_load(&shutdownPhase, &phase, __ATOMIC_RELAXED);
  switch (phase) {
    case ShutdownPhase::NotStarted:
      return "not started";
    case ShutdownPhase::Requested:
      return "shutdown request";
    case ShutdownPhase::Userspace:
      return "terminating userspace";
    case ShutdownPhase::Syscalls:
      return "draining system calls";
    case ShutdownPhase::Filesystems:
      return "detaching filesystems";
    case ShutdownPhase::Modules:
      return "unloading modules";
    case ShutdownPhase::Destructors:
      return "draining deferred destruction";
    case ShutdownPhase::Input:
      return "stopping input";
    case ShutdownPhase::Caches:
      return "flushing caches";
    case ShutdownPhase::Timers:
      return "stopping info block timer";
    case ShutdownPhase::Devices:
      return "stopping platform devices";
    case ShutdownPhase::Processors:
      return "stopping other processors";
    case ShutdownPhase::ProcessorCleanup:
      return "releasing processor services";
    case ShutdownPhase::FinalAction:
      return "final power/reset action";
  }
  return "unknown";
}

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
      auto lineLength = [cols](const char* text) {
        size_t length = 0;
        while (length < cols && text[length] && text[length] != '\n')
          ++length;
        return length;
      };
      size_t lines = 0;
      const char* text = message;
      while (*text && lines < rows) {
        text += lineLength(text);
        if (*text == '\n')
          ++text;
        ++lines;
      }
      text = message;
      const size_t firstRow = (rows - lines) / 2;
      for (size_t line = 0; line < lines; ++line) {
        const size_t length = lineLength(text);
        const size_t start = (firstRow + line) * cols + (cols - length) / 2;
        for (size_t i = 0; i < length; ++i)
          cells[start + i] = 0x0f00 | static_cast<uint8_t>(text[i]);
        text += length;
        if (*text == '\n')
          ++text;
      }
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
