/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef EVENTFD_SYSCALLS_H
#define EVENTFD_SYSCALLS_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/processor/types.h"

namespace LinuxEventFd {
constexpr int Semaphore = 0x00000001;
constexpr int NonBlock = 0x00000800;
constexpr int CloseOnExec = 0x00080000;
}  // namespace LinuxEventFd

/** A Linux eventfd counter shared by every alias of one open description. */
class EXPORTED_PUBLIC EventFd final : public ReadinessSource {
 public:
  EventFd(uint64_t initialValue, bool semaphoreMode);
  ~EventFd() override;

  /** Read one eventfd value, blocking while the counter is zero if allowed. */
  int readValue(uint64_t& value, bool canBlock);

  /** Add one eventfd value, blocking while it would overflow if allowed. */
  int writeValue(uint64_t value, bool canBlock);

  /** Return the current poll/epoll level. */
  ReadyMask queryReady();

  ReadinessGenerations readinessGenerations() override;

  /** Monotonically identifies every successful producer write for EPOLLET. */
  uint64_t writeGeneration() const;

  /** Track descriptor-table aliases independently of in-flight syscall pins. */
  MUST_USE_RESULT bool addDescriptorOwner();
  void removeDescriptorOwner();

 private:
  EventFd(const EventFd&) = delete;
  EventFd& operator=(const EventFd&) = delete;

  Mutex m_Lock;
  ConditionVariable m_Readers;
  ConditionVariable m_Writers;
  uint64_t m_Counter;
  Atomic<uint64_t> m_WriteGeneration;
  ReadinessGenerations m_Generations;
  size_t m_DescriptorOwners;
  bool m_SemaphoreMode;
  bool m_DescriptorAdmissionOpen;
};

int posix_eventfd(unsigned int initialValue);
int posix_eventfd2(unsigned int initialValue, int flags);

#endif
