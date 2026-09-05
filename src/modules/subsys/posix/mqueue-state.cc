/* Copyright (c) 2026, Pedigree Developers. */
#include "mqueue-state.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include <errno.h>
#include <fcntl.h>

#include "modules/subsys/posix/PosixSubsystem.h"
#include "mqueue-netlink.h"

namespace {
bool copyDeadline(const LinuxMqTimespec* user, Time::Timestamp& deadline) {
  deadline = Time::Infinity;
  if (!user) {
    return true;
  }
  LinuxMqTimespec value = {};
  if (!PosixSubsystem::copyFromUser(&value, user, sizeof(value))) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  if (value.seconds < 0 || value.nanoseconds < 0 || value.nanoseconds >= 1000000000) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  const uint64_t seconds = static_cast<uint64_t>(value.seconds);
  const uint64_t nanos = static_cast<uint64_t>(value.nanoseconds);
  deadline = seconds > (Time::Infinity - 1 - nanos) / Time::Multiplier::Second
                 ? Time::Infinity - 1
                 : seconds * Time::Multiplier::Second + nanos;
  return true;
}

bool waitQueue(ConditionVariable& condition, Mutex& lock, Time::Timestamp deadline, bool nonblock) {
  if (nonblock) {
    SYSCALL_ERROR(NoMoreProcesses);
    return false;
  }
  Time::Timestamp remaining = Time::Infinity;
  if (deadline != Time::Infinity) {
    const Time::Timestamp now = Time::getTimeNanoseconds();
    if (deadline <= now) {
      SYSCALL_ERROR(TimedOut);
      return false;
    }
    remaining = deadline - now;
    // Re-evaluate absolute realtime deadlines after wall-clock adjustments.
    if (remaining > Time::Multiplier::Second) {
      remaining = Time::Multiplier::Second;
    }
  }
  ConditionVariable::Error error = ConditionVariable::NoError;
  if (condition.wait(lock, remaining, error) || error == ConditionVariable::TimedOut) {
    return true;
  }
  SYSCALL_ERROR(Interrupted);
  return false;
}
}  // namespace

MqueueState::MqueueState(const String& queueName, size_t maxMessages, size_t messageSize,
                         int64_t owner, int64_t group, unsigned permissions)
    : lock(),
      readers(),
      writers(),
      name(queueName),
      capacity(maxMessages),
      size(messageSize),
      count(0),
      receiverCount(0),
      uid(owner),
      gid(group),
      mode(permissions),
      head(-1),
      free(0),
      messages(UniqueArray<Message>::allocate(maxMessages)),
      storage(UniqueArray<uint8_t>::allocate(maxMessages * messageSize)),
      notification(),
      generations() {
  for (size_t n = 0; n < capacity; ++n) {
    messages.get()[n].next = n + 1 == capacity ? -1 : static_cast<int>(n + 1);
  }
}

PosixMessageQueue::PosixMessageQueue(const String& name, size_t capacity, size_t size, int64_t uid,
                                     int64_t gid, unsigned mode)
    : m_State(new MqueueState(name, capacity, size, uid, gid, mode)) {}

PosixMessageQueue::~PosixMessageQueue() {
  {
    LockGuard<Mutex> guard(g_MqueueRegistryLock);
    for (auto it = g_Mqueues.begin(); it != g_Mqueues.end(); ++it) {
      if (*it == this) {
        g_Mqueues.erase(it);
        break;
      }
    }
  }
  m_State->notification.complete(true);
  closeReadiness();
  delete m_State;
}

const String& PosixMessageQueue::name() const {
  return m_State->name;
}

bool PosixMessageQueue::mayOpen(Process* process, int flags) const {
  const auto& state = *m_State;
  const int64_t uid = process->getEffectiveUserId();
  if (uid == 0) {
    return true;
  }
  unsigned mode = state.mode;
  bool group = state.gid == process->getEffectiveGroupId();
  if (!group) {
    Vector<int64_t> groups;
    process->getSupplementalGroupIds(groups);
    for (size_t n = 0; n < groups.count(); ++n) {
      group |= groups[n] == state.gid;
    }
  }
  mode >>= uid == state.uid ? 6 : group ? 3 : 0;
  const int access = flags & O_ACCMODE;
  return (access == O_WRONLY || (mode & 4)) && (access == O_RDONLY || (mode & 2));
}

bool PosixMessageQueue::mayUnlink(Process* process) const {
  const int64_t uid = process->getEffectiveUserId();
  return uid == 0 || uid == m_State->uid;
}

int PosixMessageQueue::send(const char* data, size_t length, unsigned priority, bool nonblock,
                            const LinuxMqTimespec* timeout) {
  auto& state = *m_State;
  if (priority >= 32768) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (length > state.size) {
    syscallError(EMSGSIZE);
    return -1;
  }
  Time::Timestamp deadline;
  if (!copyDeadline(timeout, deadline)) {
    return -1;
  }
  auto copy = UniqueArray<uint8_t>::allocate(length ? length : 1);
  if (!PosixSubsystem::copyFromUser(copy.get(), data, length)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  state.lock.acquire();
  while (state.count == state.capacity) {
    if (!waitQueue(state.writers, state.lock, deadline, nonblock)) {
      state.lock.release();
      return -1;
    }
  }
  const bool wasEmpty = !state.count;
  const int slot = state.free;
  auto& message = state.messages.get()[slot];
  state.free = message.next;
  message.length = length;
  message.priority = priority;
  MemoryCopy(state.storage.get() + slot * state.size, copy.get(), length);
  int* insertion = &state.head;
  while (*insertion >= 0 && state.messages.get()[*insertion].priority >= priority) {
    insertion = &state.messages.get()[*insertion].next;
  }
  message.next = *insertion;
  *insertion = slot;
  ++state.count;
  if (wasEmpty) {
    ++state.generations.read;
    if (!state.receiverCount) {
      state.notification.complete(false);
    }
  }
  state.lock.release();
  state.readers.broadcast();
  notifyReadiness(ReadyRead | ReadyWrite);
  return 0;
}

int PosixMessageQueue::receive(char* data, size_t length, unsigned* priority, bool nonblock,
                               const LinuxMqTimespec* timeout) {
  auto& state = *m_State;
  if (length < state.size) {
    syscallError(EMSGSIZE);
    return -1;
  }
  Time::Timestamp deadline;
  if (!copyDeadline(timeout, deadline)) {
    return -1;
  }
  state.lock.acquire();
  while (!state.count) {
    ++state.receiverCount;
    const bool resumed = waitQueue(state.readers, state.lock, deadline, nonblock);
    --state.receiverCount;
    if (!resumed) {
      state.lock.release();
      return -1;
    }
  }
  const int slot = state.head;
  auto& message = state.messages.get()[slot];
  const size_t received = message.length;
  // Retain the head until every result has reached userspace. A bad priority
  // pointer or a concurrently unmapped data buffer cannot consume a message.
  if ((priority && !PosixSubsystem::copyToUser(priority, &message.priority, sizeof(*priority))) ||
      !PosixSubsystem::copyToUser(data, state.storage.get() + slot * state.size, received)) {
    state.lock.release();
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  state.head = message.next;
  message.next = state.free;
  state.free = slot;
  if (state.count-- == state.capacity) {
    ++state.generations.write;
  }
  state.lock.release();
  state.writers.broadcast();
  notifyReadiness(ReadyRead | ReadyWrite);
  return static_cast<int>(received);
}

int PosixMessageQueue::attributes(FileDescriptor& descriptor, const LinuxMqAttr* requested,
                                  LinuxMqAttr* previous) {
  LinuxMqAttr attr = {};
  if (requested) {
    if (!PosixSubsystem::copyFromUser(&attr, requested, sizeof(attr))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (attr.flags & ~static_cast<int64_t>(O_NONBLOCK)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
  }
  auto& state = *m_State;
  LockGuard<Mutex> guard(state.lock);
  LinuxMqAttr old = {descriptor.getStatusFlags() & O_NONBLOCK,
                     static_cast<int64_t>(state.capacity),
                     static_cast<int64_t>(state.size),
                     static_cast<int64_t>(state.count),
                     {}};
  if (previous && !PosixSubsystem::copyToUser(previous, &old, sizeof(old))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (requested) {
    if (attr.flags & O_NONBLOCK) {
      descriptor.addStatusFlag(O_NONBLOCK);
    } else {
      descriptor.removeStatusFlag(O_NONBLOCK);
    }
  }
  return 0;
}

ReadyMask PosixMessageQueue::queryReady() {
  LockGuard<Mutex> guard(m_State->lock);
  return (m_State->count ? ReadyRead : ReadyNone) |
         (m_State->count < m_State->capacity ? ReadyWrite : ReadyNone);
}

ReadinessGenerations PosixMessageQueue::readinessGenerations() {
  LockGuard<Mutex> guard(m_State->lock);
  return m_State->generations;
}

void MqueueNotification::complete(bool removed) {
  if (!process) {
    return;
  }
  if (socket) {
    static_cast<MqueueNetlinkSocket*>(socket.get())->deliverCookie(cookie, removed);
  } else if (!removed && event.notify == 0) {
    // The queue lock also serializes exit cancellation. Pin the exact process
    // before dropping the registration so PID recycling cannot retarget it.
    Scheduler::ProcessLease target;
    if (Scheduler::instance().acquireProcess(target, process)) {
      Process::ThreadLease thread;
      if (target->acquireProcessSignalThread(thread)) {
        auto* subsystem = static_cast<PosixSubsystem*>(target->getSubsystem());
        subsystem->queueSignalDelivery(thread.get(), event.signal, nullptr, -3, true, event.value);
      }
    }
  }
  process = nullptr;
  socket.reset();
}

int PosixMessageQueue::notify(const LinuxMqSigevent* userEvent) {
  MqueueNotification notification;
  Process* process = Processor::information().getCurrentThread()->getParent();
  if (userEvent) {
    if (!PosixSubsystem::copyFromUser(&notification.event, userEvent, sizeof(notification.event))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    const auto& event = notification.event;
    if (event.notify < 0 || event.notify > 2 ||
        (event.notify == 0 && (event.signal <= 0 || static_cast<size_t>(event.signal) >
                                                        PosixSubsystem::MaximumSupportedSignal))) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (event.notify == 2) {
      DescriptorLease socket;
      if (!acquireDescriptor(event.signal, socket) || !socket->networkImpl ||
          socket->networkImpl->getDomain() != 16) {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
      }
      if (!PosixSubsystem::copyFromUser(notification.cookie,
                                        reinterpret_cast<const void*>(event.value), 32)) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      notification.socket = socket->networkImpl;
    }
    notification.process = process;
    notification.pid = process->getId();
  }
  auto& state = *m_State;
  LockGuard<Mutex> guard(state.lock);
  if (!userEvent) {
    if (state.notification.process == process) {
      state.notification.complete(true);
    }
    return 0;
  }
  if (state.notification.process) {
    SYSCALL_ERROR(DeviceBusy);
    return -1;
  }
  if (notification.socket &&
      !static_cast<MqueueNetlinkSocket*>(notification.socket.get())->reserveCookie()) {
    return -1;
  }
  state.notification = notification;
  return 0;
}

void PosixMessageQueue::cancelNotification(size_t pid) {
  LockGuard<Mutex> guard(m_State->lock);
  if (m_State->notification.process && m_State->notification.pid == pid) {
    m_State->notification.complete(true);
  }
}
