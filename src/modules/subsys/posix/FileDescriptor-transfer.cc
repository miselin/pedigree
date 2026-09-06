/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/assert.h"

#include "FileDescriptor.h"

namespace {
void acquireTransferLock(Mutex& lock) {
#if THREADS && !defined(STANDALONE_MUTEXES)
  const bool acquired = lock.acquireForCompletion();
#else
  const bool acquired = lock.acquire();
#endif
  if (!acquired) {
    FATAL("TransferPositionGuard could not acquire its mutex");
  }
}
}  // namespace

FileDescriptor::TransferPositionGuard::TransferPositionGuard(const OpenFileDescriptionLease& input,
                                                             const OpenFileDescriptionLease& output,
                                                             bool lockInput, bool lockOutput)
    : m_Input(input),
      m_Output(output),
      m_First(nullptr),
      m_Second(nullptr),
      m_InputFlags(0),
      m_OutputFlags(0) {
  assert(m_Input && m_Output);
  if (lockInput) {
    m_First = m_Input.get();
  }
  if (lockOutput && m_Output.get() != m_First) {
    if (m_First) {
      m_Second = m_Output.get();
    } else {
      m_First = m_Output.get();
    }
  }
  if (m_Second && reinterpret_cast<uintptr_t>(m_First) > reinterpret_cast<uintptr_t>(m_Second)) {
    OpenFileDescription* temporary = m_First;
    m_First = m_Second;
    m_Second = temporary;
  }

  const bool inputLocked = m_Input.get() == m_First || m_Input.get() == m_Second;
  const bool outputLocked = m_Output.get() == m_First || m_Output.get() == m_Second;
  // Ephemeral flag reads precede the ordered pair so they cannot invert it.
  if (!inputLocked) {
    LockGuard<Mutex> guard(m_Input->lock);
    m_InputFlags = m_Input->statusFlags;
  }
  if (!outputLocked) {
    if (sameDescription()) {
      m_OutputFlags = m_InputFlags;
    } else {
      LockGuard<Mutex> guard(m_Output->lock);
      m_OutputFlags = m_Output->statusFlags;
    }
  }

  if (m_First) {
    acquireTransferLock(m_First->lock);
  }
  if (m_Second) {
    acquireTransferLock(m_Second->lock);
  }
  if (inputLocked) {
    m_InputFlags = m_Input->statusFlags;
  }
  if (outputLocked) {
    m_OutputFlags = m_Output->statusFlags;
  }
}

FileDescriptor::TransferPositionGuard::~TransferPositionGuard() {
  // Final description release can retire backing state and take other locks.
  if (m_Second) {
    m_Second->lock.release();
  }
  if (m_First) {
    m_First->lock.release();
  }
}

int FileDescriptor::TransferPositionGuard::statusFlags(Endpoint endpoint) const {
  assert(endpoint == Endpoint::Input || endpoint == Endpoint::Output);
  return endpoint == Endpoint::Input ? m_InputFlags : m_OutputFlags;
}

FileDescriptor::OpenFileDescription& FileDescriptor::TransferPositionGuard::lockedDescription(
    Endpoint endpoint) const {
  assert(endpoint == Endpoint::Input || endpoint == Endpoint::Output);
  OpenFileDescription* description = endpoint == Endpoint::Input ? m_Input.get() : m_Output.get();
  assert(description == m_First || description == m_Second);
  return *description;
}

uint64_t FileDescriptor::TransferPositionGuard::offset(Endpoint endpoint) const {
  return lockedDescription(endpoint).offset;
}

void FileDescriptor::TransferPositionGuard::commitOffset(Endpoint endpoint, uint64_t finalOffset) {
  lockedDescription(endpoint).offset = finalOffset;
}

bool FileDescriptor::TransferPositionGuard::sameDescription() const {
  return m_Input.get() == m_Output.get();
}
