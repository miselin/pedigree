/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "inotify-syscalls.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include <fcntl.h>
#include <limits.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/ResolvedPath.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/FileEvent.h"
#include "modules/system/vfs/VFS.h"

namespace {
constexpr size_t MaxQueuedEvents = 16384;
constexpr FileEventMask AllFileEvents =
    FileEvents::Access | FileEvents::Modify | FileEvents::Attributes | FileEvents::CloseWrite |
    FileEvents::CloseNoWrite | FileEvents::Open | FileEvents::Created | FileEvents::Removed |
    FileEvents::DeletedSelf;
constexpr uint32_t AcceptedMask = LinuxInotify::AllBits;

// Capture before path and descriptor retirement can replace the selected error.
struct InotifyResult {
  InotifyResult(int result)
      : value(result),
        error(result < 0 ? Processor::information().getCurrentThread()->getErrno() : 0) {}
  int value;
  int error;
};

size_t paddedNameLength(const String& name) {
  if (!name.length()) {
    return 0;
  }
  return (name.length() + 1 + sizeof(LinuxInotifyEvent) - 1) & ~(sizeof(LinuxInotifyEvent) - 1);
}

uint32_t linuxMaskFor(FileEventMask mask) {
  uint32_t result = 0;
  if (mask & FileEvents::Access)
    result |= LinuxInotify::Access;
  if (mask & FileEvents::Modify)
    result |= LinuxInotify::Modify;
  if (mask & FileEvents::Attributes)
    result |= LinuxInotify::Attributes;
  if (mask & FileEvents::CloseWrite)
    result |= LinuxInotify::CloseWrite;
  if (mask & FileEvents::CloseNoWrite)
    result |= LinuxInotify::CloseNoWrite;
  if (mask & FileEvents::Open)
    result |= LinuxInotify::Open;
  if (mask & FileEvents::Created)
    result |= LinuxInotify::Create;
  if (mask & FileEvents::Removed)
    result |= LinuxInotify::Delete;
  if (mask & FileEvents::DeletedSelf)
    result |= LinuxInotify::DeleteSelf;
  return result;
}

struct QueuedEvent {
  QueuedEvent(int descriptor, uint32_t eventMask, uint32_t eventCookie, const StringView& eventName)
      : wd(descriptor), mask(eventMask), cookie(eventCookie), name(eventName.toString()) {}

  int wd;
  uint32_t mask;
  uint32_t cookie;
  String name;
};

class InotifyQueue {
 public:
  explicit InotifyQueue(InotifyInstance* readinessOwner)
      : owner(readinessOwner),
        lock(),
        changed(),
        events(),
        generations(),
        open(true),
        overflowQueued(false) {}

  ~InotifyQueue() {
    while (events.count()) {
      delete events.popFront();
    }
  }

  bool enqueue(int wd, uint32_t mask, uint32_t cookie, const StringView& name) {
    bool becameReadable = false;
    lock.acquire();
    if (!open) {
      lock.release();
      return false;
    }

    if (events.count() >= MaxQueuedEvents) {
      if (!overflowQueued) {
        becameReadable = !events.count();
        events.pushBack(new QueuedEvent(-1, LinuxInotify::QueueOverflow, 0, StringView()));
        overflowQueued = true;
        if (becameReadable) {
          ++generations.read;
        }
      }
      lock.release();
      changed.broadcast();
      owner->eventsQueued();
      return becameReadable;
    }

    if (events.count()) {
      QueuedEvent* last = *events.rbegin();
      if (last && !(last->mask & LinuxInotify::Ignored) && last->wd == wd && last->mask == mask &&
          last->name.view() == name) {
        lock.release();
        return false;
      }
    }

    becameReadable = !events.count();
    events.pushBack(new QueuedEvent(wd, mask, cookie, name));
    if (becameReadable) {
      ++generations.read;
    }
    lock.release();
    changed.broadcast();
    owner->eventsQueued();
    return becameReadable;
  }

  int readOne(uint8_t* buffer, size_t length, bool canBlock) {
    lock.acquire();
    while (!events.count()) {
      if (!open) {
        lock.release();
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
      }
      if (!canBlock) {
        lock.release();
        SYSCALL_ERROR(NoMoreProcesses);
        return -1;
      }
      ConditionVariable::Error error = ConditionVariable::NoError;
      if (!changed.wait(lock, error)) {
        if (ConditionVariable::mutexAcquired(error)) {
          lock.release();
        }
        if (error == ConditionVariable::Interrupted ||
            error == ConditionVariable::TerminationDeferred) {
          SYSCALL_ERROR(Interrupted);
        } else {
          SYSCALL_ERROR(BadFileDescriptor);
        }
        return -1;
      }
    }

    // Linux reports EAGAIN for an empty nonblocking queue regardless of the
    // buffer size. EINVAL applies only once a queued first record cannot fit.
    const size_t firstSize = sizeof(LinuxInotifyEvent) + paddedNameLength((*events.begin())->name);
    if (firstSize > length) {
      lock.release();
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }

    QueuedEvent* event = *events.begin();
    const size_t nameLength = paddedNameLength(event->name);
    const size_t recordSize = sizeof(LinuxInotifyEvent) + nameLength;
    LinuxInotifyEvent header = {event->wd, event->mask, event->cookie,
                                static_cast<uint32_t>(nameLength)};
    MemoryCopy(buffer, &header, sizeof(header));
    if (nameLength) {
      ByteSet(buffer + sizeof(header), 0, nameLength);
      MemoryCopy(buffer + sizeof(header), event->name.cstr(), event->name.length());
    }
    event = events.popFront();
    if (event->mask & LinuxInotify::QueueOverflow) {
      overflowQueued = false;
    }
    delete event;
    lock.release();
    return static_cast<int>(recordSize);
  }

  ReadyMask queryReady() {
    LockGuard<Mutex> guard(lock);
    return events.count() ? ReadyRead : ReadyNone;
  }

  ReadinessGenerations readinessGenerations() {
    LockGuard<Mutex> guard(lock);
    return generations;
  }

  void close() {
    lock.acquire();
    if (!open) {
      lock.release();
      return;
    }
    open = false;
    while (events.count()) {
      delete events.popFront();
    }
    overflowQueued = false;
    lock.release();
    changed.broadcast();
  }

 private:
  InotifyInstance* owner;
  Mutex lock;
  ConditionVariable changed;
  List<QueuedEvent*> events;
  ReadinessGenerations generations;
  bool open;
  bool overflowQueued;
};

class InotifyWatchObserver final : public FileEventObserver {
 public:
  InotifyWatchObserver(const SharedPointer<InotifyQueue>& eventQueue, int descriptor,
                       uint32_t eventMask, bool directory)
      : queue(eventQueue),
        lock(),
        wd(descriptor),
        mask(eventMask),
        isDirectory(directory),
        active(true) {}

  void updateMask(uint32_t eventMask, bool add) {
    LockGuard<Mutex> guard(lock);
    if (add) {
      mask |= eventMask & ~LinuxInotify::MaskAdd;
    } else {
      mask = eventMask;
    }
  }

  bool deactivate() {
    LockGuard<Mutex> guard(lock);
    const bool wasActive = active;
    active = false;
    return wasActive;
  }

  bool isActive() {
    LockGuard<Mutex> guard(lock);
    return active;
  }

  void fileEvent(const FileEvent& event) override {
    LockGuard<Mutex> guard(lock);
    if (!active) {
      return;
    }
    uint32_t selected = linuxMaskFor(event.mask) & mask;
    const bool deleted = event.mask & FileEvents::DeletedSelf;
    if (!selected && !deleted) {
      return;
    }
    if (!(selected & (LinuxInotify::DeleteSelf | LinuxInotify::MoveSelf)) &&
        ((event.name.length() && event.targetIsDirectory) ||
         (!event.name.length() && isDirectory))) {
      selected |= LinuxInotify::IsDirectory;
    }
    const bool retire = deleted || ((mask & LinuxInotify::OneShot) && selected);
    if (retire) {
      active = false;
    }
    if (selected) {
      queue->enqueue(wd, selected, 0, event.name);
    }
    if (retire) {
      queue->enqueue(wd, LinuxInotify::Ignored, 0, StringView());
    }
  }

 private:
  SharedPointer<InotifyQueue> queue;
  Mutex lock;
  int wd;
  uint32_t mask;
  bool isDirectory;
  bool active;
};

struct InotifyWatch {
  InotifyWatch(File* watchedTarget, bool retainedTarget,
               const SharedPointer<FileEventObserver>& watchObserver,
               InotifyWatchObserver* concreteObserver, int descriptor)
      : target(watchedTarget),
        retained(retainedTarget),
        observer(watchObserver),
        observerImpl(concreteObserver),
        wd(descriptor),
        subscription() {}

  File* target;
  bool retained;
  SharedPointer<FileEventObserver> observer;
  InotifyWatchObserver* observerImpl;
  int wd;
  FileEventSubscription subscription;
};

void retireWatch(InotifyWatch* watch) {
  if (!watch) {
    return;
  }
  watch->subscription.reset();
  if (watch->retained) {
    watch->target->releaseVfsReference();
  }
  delete watch;
}

}  // namespace

class InotifyState {
 public:
  explicit InotifyState(InotifyInstance* owner)
      : lock(), watches(), queue(new InotifyQueue(owner)), nextWd(1), descriptorOpen(true) {}

  Mutex lock;
  List<InotifyWatch*> watches;
  SharedPointer<InotifyQueue> queue;
  int nextWd;
  bool descriptorOpen;
};

InotifyInstance::InotifyInstance() : ReadinessSource(), m_State(new InotifyState(this)) {}

InotifyInstance::~InotifyInstance() {
  lastDescriptorClosed();
  delete m_State;
  closeReadiness();
}

void InotifyInstance::reapInactiveWatches() {
  List<InotifyWatch*> retiring;
  m_State->lock.acquire();
  for (auto it = m_State->watches.begin(); it != m_State->watches.end();) {
    InotifyWatch* watch = *it;
    if (!watch->observerImpl->isActive()) {
      it = m_State->watches.erase(it);
      retiring.pushBack(watch);
    } else {
      ++it;
    }
  }
  m_State->lock.release();

  while (retiring.count()) {
    retireWatch(retiring.popFront());
  }
}

int InotifyInstance::addWatch(File* target, uint32_t mask) {
  if (!target || (mask & ~AcceptedMask) || !(mask & AcceptedMask) ||
      ((mask & LinuxInotify::MaskAdd) && (mask & LinuxInotify::MaskCreate))) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if ((mask & LinuxInotify::OnlyDirectory) && !target->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return -1;
  }

  reapInactiveWatches();

  m_State->lock.acquire();
  if (!m_State->descriptorOpen) {
    m_State->lock.release();
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  for (auto watch : m_State->watches) {
    if (watch->target != target || !watch->observerImpl->isActive()) {
      continue;
    }
    if (mask & LinuxInotify::MaskCreate) {
      m_State->lock.release();
      SYSCALL_ERROR(FileExists);
      return -1;
    }
    watch->observerImpl->updateMask(mask, mask & LinuxInotify::MaskAdd);
    const int wd = watch->wd;
    m_State->lock.release();
    return wd;
  }

  int wd = m_State->nextWd++;
  if (wd <= 0) {
    wd = 1;
    m_State->nextWd = 2;
  }
  const bool retained = target->retainVfsReference();
  if (!retained && !target->isStableVfsRoot()) {
    m_State->lock.release();
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  InotifyWatchObserver* observerImpl =
      new InotifyWatchObserver(m_State->queue, wd, mask, target->isDirectory());
  SharedPointer<FileEventObserver> observer(observerImpl);
  InotifyWatch* watch = new InotifyWatch(target, retained, observer, observerImpl, wd);
  if (!target->subscribeFileEvents(AllFileEvents, observer, watch->subscription)) {
    if (retained) {
      target->releaseVfsReference();
    }
    delete watch;
    m_State->lock.release();
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  m_State->watches.pushBack(watch);
  m_State->lock.release();
  return wd;
}

int InotifyInstance::removeWatch(int wd) {
  reapInactiveWatches();

  InotifyWatch* retiring = nullptr;
  bool wasActive = false;
  m_State->lock.acquire();
  for (auto it = m_State->watches.begin(); it != m_State->watches.end(); ++it) {
    if ((*it)->wd == wd) {
      retiring = *it;
      retiring->subscription.reset();
      wasActive = retiring->observerImpl->deactivate();
      m_State->watches.erase(it);
      if (wasActive) {
        m_State->queue->enqueue(wd, LinuxInotify::Ignored, 0, StringView());
      }
      break;
    }
  }
  m_State->lock.release();
  if (!retiring) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  if (retiring->retained) {
    retiring->target->releaseVfsReference();
  }
  delete retiring;
  if (!wasActive) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return 0;
}

int InotifyInstance::readEvents(uint8_t* buffer, size_t length, bool canBlock) {
  int outcome = -1;
  size_t copied = 0;
  bool attempted = false;
  while (!attempted || copied < length) {
    attempted = true;
    uint8_t* destination = copied ? buffer + copied : buffer;
    const int result = m_State->queue->readOne(destination, length - copied, canBlock && !copied);
    if (result < 0) {
      if (copied) {
        Processor::information().getCurrentThread()->setErrno(0);
        outcome = static_cast<int>(copied);
      } else {
        outcome = result;
      }
      break;
    }
    copied += static_cast<size_t>(result);
    outcome = static_cast<int>(copied);
  }
  reapInactiveWatches();
  return outcome;
}

int InotifyInstance::readEventsToUser(uint8_t* buffer, size_t length, bool canBlock) {
  constexpr size_t MaximumInotifyRecord =
      sizeof(LinuxInotifyEvent) +
      ((PATH_MAX + 1 + sizeof(LinuxInotifyEvent) - 1) & ~(sizeof(LinuxInotifyEvent) - 1));
  const size_t bounceCapacity = length < MaximumInotifyRecord ? length : MaximumInotifyRecord;
  UniqueArray<uint8_t> bounce = UniqueArray<uint8_t>::allocate(bounceCapacity);

  int outcome = -1;
  size_t copied = 0;
  bool attempted = false;
  while (!attempted || copied < length) {
    attempted = true;
    const size_t remaining = length - copied;
    const size_t eventCapacity = remaining < bounceCapacity ? remaining : bounceCapacity;
    const int result = m_State->queue->readOne(bounce.get(), eventCapacity, canBlock && !copied);
    if (result < 0) {
      if (copied) {
        Processor::information().getCurrentThread()->setErrno(0);
        outcome = static_cast<int>(copied);
      } else {
        outcome = result;
      }
      break;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(buffer);
    if (copied > ~static_cast<uintptr_t>(0) - base ||
        !PosixSubsystem::copyToUser(reinterpret_cast<void*>(base + copied), bounce.get(),
                                    static_cast<size_t>(result))) {
      SYSCALL_ERROR(BadAddress);
      outcome = -1;
      break;
    }
    copied += static_cast<size_t>(result);
    outcome = static_cast<int>(copied);
  }
  reapInactiveWatches();
  return outcome;
}

ReadyMask InotifyInstance::queryReady() {
  return m_State->queue->queryReady();
}

ReadinessGenerations InotifyInstance::readinessGenerations() {
  return m_State->queue->readinessGenerations();
}

void InotifyInstance::eventsQueued() {
  notifyReadiness(ReadyRead);
}

void InotifyInstance::lastDescriptorClosed() {
  List<InotifyWatch*> retiring;
  m_State->lock.acquire();
  if (!m_State->descriptorOpen) {
    m_State->lock.release();
    return;
  }
  m_State->descriptorOpen = false;
  while (m_State->watches.count()) {
    retiring.pushBack(m_State->watches.popFront());
  }
  m_State->lock.release();

  while (retiring.count()) {
    retireWatch(retiring.popFront());
  }
  m_State->queue->close();
  closeReadiness(ReadyInvalid | ReadyHangup);
}

int posix_inotify_init() {
  return posix_inotify_init1(0);
}

int posix_inotify_init1(int flags) {
  constexpr int AllowedFlags = LinuxInotify::NonBlock | LinuxInotify::CloseOnExec;
  if (flags & ~AllowedFlags) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  const size_t fd = getAvailableDescriptor();
  const int descriptorFlags = flags & LinuxInotify::CloseOnExec ? FD_CLOEXEC : 0;
  const int statusFlags = O_RDONLY | (flags & LinuxInotify::NonBlock ? O_NONBLOCK : 0);
  FileDescriptor* descriptor = new FileDescriptor(nullptr, 0, fd, descriptorFlags, statusFlags);
  descriptor->setInotifyImpl(SharedPointer<InotifyInstance>(new InotifyInstance));
  addDescriptor(static_cast<int>(fd), descriptor);
  return static_cast<int>(fd);
}

static InotifyResult addWatch(int fd, const char* pathname, uint32_t mask) {
  ResolvedPath targetLease;
  // Linux validates the UAPI bitset before looking up either the descriptor
  // or pathname.
  if ((mask & ~AcceptedMask) || !(mask & AcceptedMask)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  DescriptorLease descriptor;
  Process* process = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if ((mask & LinuxInotify::MaskAdd) && (mask & LinuxInotify::MaskCreate)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  SharedPointer<InotifyInstance> instance = descriptor->getInotifyImpl();
  if (!instance) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  String path;
  const PosixSubsystem::UserStringResult copied =
      PosixSubsystem::copyUserString(pathname, path, PATH_MAX);
  if (copied == PosixSubsystem::UserStringBadAddress) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (copied == PosixSubsystem::UserStringTooLong) {
    SYSCALL_ERROR(NameTooLong);
    return -1;
  }
  if (!path.length()) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  String normalised;
  normalisePath(normalised, path.cstr());
  const bool requireDirectory = path[path.length() - 1] == '/';
  Processor::information().getCurrentThread()->setErrno(0);
  File* target = findFilePath(normalised, targetLease, FilesystemPathRef(),
                              !(mask & LinuxInotify::DontFollow) || requireDirectory);
  if (!target) {
    if (!Processor::information().getCurrentThread()->getErrno())
      SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  if (((mask & LinuxInotify::OnlyDirectory) || requireDirectory) && !target->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return -1;
  }
  if (!VFS::checkAccess(target, true, false, false)) {
    return -1;
  }
  return instance->addWatch(target, mask);
}

int posix_inotify_add_watch(int fd, const char* pathname, uint32_t mask) {
  const InotifyResult result = addWatch(fd, pathname, mask);
  syscallError(result.error);
  return result.value;
}

int posix_inotify_rm_watch(int fd, int wd) {
  DescriptorLease descriptor;
  Process* process = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  SharedPointer<InotifyInstance> instance = descriptor->getInotifyImpl();
  if (!instance) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return instance->removeWatch(wd);
}
