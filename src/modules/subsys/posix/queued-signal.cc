/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/List.h"

#include <signal.h>

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "linux-wait-abi.h"
#include "queued-signal.h"

namespace {
struct UserQuota {
  int64_t uid;
  size_t count;
};
Mutex quotaLock;
List<UserQuota*, 0> quotas;
constexpr size_t PendingLimit = 16;
constexpr uint64_t Unblockable = (uint64_t(1) << (SIGKILL - 1)) | (uint64_t(1) << (SIGSTOP - 1));

PosixSubsystem* subsystem(Process* process) {
  return process && process->getType() == Process::Posix
             ? static_cast<PosixSubsystem*>(process->getSubsystem())
             : nullptr;
}

template <class T>
T field(const LinuxQueuedSiginfo& info, size_t offset) {
  T result;
  MemoryCopy(&result, info.bytes + offset, sizeof(result));
  return result;
}
template <class T>
void field(LinuxQueuedSiginfo& info, size_t offset, T value) {
  MemoryCopy(info.bytes + offset, &value, sizeof(value));
}

void cancelSource(Process* process, const void* source) {
  for (size_t i = process->getNumThreads(); i > 0; --i) {
    Process::ThreadLease thread;
    if (process->acquireThread(thread, i - 1))
      thread->cullSignalSource(source);
  }
}
}  // namespace

class SignalQueueReservation {
 public:
  explicit SignalQueueReservation(UserQuota* quota) : m_Quota(quota) {}
  ~SignalQueueReservation() {
    // Event completion also runs with interrupts disabled. Retire only the
    // count here; an admission in normal thread context reclaims idle nodes.
    const size_t previous = __atomic_fetch_sub(&m_Quota->count, size_t(1), __ATOMIC_ACQ_REL);
    assert(previous);
  }
  static SharedPointer<SignalQueueReservation> reserve(Process* process) {
    LockGuard<Mutex> guard(quotaLock);
    const int64_t uid = process->getUserId();
    UserQuota* quota = nullptr;
    for (auto it = quotas.begin(); it != quotas.end();) {
      UserQuota* candidate = *it;
      if (candidate->uid == uid) {
        quota = candidate;
        ++it;
      } else if (!__atomic_load_n(&candidate->count, __ATOMIC_ACQUIRE)) {
        it = quotas.erase(it);
        delete candidate;
      } else {
        ++it;
      }
    }
    if (!quota) {
      quota = new UserQuota{uid, 0};
      quotas.pushBack(quota);
    }
    if (__atomic_load_n(&quota->count, __ATOMIC_ACQUIRE) >= PendingLimit) {
      SYSCALL_ERROR(NoMoreProcesses);
      return SharedPointer<SignalQueueReservation>();
    }
    __atomic_add_fetch(&quota->count, size_t(1), __ATOMIC_ACQ_REL);
    return SharedPointer<SignalQueueReservation>(new SignalQueueReservation(quota));
  }

 private:
  UserQuota* m_Quota;
};

class QueuedSignalState : public SignalEventState {
 public:
  explicit QueuedSignalState(const SharedPointer<SignalQueueReservation>& reservation)
      : m_Reservation(reservation), m_Generation(0), m_Done(false) {}
  explicit QueuedSignalState(const SharedPointer<PosixTimerSignalToken>& token)
      : m_Token(token), m_Generation(0), m_Done(false) {
    LockGuard<Spinlock> guard(token->m_Lock);
    m_Generation = token->m_Generation;
  }
  bool active() const override {
    if (!m_Token)
      return true;
    LockGuard<Spinlock> guard(m_Token->m_Lock);
    return m_Token->m_Active && m_Token->m_Generation == m_Generation;
  }
  bool timer() const override {
    return bool(m_Token);
  }
  const void* source() const override {
    return m_Token.get();
  }
  void timerInfo(int32_t& id, int32_t& overrun) const override {
    if (!m_Token)
      return;
    LockGuard<Spinlock> guard(m_Token->m_Lock);
    id = m_Token->timerId;
    const uint64_t count = m_Token->m_Expirations;
    overrun = count > 0x80000000ULL ? 0x7fffffff : (count ? count - 1 : 0);
  }
  void complete(bool delivered, int32_t overrun) override {
    {
      LockGuard<Spinlock> guard(m_Lock);
      if (m_Done)
        return;
      m_Done = true;
    }
    if (!m_Token) {
      m_Reservation.reset();
      return;
    }
    LockGuard<Spinlock> guard(m_Token->m_Lock);
    if (!m_Token->m_Active || m_Token->m_Generation != m_Generation)
      return;
    if (delivered) {
      const uint64_t consumed = uint64_t(overrun) + 1;
      m_Token->m_Expirations =
          m_Token->m_Expirations > consumed ? m_Token->m_Expirations - consumed : 0;
      m_Token->m_DeliveredOverrun = overrun;
    } else if (overrun >= 0) {
      // Ignored or discarded notifications retire their accumulated periods.
      m_Token->m_Expirations = 0;
    }
    m_Token->m_Pending = false;
  }

 private:
  SharedPointer<SignalQueueReservation> m_Reservation;
  SharedPointer<PosixTimerSignalToken> m_Token;
  uint64_t m_Generation;
  Spinlock m_Lock;
  bool m_Done;
};

SharedPointer<SignalEventState> posix_signal_reserve_queue(Process* process) {
  auto reservation = SignalQueueReservation::reserve(process);
  return reservation ? SharedPointer<SignalEventState>(new QueuedSignalState(reservation))
                     : SharedPointer<SignalEventState>();
}

PosixTimerSignalToken::PosixTimerSignalToken()
    : timerId(0),
      m_Active(true),
      m_Pending(false),
      m_Generation(1),
      m_Expirations(0),
      m_DeliveredOverrun(0) {}
PosixTimerSignalToken::~PosixTimerSignalToken() = default;
bool PosixTimerSignalToken::addExpirations(uint64_t count) {
  LockGuard<Spinlock> guard(m_Lock);
  if (!m_Active)
    return false;
  m_Expirations = count > ~uint64_t(0) - m_Expirations ? ~uint64_t(0) : m_Expirations + count;
  if (!m_Expirations || m_Pending)
    return false;
  m_Pending = true;
  return true;
}
void PosixTimerSignalToken::queueFailed() {
  LockGuard<Spinlock> guard(m_Lock);
  m_Pending = false;
}
int PosixTimerSignalToken::getDeliveredOverrun() {
  LockGuard<Spinlock> guard(m_Lock);
  return m_DeliveredOverrun;
}
bool posix_signal_reserve_timer(Process* process, SharedPointer<PosixTimerSignalToken>& token) {
  auto reservation = SignalQueueReservation::reserve(process);
  if (!reservation)
    return false;
  token.reset(new PosixTimerSignalToken);
  token->m_Reservation = reservation;
  return true;
}
int posix_signal_queue_timer(Process* process, Thread* target, int signal, uint64_t value,
                             const SharedPointer<PosixTimerSignalToken>& token) {
  auto* owner = subsystem(process);
  Process::ThreadLease selected;
  const bool processDirected = !target;
  if (!owner || (!target && !process->acquireProcessSignalThread(selected)))
    return -1;
  if (!target)
    target = selected.get();
  SharedPointer<SignalEventState> state(new QueuedSignalState(token));
  const auto result =
      owner->queueSignalDelivery(target, signal, nullptr, -2, processDirected, value, state);
  if (result == PosixSubsystem::SignalDeliveryResult::Ignored)
    state->complete(false, 0);
  return result == PosixSubsystem::SignalDeliveryResult::Queued ||
                 result == PosixSubsystem::SignalDeliveryResult::Ignored
             ? 0
             : -1;
}
void posix_signal_reset_timer(Process* process, const SharedPointer<PosixTimerSignalToken>& token) {
  auto* owner = subsystem(process);
  if (!owner || !token)
    return;
  PendingSignalNotification notification(owner->pendingSignalContext());
  LockGuard<Mutex> pendingGuard(owner->pendingSignalLock());
  {
    LockGuard<Spinlock> guard(token->m_Lock);
    ++token->m_Generation;
    token->m_Expirations = 0;
    token->m_DeliveredOverrun = 0;
    token->m_Pending = false;
  }
  cancelSource(process, token.get());
  owner->pendingSignalContext()->recordChange();
}
void posix_signal_cancel_timer(Process* process,
                               const SharedPointer<PosixTimerSignalToken>& token) {
  auto* owner = subsystem(process);
  if (!owner || !token)
    return;
  PendingSignalNotification notification(owner->pendingSignalContext());
  LockGuard<Mutex> pendingGuard(owner->pendingSignalLock());
  {
    LockGuard<Spinlock> guard(token->m_Lock);
    token->m_Active = false;
    ++token->m_Generation;
    token->m_Expirations = 0;
    token->m_Pending = false;
  }
  cancelSource(process, token.get());
  token->m_Reservation.reset();
  owner->pendingSignalContext()->recordChange();
}

int posix_rt_sigpending(uint64_t* signals, size_t size) {
  if (size != sizeof(uint64_t)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  Thread* current = Processor::information().getCurrentThread();
  Process* process = current->getParent();
  auto* owner = subsystem(process);
  LockGuard<Mutex> guard(owner->pendingSignalLock());
  uint64_t mask = current->pendingSignalMask();
  for (size_t i = process->getNumThreads(); i > 0; --i) {
    Process::ThreadLease thread;
    if (process->acquireThread(thread, i - 1) && thread.get() != current)
      mask |= thread->pendingSignalMask(true);
  }
  mask &= current->getSignalMask();
  if (!PosixSubsystem::copyToUser(signals, &mask, sizeof(mask))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}

int posix_rt_sigtimedwait(const uint64_t* signals, LinuxQueuedSiginfo* info,
                          const LinuxKernelTimespec* timeout, size_t size) {
  TerminationDeferral termination;
  if (size != sizeof(uint64_t)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  uint64_t mask = 0;
  LinuxKernelTimespec requested = {};
  if (!PosixSubsystem::copyFromUser(&mask, signals, sizeof(mask)) ||
      (timeout && !PosixSubsystem::copyFromUser(&requested, timeout, sizeof(requested)))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (timeout &&
      (requested.tv_sec < 0 || requested.tv_nsec < 0 || requested.tv_nsec >= 1000000000)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  Time::Timestamp remaining = Time::Infinity;
  if (timeout) {
    const uint64_t maximum = Time::Infinity - 1;
    remaining = uint64_t(requested.tv_sec) > (maximum - requested.tv_nsec) / 1000000000
                    ? maximum
                    : uint64_t(requested.tv_sec) * 1000000000 + requested.tv_nsec;
  }
  mask &= ~Unblockable;
  Thread* current = Processor::information().getCurrentThread();
  Process* process = current->getParent();
  auto* owner = subsystem(process);
  PendingSignalNotification notification(owner->pendingSignalContext());
  LockGuard<Mutex> guard(owner->pendingSignalLock());
  current->setSynchronousSignalMask(mask);
  struct Enrollment {
    Thread* thread;
    ~Enrollment() {
      thread->setSynchronousSignalMask(0);
    }
  } enrollment{current};
  while (true) {
    PendingSignalReservation reservation;
    if (reservation.reserve(current, mask)) {
      const PendingSignalRecord record = reservation.record();
      LinuxQueuedSiginfo result;
      posix_signal_record_siginfo(record, result);
      if (info && !PosixSubsystem::copyToUser(info, &result, sizeof(result))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      reservation.commit(record.overrun);
      return record.number;
    }
    if (!remaining) {
      SYSCALL_ERROR(NoMoreProcesses);
      return -1;
    }
    if (current->hasEvents()) {
      SYSCALL_ERROR(Interrupted);
      return -1;
    }
    ConditionVariable::Error error = ConditionVariable::NoError;
    if (!owner->pendingSignalChanged().wait(owner->pendingSignalLock(), remaining, error)) {
      if (!ConditionVariable::mutexAcquired(error))
        guard.disown();
      if (error == ConditionVariable::TimedOut)
        SYSCALL_ERROR(NoMoreProcesses);
      else
        SYSCALL_ERROR(Interrupted);
      return -1;
    }
  }
}

int posix_rt_sigqueueinfo(int pid, int signal, const LinuxQueuedSiginfo* info) {
  TerminationDeferral termination;
  if (pid <= 0 || signal <= 0 || signal > 64) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  LinuxQueuedSiginfo supplied = {};
  if (!PosixSubsystem::copyFromUser(&supplied, info, sizeof(supplied))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  // The public sigqueue protocol carries SI_QUEUE. Kernel-generated codes
  // and timer identity are always constructed from trusted kernel state.
  if (field<int32_t>(supplied, 8) != -1) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  Scheduler::ProcessLease target;
  if (!Scheduler::instance().acquireProcessById(target, pid) || !subsystem(target.get())) {
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }
  auto* caller =
      static_cast<PosixProcess*>(Processor::information().getCurrentThread()->getParent());
  auto* recipient = static_cast<PosixProcess*>(target.get());
  const int64_t real = caller->getUserId(), effective = caller->getEffectiveUserId();
  const int64_t targetReal = recipient->getUserId(), saved = recipient->getSavedUserId();
  if (effective != 0 &&
      !((real >= 0 && (real == targetReal || real == saved)) ||
        (effective >= 0 && (effective == targetReal || effective == saved))) &&
      !(signal == SIGCONT && caller->getSession() &&
        caller->getSession() == recipient->getSession())) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  Process::ThreadLease thread;
  if (!recipient->acquireProcessSignalThread(thread)) {
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }
  const auto result = subsystem(recipient)->queueSignalDelivery(
      thread.get(), signal, nullptr, -1, true, field<uint64_t>(supplied, 24));
  if (result == PosixSubsystem::SignalDeliveryResult::Full)
    return -1;
  if (result == PosixSubsystem::SignalDeliveryResult::Unavailable ||
      result == PosixSubsystem::SignalDeliveryResult::Rejected) {
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }
  return 0;
}
