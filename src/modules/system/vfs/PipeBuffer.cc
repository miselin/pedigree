/* Copyright (c) 2026, Pedigree Developers. */
#include "PipeBuffer.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#if THREADS && !defined(PEDIGREE_BUILDUTILS)
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#endif

PipeBuffer::PipeBuffer(ChangeCallback changed, void* context)
    : m_ReadGeneration(0), m_WriteGeneration(0), m_Changed(changed), m_Context(context) {}

PipeBuffer::~PipeBuffer() {
  TerminationDeferral termination;
  close();
  lock(m_Lock);
  while (m_ActiveOperations) {
    m_DrainCondition.waitForCompletion(m_Lock);
  }
  assert(!m_PairWaiters && !m_ReadReserved && !m_WriteReserved);
  m_Lock.release();
}

void PipeBuffer::lock(Mutex& mutex) {
#ifdef STANDALONE_MUTEXES
  const bool acquired = mutex.acquire();
#else
  const bool acquired = mutex.acquireForCompletion();
#endif
  if (!acquired) {
    FATAL("PipeBuffer could not acquire its lifetime mutex.");
  }
}

bool PipeBuffer::wait(ConditionVariable& condition, Mutex& mutex) {
  ConditionVariable::Error error = ConditionVariable::NoError;
  const bool success = condition.wait(mutex, error);
  if (!ConditionVariable::mutexAcquired(error)) {
    lock(mutex);
  }
#if THREADS && !defined(PEDIGREE_BUILDUTILS)
  if (error == ConditionVariable::Interrupted) {
    Processor::information().getCurrentThread()->setInterruptionReason(Thread::InterruptedBySignal);
  }
#endif
  return success;
}

bool PipeBuffer::interrupted() {
#if THREADS && !defined(PEDIGREE_BUILDUTILS)
  Thread* thread = Processor::information().getCurrentThread();
  return thread && (thread->getInterruptionReason() == Thread::InterruptedBySignal ||
                    thread->getUnwindState() != Thread::Continue);
#else
  return false;
#endif
}

PipeBuffer::ActiveOperation::ActiveOperation(PipeBuffer& buffer)
    : m_Termination(), m_Buffer(buffer.beginOperation() ? &buffer : nullptr) {}

PipeBuffer::ActiveOperation::~ActiveOperation() {
  if (m_Buffer) {
    m_Buffer->endOperation();
  }
}

PipeBuffer::ActiveOperation::operator bool() const {
  return m_Buffer != nullptr;
}

void PipeBuffer::ActiveOperation::detach() {
  m_Buffer = nullptr;
}

bool PipeBuffer::beginOperation() {
  lock(m_Lock);
  const bool admitted = !m_Closing;
  if (admitted) {
    ++m_ActiveOperations;
  }
  m_Lock.release();
  return admitted;
}

void PipeBuffer::endOperation() {
  lock(m_Lock);
  assert(m_ActiveOperations);
  if (!--m_ActiveOperations) {
    m_DrainCondition.broadcast();
  }
  m_Lock.release();
}

bool PipeBuffer::readableLocked() const {
  return !m_Closing && m_ReadEnabled && !m_ResetPending && !m_ReadReserved && m_Size;
}

bool PipeBuffer::writableLocked() const {
  return !m_Closing && m_ReadEnabled && m_WriteEnabled && !m_ResetPending && !m_WriteReserved &&
         m_Size < Capacity;
}

PipeBuffer::Result PipeBuffer::readyLocked(bool writing, size_t minimum) const {
  if (writing) {
    if (m_Closing || !m_ReadEnabled || !m_WriteEnabled) {
      return {Status::Closed, 0};
    }
    if (writableLocked() && Capacity - m_Size >= minimum) {
      return {Status::Ready, Capacity - m_Size};
    }
  } else {
    if (m_Closing || !m_ReadEnabled || (!m_WriteEnabled && !m_Size)) {
      return {Status::Eof, 0};
    }
    if (readableLocked()) {
      return {Status::Ready, m_Size};
    }
  }
  return {Status::WouldBlock, 0};
}

PipeBuffer::Result PipeBuffer::waitLocked(bool writing, size_t minimum, bool block) {
  for (;;) {
    const Result result = readyLocked(writing, minimum);
    if (result.status != Status::WouldBlock || !block) {
      return result;
    }
    if (interrupted() || !wait(writing ? m_WriteCondition : m_ReadCondition, m_Lock)) {
      return {Status::Interrupted, 0};
    }
  }
}

void PipeBuffer::changedLocked() {
  const bool readable = readableLocked();
  const bool writable = writableLocked();
  if (readable && !m_WasReadable) {
    m_ReadGeneration += 1;
  }
  if (writable && !m_WasWritable) {
    m_WriteGeneration += 1;
  }
  m_WasReadable = readable;
  m_WasWritable = writable;
  m_ReadCondition.broadcast();
  m_WriteCondition.broadcast();
  wakePairsLocked();
}

void PipeBuffer::resetLocked() {
  m_ResetPending = true;
  finishResetLocked();
}

void PipeBuffer::finishResetLocked() {
  if (m_ResetPending && !m_ReadReserved && !m_WriteReserved) {
    m_Head = 0;
    m_Size = 0;
    m_ResetPending = false;
  }
}

void PipeBuffer::copyOutLocked(uint8_t* destination, size_t count) const {
  assert(count <= m_Size);
  const size_t first = count < Capacity - m_Head ? count : Capacity - m_Head;
  pedigree_std::copy(destination, m_Data + m_Head, first);
  if (count > first) {
    pedigree_std::copy(destination + first, m_Data, count - first);
  }
}

void PipeBuffer::appendLocked(const uint8_t* source, size_t count) {
  assert(count <= Capacity - m_Size);
  const size_t tail = (m_Head + m_Size) % Capacity;
  const size_t first = count < Capacity - tail ? count : Capacity - tail;
  pedigree_std::copy(m_Data + tail, source, first);
  if (count > first) {
    pedigree_std::copy(m_Data, source + first, count - first);
  }
  m_Size += count;
}

void PipeBuffer::consumeLocked(size_t count) {
  assert(count <= m_Size);
  m_Head = (m_Head + count) % Capacity;
  m_Size -= count;
}

void PipeBuffer::publish() {
  if (m_Changed) {
    m_Changed(m_Context);
  }
}

size_t PipeBuffer::read(uint8_t* destination, size_t count, bool block) {
  ActiveOperation operation(*this);
  if (!operation || !count) {
    return 0;
  }
  lock(m_Lock);
  const Result result = waitLocked(false, 1, block);
  size_t accepted = 0;
  if (result.status == Status::Ready) {
    accepted = count < result.count ? count : result.count;
    copyOutLocked(destination, accepted);
    consumeLocked(accepted);
    changedLocked();
  }
  m_Lock.release();
  if (accepted) {
    publish();
  }
  return accepted;
}

size_t PipeBuffer::write(const uint8_t* source, size_t count, bool block) {
  ActiveOperation operation(*this);
  if (!operation) {
    return 0;
  }
  size_t written = 0;
  while (written < count) {
    if (interrupted()) {
      break;
    }
    lock(m_Lock);
    const Result result = waitLocked(true, 1, block);
    if (result.status != Status::Ready) {
      m_Lock.release();
      break;
    }
    const size_t remaining = count - written;
    const size_t accepted = remaining < result.count ? remaining : result.count;
    appendLocked(source + written, accepted);
    changedLocked();
    m_Lock.release();
    written += accepted;
    // A readiness-driven reader must see this chunk before we wait for space.
    publish();
  }
  return written;
}

size_t PipeBuffer::writeAtomic(const uint8_t* source, size_t count, bool block) {
  ActiveOperation operation(*this);
  if (!operation || !count || count > Capacity) {
    return 0;
  }
  lock(m_Lock);
  const Result result = waitLocked(true, count, block);
  if (result.status == Status::Ready) {
    appendLocked(source, count);
    changedLocked();
  }
  m_Lock.release();
  if (result.status != Status::Ready) {
    return 0;
  }
  publish();
  return count;
}

PipeBuffer::Result PipeBuffer::waitTransfer(bool writing, bool block) {
  ActiveOperation operation(*this);
  if (!operation) {
    return {writing ? Status::Closed : Status::Eof, 0};
  }
  lock(m_Lock);
  const Result result = waitLocked(writing, 1, block);
  m_Lock.release();
  return result;
}

bool PipeBuffer::canRead(bool block) {
  return waitTransfer(false, block).status == Status::Ready;
}

bool PipeBuffer::canWrite(bool block) {
  return waitTransfer(true, block).status == Status::Ready;
}

uint64_t PipeBuffer::readableGeneration() const {
  return m_ReadGeneration.value();
}

uint64_t PipeBuffer::writableGeneration() const {
  return m_WriteGeneration.value();
}

size_t PipeBuffer::getDataSize() {
  TerminationDeferral termination;
  lock(m_Lock);
  const size_t size = m_Size;
  m_Lock.release();
  return size;
}

void PipeBuffer::disableReads() {
  ActiveOperation operation(*this);
  if (operation) {
    lock(m_Lock);
    m_ReadEnabled = false;
    changedLocked();
    m_Lock.release();
  }
}

void PipeBuffer::disableWrites() {
  ActiveOperation operation(*this);
  if (operation) {
    lock(m_Lock);
    m_WriteEnabled = false;
    changedLocked();
    m_Lock.release();
  }
}

bool PipeBuffer::enableReads() {
  ActiveOperation operation(*this);
  if (!operation) {
    return false;
  }
  lock(m_Lock);
  const bool previous = m_ReadEnabled;
  m_ReadEnabled = true;
  changedLocked();
  m_Lock.release();
  return previous;
}

bool PipeBuffer::enableWrites(bool resetIfPreviouslyDisabled) {
  ActiveOperation operation(*this);
  if (!operation) {
    return false;
  }
  lock(m_Lock);
  const bool previous = m_WriteEnabled;
  m_WriteEnabled = true;
  if (!previous && resetIfPreviouslyDisabled) {
    resetLocked();
  }
  changedLocked();
  m_Lock.release();
  return previous;
}

void PipeBuffer::wipe() {
  ActiveOperation operation(*this);
  if (operation) {
    lock(m_Lock);
    resetLocked();
    changedLocked();
    m_Lock.release();
  }
}

void PipeBuffer::close() {
  TerminationDeferral termination;
  lock(m_Lock);
  m_Closing = true;
  m_ReadEnabled = false;
  m_WriteEnabled = false;
  changedLocked();
  m_Lock.release();
}
