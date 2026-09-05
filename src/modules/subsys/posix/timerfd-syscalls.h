/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_TIMERFD_SYSCALLS_H
#define POSIX_TIMERFD_SYSCALLS_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

#include "descriptor-read.h"
#include "posix-timer-state.h"

struct PosixClockSnapshot;
class TimerFdService;

namespace LinuxTimerFd {
constexpr int NonBlock = 0x00000800;
constexpr int CloseOnExec = 0x00080000;
constexpr int Absolute = 1;
constexpr int CancelOnSet = 2;
constexpr size_t MaximumObjects = 256;
}  // namespace LinuxTimerFd

/** Settings and unread expirations belong to the shared open description. */
class EXPORTED_PUBLIC TimerFd final : public ReadinessSource {
 public:
  explicit TimerFd(int clock);
  ~TimerFd() override;

  int readToUser(void* buffer, size_t count, bool canBlock);
  int readWithCopy(size_t count, bool canBlock, PosixDescriptorReadCopy copy, void* context);
  ReadyMask queryReady();
  ReadinessGenerations readinessGenerations() override;
  MUST_USE_RESULT bool addDescriptorOwner();
  void removeDescriptorOwner();

 private:
  friend class TimerFdService;
  friend int posix_timerfd_settime(int, int, const void*, void*);
  friend int posix_timerfd_gettime(int, void*);

  TimerFd(const TimerFd&) = delete;
  TimerFd& operator=(const TimerFd&) = delete;

  bool readable() const;
  Time::Timestamp now(const PosixClockSnapshot& clock) const;
  void update(const PosixClockSnapshot& clock);
  uint64_t forward(PosixTimerState::State& state, Time::Timestamp current) const;
  void service(uint64_t generation);
  void changed();
  int configure(int flags, Time::Timestamp value, Time::Timestamp interval, void* previous);
  int getTime(void* value);

  Mutex m_Lock;
  ConditionVariable m_Readers;
  PosixTimerState::State m_State;
  ReadinessGenerations m_Generations;
  uint64_t m_Counter;
  uint64_t m_ClockGeneration;
  size_t m_DescriptorOwners;
  int m_Clock;
  bool m_Expired;
  bool m_CancelOnSet;
  bool m_CancelPending;
  bool m_CancelWake;
  bool m_AdmissionOpen;
};

int posix_timerfd_create(int clock, int flags);
int posix_timerfd_settime(int fd, int flags, const void* newValue, void* oldValue);
int posix_timerfd_gettime(int fd, void* value);
void posix_timerfd_clock_changed(uint64_t generation);

#endif
