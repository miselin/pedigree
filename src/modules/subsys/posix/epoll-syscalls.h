/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef EPOLL_SYSCALLS_H
#define EPOLL_SYSCALLS_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/processor/types.h"

/** The packed event layout used by the Linux x86-64 syscall ABI. */
struct LinuxEpollEvent {
  uint32_t events;
  uint64_t data;
} PACKED;

static_assert(sizeof(LinuxEpollEvent) == 12, "Linux x86-64 epoll_event must be packed");

namespace LinuxEpoll {
constexpr uint32_t In = 0x00000001U;
constexpr uint32_t Priority = 0x00000002U;
constexpr uint32_t Out = 0x00000004U;
constexpr uint32_t Error = 0x00000008U;
constexpr uint32_t Hangup = 0x00000010U;
constexpr uint32_t Invalid = 0x00000020U;
constexpr uint32_t ReadNormal = 0x00000040U;
constexpr uint32_t ReadBand = 0x00000080U;
constexpr uint32_t WriteNormal = 0x00000100U;
constexpr uint32_t WriteBand = 0x00000200U;
constexpr uint32_t Message = 0x00000400U;
constexpr uint32_t ReadHangup = 0x00002000U;
constexpr uint32_t Exclusive = 1U << 28;
constexpr uint32_t Wakeup = 1U << 29;
constexpr uint32_t OneShot = 1U << 30;
constexpr uint32_t EdgeTriggered = 1U << 31;

constexpr int ControlAdd = 1;
constexpr int ControlDelete = 2;
constexpr int ControlModify = 3;

// Linux defines EPOLL_CLOEXEC as O_CLOEXEC. Keep the raw ABI value here so a
// hosted build's fcntl constants cannot accidentally alter the guest ABI.
constexpr int CloseOnExec = 0x00080000;
}  // namespace LinuxEpoll

class EpollState;
class EpollReadinessObserver;

/** One open epoll object, shared by dup aliases of its descriptor. */
class EXPORTED_PUBLIC EpollInstance final : public ReadinessSource {
 public:
  EpollInstance();
  ~EpollInstance() override;

  /** event is a kernel-owned snapshot for ADD and MOD, and null for DEL. */
  int control(int operation, int targetFd, const LinuxEpollEvent* event);

  /** events is kernel-owned storage and is never retained after this call. */
  int wait(LinuxEpollEvent* events, int maxEvents, int timeoutMilliseconds);

  /** Exposes the epoll object's current level to poll/select infrastructure. */
  ReadyMask queryReady();

 private:
  friend class EpollReadinessObserver;

  EpollInstance(const EpollInstance&) = delete;
  EpollInstance& operator=(const EpollInstance&) = delete;

  int collectEvents(LinuxEpollEvent* events, int maxEvents, bool consumeOneShot);
  void wakeWaiter();
  void sourceReadinessChanged(ReadyMask mask);

  EpollState* m_State;
};

int posix_epoll_create1(int flags);
int posix_epoll_create(int size);
int posix_epoll_ctl(int epollFd, int operation, int targetFd, const LinuxEpollEvent* event);
int posix_epoll_wait(int epollFd, LinuxEpollEvent* events, int maxEvents, int timeoutMilliseconds);
int posix_epoll_pwait(int epollFd, LinuxEpollEvent* events, int maxEvents, int timeoutMilliseconds,
                      const void* signalMask, size_t signalMaskSize);

#endif
