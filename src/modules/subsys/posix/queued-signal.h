/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_QUEUED_SIGNAL_H
#define POSIX_QUEUED_SIGNAL_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

class Process;
class Thread;
class SignalQueueReservation;
class QueuedSignalState;
class SignalEventState;
struct LinuxKernelTimespec;

struct alignas(8) LinuxQueuedSiginfo {
  uint8_t bytes[128];
};
static_assert(sizeof(LinuxQueuedSiginfo) == 128, "Linux amd64 siginfo layout");

class PosixTimerSignalToken {
 public:
  PosixTimerSignalToken();
  ~PosixTimerSignalToken();
  bool addExpirations(uint64_t count);
  void queueFailed();
  int getDeliveredOverrun();
  int32_t timerId;

 private:
  friend class QueuedSignalState;
  friend bool posix_signal_reserve_timer(Process*, SharedPointer<PosixTimerSignalToken>&);
  friend int posix_signal_queue_timer(Process*, Thread*, int, uint64_t,
                                      const SharedPointer<PosixTimerSignalToken>&);
  friend void posix_signal_reset_timer(Process*, const SharedPointer<PosixTimerSignalToken>&);
  friend void posix_signal_cancel_timer(Process*, const SharedPointer<PosixTimerSignalToken>&);
  Spinlock m_Lock;
  bool m_Active, m_Pending;
  uint64_t m_Generation, m_Expirations;
  int32_t m_DeliveredOverrun;
  SharedPointer<SignalQueueReservation> m_Reservation;
};

SharedPointer<SignalEventState> posix_signal_reserve_queue(Process*);
bool posix_signal_reserve_timer(Process*, SharedPointer<PosixTimerSignalToken>&);
int posix_signal_queue_timer(Process*, Thread* target, int signal, uint64_t value,
                             const SharedPointer<PosixTimerSignalToken>&);
void posix_signal_reset_timer(Process*, const SharedPointer<PosixTimerSignalToken>&);
void posix_signal_cancel_timer(Process*, const SharedPointer<PosixTimerSignalToken>&);

int posix_rt_sigpending(uint64_t* signals, size_t signalSetSize);
int posix_rt_sigtimedwait(const uint64_t* signals, LinuxQueuedSiginfo* info,
                          const LinuxKernelTimespec* timeout, size_t signalSetSize);
int posix_rt_sigqueueinfo(int pid, int signal, const LinuxQueuedSiginfo* info);

#endif
