/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/machine/TimerHandler.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/assert.h"

#include <fcntl.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "clock-syscalls.h"
#include "timerfd-syscalls.h"

namespace {
constexpr int ClockRealtime = 0, ClockMonotonic = 1;

uint64_t addExpirations(uint64_t counter, uint64_t added) {
  return added > ~uint64_t(0) - counter ? ~uint64_t(0) : counter + added;
}

bool copyRead(void* context, const void* value, size_t count) {
  return PosixSubsystem::copyToUser(context, value, count);
}
}  // namespace

class TimerFdService final : public TimerHandler {
 public:
  ~TimerFdService() override {
    if (m_Source && !m_Source->unregisterHandler(this))
      panic("timerfd callback could not be drained");
  }

  bool add(const SharedPointer<TimerFd>& timer) {
    LockGuard<Mutex> guard(m_Lock);
    size_t slot = LinuxTimerFd::MaximumObjects;
    for (size_t i = 0; i < LinuxTimerFd::MaximumObjects; ++i)
      if (!m_Timers[i]) {
        slot = i;
        break;
      }
    if (slot == LinuxTimerFd::MaximumObjects) {
      SYSCALL_ERROR(TooManyOpenFiles);
      return false;
    }
    if (!m_Source) {
      Timer* source = Machine::instance().getTimer();
      if (!source || !source->registerHandler(this)) {
        SYSCALL_ERROR(OutOfMemory);
        return false;
      }
      m_Source = source;
    }
    m_Timers[slot] = timer;
    if (slot >= m_End)
      m_End = slot + 1;
    return true;
  }

  void remove(TimerFd* timer) {
    SharedPointer<TimerFd> retired;
    {
      LockGuard<Mutex> guard(m_Lock);
      for (auto& entry : m_Timers)
        if (entry.get() == timer) {
          retired = pedigree_std::move(entry);
          break;
        }
      while (m_End && !m_Timers[m_End - 1])
        --m_End;
    }
  }

  void refresh(uint64_t generation) {
    TerminationDeferral lifetime;
    for (size_t i = 0; i < LinuxTimerFd::MaximumObjects; ++i) {
      SharedPointer<TimerFd> timer;
      if (generation)
        m_Lock.acquire();
      else if (!m_Lock.tryAcquire())
        return;
      if (i >= m_End) {
        m_Lock.release();
        return;
      }
      timer = m_Timers[i];
      m_Lock.release();
      // A callback pin outlives final descriptor close, but cannot keep the
      // descriptor admission open. Never notify observers under the registry.
      if (timer)
        timer->service(generation);
    }
  }

  void timer(uint64_t) override {
    if (!Processor::inDeviceHardIrq())
      refresh(0);
  }

 private:
  Mutex m_Lock;
  Timer* m_Source = nullptr;
  size_t m_End = 0;
  SharedPointer<TimerFd> m_Timers[LinuxTimerFd::MaximumObjects];
};

namespace {
TimerFdService timerService;
}  // namespace

TimerFd::TimerFd(int clock)
    : m_Lock(),
      m_Readers(),
      m_State(),
      m_Generations(),
      m_Counter(0),
      m_ClockGeneration(posix_clock_change_generation()),
      m_DescriptorOwners(0),
      m_Clock(clock),
      m_Expired(false),
      m_CancelOnSet(false),
      m_CancelPending(false),
      m_CancelWake(false),
      m_AdmissionOpen(true) {}

TimerFd::~TimerFd() {
  assert(!m_DescriptorOwners);
  closeReadiness();
}

bool TimerFd::readable() const {
  return m_Counter || m_CancelWake;
}

Time::Timestamp TimerFd::now(const PosixClockSnapshot& clock) const {
  return m_State.realtime ? clock.realtime : clock.monotonic;
}

void TimerFd::update(const PosixClockSnapshot& clock) {
  if (clock.generation > m_ClockGeneration) {
    m_ClockGeneration = clock.generation;
    if (m_CancelOnSet) {
      m_CancelPending = true;
      m_CancelWake = true;
      ++m_Generations.read;
    }
  }
  if (m_State.armed && now(clock) >= m_State.deadline) {
    m_State.armed = false;
    m_Expired = true;
    m_Counter = addExpirations(m_Counter, 1);
    ++m_Generations.read;
  }
}

uint64_t TimerFd::forward(PosixTimerState::State& state, Time::Timestamp current) const {
  if (!m_Expired || !state.interval)
    return m_Counter;
  assert(m_Counter);
  state.armed = true;
  const uint64_t expired = PosixTimerState::advance(state, current);
  // The service counted the first expiration. A backward realtime change
  // can put that deadline in the future again, yielding a zero-byte read.
  return addExpirations(m_Counter - 1, expired);
}

void TimerFd::changed() {
  m_Readers.broadcast();
  notifyReadiness(ReadyRead);
}

void TimerFd::service(uint64_t generation) {
  // A read copy may fault or block. The timer worker must remain available
  // to drive unrelated waits while that copy owns the object mutex.
  if (generation)
    m_Lock.acquire();
  else if (!m_Lock.tryAcquire())
    return;
  if (!m_AdmissionOpen || (generation && generation <= m_ClockGeneration)) {
    m_Lock.release();
    return;
  }
  const uint64_t oldGeneration = m_Generations.read;
  update(posix_clock_snapshot());
  const bool notify = oldGeneration != m_Generations.read;
  m_Lock.release();
  if (notify)
    changed();
}

int TimerFd::readToUser(void* buffer, size_t count, bool canBlock) {
  return readWithCopy(count, canBlock, copyRead, buffer);
}

int TimerFd::readWithCopy(size_t count, bool canBlock, PosixDescriptorReadCopy copy,
                          void* context) {
  if (count < sizeof(uint64_t)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  TerminationDeferral lifetime;
  m_Lock.acquire();
  PosixClockSnapshot clock = {};
  for (;;) {
    if (!m_AdmissionOpen) {
      m_Lock.release();
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    clock = posix_clock_snapshot();
    update(clock);
    if (readable() || (!canBlock && m_CancelOnSet && m_CancelPending))
      break;
    if (!canBlock) {
      m_Lock.release();
      SYSCALL_ERROR(NoMoreProcesses);
      return -1;
    }
    ConditionVariable::Error error = ConditionVariable::NoError;
    if (!m_Readers.wait(m_Lock, error)) {
      if (ConditionVariable::mutexAcquired(error))
        m_Lock.release();
      if (error == ConditionVariable::Interrupted ||
          error == ConditionVariable::TerminationDeferred)
        SYSCALL_ERROR(Interrupted);
      else
        SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
  }
  if (m_CancelOnSet && m_CancelPending) {
    m_CancelPending = false;
    m_CancelWake = false;
    m_Counter = 0;
    m_Expired = false;
    m_Lock.release();
    changed();
    SYSCALL_ERROR(Cancelled);
    return -1;
  }

  PosixTimerState::State next = m_State;
  const uint64_t value = forward(next, now(clock));
  if (value && !copy(context, &value, sizeof(value))) {
    m_Lock.release();
    changed();
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  m_State = next;
  m_Counter = 0;
  m_Expired = false;
  m_Lock.release();
  changed();
  return value ? sizeof(value) : 0;
}

int TimerFd::configure(int flags, Time::Timestamp value, Time::Timestamp interval, void* previous) {
  TerminationDeferral lifetime;
  m_Lock.acquire();
  if (!m_AdmissionOpen) {
    m_Lock.release();
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  const PosixClockSnapshot clock = posix_clock_snapshot();
  update(clock);
  if (previous) {
    PosixTimerState::State old = m_State;
    forward(old, now(clock));
    const PosixTimerState::Setting setting = PosixTimerState::snapshot(old, now(clock));
    if (!PosixSubsystem::copyToUser(previous, &setting, sizeof(setting))) {
      m_Lock.release();
      changed();
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }
  m_CancelOnSet = m_Clock == ClockRealtime && (flags & LinuxTimerFd::Absolute) &&
                  (flags & LinuxTimerFd::CancelOnSet);
  const bool cancelled = value && m_CancelOnSet && m_CancelPending;
  if (cancelled)
    m_CancelPending = false;
  m_CancelWake = false;
  m_ClockGeneration = clock.generation;
  m_Counter = 0;
  m_Expired = false;
  m_State.realtime = m_Clock == ClockRealtime && (flags & LinuxTimerFd::Absolute);
  m_State.interval = interval;
  m_State.deadline =
      flags & LinuxTimerFd::Absolute ? value : PosixTimerState::add(now(clock), value);
  m_State.armed = value != 0;
  update(clock);
  m_Lock.release();
  changed();
  if (cancelled) {
    SYSCALL_ERROR(Cancelled);
    return -1;
  }
  return 0;
}

int TimerFd::getTime(void* value) {
  TerminationDeferral lifetime;
  m_Lock.acquire();
  if (!m_AdmissionOpen) {
    m_Lock.release();
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  const PosixClockSnapshot clock = posix_clock_snapshot();
  update(clock);
  PosixTimerState::State next = m_State;
  const uint64_t counter = forward(next, now(clock));
  const PosixTimerState::Setting setting = PosixTimerState::snapshot(next, now(clock));
  if (!PosixSubsystem::copyToUser(value, &setting, sizeof(setting))) {
    m_Lock.release();
    changed();
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  m_State = next;
  m_Counter = counter;
  m_Expired = false;
  m_Lock.release();
  changed();
  return 0;
}

ReadyMask TimerFd::queryReady() {
  LockGuard<Mutex> guard(m_Lock);
  if (!m_AdmissionOpen)
    return ReadyInvalid | ReadyHangup;
  return readable() ? ReadyRead : ReadyNone;
}

ReadinessGenerations TimerFd::readinessGenerations() {
  LockGuard<Mutex> guard(m_Lock);
  return m_Generations;
}

bool TimerFd::addDescriptorOwner() {
  TerminationDeferral lifetime;
  LockGuard<Mutex> guard(m_Lock);
  if (!m_AdmissionOpen)
    return false;
  ++m_DescriptorOwners;
  return true;
}

void TimerFd::removeDescriptorOwner() {
  TerminationDeferral lifetime;
  m_Lock.acquire();
  assert(m_DescriptorOwners);
  const bool last = !--m_DescriptorOwners;
  if (last) {
    m_AdmissionOpen = false;
    m_State.armed = false;
    ++m_Generations.hangup;
  }
  m_Lock.release();
  if (last) {
    timerService.remove(this);
    m_Readers.broadcast();
    closeReadiness();
  }
}

int posix_timerfd_create(int clock, int flags) {
  if ((clock != ClockRealtime && clock != ClockMonotonic) ||
      (flags & ~(LinuxTimerFd::NonBlock | LinuxTimerFd::CloseOnExec))) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  TerminationDeferral lifetime;
  SharedPointer<TimerFd> timer(new TimerFd(clock));
  if (!timerService.add(timer))
    return -1;
  const size_t fd = getAvailableDescriptor();
  const int descriptorFlags = flags & LinuxTimerFd::CloseOnExec ? FD_CLOEXEC : 0;
  const int statusFlags = O_RDWR | (flags & LinuxTimerFd::NonBlock ? O_NONBLOCK : 0);
  auto* descriptor = new FileDescriptor(nullptr, 0, fd, descriptorFlags, statusFlags);
  descriptor->setTimerFdImpl(timer);
  addDescriptor(static_cast<int>(fd), descriptor);
  return static_cast<int>(fd);
}

int posix_timerfd_settime(int fd, int flags, const void* newValue, void* oldValue) {
  PosixTimerState::Setting requested;
  if (!PosixSubsystem::copyFromUser(&requested, newValue, sizeof(requested))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  Time::Timestamp value, interval;
  if ((flags & ~(LinuxTimerFd::Absolute | LinuxTimerFd::CancelOnSet)) ||
      !PosixTimerState::decode(requested.value, value) ||
      !PosixTimerState::decode(requested.interval, interval)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  DescriptorLease descriptor;
  if (!acquireDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  SharedPointer<TimerFd> timer = descriptor->getTimerFdImpl();
  if (!timer) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return timer->configure(flags, value, interval, oldValue);
}

int posix_timerfd_gettime(int fd, void* value) {
  DescriptorLease descriptor;
  if (!acquireDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  SharedPointer<TimerFd> timer = descriptor->getTimerFdImpl();
  if (!timer) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return timer->getTime(value);
}

void posix_timerfd_clock_changed(uint64_t generation) {
  timerService.refresh(generation);
}
