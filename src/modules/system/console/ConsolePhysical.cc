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

#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Console.h"
#include "ConsoleDefines.h"
#include "modules/system/vfs/File.h"

class Filesystem;

ConsolePhysicalFile::ConsolePhysicalFile(size_t nth, File* pTerminal, String consoleName,
                                         Filesystem* pFs)
    : ConsoleFile(~0U, consoleName, pFs), m_pTerminal(pTerminal), m_TerminalNumber(nth) {}

namespace {
// Physical sources without notifications retain their existing polling policy,
// but sleep between nonblocking attempts so revocation can cancel every wait.
bool waitPhysical(ConsoleIoState& state) {
  if (state.revoked())
    return false;
  Semaphore::SemaphoreError error = Semaphore::NoError;
  if (!state.physicalWake.acquireWithError(1, 0, 10000, error) && error != Semaphore::TimedOut) {
    SYSCALL_ERROR(Interrupted);
    return false;
  }
  return !state.revoked();
}
}  // namespace

uint64_t ConsolePhysicalFile::readIo(ConsoleIoState& state, uint64_t size, uintptr_t buffer,
                                     bool canBlock) {
  while (!state.revoked()) {
    if (state.input.canRead(false))
      return state.input.read(reinterpret_cast<char*>(buffer), size, false);
    char input[512];
    size_t amount = m_pTerminal->read(0, sizeof(input), reinterpret_cast<uintptr_t>(input), false);
    if (amount)
      inputLineDiscipline(state, input, amount, canBlock, m_Flags, m_ControlChars);
    char echo[512];
    while (!state.revoked() && state.output.canRead(false)) {
      size_t echoed = state.output.read(echo, sizeof(echo), false);
      if (!echoed || writeIo(state, echoed, reinterpret_cast<uintptr_t>(echo), canBlock) != echoed)
        break;
    }
    if (state.input.canRead(false))
      continue;
    if (!canBlock || !waitPhysical(state))
      break;
  }
  return 0;
}

uint64_t ConsolePhysicalFile::writeIo(ConsoleIoState& state, uint64_t size, uintptr_t buffer,
                                      bool canBlock) {
  size_t total = 0;
  while (total < size && !state.revoked()) {
    char output[512];
    size_t chunk = size - total;
    if (chunk > sizeof(output) / 2)
      chunk = sizeof(output) / 2;
    MemoryCopy(output, reinterpret_cast<const void*>(buffer + total), chunk);
    size_t length = outputLineDiscipline(output, chunk, sizeof(output), m_Flags);
    size_t sent = 0;
    while (sent < length && !state.revoked()) {
      size_t amount =
          m_pTerminal->write(0, length - sent, reinterpret_cast<uintptr_t>(output + sent), false);
      sent += amount;
      if (!amount && (!canBlock || !waitPhysical(state)))
        return total;
    }
    if (sent != length)
      break;
    total += chunk;
  }
  return total;
}

uint64_t ConsolePhysicalFile::readBytewise(uint64_t, uint64_t size, uintptr_t buffer,
                                           bool canBlock) {
  return readEpoch(captureOpenEpoch(), size, buffer, canBlock);
}

uint64_t ConsolePhysicalFile::writeBytewise(uint64_t, uint64_t size, uintptr_t buffer,
                                            bool canBlock) {
  return writeEpoch(captureOpenEpoch(), size, buffer, canBlock);
}

int ConsolePhysicalFile::select(bool writing, int timeout) {
  if (writing)
    return m_pTerminal->select(true, timeout);
  auto state = captureOpenEpoch(false);
  if (state && state->input.canRead(false))
    return 1;
  return m_pTerminal->select(false, timeout);
}
