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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Console.h"
#include "ConsoleDefines.h"
#include "modules/system/vfs/File.h"

class Filesystem;

extern const char defaultControl[MAX_CONTROL_CHAR];

ConsoleFile::ConsoleFile(size_t consoleNumber, String consoleName, Filesystem* pFs)
    : ConsoleFile(consoleNumber, consoleName, pFs, nullptr) {}

ConsoleFile::ConsoleFile(size_t consoleNumber, String consoleName, Filesystem* pFs, File* pParent)
    : File(consoleName, 0, 0, 0, 0xdeadbeef, pFs, 0, pParent),
      m_pOther(0),
      m_Flags(DEFAULT_FLAGS),
      m_Rows(25),
      m_Cols(80),
      m_IoLock(),
      m_IoChanged(),
      m_HangingUp(false),
      m_IoState(SharedPointer<ConsoleIoState>::tryAllocate()),
      m_ControlState(),
      m_ConsoleNumber(consoleNumber),
      m_ConsoleName(consoleName) {
  MemoryCopy(m_ControlChars, defaultControl, MAX_CONTROL_CHAR);

  // r/w for all (todo: when a console is locked, it should become owned
  // by the locking user)
  setPermissionsOnly(FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
  setUidOnly(0);
  setGidOnly(0);
}

ConsoleIoState::ConsoleIoState()
    : operations(),
      input(PTY_BUFFER_SIZE),
      output(PTY_BUFFER_SIZE),
      inputLock(),
      line(),
      lineSize(0),
      firstNewline(~0UL),
      physicalWake(0, true),
      m_Revoked(false) {}

ConsoleIoState::~ConsoleIoState() {
  // Unused consoles and unpublished replacements never pass through hangup.
  // Epoch users retain a shared owner until their operation lease is released.
  closeAdmission();
  cancelAndDrain();
}

bool ConsoleIoState::revoked() const {
  return __atomic_load_n(&m_Revoked, __ATOMIC_ACQUIRE);
}

void ConsoleIoState::closeAdmission() {
  __atomic_store_n(&m_Revoked, true, __ATOMIC_RELEASE);
  operations.close();
}

void ConsoleIoState::cancelAndDrain() {
  input.disableReads();
  input.disableWrites();
  output.disableReads();
  output.disableWrites();
  physicalWake.release();
  operations.wait();
}

ConsoleFile* ConsoleFile::stateOwner() {
  return isMaster() ? m_pOther : this;
}

SharedPointer<ConsoleIoState> ConsoleFile::captureOpenEpoch(bool waitForReopen) {
  ConsoleFile* owner = stateOwner();
  LockGuard<Mutex> guard(owner->m_IoLock);
  while (waitForReopen && owner->m_HangingUp)
    owner->m_IoChanged.waitForCompletion(owner->m_IoLock);
  if (waitForReopen && !owner->m_IoState)
    owner->m_IoState = SharedPointer<ConsoleIoState>::tryAllocate();
  return owner->m_IoState;
}

bool ConsoleFile::beginRevocation(SharedPointer<ConsoleIoState>& retired) {
  assert(!retired);
  ConsoleFile* owner = stateOwner();
  LockGuard<Mutex> guard(owner->m_IoLock);
  if (owner->m_HangingUp)
    return false;
  owner->m_HangingUp = true;
  retired = owner->m_IoState;
  if (retired)
    retired->closeAdmission();
  return true;
}

void ConsoleFile::finishRevocation(const SharedPointer<ConsoleIoState>& retired,
                                   const SharedPointer<ConsoleIoState>& replacement) {
  TerminationDeferral deferral;
  ConsoleFile* owner = stateOwner();
  if (retired)
    retired->cancelAndDrain();
  {
    LockGuard<Mutex> guard(owner->m_IoLock);
    assert(owner->m_HangingUp && owner->m_IoState.get() == retired.get());
    owner->m_IoState = replacement;
    owner->m_Flags = DEFAULT_FLAGS;
    MemoryCopy(owner->m_ControlChars, defaultControl, MAX_CONTROL_CHAR);
    owner->m_HangingUp = false;
    owner->m_IoChanged.broadcast();
  }
  changed();
}

SharedPointer<ConsoleControlState> ConsoleFile::controlState() {
  ConsoleFile* owner = stateOwner();
  LockGuard<Mutex> guard(owner->m_IoLock);
  return owner->m_ControlState;
}

void ConsoleFile::setControlState(const SharedPointer<ConsoleControlState>& state) {
  ConsoleFile* owner = stateOwner();
  SharedPointer<ConsoleControlState> retired;
  {
    LockGuard<Mutex> guard(owner->m_IoLock);
    retired = pedigree_std::move(owner->m_ControlState);
    owner->m_ControlState = state;
  }
}

uint64_t ConsoleFile::readEpoch(const SharedPointer<ConsoleIoState>& epoch, uint64_t size,
                                uintptr_t buffer, bool canBlock) {
  TerminationDeferral deferral;
  OperationBarrier::Lease operation;
  if (!epoch || !epoch->operations.tryAcquire(operation) || epoch->revoked())
    return 0;
  uint64_t amount = readIo(*epoch, size, buffer, canBlock);
  return epoch->revoked() ? 0 : amount;
}

uint64_t ConsoleFile::writeEpoch(const SharedPointer<ConsoleIoState>& epoch, uint64_t size,
                                 uintptr_t buffer, bool canBlock) {
  TerminationDeferral deferral;
  OperationBarrier::Lease operation;
  if (!epoch || !epoch->operations.tryAcquire(operation) || epoch->revoked()) {
    SYSCALL_ERROR(IoError);
    return 0;
  }
  uint64_t amount = writeIo(*epoch, size, buffer, canBlock);
  if (epoch->revoked() && !amount)
    SYSCALL_ERROR(IoError);
  return amount;
}

void ConsoleFile::changed() {
  dataChanged();
  if (m_pOther)
    m_pOther->dataChanged();
}

int ConsoleFile::select(bool writing, int timeout) {
  auto state = captureOpenEpoch(false);
  if (!state)
    return 0;
  Buffer<char>& source = isMaster() ? state->output : state->input;
  Buffer<char>& destination = isMaster() ? state->input : state->output;
  return (writing ? destination.canWrite(timeout > 0) : source.canRead(timeout > 0)) ? 1 : 0;
}

ReadyMask ConsoleFile::queryEpoch(const SharedPointer<ConsoleIoState>& epoch, bool reading,
                                  bool writing) {
  if (!epoch || epoch->revoked())
    return ReadyRead | ReadyWrite | ReadyError | ReadyHangup;
  // Physical terminals have no PTY peer, including serial ports without a VT number.
  if (!m_pOther) {
    return File::queryReady(reading, writing);
  }
  ReadyMask ready = ReadyNone;
  Buffer<char>& source = isMaster() ? epoch->output : epoch->input;
  Buffer<char>& destination = isMaster() ? epoch->input : epoch->output;
  if (reading && source.canRead(false))
    ready |= ReadyRead;
  if (writing && destination.canWrite(false))
    ready |= ReadyWrite;
  return ready;
}

ReadyMask ConsoleFile::queryReady(bool reading, bool writing) {
  return queryEpoch(captureOpenEpoch(false), reading, writing);
}

ReadinessGenerations ConsoleFile::epochGenerations(const SharedPointer<ConsoleIoState>& epoch) {
  ReadinessGenerations result;
  if (epoch) {
    result.read = (isMaster() ? epoch->output : epoch->input).readableGeneration();
    result.write = (isMaster() ? epoch->input : epoch->output).writableGeneration();
    result.error = result.hangup = epoch->revoked() ? 1 : 0;
  }
  return result;
}

ReadinessGenerations ConsoleFile::readinessGenerations() {
  return epochGenerations(captureOpenEpoch(false));
}

size_t ConsoleFile::outputLineDiscipline(char* buf, size_t len, size_t maxSz, size_t flags) {
  // Make sure we always have the latest flags from the slave.
  size_t slaveFlags = flags;

  // Post-process output if enabled.
  if (slaveFlags & (ConsoleManager::OPostProcess)) {
    const size_t tmpSize = maxSz > len ? maxSz : len;
    char* tmpBuff = new char[tmpSize];
    if (!tmpBuff) {
      return len;
    }
    size_t realSize = len;

    char* pC = buf;
    for (size_t i = 0, j = 0; j < len; j++) {
      bool bInsert = true;

      // OCRNL: Map CR to NL on output
      if (pC[j] == '\r' && (slaveFlags & ConsoleManager::OMapCRToNL)) {
        tmpBuff[i++] = '\n';
        continue;
      }

      // ONLCR: Map NL to CR-NL on output
      else if (pC[j] == '\n' && (slaveFlags & ConsoleManager::OMapNLToCRNL)) {
        if (realSize >= maxSz) {
          // We do not have any room to add in the mapped character.
          // Drop it.
          WARNING(
              "Console ignored an NL -> CRNL conversion due to a "
              "full buffer.");
          tmpBuff[i++] = '\n';
          continue;
        }

        realSize++;

        // Add the newline and the caused carriage return
        tmpBuff[i++] = '\r';
        tmpBuff[i++] = '\n';

        continue;
      }

      // ONLRET: NL performs CR function
      if (pC[j] == '\n' && (slaveFlags & ConsoleManager::ONLCausesCR)) {
        tmpBuff[i++] = '\r';
        continue;
      }

      if (bInsert) {
        tmpBuff[i++] = pC[j];
      }
    }

    MemoryCopy(buf, tmpBuff, realSize);
    delete[] tmpBuff;
    len = realSize;
  }

  return len;
}

size_t ConsoleFile::processInput(char* buf, size_t len) {
  // Perform input processing.
  char* pC = buf;
  size_t realLen = len;
  for (size_t i = 0; i < len; i++) {
    if (m_Flags & ConsoleManager::IStripToSevenBits)
      pC[i] = static_cast<uint8_t>(pC[i]) & 0x7F;
    if (m_Flags & ConsoleManager::LCookedMode) {
      if (pC[i] == m_ControlChars[VEOF]) {
        // Zero-length read: EOF.
        realLen = 0;
        break;
      }
    }

    if (pC[i] == '\n' && (m_Flags & ConsoleManager::IMapNLToCR))
      pC[i] = '\r';
    else if (pC[i] == '\r' && (m_Flags & ConsoleManager::IMapCRToNL))
      pC[i] = '\n';
    else if (pC[i] == '\r' && (m_Flags & ConsoleManager::IIgnoreCR)) {
      MemoryCopy(buf + i, buf + i + 1, len - i - 1);
      i--;  // Need to process this byte again, its contents have changed.
      realLen--;
    }
  }

  return realLen;
}

void ConsoleFile::inputLineDiscipline(ConsoleIoState& state, char* buf, size_t len, bool canBlock,
                                      size_t flags, const char* controlChars) {
  LockGuard<Mutex> inputGuard(state.inputLock);
  if (state.revoked())
    return;
  // Make sure we always have the latest flags from the slave.
  if (flags == ~0U) {
    flags = m_pOther->m_Flags;
  }
  size_t slaveFlags = flags;
  if (controlChars == 0) {
    controlChars = m_pOther->m_ControlChars;
  }
  const char* slaveControlChars = controlChars;

  size_t localWritten = 0;

  // Handle temios local modes
  if (slaveFlags & (ConsoleManager::LCookedMode | ConsoleManager::LEcho)) {
    // Whether or not the application buffer has already been filled
    bool bAppBufferComplete = false;

    // Used for raw mode - just a buffer for erase echo etc.
    char* destBuff = new char[len];
    if (!destBuff) {
      return;
    }
    size_t destBuffOffset = 0;

    // Iterate over the buffer
    while (!bAppBufferComplete) {
      for (size_t i = 0; i < len && !state.revoked(); i++) {
        if (state.lineSize == LINEBUFFER_MAXIMUM) {
          state.input.write(state.line, state.lineSize, canBlock);
          state.lineSize = 0;
          state.firstNewline = ~0UL;
        }
        // Handle incoming newline
        bool isCanonical = (slaveFlags & ConsoleManager::LCookedMode);
        if (isCanonical && (buf[i] == slaveControlChars[VEOF])) {
          // EOF. Write it and it alone to the slave.
          state.input.write(&buf[i], 1, canBlock);
          delete[] destBuff;
          changed();
          return;
        }

        if ((buf[i] == '\r') || (isCanonical && (buf[i] == slaveControlChars[VEOL]))) {
          // LEcho - output the newline. LCookedMode - handle line
          // buffer.
          if ((slaveFlags & ConsoleManager::LEcho) || (slaveFlags & ConsoleManager::LCookedMode)) {
            // Only echo the newline if we are supposed to
            state.line[state.lineSize++] = '\n';
            if ((slaveFlags & ConsoleManager::LEchoNewline) ||
                (slaveFlags & ConsoleManager::LEcho)) {
              char tmp[] = {'\n', 0};
              state.output.write(tmp, 1);
              ++localWritten;
            }

            if ((slaveFlags & ConsoleManager::LCookedMode) && !bAppBufferComplete) {
              // Transmit full buffer to slave.
              size_t realSize = state.lineSize;
              if (state.firstNewline < realSize) {
                realSize = state.firstNewline;
                state.firstNewline = ~0UL;
              }

              state.input.write(state.line, realSize, canBlock);

              // And now move the buffer over the space we just
              // consumed
              uint64_t nConsumedBytes = state.lineSize - realSize;
              if (nConsumedBytes)  // If zero, the buffer was
                                   // consumed completely
                MemoryCopy(state.line, &state.line[realSize], nConsumedBytes);

              // Reduce the buffer size now
              state.lineSize -= realSize;

              // The buffer has been filled!
              bAppBufferComplete = true;
            } else if ((slaveFlags & ConsoleManager::LCookedMode) && (state.firstNewline == ~0UL)) {
              // Application buffer has already been filled, let
              // future runs know where the limit is
              state.firstNewline = state.lineSize - 1;
            } else if (!(slaveFlags & ConsoleManager::LCookedMode)) {
              // Inject this byte into the slave...
              destBuff[destBuffOffset++] = buf[i];
            }

            // Ignore the \n if one is present
            if (i + 1 < len && buf[i + 1] == '\n')
              i++;
          }
        } else if (buf[i] == m_ControlChars[VERASE]) {
          if (slaveFlags & (ConsoleManager::LCookedMode | ConsoleManager::LEchoErase)) {
            if ((slaveFlags & ConsoleManager::LCookedMode) && state.lineSize) {
              char ctl[3] = {'\x08', ' ', '\x08'};
              state.output.write(ctl, 3);
              state.lineSize--;
              ++localWritten;
            } else if ((!(slaveFlags & ConsoleManager::LCookedMode)) && destBuffOffset) {
              char ctl[3] = {'\x08', ' ', '\x08'};
              state.output.write(ctl, 3);
              destBuffOffset--;
              ++localWritten;
            }
          }
        } else {
          // Do we need to handle this character differently?
          if (isControlCharacter(slaveFlags, buf[i], controlChars)) {
            // So, normally we'll be fine to print nicely, but if
            // we can't write to the ring buffer, we must not try
            // to do so. This event may be necessary to unblock the
            // buffer!
            if (!state.output.canWrite(false)) {
              // Forcefully clear out bytes so we can write what
              // we need to to the ring buffer.
              WARNING(
                  "Console: dropping bytes to be able to "
                  "render visual control code (e.g. ^C)");
              char tmp[3];
              state.output.read(tmp, 3);
            }

            // Write it to the master nicely (eg, ^C, ^D)
            char ctl_c = '@' + buf[i];
            char ctl[3] = {'^', ctl_c, '\n'};
            state.output.write(ctl, 3);
            ++localWritten;

            notifyControlCharacter(buf[i], controlChars);
            continue;
          }

          // Write the character to the slave
          if (slaveFlags & ConsoleManager::LEcho) {
            state.output.write(&buf[i], 1);
            ++localWritten;
          }

          // Add to the buffer
          if (slaveFlags & ConsoleManager::LCookedMode)
            state.line[state.lineSize++] = buf[i];
          else {
            destBuff[destBuffOffset++] = buf[i];
          }
        }
      }

      // We appear to have hit the top of the line buffer!
      if (state.lineSize >= LINEBUFFER_MAXIMUM) {
        // Our best bet is to return early, giving the application what
        // we can of the line buffer
        size_t numBytesToRemove = state.lineSize;

        // Copy the buffer across
        state.input.write(state.line, numBytesToRemove, canBlock);

        // And now move the buffer over the space we just consumed
        uint64_t nConsumedBytes = state.lineSize - numBytesToRemove;
        if (nConsumedBytes)  // If zero, the buffer was consumed
                             // completely
          MemoryCopy(state.line, &state.line[numBytesToRemove], nConsumedBytes);

        // Reduce the buffer size now
        state.lineSize -= numBytesToRemove;
      }

      /// \todo remove me, this is because of the port
      break;
    }

    if (destBuffOffset) {
      state.input.write(destBuff, destBuffOffset, canBlock);
    }

    delete[] destBuff;
  } else {
    for (size_t i = 0; i < len && !state.revoked(); ++i) {
      if (isControlCharacter(slaveFlags, buf[i], controlChars)) {
        notifyControlCharacter(buf[i], controlChars);
        continue;
      }

      // No event. Simply write the character out.
      state.input.write(&buf[i], 1, canBlock);
    }
  }

  // Wake up anything waiting on data to read from us.
  (void)localWritten;
  changed();
}

bool ConsoleFile::isControlCharacter(size_t flags, char check, const char* controlChars) {
  // ISIG?
  if (flags & ConsoleManager::LGenerateEvent) {
    if (check && (check == controlChars[VINTR] || check == controlChars[VQUIT] ||
                  check == controlChars[VSUSP])) {
      return true;
    }
  }
  return false;
}

void ConsoleFile::notifyControlCharacter(char cause, const char* controlChars) {
  auto control = controlState();
  if (!control)
    return;
  // The retained callback does not need the console mutex or an event ack.
  // Its POSIX implementation queues signals without dispatching user code.
  if (cause == controlChars[VINTR])
    control->controlCharacter(ConsoleControlState::Character::Interrupt);
  else if (cause == controlChars[VQUIT])
    control->controlCharacter(ConsoleControlState::Character::Quit);
  else if (cause == controlChars[VSUSP])
    control->controlCharacter(ConsoleControlState::Character::Suspend);
}
