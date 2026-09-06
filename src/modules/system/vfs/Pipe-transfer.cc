/* Copyright (c) 2026, Pedigree Developers. */
#include "Pipe.h"

namespace {
class PipeStoragePin {
 public:
  explicit PipeStoragePin(Pipe& pipe) : m_Pipe(pipe.retainVfsReference() ? &pipe : nullptr) {}
  ~PipeStoragePin() {
    if (m_Pipe) {
      m_Pipe->releaseVfsReference();
    }
  }
  explicit operator bool() const {
    return m_Pipe != nullptr;
  }

 private:
  PipeStoragePin(const PipeStoragePin&) = delete;
  PipeStoragePin& operator=(const PipeStoragePin&) = delete;
  TerminationDeferral m_Termination;
  Pipe* m_Pipe;
};
}  // namespace

Pipe::ReadReservation::~ReadReservation() {
  cancel();
}

size_t Pipe::ReadReservation::size() const {
  return m_Reservation.size();
}

void Pipe::ReadReservation::copyTo(uint8_t* destination, size_t count) const {
  m_Reservation.copyTo(destination, count);
}

void Pipe::ReadReservation::consume(size_t accepted) {
  m_Reservation.consume(accepted);
  if (m_Pipe) {
    Pipe* pipe = m_Pipe;
    m_Pipe = nullptr;
    pipe->releaseVfsReference();
  }
}

void Pipe::ReadReservation::cancel() {
  consume(0);
}

Pipe::WriteReservation::~WriteReservation() {
  cancel();
}

size_t Pipe::WriteReservation::size() const {
  return m_Reservation.size();
}

PipeBuffer::Result Pipe::WriteReservation::commit(const uint8_t* source, size_t accepted) {
  const auto result = m_Reservation.commit(source, accepted);
  if (m_Pipe) {
    Pipe* pipe = m_Pipe;
    m_Pipe = nullptr;
    if (result.count) {
      pipe->publishEvent(FileEvents::Modify);
    }
    pipe->releaseVfsReference();
  }
  return result;
}

void Pipe::WriteReservation::cancel() {
  m_Reservation.cancel();
  if (m_Pipe) {
    Pipe* pipe = m_Pipe;
    m_Pipe = nullptr;
    pipe->releaseVfsReference();
  }
}

PipeBuffer::Result Pipe::reserveRead(size_t maximum, bool block, ReadReservation& reservation) {
  if (reservation.m_Pipe) {
    return {PipeBuffer::Status::Invalid, 0};
  }
  if (!retainVfsReference()) {
    return {PipeBuffer::Status::Closed, 0};
  }
  const auto result = m_Buffer.reserveRead(maximum, block, reservation.m_Reservation);
  if (result.status == PipeBuffer::Status::Ready && result.count) {
    reservation.m_Pipe = this;
  } else {
    releaseVfsReference();
  }
  return result;
}

PipeBuffer::Result Pipe::reserveWrite(size_t maximum, bool block, WriteReservation& reservation) {
  if (reservation.m_Pipe) {
    return {PipeBuffer::Status::Invalid, 0};
  }
  if (!retainVfsReference()) {
    return {PipeBuffer::Status::Closed, 0};
  }
  const auto result = m_Buffer.reserveWrite(maximum, block, reservation.m_Reservation);
  if (result.status == PipeBuffer::Status::Ready && result.count) {
    reservation.m_Pipe = this;
  } else {
    releaseVfsReference();
  }
  return result;
}

PipeBuffer::Result Pipe::waitTransfer(bool writing, bool block) {
  PipeStoragePin pin(*this);
  return pin ? m_Buffer.waitTransfer(writing, block)
             : PipeBuffer::Result{PipeBuffer::Status::Closed, 0};
}

PipeBuffer::Result Pipe::transferTo(Pipe& output, size_t maximum, bool consume, bool block) {
  PipeStoragePin inputPin(*this);
  PipeStoragePin outputPin(output);
  if (!inputPin || !outputPin) {
    return {PipeBuffer::Status::Closed, 0};
  }
  const auto result = m_Buffer.transferTo(output.m_Buffer, maximum, consume, block);
  if (result.count) {
    output.publishEvent(FileEvents::Modify);
  }
  return result;
}
