/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_QUEUED_SIGNAL_H
#define POSIX_QUEUED_SIGNAL_H

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
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

struct PendingSignalRecord {
  int32_t number = 0, code = 0, pid = 0;
  uint32_t uid = 0;
  uint64_t value = 0;
  int32_t timerId = 0, overrun = 0, status = 0;
  uint64_t userTime = 0, systemTime = 0;
};

class PendingSignalBinding {
 public:
  Thread* thread = nullptr;
  bool alive = true;
  uint64_t generations[64] = {};
};

/** Durable notification state; bindings never retain a Process or Thread lease. */
class PendingSignalContext final : public ReadinessSource {
 public:
  void attach(Process* process);
  void close();
  void retireThread(Thread* thread);
  SharedPointer<PendingSignalBinding> bind(Thread* thread);
  ReadyMask query(const SharedPointer<PendingSignalBinding>& binding, uint64_t mask,
                  ReadinessGenerations* generations = nullptr);
  void recordChange(size_t signal = 0, Thread* target = nullptr, bool processDirected = false);
  void publish();
  void wake();
  uint64_t version() const {
    return __atomic_load_n(&m_Version, __ATOMIC_ACQUIRE);
  }
  Mutex lock;
  ConditionVariable changed;

 private:
  Process* m_Process = nullptr;
  bool m_Closed = false;
  uint64_t m_Version = 0;
  uint64_t m_ProcessGenerations[64] = {};
  List<SharedPointer<PendingSignalBinding>, 0> m_Bindings;
};

/** Declare before the pending lock guard so callbacks run only after unlock. */
class PendingSignalNotification {
 public:
  explicit PendingSignalNotification(const SharedPointer<PendingSignalContext>& context)
      : m_Context(context), m_Version(0) {
    PendingSignalContext* retained = m_Context.get();
    if (!retained) {
      FATAL("Pending signal notification has no context.");
      return;
    }
    m_Version = retained->version();
  }
  ~PendingSignalNotification() {
    PendingSignalContext* retained = m_Context.get();
    if (!retained) {
      FATAL("Pending signal notification lost its context.");
      return;
    }
    if (retained->version() != m_Version)
      retained->publish();
  }

 private:
  SharedPointer<PendingSignalContext> m_Context;
  uint64_t m_Version;
};

/** Caller holds its process pending lock until commit or rollback completes. */
class PendingSignalReservation {
 public:
  PendingSignalReservation() = default;
  ~PendingSignalReservation();
  bool reserve(Thread* caller, uint64_t mask);
  PendingSignalRecord record() const;
  void commit(int32_t overrun);
  void rollback();

 private:
  PendingSignalReservation(const PendingSignalReservation&) = delete;
  PendingSignalReservation& operator=(const PendingSignalReservation&) = delete;
  Thread* m_Caller = nullptr;
  Process::ThreadLease m_Selected;
  Event::Delivery m_Delivery;
};

bool posix_matching_pending(Thread* caller, uint64_t mask);
void posix_signal_record_siginfo(const PendingSignalRecord&, LinuxQueuedSiginfo&);

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
