/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "eventfd-syscalls.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/assert.h"

#include <fcntl.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"

namespace {
constexpr uint64_t MaximumCounter = ~static_cast<uint64_t>(0) - 1;

void setWaitError(ConditionVariable::Error error) {
  if (error == ConditionVariable::Interrupted || error == ConditionVariable::TerminationDeferred) {
    SYSCALL_ERROR(Interrupted);
  } else {
    SYSCALL_ERROR(BadFileDescriptor);
  }
}
}  // namespace

EventFd::EventFd(uint64_t initialValue, bool semaphoreMode)
    : ReadinessSource(),
      m_Lock(),
      m_Readers(),
      m_Writers(),
      m_Counter(initialValue),
      m_WriteGeneration(0),
      m_Generations(),
      m_DescriptorOwners(0),
      m_SemaphoreMode(semaphoreMode),
      m_DescriptorAdmissionOpen(true) {}

EventFd::~EventFd() {
  assert(!m_DescriptorOwners);
  closeReadiness();
}

int EventFd::readValue(uint64_t& value, bool canBlock) {
  m_Lock.acquire();
  while (!m_Counter) {
    if (!canBlock) {
      m_Lock.release();
      SYSCALL_ERROR(NoMoreProcesses);
      return -1;
    }

    ConditionVariable::Error error = ConditionVariable::NoError;
    if (!m_Readers.wait(m_Lock, error)) {
      if (ConditionVariable::mutexAcquired(error)) {
        m_Lock.release();
      }
      setWaitError(error);
      return -1;
    }
  }

  const bool wasWritable = m_Counter < MaximumCounter;
  if (m_SemaphoreMode) {
    value = 1;
    --m_Counter;
  } else {
    value = m_Counter;
    m_Counter = 0;
  }
  if (!wasWritable) {
    ++m_Generations.write;
  }
  m_Lock.release();

  m_Writers.broadcast();
  notifyReadiness(ReadyRead | ReadyWrite);
  return static_cast<int>(sizeof(value));
}

int EventFd::writeValue(uint64_t value, bool canBlock) {
  if (value == ~static_cast<uint64_t>(0)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  m_Lock.acquire();
  while (value > MaximumCounter - m_Counter) {
    if (!canBlock) {
      m_Lock.release();
      SYSCALL_ERROR(NoMoreProcesses);
      return -1;
    }

    ConditionVariable::Error error = ConditionVariable::NoError;
    if (!m_Writers.wait(m_Lock, error)) {
      if (ConditionVariable::mutexAcquired(error)) {
        m_Lock.release();
      }
      setWaitError(error);
      return -1;
    }
  }

  const bool wasReadable = m_Counter != 0;
  m_Counter += value;
  if (!wasReadable && m_Counter) {
    ++m_Generations.read;
  }
  m_WriteGeneration += 1;
  m_Lock.release();

  m_Readers.broadcast();
  notifyReadiness(ReadyRead | ReadyWrite);
  return static_cast<int>(sizeof(value));
}

ReadyMask EventFd::queryReady() {
  LockGuard<Mutex> guard(m_Lock);
  ReadyMask ready = ReadyNone;
  if (m_Counter) {
    ready |= ReadyRead;
  }
  if (m_Counter < MaximumCounter) {
    ready |= ReadyWrite;
  }
  return ready;
}

ReadinessGenerations EventFd::readinessGenerations() {
  LockGuard<Mutex> guard(m_Lock);
  return m_Generations;
}

uint64_t EventFd::writeGeneration() const {
  return m_WriteGeneration.value();
}

bool EventFd::addDescriptorOwner() {
  LockGuard<Mutex> guard(m_Lock);
  if (!m_DescriptorAdmissionOpen) {
    return false;
  }
  ++m_DescriptorOwners;
  return true;
}

void EventFd::removeDescriptorOwner() {
  LockGuard<Mutex> guard(m_Lock);
  assert(m_DescriptorOwners);
  --m_DescriptorOwners;
  if (!m_DescriptorOwners) {
    m_DescriptorAdmissionOpen = false;
  }
}

int posix_eventfd(unsigned int initialValue) {
  return posix_eventfd2(initialValue, 0);
}

int posix_eventfd2(unsigned int initialValue, int flags) {
  constexpr int AllowedFlags =
      LinuxEventFd::Semaphore | LinuxEventFd::NonBlock | LinuxEventFd::CloseOnExec;
  if (flags & ~AllowedFlags) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  const size_t fd = getAvailableDescriptor();
  const int descriptorFlags = flags & LinuxEventFd::CloseOnExec ? FD_CLOEXEC : 0;
  const int statusFlags = O_RDWR | (flags & LinuxEventFd::NonBlock ? O_NONBLOCK : 0);
  FileDescriptor* descriptor = new FileDescriptor(nullptr, 0, fd, descriptorFlags, statusFlags);
  descriptor->setEventFdImpl(
      SharedPointer<EventFd>(new EventFd(initialValue, flags & LinuxEventFd::Semaphore)));
  addDescriptor(static_cast<int>(fd), descriptor);
  return static_cast<int>(fd);
}
