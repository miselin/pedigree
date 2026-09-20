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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Serial.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/StaticString.h"

extern "C" void _assert(bool b, const char* file, int line, const char* func) {
  if (b)
    return;

  if (Processor::m_Initialised) {
    // Ordinary log sinks may be disabled, and debugger entry may itself fail.
    if (Machine::instance().getNumSerial()) {
      Serial* serial = Machine::instance().getSerial(0);
      if (serial) {
        TinyStaticString location;
        location.append(line, 10);
        serial->write_str("\nASSERT: ");
        serial->write_str(file);
        serial->write_str(":");
        serial->write_str(location);
        serial->write_str(" in ");
        serial->write_str(func);
        serial->write_str("\n");
      }
    }
    ERROR_NOLOCK("Assertion failed in file " << file);
    ERROR_NOLOCK("In function '" << func << "'");
    ERROR_NOLOCK("On line " << Dec << line << Hex << ".");
    Processor::breakpoint();

    ERROR_NOLOCK("You may not resume after a failed assertion.");
  }

  // Best reason for a return is that the debugger isn't active. Either way,
  // it's an error condition, panic.
  panic("assertion failed");
}
