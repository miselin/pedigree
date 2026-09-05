/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/machine/TimerHandler.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

#include "PosixSubsystem.h"
#include "posix-timer-state.h"
#include "posix-timer-syscalls.h"
#include "queued-signal.h"

namespace {
constexpr size_t MaximumTimers = 256;
constexpr size_t MaximumProcessTimers = 64;
constexpr unsigned MaximumGeneration = 0x7fffff;
constexpr int ClockRealtime = 0, ClockMonotonic = 1, Absolute = 1;
constexpr int Signal = 0, None = 1, ThreadId = 4;

struct EventPrefix {
  uint64_t value;
  int32_t signal;
  int32_t notification;
};
static_assert(sizeof(EventPrefix) == 16, "Linux sigevent prefix ABI");

struct Entry {
  Process* owner = nullptr;
  Thread* target = nullptr;
  size_t targetId = 0;
  int id = 0;
  int clock = ClockRealtime;
  EventPrefix event = {};
  PosixTimerState::State state;
  SharedPointer<PosixTimerSignalToken> notification;
};

class Registry : public TimerHandler {
 public:
  ~Registry() override {
    if (source && !source->unregisterHandler(this))
      panic("POSIX timer callback could not be drained");
  }

  Entry* find(int id) {
    if (id < 0)
      return nullptr;
    Entry& entry = entries[static_cast<unsigned>(id) % MaximumTimers];
    return entry.owner == Processor::information().getCurrentThread()->getParent() && entry.id == id
               ? &entry
               : nullptr;
  }

  void advance(Entry& entry, Time::Timestamp now) {
    const uint64_t expired = PosixTimerState::advance(entry.state, now);
    if (entry.event.notification == None || !entry.notification->addExpirations(expired))
      return;

    Scheduler::ProcessLease process;
    Process::ThreadLease target;
    if (!Scheduler::instance().acquireProcess(process, entry.owner) ||
        (entry.target && (!process->acquireThread(target, entry.target) ||
                          target->getTaskId() != entry.targetId)) ||
        posix_signal_queue_timer(process.get(), target.get(), entry.event.signal, entry.event.value,
                                 entry.notification) < 0)
      entry.notification->queueFailed();
  }

  void remove(Entry& entry) {
    posix_signal_cancel_timer(entry.owner, entry.notification);
    entry = Entry();
  }

  void timer(uint64_t) override {
    // The RTC and hosted timer dispatch from their IRQ workers. Skipping a
    // busy registry never loses expirations because deadlines are absolute.
    if (Processor::inDeviceHardIrq())
      return;
    TerminationDeferral lifetime;
    if (!lock.tryAcquire())
      return;
    const Time::Timestamp monotonic = Time::getTicks();
    const Time::Timestamp realtime = Time::getTimeNanoseconds();
    for (Entry& entry : entries)
      if (entry.owner)
        advance(entry, entry.state.realtime ? realtime : monotonic);
    lock.release();
  }

  Mutex lock;
  Timer* source = nullptr;
  Entry entries[MaximumTimers];
  unsigned generations[MaximumTimers] = {};
};

Registry timers;
}  // namespace

int posix_timer_create(int clock, const void* event, int* timerId) {
  if (clock != ClockRealtime && clock != ClockMonotonic) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  EventPrefix requested = {0, 14, Signal};
  int32_t targetId = 0;
  if (event) {
    // musl uses a compact ksigevent, so the unused public sigevent tail is
    // neither accessible nor meaningful to this syscall.
    if (!PosixSubsystem::copyFromUser(&requested, event, sizeof(requested))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (requested.notification == ThreadId &&
        !PosixSubsystem::copyFromUser(&targetId,
                                      reinterpret_cast<const uint8_t*>(event) + sizeof(requested),
                                      sizeof(targetId))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }
  if ((requested.notification != Signal && requested.notification != None &&
       requested.notification != ThreadId) ||
      (requested.notification != None && (requested.signal < 1 || requested.signal > 64))) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  Process* process = Processor::information().getCurrentThread()->getParent();
  LockGuard<Mutex> guard(timers.lock);
  Process::ThreadLease target;
  if (requested.notification == ThreadId &&
      (targetId <= 0 || !process->acquireThreadByTaskId(target, targetId) ||
       target->getUnwindState() != Thread::Continue || !target->acceptingEvents())) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  size_t freeSlot = MaximumTimers, owned = 0;
  for (size_t i = 0; i < MaximumTimers; ++i) {
    if (!timers.entries[i].owner && freeSlot == MaximumTimers)
      freeSlot = i;
    if (timers.entries[i].owner == process)
      ++owned;
  }
  if (freeSlot == MaximumTimers || owned == MaximumProcessTimers) {
    SYSCALL_ERROR(NoMoreProcesses);
    return -1;
  }
  if (!timers.source) {
    Timer* source = Machine::instance().getTimer();
    if (!source || !source->registerHandler(&timers)) {
      SYSCALL_ERROR(NoMoreProcesses);
      return -1;
    }
    timers.source = source;
  }

  SharedPointer<PosixTimerSignalToken> notification;
  if (!posix_signal_reserve_timer(process, notification))
    return -1;
  unsigned generation = (timers.generations[freeSlot] + 1) & MaximumGeneration;
  if (!generation)
    generation = 1;
  const int id = generation * MaximumTimers + freeSlot;
  timers.generations[freeSlot] = generation;
  notification->timerId = id;
  if (!PosixSubsystem::copyToUser(timerId, &id, sizeof(id))) {
    posix_signal_cancel_timer(process, notification);
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  Entry& entry = timers.entries[freeSlot];
  entry.id = id;
  entry.clock = clock;
  entry.event = requested;
  if (!event)
    entry.event.value = id;
  entry.target = target.get();
  entry.targetId = targetId;
  entry.notification = notification;
  entry.owner = process;
  return 0;
}

int posix_timer_settime(int timerId, int flags, const void* setting, void* previous) {
  if ((flags & ~Absolute) || !setting) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  PosixTimerState::Setting requested;
  if (!PosixSubsystem::copyFromUser(&requested, setting, sizeof(requested))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  Time::Timestamp value, interval;
  if (!PosixTimerState::decode(requested.value, value) ||
      !PosixTimerState::decode(requested.interval, interval)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  LockGuard<Mutex> guard(timers.lock);
  Entry* entry = timers.find(timerId);
  if (!entry) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  const Time::Timestamp oldNow =
      entry->state.realtime ? Time::getTimeNanoseconds() : Time::getTicks();
  timers.advance(*entry, oldNow);
  if (previous) {
    const PosixTimerState::Setting old = PosixTimerState::snapshot(entry->state, oldNow);
    if (!PosixSubsystem::copyToUser(previous, &old, sizeof(old))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  posix_signal_reset_timer(entry->owner, entry->notification);
  entry->state.realtime = (flags & Absolute) && entry->clock == ClockRealtime;
  const Time::Timestamp now = entry->state.realtime ? Time::getTimeNanoseconds() : Time::getTicks();
  entry->state.armed = value != 0;
  entry->state.interval = value ? interval : 0;
  entry->state.deadline = flags & Absolute ? value : PosixTimerState::add(now, value);
  timers.advance(*entry, now);
  return 0;
}

int posix_timer_gettime(int timerId, void* setting) {
  LockGuard<Mutex> guard(timers.lock);
  Entry* entry = timers.find(timerId);
  if (!entry) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  const Time::Timestamp now = entry->state.realtime ? Time::getTimeNanoseconds() : Time::getTicks();
  timers.advance(*entry, now);
  const PosixTimerState::Setting result = PosixTimerState::snapshot(entry->state, now);
  if (!PosixSubsystem::copyToUser(setting, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}

int posix_timer_getoverrun(int timerId) {
  LockGuard<Mutex> guard(timers.lock);
  Entry* entry = timers.find(timerId);
  if (!entry) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return entry->notification->getDeliveredOverrun();
}

int posix_timer_delete(int timerId) {
  LockGuard<Mutex> guard(timers.lock);
  Entry* entry = timers.find(timerId);
  if (!entry) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  timers.remove(*entry);
  return 0;
}

void posix_timer_process_exit(Process* process) {
  if (!process)
    return;
  LockGuard<Mutex> guard(timers.lock);
  for (Entry& entry : timers.entries)
    if (entry.owner == process)
      timers.remove(entry);
}

void posix_timer_thread_exit(Thread* thread) {
  if (!thread)
    return;
  LockGuard<Mutex> guard(timers.lock);
  for (Entry& entry : timers.entries)
    if (entry.owner && entry.target == thread)
      timers.remove(entry);
}
