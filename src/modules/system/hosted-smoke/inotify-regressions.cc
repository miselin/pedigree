/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

#include <fcntl.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/epoll-syscalls.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/subsys/posix/inotify-syscalls.h"
#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/VFS.h"

namespace {
constexpr size_t HostedAttempts = 10000;
constexpr uint64_t InotifyEpollData = 0x494E4F5449465901ULL;

class InotifyLifetimeFile final : public File {
 public:
  explicit InotifyLifetimeFile(Atomic<size_t>& destructions)
      : File(String("oneshot"), 0, 0, 0, 0, nullptr, 0, nullptr), m_Destructions(destructions) {}

  ~InotifyLifetimeFile() override {
    m_Destructions += 1;
  }

 private:
  Atomic<size_t>& m_Destructions;
};

class FileEventCloseProbe final : public File {
 public:
  FileEventCloseProbe() : File(String("close-probe"), 0, 0, 0, 0, nullptr, 0, nullptr) {}

  void closeEventsForTest() {
    closeFileEvents();
  }
};

class BlockingFileEventObserver final : public FileEventObserver {
 public:
  BlockingFileEventObserver()
      : entered(0, false), release(0, false), calls(0), lastMask(FileEvents::None) {}

  void fileEvent(const FileEvent& event) override {
    calls += 1;
    lastMask = event.mask;
    entered.release();
    const bool released = release.acquireForCompletion();
    (void)released;
  }

  Semaphore entered;
  Semaphore release;
  Atomic<size_t> calls;
  Atomic<FileEventMask> lastMask;
};

bool readEvent(const SharedPointer<InotifyInstance>& instance, int expectedWd,
               uint32_t expectedMask, bool exactMask = true) {
  uint64_t storage[8] = {};
  const int result =
      instance->readEvents(reinterpret_cast<uint8_t*>(storage), sizeof(storage), false);
  if (result != static_cast<int>(sizeof(LinuxInotifyEvent))) {
    return false;
  }
  const LinuxInotifyEvent* event = reinterpret_cast<const LinuxInotifyEvent*>(storage);
  const bool maskMatches =
      exactMask ? event->mask == expectedMask : (event->mask & expectedMask) == expectedMask;
  return event->wd == expectedWd && maskMatches && event->cookie == 0 && event->len == 0;
}

bool readEventPair(const SharedPointer<InotifyInstance>& instance, int expectedWd,
                   uint32_t firstMask, uint32_t secondMask) {
  uint64_t storage[8] = {};
  const int result =
      instance->readEvents(reinterpret_cast<uint8_t*>(storage), sizeof(storage), false);
  if (result != static_cast<int>(2 * sizeof(LinuxInotifyEvent))) {
    return false;
  }
  const LinuxInotifyEvent* first = reinterpret_cast<const LinuxInotifyEvent*>(storage);
  const LinuxInotifyEvent* second = reinterpret_cast<const LinuxInotifyEvent*>(
      reinterpret_cast<const uint8_t*>(storage) + sizeof(LinuxInotifyEvent));
  return first->wd == expectedWd && first->mask == firstMask && first->cookie == 0 &&
         first->len == 0 && second->wd == expectedWd && second->mask == secondMask &&
         second->cookie == 0 && second->len == 0;
}

struct InotifyRegressionContext {
  InotifyRegressionContext(File* watchedFile, Directory* watchedDirectory,
                           InotifyLifetimeFile* oneShotFile, InotifyLifetimeFile* deletedFile,
                           File* overflowFile, const SharedPointer<EpollInstance>& epollInstance)
      : watchedFile(watchedFile),
        watchedDirectory(watchedDirectory),
        oneShotFile(oneShotFile),
        deletedFile(deletedFile),
        overflowFile(overflowFile),
        epoll(epollInstance),
        waitEntryGate(0, false),
        setupPassed(0),
        waitEntered(0),
        returned(0),
        passed(false),
        inotifyFd(-1),
        oneShotWd(-1) {}

  File* watchedFile;
  Directory* watchedDirectory;
  InotifyLifetimeFile* oneShotFile;
  InotifyLifetimeFile* deletedFile;
  File* overflowFile;
  SharedPointer<EpollInstance> epoll;
  Semaphore waitEntryGate;
  Atomic<size_t> setupPassed;
  Atomic<size_t> waitEntered;
  Atomic<size_t> returned;
  bool passed;
  int inotifyFd;
  int oneShotWd;
};

int exerciseInotify(void* parameter) {
  InotifyRegressionContext* context = reinterpret_cast<InotifyRegressionContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  bool passed = true;

  thread->setErrno(0);
  const int invalid = posix_inotify_init1(0x40000000);
  passed &= invalid == -1 && thread->getErrno() == Error::InvalidArgument;

  context->inotifyFd = posix_inotify_init1(LinuxInotify::NonBlock | LinuxInotify::CloseOnExec);
  DescriptorLease descriptor;
  SharedPointer<InotifyInstance> instance;
  if (context->inotifyFd >= 0 && static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem())
                                     ->acquireFileDescriptor(context->inotifyFd, descriptor)) {
    instance = descriptor->getInotifyImpl();
    passed &= descriptor->getFlags() == FD_CLOEXEC;
    passed &= descriptor->getStatusFlags() == (O_RDONLY | O_NONBLOCK);
  } else {
    passed = false;
  }
  descriptor.reset();

  int fileWd = -1;
  int directoryWd = -1;
  if (instance) {
    uint8_t shortBuffer[1] = {};
    thread->setErrno(0);
    passed &= instance->readEvents(shortBuffer, sizeof(shortBuffer), false) == -1 &&
              thread->getErrno() == Error::NoMoreProcesses;
    thread->setErrno(0);
    passed &= posix_read(context->inotifyFd, reinterpret_cast<char*>(1),
                         sizeof(LinuxInotifyEvent)) == -1 &&
              thread->getErrno() == Error::NoMoreProcesses;
    thread->setErrno(0);
    passed &= instance->addWatch(context->watchedFile, 0) == -1 &&
              thread->getErrno() == Error::InvalidArgument;
    thread->setErrno(0);
    passed &= instance->addWatch(context->watchedFile, LinuxInotify::Modify | 0x00800000U) == -1 &&
              thread->getErrno() == Error::InvalidArgument;
    const int outputOnlyWd = instance->addWatch(context->watchedFile, LinuxInotify::QueueOverflow);
    passed &= outputOnlyWd > 0 && instance->removeWatch(outputOnlyWd) == 0;
    passed &= readEvent(instance, outputOnlyWd, LinuxInotify::Ignored);
    fileWd = instance->addWatch(context->watchedFile, LinuxInotify::Modify);
    thread->setErrno(0);
    passed &= instance->addWatch(context->watchedFile,
                                 LinuxInotify::Modify | LinuxInotify::MaskCreate) == -1 &&
              thread->getErrno() == Error::FileExists;
    thread->setErrno(0);
    passed &=
        instance->addWatch(context->watchedFile, LinuxInotify::Modify | LinuxInotify::MaskCreate |
                                                     LinuxInotify::MaskAdd) == -1 &&
        thread->getErrno() == Error::InvalidArgument;
    const int mergedWd =
        instance->addWatch(context->watchedFile, LinuxInotify::Attributes | LinuxInotify::MaskAdd);
    passed &= fileWd > 0 && mergedWd == fileWd;

    context->watchedFile->publishEvent(FileEvents::Attributes);
    context->watchedFile->publishEvent(FileEvents::Modify);
    thread->setErrno(0);
    passed &= instance->readEvents(shortBuffer, sizeof(shortBuffer), false) == -1 &&
              thread->getErrno() == Error::InvalidArgument;
    thread->setErrno(0);
    passed &= posix_read(context->inotifyFd, reinterpret_cast<char*>(1),
                         sizeof(LinuxInotifyEvent)) == -1 &&
              thread->getErrno() == Error::BadAddress && instance->queryReady() == ReadyRead;
    uint64_t remainingStorage[4] = {};
    thread->setErrno(0);
    const int remainingResult = posix_read(
        context->inotifyFd, reinterpret_cast<char*>(remainingStorage), sizeof(remainingStorage));
    const LinuxInotifyEvent* remainingEvent =
        reinterpret_cast<const LinuxInotifyEvent*>(remainingStorage);
    passed &= remainingResult == static_cast<int>(sizeof(LinuxInotifyEvent)) &&
              thread->getErrno() == 0 && remainingEvent->wd == fileWd &&
              remainingEvent->mask == LinuxInotify::Modify && remainingEvent->cookie == 0 &&
              remainingEvent->len == 0 && instance->queryReady() == ReadyNone;
    context->watchedFile->publishEvent(FileEvents::Attributes);
    passed &= readEvent(instance, fileWd, LinuxInotify::Attributes);

    // IN_MASK_ADD must preserve the old mask without broadening it or
    // accidentally adding IN_ONESHOT.
    context->watchedFile->publishEvent(FileEvents::Open);
    passed &= instance->queryReady() == ReadyNone;

    LinuxEpollEvent interest = {
        LinuxEpoll::In | LinuxEpoll::EdgeTriggered,
        InotifyEpollData,
    };
    passed &= context->epoll->control(LinuxEpoll::ControlAdd, context->inotifyFd, &interest) == 0;
  }

  context->setupPassed = passed ? 1 : 0;
  context->waitEntered = passed ? 1 : 0;
  context->waitEntryGate.release();
  if (!passed) {
    context->returned += 1;
    return 1;
  }

  LinuxEpollEvent ready = {};
  const int waitResult = context->epoll->wait(&ready, 1, 5000);
  passed &= waitResult == 1 && ready.events == LinuxEpoll::In && ready.data == InotifyEpollData;
  passed &= readEvent(instance, fileWd, LinuxInotify::Modify);

  context->watchedFile->publishEvent(FileEvents::Attributes);
  passed &= readEvent(instance, fileWd, LinuxInotify::Attributes);
  context->watchedFile->publishEvent(FileEvents::Modify);
  context->watchedFile->publishEvent(FileEvents::Modify);
  passed &= readEvent(instance, fileWd, LinuxInotify::Modify);
  const int replacedWd = instance->addWatch(context->watchedFile, LinuxInotify::Attributes);
  context->watchedFile->publishEvent(FileEvents::Modify);
  passed &= replacedWd == fileWd && instance->queryReady() == ReadyNone;
  context->watchedFile->publishEvent(FileEvents::Attributes);
  passed &= readEvent(instance, fileWd, LinuxInotify::Attributes);

  directoryWd = instance->addWatch(context->watchedDirectory,
                                   LinuxInotify::Attributes | LinuxInotify::DeleteSelf);
  context->watchedDirectory->publishEvent(FileEvents::Attributes);
  passed &= directoryWd > 0 &&
            readEvent(instance, directoryWd, LinuxInotify::Attributes | LinuxInotify::IsDirectory);
  context->watchedDirectory->publishEvent(FileEvents::DeletedSelf);
  passed &= readEventPair(instance, directoryWd, LinuxInotify::DeleteSelf, LinuxInotify::Ignored);
  thread->setErrno(0);
  passed &=
      instance->removeWatch(directoryWd) == -1 && thread->getErrno() == Error::InvalidArgument;

  const int deletedWd = instance->addWatch(context->deletedFile, LinuxInotify::Modify);
  context->deletedFile->publishEvent(FileEvents::DeletedSelf);
  passed &= deletedWd > 0 && readEvent(instance, deletedWd, LinuxInotify::Ignored);
  thread->setErrno(0);
  passed &= instance->addWatch(context->deletedFile, LinuxInotify::Modify) == -1 &&
            thread->getErrno() == Error::DoesNotExist;

  context->oneShotWd =
      instance->addWatch(context->oneShotFile, LinuxInotify::Modify | LinuxInotify::OneShot);
  context->oneShotFile->publishEvent(FileEvents::Modify);
  context->oneShotFile->publishEvent(FileEvents::Modify);
  passed &= context->oneShotWd > 0 && readEventPair(instance, context->oneShotWd,
                                                    LinuxInotify::Modify, LinuxInotify::Ignored);
  thread->setErrno(0);
  passed &= instance->removeWatch(context->oneShotWd) == -1 &&
            thread->getErrno() == Error::InvalidArgument;

  passed &= instance->removeWatch(fileWd) == 0;
  passed &= readEvent(instance, fileWd, LinuxInotify::Ignored);

  SharedPointer<InotifyInstance> overflow(new InotifyInstance);
  const int overflowWd =
      overflow->addWatch(context->overflowFile, LinuxInotify::Modify | LinuxInotify::Attributes);
  for (size_t i = 0; i < 16384 && overflowWd > 0; ++i) {
    context->overflowFile->publishEvent(i & 1 ? FileEvents::Attributes : FileEvents::Modify);
  }
  // Linux checks the queue limit before duplicate merging, so an event that is
  // identical to the full queue's tail must still generate overflow.
  context->overflowFile->publishEvent(FileEvents::Attributes);
  size_t normalEvents = 0;
  size_t overflowEvents = 0;
  while (normalEvents + overflowEvents < 16385) {
    uint64_t storage[128] = {};
    const int amount =
        overflow->readEvents(reinterpret_cast<uint8_t*>(storage), sizeof(storage), false);
    if (amount <= 0 || amount % static_cast<int>(sizeof(LinuxInotifyEvent))) {
      passed = false;
      break;
    }
    for (size_t offset = 0; offset < static_cast<size_t>(amount);
         offset += sizeof(LinuxInotifyEvent)) {
      const LinuxInotifyEvent* event = reinterpret_cast<const LinuxInotifyEvent*>(
          reinterpret_cast<const uint8_t*>(storage) + offset);
      if (event->wd == -1 && event->mask == LinuxInotify::QueueOverflow) {
        ++overflowEvents;
      } else if (event->wd == overflowWd &&
                 (event->mask == LinuxInotify::Modify || event->mask == LinuxInotify::Attributes)) {
        ++normalEvents;
      } else {
        passed = false;
      }
    }
  }
  passed &= normalEvents == 16384 && overflowEvents == 1;
  passed &= overflow->removeWatch(overflowWd) == 0;
  passed &= readEvent(overflow, overflowWd, LinuxInotify::Ignored);
  overflow.reset();

  context->passed = passed;
  context->returned += 1;
  return passed ? 0 : 1;
}

struct FileEventCloseContext {
  FileEventCloseContext(FileEventCloseProbe* source, bool terminal)
      : source(source), terminal(terminal), publishReturned(0), closeEntered(0), closeReturned(0) {}

  FileEventCloseProbe* source;
  bool terminal;
  Atomic<size_t> publishReturned;
  Atomic<size_t> closeEntered;
  Atomic<size_t> closeReturned;
};

int publishBlockingFileEvent(void* parameter) {
  FileEventCloseContext* context = reinterpret_cast<FileEventCloseContext*>(parameter);
  context->source->publishEvent(context->terminal ? FileEvents::DeletedSelf : FileEvents::Modify);
  context->publishReturned += 1;
  return 0;
}

int closeBlockingFileEvents(void* parameter) {
  FileEventCloseContext* context = reinterpret_cast<FileEventCloseContext*>(parameter);
  context->closeEntered += 1;
  context->source->closeEventsForTest();
  context->closeReturned += 1;
  return 0;
}

bool fileEventCloseDrain(Process* kernelProcess, bool terminal) {
  FileEventCloseProbe* source = new FileEventCloseProbe;
  BlockingFileEventObserver* concreteObserver = new BlockingFileEventObserver;
  SharedPointer<FileEventObserver> observer(concreteObserver);
  FileEventSubscription subscription;
  const FileEventMask expectedMask = terminal ? FileEvents::DeletedSelf : FileEvents::Modify;
  bool passed = source->subscribeFileEvents(expectedMask, observer, subscription);
  FileEventCloseContext context(source, terminal);
  Thread* publisher =
      new Thread(kernelProcess, publishBlockingFileEvent, &context, nullptr, false, true, true);
  publisher->setName("hosted file-event blocking publisher");
  const bool publisherStarted = publisher->start();
  const bool callbackEntered = publisherStarted && concreteObserver->entered.acquireForCompletion();

  Thread* closer = nullptr;
  bool closerStarted = false;
  if (callbackEntered) {
    closer =
        new Thread(kernelProcess, closeBlockingFileEvents, &context, nullptr, false, true, true);
    closer->setName("hosted file-event closer");
    closerStarted = closer->start();
  }
  bool closeBlocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && closerStarted; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (context.closeEntered && !context.closeReturned && closer->getWaitDebugInfo(info) &&
        info.queue && info.queued && closer->getStatus() == Thread::Sleeping) {
      closeBlocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  concreteObserver->release.release();
  const bool publisherJoined = publisherStarted && publisher->joinForCompletion();
  const bool closerJoined = closerStarted && closer->joinForCompletion();
  if (!publisherStarted) {
    delete publisher;
  }
  if (closer && !closerStarted) {
    delete closer;
  }
  source->publishEvent(FileEvents::Modify);
  subscription.reset();
  passed &= publisherStarted && callbackEntered && closerStarted && closeBlocked &&
            publisherJoined && closerJoined && context.publishReturned == static_cast<size_t>(1) &&
            context.closeReturned == static_cast<size_t>(1) &&
            concreteObserver->calls == static_cast<size_t>(1) &&
            concreteObserver->lastMask == expectedMask;
  observer.reset();
  delete source;
  return passed;
}

struct InotifyReadCloseContext {
  InotifyReadCloseContext()
      : ready(0, false), returned(0), fd(-1), alias(-1), result(-2), error(0) {}

  Semaphore ready;
  Atomic<size_t> returned;
  int fd;
  int alias;
  int result;
  int error;
};

int blockOnInotifyRead(void* parameter) {
  InotifyReadCloseContext* context = reinterpret_cast<InotifyReadCloseContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  context->fd = posix_inotify_init();
  context->alias = context->fd >= 0 ? posix_dup(context->fd) : -1;
  context->ready.release();
  uint64_t storage[8] = {};
  thread->setErrno(0);
  context->result = posix_read(context->fd, reinterpret_cast<char*>(storage), sizeof(storage));
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

bool blockingReadFinalClose(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  InotifyReadCloseContext context;
  Thread* reader = new Thread(process, blockOnInotifyRead, &context, nullptr, false, true, true);
  reader->setName("hosted inotify close waiter");
  const bool started = reader->start();
  const bool ready = started && context.ready.acquireForCompletion();

  bool readBlocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && ready; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (reader->getWaitDebugInfo(info) && info.queue && info.queued &&
        reader->getStatus() == Thread::Sleeping) {
      readBlocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  auto closeDescriptor = [&](int fd) {
    DescriptorLease descriptor;
    const bool acquired = subsystem->acquireFileDescriptor(fd, descriptor);
    const bool closed = acquired && subsystem->closeFileDescriptor(fd, descriptor);
    descriptor.reset();
    return closed;
  };
  const bool originalClosed = readBlocked && closeDescriptor(context.fd);
  for (size_t attempt = 0; attempt < 256 && !context.returned; ++attempt) {
    Scheduler::instance().yield();
  }
  const bool aliasKeptOpen = !context.returned;
  const bool aliasClosed = aliasKeptOpen && closeDescriptor(context.alias);
  const bool joined = started && reader->joinForCompletion();
  if (!started) {
    delete reader;
  }

  const bool passed = started && ready && readBlocked && originalClosed && aliasKeptOpen &&
                      aliasClosed && joined && context.returned == static_cast<size_t>(1) &&
                      context.result == -1 && context.error == Error::BadFileDescriptor;
  delete process;
  return passed;
}

struct InotifyBlockingWakeContext {
  explicit InotifyBlockingWakeContext(File* watched)
      : watched(watched), ready(0, false), returned(0), result(-2), wd(-1), event() {}

  File* watched;
  Semaphore ready;
  Atomic<size_t> returned;
  int result;
  int wd;
  LinuxInotifyEvent event;
};

int blockUntilInotifyEvent(void* parameter) {
  InotifyBlockingWakeContext* context = reinterpret_cast<InotifyBlockingWakeContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  const int fd = posix_inotify_init();
  DescriptorLease descriptor;
  SharedPointer<InotifyInstance> instance;
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  if (fd >= 0 && subsystem->acquireFileDescriptor(fd, descriptor)) {
    instance = descriptor->getInotifyImpl();
  }
  if (instance) {
    context->wd = instance->addWatch(context->watched, LinuxInotify::Modify);
  }
  descriptor.reset();
  context->ready.release();
  if (fd >= 0 && context->wd > 0) {
    context->result =
        posix_read(fd, reinterpret_cast<char*>(&context->event), sizeof(context->event));
  }
  context->returned += 1;
  return 0;
}

bool blockingReadEventWake(Process* kernelProcess) {
  File* watched = new File(String("blocking-read"), 0, 0, 0, 0, nullptr, 0, nullptr);
  VFS::instance().trackFile(watched);
  Process* process = new Process(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  InotifyBlockingWakeContext context(watched);
  Thread* reader =
      new Thread(process, blockUntilInotifyEvent, &context, nullptr, false, true, true);
  reader->setName("hosted inotify event waiter");
  const bool started = reader->start();
  const bool ready = started && context.ready.acquireForCompletion();

  bool readBlocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && ready && context.wd > 0; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (reader->getWaitDebugInfo(info) && info.queue && info.queued &&
        reader->getStatus() == Thread::Sleeping) {
      readBlocked = true;
      break;
    }
    Scheduler::instance().yield();
  }
  if (ready) {
    watched->publishEvent(FileEvents::Modify);
  }
  const bool joined = started && reader->joinForCompletion();
  if (!started) {
    delete reader;
  }
  const bool passed =
      started && ready && readBlocked && joined && context.returned == static_cast<size_t>(1) &&
      context.result == static_cast<int>(sizeof(LinuxInotifyEvent)) &&
      context.event.wd == context.wd && context.event.mask == LinuxInotify::Modify &&
      context.event.cookie == 0 && context.event.len == 0;
  delete process;
  VFS::instance().untrackFile(watched);
  return passed;
}
}  // namespace

bool runHostedInotifyRegressions(Process* kernelProcess) {
  Atomic<size_t> oneShotDestructions(0);
  Atomic<size_t> deletedDestructions(0);
  File* watchedFile = new File(String("watched"), 0, 0, 0, 0, nullptr, 0, nullptr);
  Directory* watchedDirectory =
      new Directory(String("watched-dir"), 0, 0, 0, 0, nullptr, 0, nullptr);
  InotifyLifetimeFile* oneShotFile = new InotifyLifetimeFile(oneShotDestructions);
  InotifyLifetimeFile* deletedFile = new InotifyLifetimeFile(deletedDestructions);
  File* overflowFile = new File(String("overflow"), 0, 0, 0, 0, nullptr, 0, nullptr);
  VFS::instance().trackFile(watchedFile);
  VFS::instance().trackFile(watchedDirectory);
  VFS::instance().trackFile(oneShotFile);
  VFS::instance().trackFile(deletedFile);
  VFS::instance().trackFile(overflowFile);

  Process* process = new Process(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  SharedPointer<EpollInstance> epoll(new EpollInstance);
  InotifyRegressionContext context(watchedFile, watchedDirectory, oneShotFile, deletedFile,
                                   overflowFile, epoll);
  Thread* worker = new Thread(process, exerciseInotify, &context, nullptr, false, true, true);
  worker->setName("hosted inotify regression worker");
  const bool started = worker->start();
  const bool waitEntered = started && context.waitEntryGate.acquireForCompletion();

  bool waitBlocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && waitEntered && context.setupPassed;
       ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (worker->getWaitDebugInfo(info) && info.queue && info.queued &&
        worker->getStatus() == Thread::Sleeping) {
      waitBlocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  if (waitEntered && context.waitEntered) {
    watchedFile->publishEvent(FileEvents::Modify);
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  // A successful one-shot read must have reaped the subscription and its VFS
  // lease even while the inotify descriptor itself remains open.
  const bool oneShotWasFinal = VFS::instance().untrackFile(oneShotFile);
  const bool oneShotRetired = oneShotWasFinal && oneShotDestructions == static_cast<size_t>(1);
  const bool deletedWasFinal = VFS::instance().untrackFile(deletedFile);
  const bool deletedRetired = deletedWasFinal && deletedDestructions == static_cast<size_t>(1);

  const bool passed = started && waitEntered && waitBlocked && joined &&
                      context.returned == static_cast<size_t>(1) && context.passed &&
                      oneShotRetired && deletedRetired &&
                      fileEventCloseDrain(kernelProcess, false) &&
                      fileEventCloseDrain(kernelProcess, true) &&
                      blockingReadFinalClose(kernelProcess) && blockingReadEventWake(kernelProcess);
  delete process;
  epoll.reset();
  VFS::instance().untrackFile(watchedFile);
  VFS::instance().untrackFile(watchedDirectory);
  VFS::instance().untrackFile(overflowFile);

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL inotify-vfs-epoll-lifetime: "
        "ABI, queue, epoll wakeup, terminal event, callback drain, or close lifetime failed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS inotify-vfs-epoll-lifetime");
  return true;
}
