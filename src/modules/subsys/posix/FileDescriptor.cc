/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "FileDescriptor.h"

#include <fcntl.h>

#include "modules/subsys/posix/IoEvent.h"
#include "modules/subsys/posix/epoll-syscalls.h"
#include "modules/subsys/posix/eventfd-syscalls.h"
#include "modules/subsys/posix/inotify-syscalls.h"
#include "modules/subsys/posix/mqueue-syscalls.h"
#include "modules/subsys/posix/signalfd-syscalls.h"
#include "modules/subsys/posix/timerfd-syscalls.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/VFS.h"
#include "net-syscalls.h"  // to get destructor for SharedPointer<NetworkSyscalls>

#define ENABLE_LOCKED_FILES 0

namespace {
constexpr int MutableStatusFlags = O_APPEND | O_NONBLOCK;

bool isReadWrite(int flags) {
  return (flags & O_ACCMODE) == O_RDWR;
}

void increaseFileReferences(File* file, int flags) {
  if (!file) {
    return;
  }

  const bool writer = (flags & O_ACCMODE) != O_RDONLY;
  file->increaseRefCount(writer);

  // The historical boolean API cannot express that O_RDWR is both ends of a
  // FIFO. Account its reader side separately so it satisfies waiting writers.
  if (isReadWrite(flags) && (file->isPipe() || file->isFifo())) {
    file->increaseRefCount(false);
  }
}

void decreaseFileReferences(File* file, int flags) {
  if (!file) {
    return;
  }

  const bool writer = (flags & O_ACCMODE) != O_RDONLY;
  file->decreaseRefCount(writer);
  if (isReadWrite(flags) && (file->isPipe() || file->isFifo())) {
    file->decreaseRefCount(false);
  }
}

void retireIoEvent(File* file, SharedPointer<NetworkSyscalls>& networkImpl, IoEvent*& ioevent) {
  if (!ioevent) {
    return;
  }

  IoEvent* retiring = ioevent;
  ioevent = nullptr;
  Event::Retirement retirement;
  retiring->beginRetirement(retirement);

  // Close admission before removing raw registry pointers. A callback which
  // is already active can no longer re-arm itself after this pass.
  if (file) {
    file->cullMonitorTargets(retiring);
  }
  if (networkImpl) {
    networkImpl->unmonitor(retiring);
  }
}
}  // namespace

#if ENABLE_LOCKED_FILES
RadixTree<LockedFile*> g_PosixGlobalLockedFiles;
#endif

FileDescriptor::OpenFileDescription::OpenFileDescription(File* newFile, uint64_t initialOffset,
                                                         int initialStatusFlags)
    : lock(),
      file(newFile),
      networkImpl(nullptr),
      eventFdImpl(nullptr),
      inotifyImpl(nullptr),
      mqueueImpl(nullptr),
      offset(initialOffset),
      statusFlags(initialStatusFlags),
      descriptorOwners(1),
      vfsLease(newFile && newFile->retainVfsReference()) {
  increaseFileReferences(file, statusFlags);
}

FileDescriptor::OpenFileDescription::~OpenFileDescription() {
  assert(!descriptorOwners);
  if (vfsLease) {
    file->releaseVfsReference();
  }
}

File* FileDescriptor::OpenFileDescription::getFile() const {
  return file;
}

SharedPointer<NetworkSyscalls> FileDescriptor::OpenFileDescription::getNetworkImpl() const {
  LockGuard<Mutex> guard(lock);
  return networkImpl;
}

SharedPointer<EventFd> FileDescriptor::OpenFileDescription::getEventFdImpl() const {
  LockGuard<Mutex> guard(lock);
  return eventFdImpl;
}

SharedPointer<TimerFd> FileDescriptor::OpenFileDescription::getTimerFdImpl() const {
  LockGuard<Mutex> guard(lock);
  return timerFdImpl;
}

SharedPointer<SignalFd> FileDescriptor::OpenFileDescription::getSignalFdImpl() const {
  LockGuard<Mutex> guard(lock);
  return signalFdImpl;
}

SharedPointer<InotifyInstance> FileDescriptor::OpenFileDescription::getInotifyImpl() const {
  LockGuard<Mutex> guard(lock);
  return inotifyImpl;
}

size_t FileDescriptor::OpenFileDescription::descriptorOwnerCount() const {
  LockGuard<Mutex> guard(lock);
  return descriptorOwners;
}

void FileDescriptor::OpenFileDescription::addDescriptorOwner() {
  LockGuard<Mutex> guard(lock);
  assert(descriptorOwners);
  ++descriptorOwners;
}

void FileDescriptor::OpenFileDescription::removeDescriptorOwner() {
  bool closeEndpoint = false;
  int flags = 0;
  SharedPointer<NetworkSyscalls> closingNetwork;
  SharedPointer<InotifyInstance> closingInotify;
  {
    LockGuard<Mutex> guard(lock);
    assert(descriptorOwners);
    --descriptorOwners;
    closeEndpoint = descriptorOwners == 0;
    flags = statusFlags;
    if (closeEndpoint) {
      closingNetwork = networkImpl;
      closingInotify = inotifyImpl;
    }
  }
  if (closeEndpoint) {
    decreaseFileReferences(file, flags);
    if (file) {
      file->publishEvent((flags & O_ACCMODE) == O_RDONLY ? FileEvents::CloseNoWrite
                                                         : FileEvents::CloseWrite);
    }
    if (closingNetwork) {
      closingNetwork->lastDescriptorClosed();
    }
    if (closingInotify) {
      closingInotify->lastDescriptorClosed();
    }
  }
}

void FileDescriptor::OpenFileDescription::ensureVfsLease() {
  LockGuard<Mutex> guard(lock);
  if (!vfsLease && file) {
    vfsLease = file->retainVfsReference();
  }
}

/// Default constructor
FileDescriptor::FileDescriptor()
    : file(0),
      fd(0xFFFFFFFF),
      lockedFile(0),
      networkImpl(nullptr),
      epollImpl(nullptr),
      ioevent(nullptr),
      fdflags(0),
      m_OpenFile(new OpenFileDescription(nullptr, 0, 0)),
      m_NetworkPublished(false),
      m_EventFdPublished(false) {}

/// Parameterised constructor
FileDescriptor::FileDescriptor(File* newFile, uint64_t newOffset, size_t newFd, int fdFlags,
                               int flFlags, LockedFile* lf)
    : file(newFile),
      fd(newFd),
      lockedFile(lf),
      networkImpl(nullptr),
      epollImpl(nullptr),
      ioevent(nullptr),
      fdflags(fdFlags | ((flFlags & O_CLOEXEC) ? FD_CLOEXEC : 0)),
      m_OpenFile(new OpenFileDescription(newFile, newOffset, flFlags & ~O_CLOEXEC)),
      m_NetworkPublished(false),
      m_EventFdPublished(false) {
  /// \todo need a copy constructor for networkImpl
  if (file) {
#if ENABLE_LOCKED_FILES
    lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
#endif
  }
}

/// Copy constructor
FileDescriptor::FileDescriptor(FileDescriptor& desc)
    : file(desc.file),
      fd(desc.fd),
      lockedFile(0),
      networkImpl(desc.networkImpl),
      epollImpl(desc.epollImpl),
      ioevent(nullptr),
      fdflags(desc.fdflags),
      m_OpenFile(desc.m_OpenFile),
      m_NetworkPublished(false),
      m_EventFdPublished(false) {
  m_OpenFile->addDescriptorOwner();
  m_OpenFile->ensureVfsLease();
  if (networkImpl) {
    m_NetworkPublished = networkImpl->addDescriptorOwner();
  }
  SharedPointer<EventFd> eventFd = m_OpenFile->getEventFdImpl();
  if (eventFd) {
    m_EventFdPublished = eventFd->addDescriptorOwner();
  }
  auto timerFd = m_OpenFile->getTimerFdImpl();
  if (timerFd) {
    m_TimerFdPublished = timerFd->addDescriptorOwner();
  }
  auto signalFd = m_OpenFile->getSignalFdImpl();
  if (signalFd) {
    m_SignalFdPublished = signalFd->addDescriptorOwner();
  }
  if (file) {
#if ENABLE_LOCKED_FILES
    lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
#endif
  }

#if THREADS
  if (desc.ioevent) {
    ioevent = new IoEvent(*desc.ioevent);
  }
#endif
}

/// Pointer copy constructor
FileDescriptor::FileDescriptor(FileDescriptor* desc)
    : file(0),
      fd(0),
      lockedFile(0),
      networkImpl(nullptr),
      epollImpl(nullptr),
      ioevent(nullptr),
      fdflags(0),
      m_OpenFile(nullptr),
      m_NetworkPublished(false),
      m_EventFdPublished(false) {
  if (!desc) {
    m_OpenFile.reset(new OpenFileDescription(nullptr, 0, 0));
    return;
  }

  file = desc->file;
  fd = desc->fd;
  fdflags = desc->fdflags;
  networkImpl = desc->networkImpl;
  epollImpl = desc->epollImpl;
  m_OpenFile = desc->m_OpenFile;
  m_OpenFile->addDescriptorOwner();
  m_OpenFile->ensureVfsLease();
  if (networkImpl) {
    m_NetworkPublished = networkImpl->addDescriptorOwner();
  }
  SharedPointer<EventFd> eventFd = m_OpenFile->getEventFdImpl();
  if (eventFd) {
    m_EventFdPublished = eventFd->addDescriptorOwner();
  }
  auto timerFd = m_OpenFile->getTimerFdImpl();
  if (timerFd) {
    m_TimerFdPublished = timerFd->addDescriptorOwner();
  }
  auto signalFd = m_OpenFile->getSignalFdImpl();
  if (signalFd) {
    m_SignalFdPublished = signalFd->addDescriptorOwner();
  }
  if (file) {
#if ENABLE_LOCKED_FILES
    lockedFile = g_PosixGlobalLockedFiles.lookup(file->getFullPath());
#endif
  }

#if THREADS
  if (desc->ioevent) {
    ioevent = new IoEvent(*desc->ioevent);
  }
#endif
}

/// Destructor - decreases file reference count
FileDescriptor::~FileDescriptor() {
  unpublish();
#if THREADS
  retireIoEvent(file, networkImpl, ioevent);
#endif

  if (file) {
#if ENABLE_LOCKED_FILES
    // Unlock the file we have a lock on, release from the global lock table
    if (lockedFile) {
      g_PosixGlobalLockedFiles.remove(file->getFullPath());
      lockedFile->unlock();
      delete lockedFile;
    }
#endif
  }

  m_OpenFile->removeDescriptorOwner();

  /// \note sockets are cleaned up by their reference count hitting zero
  /// (SharedPointer)
}

void FileDescriptor::setFlags(int newFlags) {
  fdflags = newFlags;
}

void FileDescriptor::addFlag(int newFlag) {
  setFlags(fdflags | newFlag);
}

int FileDescriptor::getFlags() const {
  return fdflags;
}

void FileDescriptor::setStatusFlags(int newFlags) {
  LockGuard<Mutex> guard(m_OpenFile->lock);
  m_OpenFile->statusFlags =
      (m_OpenFile->statusFlags & ~MutableStatusFlags) | (newFlags & MutableStatusFlags);

  if (m_OpenFile->networkImpl) {
    bool nonblock = (m_OpenFile->statusFlags & O_NONBLOCK) == O_NONBLOCK;
    m_OpenFile->networkImpl->setBlocking(!nonblock);
  }
}

void FileDescriptor::addStatusFlag(int newFlag) {
  LockGuard<Mutex> guard(m_OpenFile->lock);
  m_OpenFile->statusFlags |= newFlag & MutableStatusFlags;
  if (m_OpenFile->networkImpl) {
    m_OpenFile->networkImpl->setBlocking(!(m_OpenFile->statusFlags & O_NONBLOCK));
  }
}

void FileDescriptor::removeStatusFlag(int flag) {
  LockGuard<Mutex> guard(m_OpenFile->lock);
  m_OpenFile->statusFlags &= ~(flag & MutableStatusFlags);
  if (m_OpenFile->networkImpl) {
    m_OpenFile->networkImpl->setBlocking(!(m_OpenFile->statusFlags & O_NONBLOCK));
  }
}

int FileDescriptor::getStatusFlags() const {
  LockGuard<Mutex> guard(m_OpenFile->lock);
  return m_OpenFile->statusFlags;
}

FileDescriptor::OpenFileDescriptionLease FileDescriptor::acquireOpenFileDescription() const {
  return m_OpenFile;
}

void FileDescriptor::setNetworkImpl(const SharedPointer<NetworkSyscalls>& implementation) {
  LockGuard<Mutex> guard(m_OpenFile->lock);
  networkImpl = implementation;
  m_OpenFile->networkImpl = implementation;
  if (networkImpl) {
    networkImpl->setBlocking(!(m_OpenFile->statusFlags & O_NONBLOCK));
  }
  if (implementation) {
    m_NetworkPublished = implementation->addDescriptorOwner();
    assert(m_NetworkPublished);
    if (m_NetworkPublished) {
      implementation->retainDescriptorLifetime(implementation);
    }
  }
}

bool FileDescriptor::networkPublished() const {
  return m_NetworkPublished;
}

void FileDescriptor::setEventFdImpl(const SharedPointer<EventFd>& implementation) {
  {
    LockGuard<Mutex> guard(m_OpenFile->lock);
    assert(!m_OpenFile->eventFdImpl);
    m_OpenFile->eventFdImpl = implementation;
  }

  if (implementation) {
    m_EventFdPublished = implementation->addDescriptorOwner();
    assert(m_EventFdPublished);
  }
}

SharedPointer<EventFd> FileDescriptor::getEventFdImpl() const {
  return m_OpenFile->getEventFdImpl();
}

bool FileDescriptor::eventFdPublished() const {
  return m_EventFdPublished;
}

void FileDescriptor::setTimerFdImpl(const SharedPointer<TimerFd>& implementation) {
  {
    LockGuard<Mutex> guard(m_OpenFile->lock);
    assert(!m_OpenFile->timerFdImpl);
    m_OpenFile->timerFdImpl = implementation;
  }
  if (implementation) {
    m_TimerFdPublished = implementation->addDescriptorOwner();
    assert(m_TimerFdPublished);
  }
}

SharedPointer<TimerFd> FileDescriptor::getTimerFdImpl() const {
  return m_OpenFile->getTimerFdImpl();
}

bool FileDescriptor::timerFdPublished() const {
  return m_TimerFdPublished;
}

void FileDescriptor::setSignalFdImpl(const SharedPointer<SignalFd>& implementation) {
  {
    LockGuard<Mutex> guard(m_OpenFile->lock);
    assert(!m_OpenFile->signalFdImpl);
    m_OpenFile->signalFdImpl = implementation;
  }
  if (implementation) {
    m_SignalFdPublished = implementation->addDescriptorOwner();
    assert(m_SignalFdPublished);
  }
}

SharedPointer<SignalFd> FileDescriptor::getSignalFdImpl() const {
  return m_OpenFile->getSignalFdImpl();
}

bool FileDescriptor::signalFdPublished() const {
  return m_SignalFdPublished;
}

void FileDescriptor::setInotifyImpl(const SharedPointer<InotifyInstance>& implementation) {
  LockGuard<Mutex> guard(m_OpenFile->lock);
  assert(!m_OpenFile->inotifyImpl);
  m_OpenFile->inotifyImpl = implementation;
}

SharedPointer<InotifyInstance> FileDescriptor::getInotifyImpl() const {
  return m_OpenFile->getInotifyImpl();
}

SharedPointer<PosixMessageQueue> FileDescriptor::OpenFileDescription::getMqueueImpl() const {
  LockGuard<Mutex> guard(lock);
  return mqueueImpl;
}

void FileDescriptor::setMqueueImpl(const SharedPointer<PosixMessageQueue>& implementation) {
  LockGuard<Mutex> guard(m_OpenFile->lock);
  assert(!m_OpenFile->mqueueImpl);
  m_OpenFile->mqueueImpl = implementation;
}

SharedPointer<PosixMessageQueue> FileDescriptor::getMqueueImpl() const {
  return m_OpenFile->getMqueueImpl();
}

void FileDescriptor::unpublish() {
  if (m_TimerFdPublished) {
    m_TimerFdPublished = false;
    auto timerFd = m_OpenFile->getTimerFdImpl();
    if (timerFd) {
      timerFd->removeDescriptorOwner();
    }
  }
  if (m_SignalFdPublished) {
    m_SignalFdPublished = false;
    auto signalFd = m_OpenFile->getSignalFdImpl();
    if (signalFd) {
      signalFd->removeDescriptorOwner();
    }
  }
  if (m_NetworkPublished) {
    m_NetworkPublished = false;
    if (networkImpl) {
      networkImpl->removeDescriptorOwner();
    }
  }

  if (m_EventFdPublished) {
    m_EventFdPublished = false;
    SharedPointer<EventFd> eventFd = m_OpenFile->getEventFdImpl();
    if (eventFd) {
      eventFd->removeDescriptorOwner();
    }
  }
}

FileDescriptor::PositionGuard::PositionGuard(const SharedPointer<OpenFileDescription>& description)
    : m_Description(description), m_Guard(m_Description->lock) {}

uint64_t FileDescriptor::PositionGuard::offset() const {
  return m_Description->offset;
}

int FileDescriptor::PositionGuard::statusFlags() const {
  return m_Description->statusFlags;
}

void FileDescriptor::PositionGuard::setOffset(uint64_t offset) {
  m_Description->offset = offset;
}

void FileDescriptor::PositionGuard::advanceOffset(uint64_t amount) {
  m_Description->offset += amount;
}

FileDescriptor::PositionGuard FileDescriptor::lockPosition() const {
  return PositionGuard(m_OpenFile);
}

uint64_t FileDescriptor::getOffset() const {
  PositionGuard position = lockPosition();
  return position.offset();
}

void FileDescriptor::setOffset(uint64_t offset) {
  PositionGuard position = lockPosition();
  position.setOffset(offset);
}

uint64_t FileDescriptor::read(uint64_t size, uintptr_t buffer, bool canBlock) {
  if (!file) {
    return 0;
  }
  if (!file->isSeekable()) {
    return file->read(0, size, buffer, canBlock && !(getStatusFlags() & O_NONBLOCK));
  }

  LockGuard<Mutex> guard(m_OpenFile->lock);
  const bool shouldBlock = canBlock && !(m_OpenFile->statusFlags & O_NONBLOCK);
  uint64_t amount = file->read(m_OpenFile->offset, size, buffer, shouldBlock);
  m_OpenFile->offset += amount;
  return amount;
}

uint64_t FileDescriptor::write(uint64_t size, uintptr_t buffer, bool canBlock) {
  if (!file) {
    return 0;
  }
  if (!file->isSeekable()) {
    return file->write(0, size, buffer, canBlock && !(getStatusFlags() & O_NONBLOCK));
  }

  LockGuard<Mutex> guard(m_OpenFile->lock);
  const bool shouldBlock = canBlock && !(m_OpenFile->statusFlags & O_NONBLOCK);
  uint64_t location = m_OpenFile->offset;
  const uint64_t amount = (m_OpenFile->statusFlags & O_APPEND)
                              ? file->append(size, buffer, location, shouldBlock)
                              : file->write(location, size, buffer, shouldBlock);
  if (amount) {
    m_OpenFile->offset = location + amount;
  }
  return amount;
}
