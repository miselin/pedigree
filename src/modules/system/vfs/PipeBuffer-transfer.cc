/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/utilities/assert.h"

#include "PipeBuffer.h"

PipeBuffer::ReadReservation::ReadReservation() = default;

PipeBuffer::ReadReservation::~ReadReservation() {
  cancel();
}

size_t PipeBuffer::ReadReservation::size() const {
  return m_Size;
}

void PipeBuffer::ReadReservation::copyTo(uint8_t* destination, size_t count) const {
  assert(count <= m_Size);
  if (!count) {
    return;
  }
  assert(m_Buffer);
  PipeBuffer::lock(m_Buffer->m_Lock);
  m_Buffer->copyOutLocked(destination, count);
  m_Buffer->m_Lock.release();
}

void PipeBuffer::ReadReservation::consume(size_t accepted) {
  assert(accepted <= m_Size);
  PipeBuffer* buffer = m_Buffer;
  if (!buffer) {
    return;
  }
  PipeBuffer::lock(buffer->m_Lock);
  assert(buffer->m_ReadReserved);
  buffer->consumeLocked(accepted);
  buffer->m_ReadReserved = false;
  buffer->finishResetLocked();
  buffer->changedLocked();
  m_Buffer = nullptr;
  m_Size = 0;
  buffer->m_Lock.release();
  buffer->publish();
  buffer->endOperation();
}

void PipeBuffer::ReadReservation::cancel() {
  consume(0);
}

PipeBuffer::WriteReservation::WriteReservation() = default;

PipeBuffer::WriteReservation::~WriteReservation() {
  cancel();
}

size_t PipeBuffer::WriteReservation::size() const {
  return m_Size;
}

PipeBuffer::Result PipeBuffer::WriteReservation::commit(const uint8_t* source, size_t accepted) {
  PipeBuffer* buffer = m_Buffer;
  if (!buffer) {
    return {Status::Invalid, 0};
  }
  if (accepted > m_Size) {
    cancel();
    return {Status::Invalid, 0};
  }
  PipeBuffer::lock(buffer->m_Lock);
  assert(buffer->m_WriteReserved);
  // A reopen/reset invalidates an old append admission before any source is consumed.
  const bool closed = buffer->m_Closing || !buffer->m_ReadEnabled || !buffer->m_WriteEnabled ||
                      buffer->m_ResetPending;
  if (!closed) {
    buffer->appendLocked(source, accepted);
  }
  buffer->m_WriteReserved = false;
  buffer->finishResetLocked();
  buffer->changedLocked();
  m_Buffer = nullptr;
  m_Size = 0;
  buffer->m_Lock.release();
  buffer->publish();
  buffer->endOperation();
  return {closed ? Status::Closed : Status::Ready, closed ? 0 : accepted};
}

void PipeBuffer::WriteReservation::cancel() {
  PipeBuffer* buffer = m_Buffer;
  if (!buffer) {
    return;
  }
  PipeBuffer::lock(buffer->m_Lock);
  assert(buffer->m_WriteReserved);
  buffer->m_WriteReserved = false;
  buffer->finishResetLocked();
  buffer->changedLocked();
  m_Buffer = nullptr;
  m_Size = 0;
  buffer->m_Lock.release();
  buffer->publish();
  buffer->endOperation();
}

PipeBuffer::Result PipeBuffer::reserveRead(size_t maximum, bool block,
                                           ReadReservation& reservation) {
  if (reservation.m_Buffer) {
    return {Status::Invalid, 0};
  }
  if (!maximum) {
    return {Status::Ready, 0};
  }
  ActiveOperation operation(*this);
  if (!operation) {
    return {Status::Eof, 0};
  }
  lock(m_Lock);
  Result result = waitLocked(false, 1, block);
  if (result.status == Status::Ready) {
    result.count = maximum < result.count ? maximum : result.count;
    reservation.m_Buffer = this;
    reservation.m_Size = result.count;
    m_ReadReserved = true;
    changedLocked();
    operation.detach();
  }
  m_Lock.release();
  if (result.status == Status::Ready) {
    publish();
  }
  return result;
}

PipeBuffer::Result PipeBuffer::reserveWrite(size_t maximum, bool block,
                                            WriteReservation& reservation) {
  if (reservation.m_Buffer) {
    return {Status::Invalid, 0};
  }
  if (!maximum) {
    return {Status::Ready, 0};
  }
  ActiveOperation operation(*this);
  if (!operation) {
    return {Status::Closed, 0};
  }
  lock(m_Lock);
  Result result = waitLocked(true, 1, block);
  if (result.status == Status::Ready) {
    result.count = maximum < result.count ? maximum : result.count;
    reservation.m_Buffer = this;
    reservation.m_Size = result.count;
    m_WriteReserved = true;
    changedLocked();
    operation.detach();
  }
  m_Lock.release();
  if (result.status == Status::Ready) {
    publish();
  }
  return result;
}

struct PipeBuffer::PairWaiter {
  Mutex mutex;
  ConditionVariable condition;
  bool pending = false;
};

struct PipeBuffer::PairLink {
  PairWaiter* waiter;
  PairLink* next = nullptr;
};

void PipeBuffer::linkPairLocked(PairLink& link) {
  link.next = m_PairWaiters;
  m_PairWaiters = &link;
}

void PipeBuffer::unlinkPairLocked(PairLink& link) {
  PairLink** position = &m_PairWaiters;
  while (*position && *position != &link) {
    position = &(*position)->next;
  }
  assert(*position == &link);
  *position = link.next;
  link.next = nullptr;
}

void PipeBuffer::wakePairsLocked() {
  for (PairLink* link = m_PairWaiters; link; link = link->next) {
    lock(link->waiter->mutex);
    link->waiter->pending = true;
    link->waiter->condition.signal();
    link->waiter->mutex.release();
  }
}

PipeBuffer::Result PipeBuffer::transferTo(PipeBuffer& output, size_t maximum, bool consume,
                                          bool block) {
  if (this == &output) {
    return {Status::Invalid, 0};
  }
  if (!maximum) {
    return {Status::Ready, 0};
  }
  ActiveOperation inputOperation(*this);
  ActiveOperation outputOperation(output);
  if (!inputOperation) {
    return {Status::Eof, 0};
  }
  if (!outputOperation) {
    return {Status::Closed, 0};
  }
  PipeBuffer* first =
      reinterpret_cast<uintptr_t>(this) < reinterpret_cast<uintptr_t>(&output) ? this : &output;
  PipeBuffer* second = first == this ? &output : this;
  PairWaiter waiter;
  PairLink inputLink{&waiter};
  PairLink outputLink{&waiter};

  lock(first->m_Lock);
  lock(second->m_Lock);
  Result result{Status::WouldBlock, 0};
  for (;;) {
    const Result input = readyLocked(false);
    const Result destination = output.readyLocked(true);
    if (destination.status == Status::Closed || input.status == Status::Eof) {
      result = destination.status == Status::Closed ? destination : input;
      break;
    }
    if (input.status == Status::Ready && destination.status == Status::Ready) {
      size_t accepted = maximum < input.count ? maximum : input.count;
      accepted = accepted < destination.count ? accepted : destination.count;
      const size_t tail = (output.m_Head + output.m_Size) % Capacity;
      for (size_t i = 0; i < accepted; ++i) {
        output.m_Data[(tail + i) % Capacity] = m_Data[(m_Head + i) % Capacity];
      }
      output.m_Size += accepted;
      output.changedLocked();
      if (consume) {
        consumeLocked(accepted);
        changedLocked();
      }
      result = {Status::Ready, accepted};
      break;
    }
    if (!block) {
      break;
    }
    if (interrupted()) {
      result = {Status::Interrupted, 0};
      break;
    }
    linkPairLocked(inputLink);
    output.linkPairLocked(outputLink);
    second->m_Lock.release();
    first->m_Lock.release();

    // The predicate and enrollment share the pair locks. Sticky notification
    // bridges their release and this wait without an inverse waiter→buffer lock.
    lock(waiter.mutex);
    bool success = true;
    while (!waiter.pending && success) {
      success = !interrupted() && wait(waiter.condition, waiter.mutex);
    }
    waiter.pending = false;
    waiter.mutex.release();
    lock(first->m_Lock);
    lock(second->m_Lock);
    unlinkPairLocked(inputLink);
    output.unlinkPairLocked(outputLink);
    if (!success) {
      result = {Status::Interrupted, 0};
      break;
    }
  }
  second->m_Lock.release();
  first->m_Lock.release();
  if (result.count) {
    output.publish();
    if (consume) {
      publish();
    }
  }
  return result;
}
