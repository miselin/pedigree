/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "epoll-syscalls.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/assert.h"

#include <config.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/eventfd-syscalls.h"
#include "modules/subsys/posix/net-syscalls.h"
#include "modules/system/vfs/File.h"

namespace {
constexpr int MaximumEpollBatch = 16384;
constexpr int LinuxMaximumEpollEvents =
    INT_MAX / static_cast<int>(sizeof(LinuxEpollEvent));
constexpr size_t LinuxKernelSigsetSize = sizeof(uint64_t);
constexpr uint64_t UnblockableSignals =
    (static_cast<uint64_t>(1) << (SIGKILL - 1)) | (static_cast<uint64_t>(1) << (SIGSTOP - 1));

constexpr uint32_t ReadEvents = LinuxEpoll::In | LinuxEpoll::ReadNormal | LinuxEpoll::ReadBand;
constexpr uint32_t WriteEvents = LinuxEpoll::Out | LinuxEpoll::WriteNormal | LinuxEpoll::WriteBand;
constexpr uint32_t RequestedEvents =
    ReadEvents | WriteEvents | LinuxEpoll::Priority | LinuxEpoll::ReadHangup;
constexpr uint32_t AlwaysReturnedEvents = LinuxEpoll::Error | LinuxEpoll::Hangup;
constexpr uint32_t SupportedEvents =
    RequestedEvents | AlwaysReturnedEvents | LinuxEpoll::OneShot | LinuxEpoll::EdgeTriggered;
constexpr uint32_t UnsupportedModes =
    LinuxEpoll::Exclusive | LinuxEpoll::Wakeup | LinuxEpoll::Message;

struct EpollWatch {
  EpollWatch(int watchedFd, const FileDescriptor::OpenFileDescriptionLease& openFile,
             File* watchedFile, const SharedPointer<NetworkSyscalls>& watchedNetwork,
             const SharedPointer<EventFd>& watchedEventFd, bool readable, bool writable,
             const LinuxEpollEvent& event)
      : fd(watchedFd),
        description(openFile),
        file(watchedFile),
        network(watchedNetwork),
        eventFd(watchedEventFd),
        canRead(readable),
        canWrite(writable),
        events(event.events),
        data(event.data),
        armed(true),
        observedEvents(0),
        pendingEvents(0),
        observedWriteGeneration(watchedEventFd ? watchedEventFd->writeGeneration() : 0),
        observedGenerations(),
        subscription() {}

  int fd;
  FileDescriptor::OpenFileDescriptionLease description;
  File* file;
  SharedPointer<NetworkSyscalls> network;
  SharedPointer<EventFd> eventFd;
  bool canRead;
  bool canWrite;
  uint32_t events;
  uint64_t data;
  bool armed;
  uint32_t observedEvents;
  uint32_t pendingEvents;
  uint64_t observedWriteGeneration;
  ReadinessGenerations observedGenerations;
  ReadinessSubscription subscription;
};

ReadinessSource* watchSource(const EpollWatch& watch) {
  if (watch.file) {
    return watch.file;
  }
  if (watch.network) {
    return watch.network.get();
  }
  return watch.eventFd.get();
}

uint32_t eventsFor(ReadyMask ready, uint32_t requested) {
  uint32_t result = 0;
  if (ready & ReadyRead) {
    result |= requested & (LinuxEpoll::In | LinuxEpoll::ReadNormal);
  }
  if (ready & ReadyPriority) {
    result |= requested & (LinuxEpoll::Priority | LinuxEpoll::ReadBand);
  }
  if (ready & ReadyWrite) {
    result |= requested & WriteEvents;
  }
  if (ready & ReadyError) {
    result |= LinuxEpoll::Error;
  }
  if (ready & ReadyHangup) {
    result |= LinuxEpoll::Hangup;
  }
  if ((ready & ReadyReadHangup) && (requested & LinuxEpoll::ReadHangup)) {
    result |= LinuxEpoll::ReadHangup;
  }
  if (ready & ReadyInvalid) {
    result |= LinuxEpoll::Error | LinuxEpoll::Hangup;
  }
  return result;
}

ReadyMask queryWatch(const EpollWatch& watch) {
  const bool reading = watch.canRead && (watch.events & (ReadEvents | LinuxEpoll::ReadHangup));
  const bool writing = watch.canWrite && (watch.events & WriteEvents);
  if (watch.file) {
    return watch.file->queryReady(reading, writing);
  }
  if (watch.network) {
    return watch.network->queryReady(reading, writing);
  }
  if (watch.eventFd) {
    return watch.eventFd->queryReady();
  }
  return ReadyInvalid;
}

uint32_t sampleWatch(EpollWatch& watch) {
  const uint32_t readyEvents = eventsFor(queryWatch(watch), watch.events);
  if (watch.events & LinuxEpoll::EdgeTriggered) {
    ReadinessSource* source = watchSource(watch);
    const ReadinessGenerations generations =
        source ? source->readinessGenerations() : ReadinessGenerations();
    const uint64_t writeGeneration = watch.eventFd ? watch.eventFd->writeGeneration() : 0;
    // Keep edges which have not yet been consumed, but do not return a stale
    // edge after another thread has made that predicate false. Source-owned
    // generations preserve a false-to-true transition even when the producer's
    // callback overtakes the delayed notification for the preceding drain.
    watch.pendingEvents &= readyEvents;
    watch.pendingEvents |= readyEvents & ~watch.observedEvents;
    if (generations.read != watch.observedGenerations.read) {
      watch.pendingEvents |= readyEvents & (LinuxEpoll::In | LinuxEpoll::ReadNormal);
    }
    if (generations.write != watch.observedGenerations.write) {
      watch.pendingEvents |= readyEvents & WriteEvents;
    }
    if (generations.priority != watch.observedGenerations.priority) {
      watch.pendingEvents |= readyEvents & (LinuxEpoll::Priority | LinuxEpoll::ReadBand);
    }
    if (generations.error != watch.observedGenerations.error) {
      watch.pendingEvents |= readyEvents & LinuxEpoll::Error;
    }
    if (generations.hangup != watch.observedGenerations.hangup) {
      watch.pendingEvents |= readyEvents & LinuxEpoll::Hangup;
    }
    if (generations.readHangup != watch.observedGenerations.readHangup) {
      watch.pendingEvents |= readyEvents & LinuxEpoll::ReadHangup;
    }
    if (watch.eventFd && writeGeneration != watch.observedWriteGeneration) {
      // eventfd is a counter rather than a byte stream. Linux's async-event
      // users can deliberately leave it readable and treat every successful
      // producer write as a new edge.
      watch.pendingEvents |= readyEvents & ReadEvents;
    }
    watch.observedEvents = readyEvents;
    watch.observedWriteGeneration = writeGeneration;
    watch.observedGenerations = generations;
  }
  return readyEvents;
}

uint32_t reportableEvents(const EpollWatch& watch, uint32_t readyEvents) {
  if (watch.events & LinuxEpoll::EdgeTriggered) {
    return watch.pendingEvents;
  }
  return readyEvents;
}

void retireWatch(EpollWatch* watch) {
  if (!watch) {
    return;
  }
  watch->subscription.reset();
  delete watch;
}

void retireWatches(List<EpollWatch*>& watches) {
  while (watches.count()) {
    retireWatch(watches.popFront());
  }
}

bool acquireEpoll(int fd, SharedPointer<EpollInstance>& instance) {
  DescriptorLease descriptor;
  if (!acquireDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }

  instance = descriptor->epollImpl;
  if (!instance) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  return true;
}
}  // namespace

class EpollReadinessObserver final : public ReadinessObserver {
 public:
  explicit EpollReadinessObserver(EpollInstance* instance) : m_Instance(instance) {}

  void readinessChanged(ReadyMask mask) override {
    m_Instance->sourceReadinessChanged(mask);
  }

 private:
  EpollInstance* m_Instance;
};

class EpollState {
 public:
  explicit EpollState(EpollInstance* instance)
      : lock(),
        watches(),
        wakeup(0, true),
        wakePending(false),
        observer(new EpollReadinessObserver(instance)) {}

  ~EpollState() {
    // EpollInstance retires subscriptions before releasing their source and
    // observer references.
    assert(!watches.count());
  }

  Mutex lock;
  List<EpollWatch*> watches;
  Semaphore wakeup;
  Atomic<bool> wakePending;
  SharedPointer<ReadinessObserver> observer;
};

EpollInstance::EpollInstance() : ReadinessSource(), m_State(new EpollState(this)) {}

EpollInstance::~EpollInstance() {
  List<EpollWatch*> retiring;
  {
    LockGuard<Mutex> guard(m_State->lock);
    while (m_State->watches.count()) {
      retiring.pushBack(m_State->watches.popFront());
    }
  }

  // ReadinessSubscription::reset closes admission and drains a callback which
  // already captured this instance. Never wait for that callback while the
  // instance lock is held.
  retireWatches(retiring);
  m_State->observer.reset();
  closeReadiness(ReadyInvalid | ReadyHangup);
  delete m_State;
  m_State = nullptr;
}

void EpollInstance::wakeWaiter() {
  // Treat the semaphore as a binary wakeup. Coalescing prevents repeated
  // level-triggered waits from accumulating an unbounded stale count.
  if (m_State->wakePending.compareAndSwap(false, true)) {
    m_State->wakeup.release();
  }
}

void EpollInstance::sourceReadinessChanged(ReadyMask) {
  bool reportable = false;
  {
    LockGuard<Mutex> guard(m_State->lock);
    for (EpollWatch* watch : m_State->watches) {
      if (!watch->description->descriptorOwnerCount()) {
        continue;
      }

      const uint32_t readyEvents = sampleWatch(*watch);
      if (watch->armed && reportableEvents(*watch, readyEvents)) {
        reportable = true;
      }
    }
  }

  // Notifications are change hints rather than payloads. Source generations
  // make reordered callbacks safe; a waiter still performs an authoritative
  // rescan before returning anything to userspace.
  if (reportable) {
    wakeWaiter();
    notifyReadiness(ReadyRead);
  }
}

int EpollInstance::control(int operation, int targetFd, const LinuxEpollEvent* event) {
  if (operation != LinuxEpoll::ControlAdd && operation != LinuxEpoll::ControlDelete &&
      operation != LinuxEpoll::ControlModify) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  if ((operation == LinuxEpoll::ControlAdd || operation == LinuxEpoll::ControlModify) && !event) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  if (event) {
    if (event->events & UnsupportedModes) {
      SYSCALL_ERROR(OperationNotSupported);
      return -1;
    }
    if (event->events & ~SupportedEvents) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
  }

  DescriptorLease descriptor;
  if (!acquireDescriptor(targetFd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  // Nested epoll needs cycle detection and a ready-list propagation model.
  // Until those semantics exist, reject every epoll target explicitly.
  if (descriptor->epollImpl) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  FileDescriptor::OpenFileDescriptionLease description = descriptor->acquireOpenFileDescription();
  File* file = description->getFile();
  SharedPointer<NetworkSyscalls> network = description->getNetworkImpl();
  SharedPointer<EventFd> eventFd = description->getEventFdImpl();
  if (!file && !network && !eventFd) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }

  if ((operation == LinuxEpoll::ControlAdd || operation == LinuxEpoll::ControlModify) && file &&
      !file->supportsReadinessNotifications()) {
    // Linux rejects regular files and directories with EPERM. Opt-in keeps a
    // File subclass from appearing epollable merely because select() can be
    // sampled once; it must also publish later readiness transitions.
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }

  if (operation == LinuxEpoll::ControlDelete) {
    EpollWatch* retiring = nullptr;
    {
      LockGuard<Mutex> guard(m_State->lock);
      for (auto it = m_State->watches.begin(); it != m_State->watches.end(); ++it) {
        EpollWatch* watch = *it;
        if (watch->fd == targetFd && watch->description.get() == description.get()) {
          retiring = watch;
          m_State->watches.erase(it);
          break;
        }
      }
    }

    if (!retiring) {
      SYSCALL_ERROR(DoesNotExist);
      return -1;
    }
    retireWatch(retiring);
    return 0;
  }

  if (operation == LinuxEpoll::ControlModify) {
    bool found = false;
    {
      LockGuard<Mutex> guard(m_State->lock);
      for (EpollWatch* watch : m_State->watches) {
        if (watch->fd == targetFd && watch->description.get() == description.get()) {
          watch->events = event->events;
          watch->data = event->data;
          watch->armed = true;
          // MOD both rearms EPOLLONESHOT and republishes an already-ready
          // level for EPOLLET, matching Linux's re-poll-on-modify behavior.
          watch->observedEvents = 0;
          watch->pendingEvents = 0;
          watch->observedWriteGeneration = watch->eventFd ? watch->eventFd->writeGeneration() : 0;
          ReadinessSource* source = watchSource(*watch);
          watch->observedGenerations =
              source ? source->readinessGenerations() : ReadinessGenerations();
          found = true;
          break;
        }
      }
    }

    if (!found) {
      SYSCALL_ERROR(DoesNotExist);
      return -1;
    }

    // MOD rearms EPOLLONESHOT and may make an already-true level relevant.
    sourceReadinessChanged(ReadyAll);
    return 0;
  }

  const int accessMode = descriptor->getStatusFlags() & O_ACCMODE;
  const bool canRead = network || eventFd || accessMode != O_WRONLY;
  const bool canWrite = network || eventFd || accessMode != O_RDONLY;
  EpollWatch* watch =
      new EpollWatch(targetFd, description, file, network, eventFd, canRead, canWrite, *event);

  // Subscription precedes publication, so the first subsequent wait cannot
  // miss a readiness transition between registration and its initial scan.
  ReadinessSource* source = watchSource(*watch);
  // Keep the subscription broad: EPOLL_CTL_MOD may change the interest mask
  // without replacing the target registration.
  if (!source->subscribeReadiness(ReadyAll, m_State->observer, watch->subscription)) {
    delete watch;
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }

  bool duplicate = false;
  {
    LockGuard<Mutex> guard(m_State->lock);
    for (EpollWatch* existing : m_State->watches) {
      if (existing->fd == targetFd && existing->description.get() == description.get()) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      m_State->watches.pushBack(watch);
    }
  }

  if (duplicate) {
    retireWatch(watch);
    SYSCALL_ERROR(FileExists);
    return -1;
  }

  // An already-ready source need not generate a transition after ADD.
  sourceReadinessChanged(ReadyAll);
  return 0;
}

int EpollInstance::collectEvents(LinuxEpollEvent* events, int maxEvents, bool consumeOneShot) {
  List<EpollWatch*> retiring;
  int count = 0;
  {
    LockGuard<Mutex> guard(m_State->lock);
    const size_t candidates = m_State->watches.count();
    size_t inspected = 0;

    // Moving each inspected watch to the tail gives bounded fairness when the
    // caller's result array is smaller than the ready set.
    while (inspected < candidates && count < maxEvents) {
      EpollWatch* watch = m_State->watches.popFront();
      ++inspected;

      if (!watch->description->descriptorOwnerCount()) {
        retiring.pushBack(watch);
        continue;
      }

      if (!watch->armed) {
        m_State->watches.pushBack(watch);
        continue;
      }

      const uint32_t readyEvents = sampleWatch(*watch);
      if (!watch->description->descriptorOwnerCount()) {
        retiring.pushBack(watch);
        continue;
      }

      const uint32_t returnedEvents = reportableEvents(*watch, readyEvents);
      if (returnedEvents) {
        if (events) {
          events[count].events = returnedEvents;
          events[count].data = watch->data;
        }
        ++count;
        if (consumeOneShot) {
          if (watch->events & LinuxEpoll::EdgeTriggered) {
            watch->pendingEvents &= ~returnedEvents;
          }
          if (watch->events & LinuxEpoll::OneShot) {
            watch->armed = false;
          }
        }
      }

      m_State->watches.pushBack(watch);
    }
  }

  // A readiness callback admitted before removal may still be executing.
  // Reset outside m_State->lock so its drain cannot deadlock with a waiter.
  retireWatches(retiring);
  return count;
}

ReadyMask EpollInstance::queryReady() {
  return collectEvents(nullptr, 1, false) ? ReadyRead : ReadyNone;
}

int EpollInstance::wait(LinuxEpollEvent* events, int maxEvents, int timeoutMilliseconds) {
  const bool hasTimeout = timeoutMilliseconds >= 0;
  const Time::Timestamp deadline =
      timeoutMilliseconds > 0
          ? Time::getTicks() +
                static_cast<Time::Timestamp>(timeoutMilliseconds) * Time::Multiplier::Millisecond
          : 0;

  EMIT_IF(!THREADS) {
    return collectEvents(events, maxEvents, true);
  }
  else {
    while (true) {
      // Always take the authoritative snapshot first. A level which changes
      // after this scan leaves a semaphore count for the wait below.
      int ready = collectEvents(events, maxEvents, true);
      if (ready) {
        // One source hint wakes one waiter. Hand the wake onward while a
        // level may still be true so other threads already blocked on this
        // epoll object are not stranded. An EPOLLONESHOT-only result causes
        // at most one harmless extra rescan.
        wakeWaiter();
        return ready;
      }
      if (timeoutMilliseconds == 0) {
        return 0;
      }

      // Consume an older hint without blocking, clear its binary admission,
      // and rescan. A concurrent producer after the clear publishes a fresh
      // semaphore count.
      if (m_State->wakeup.tryAcquire()) {
        m_State->wakePending = false;
        continue;
      }

      size_t waitSeconds = 0;
      size_t waitMicroseconds = 0;
      if (hasTimeout) {
        const Time::Timestamp now = Time::getTicks();
        if (now >= deadline) {
          return 0;
        }

        const Time::Timestamp remaining = deadline - now;
        waitSeconds = remaining / Time::Multiplier::Second;
        waitMicroseconds =
            (remaining % Time::Multiplier::Second + Time::Multiplier::Microsecond - 1) /
            Time::Multiplier::Microsecond;
        if (waitMicroseconds >= 1000000) {
          ++waitSeconds;
          waitMicroseconds = 0;
        }
      }

      Semaphore::SemaphoreError error = Semaphore::NoError;
      const bool acquired =
          m_State->wakeup.acquireWithError(1, waitSeconds, waitMicroseconds, error);
      if (acquired) {
        m_State->wakePending = false;
        // The loop always rescans; callbacks carry hints, not event payloads.
        continue;
      }

      // Readiness which became visible concurrently with timeout or a signal
      // wins, matching the rest of the POSIX blocking-I/O boundary.
      ready = collectEvents(events, maxEvents, true);
      if (ready) {
        return ready;
      }
      if (error == Semaphore::TimedOut) {
        return 0;
      }

      SYSCALL_ERROR(Interrupted);
      return -1;
    }
  }
}

int posix_epoll_create1(int flags) {
  if (flags & ~LinuxEpoll::CloseOnExec) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  const size_t fd = getAvailableDescriptor();
  const int descriptorFlags = flags & LinuxEpoll::CloseOnExec ? FD_CLOEXEC : 0;
  FileDescriptor* descriptor = new FileDescriptor(nullptr, 0, fd, descriptorFlags, O_RDWR);
  descriptor->epollImpl.reset(new EpollInstance);
  addDescriptor(static_cast<int>(fd), descriptor);
  return static_cast<int>(fd);
}

int posix_epoll_create(int size) {
  if (size <= 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return posix_epoll_create1(0);
}

int posix_epoll_ctl(int epollFd, int operation, int targetFd, const LinuxEpollEvent* event) {
  SharedPointer<EpollInstance> instance;
  if (!acquireEpoll(epollFd, instance)) {
    return -1;
  }

  LinuxEpollEvent snapshot = {};
  const LinuxEpollEvent* kernelEvent = nullptr;
  if (operation == LinuxEpoll::ControlAdd || operation == LinuxEpoll::ControlModify) {
    if (!PosixSubsystem::copyFromUser(&snapshot, event, 1, sizeof(snapshot))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    kernelEvent = &snapshot;
  }
  return instance->control(operation, targetFd, kernelEvent);
}

namespace {
int epollWait(int epollFd, LinuxEpollEvent* events, int maxEvents, int timeoutMilliseconds,
              const uint64_t* temporarySignalMask) {
  if (maxEvents <= 0 || maxEvents > LinuxMaximumEpollEvents) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  // Linux permits a much larger maxevents value than we want to allocate in
  // one contiguous kernel buffer. Returning a smaller ready batch is valid;
  // the rotating scan cursor exposes the remainder on subsequent waits.
  const int eventCapacity =
      maxEvents < MaximumEpollBatch ? maxEvents : MaximumEpollBatch;

  SharedPointer<EpollInstance> instance;
  if (!acquireEpoll(epollFd, instance)) {
    return -1;
  }

  size_t extent = 0;
  if (!PosixSubsystem::checkUserBuffer(reinterpret_cast<uintptr_t>(events), eventCapacity,
                                       sizeof(LinuxEpollEvent), PosixSubsystem::SafeWrite,
                                       &extent)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  LinuxEpollEvent* snapshot = new LinuxEpollEvent[eventCapacity];
  int result = 0;
  bool signalInterrupted = false;
  if (temporarySignalMask) {
    Thread* thread = Processor::information().getCurrentThread();
    if (!thread) {
      FATAL("epoll_pwait has no current Thread.");
    }

    Thread::TemporarySignalMask signalWait(*thread, *temporarySignalMask);
    result = instance->wait(snapshot, eventCapacity, timeoutMilliseconds);
    signalInterrupted = signalWait.finish();
  } else {
    result = instance->wait(snapshot, eventCapacity, timeoutMilliseconds);
  }

  if (!result && signalInterrupted) {
    SYSCALL_ERROR(Interrupted);
    result = -1;
  }
  if (result <= 0) {
    delete[] snapshot;
    return result;
  }

  const bool copied = PosixSubsystem::copyToUser(events, snapshot, result, sizeof(*snapshot));
  delete[] snapshot;
  if (!copied) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return result;
}
}  // namespace

int posix_epoll_wait(int epollFd, LinuxEpollEvent* events, int maxEvents, int timeoutMilliseconds) {
  return epollWait(epollFd, events, maxEvents, timeoutMilliseconds, nullptr);
}

int posix_epoll_pwait(int epollFd, LinuxEpollEvent* events, int maxEvents, int timeoutMilliseconds,
                      const void* signalMask, size_t signalMaskSize) {
  if (!signalMask) {
    return epollWait(epollFd, events, maxEvents, timeoutMilliseconds, nullptr);
  }
  if (signalMaskSize != LinuxKernelSigsetSize) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  uint64_t temporarySignalMask = 0;
  if (!PosixSubsystem::copyFromUser(&temporarySignalMask, signalMask, LinuxKernelSigsetSize)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  temporarySignalMask &= ~UnblockableSignals;
  return epollWait(epollFd, events, maxEvents, timeoutMilliseconds, &temporarySignalMask);
}
