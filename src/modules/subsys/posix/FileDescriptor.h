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

#ifndef POSIX_FILEDESCRIPTOR_H
#define POSIX_FILEDESCRIPTOR_H

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/FilesystemContext.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/String.h"

#include "advisory-lock-state.h"

class ConsoleIoState;
class File;
class LockedFile;
class UnixSocket;
class IoEvent;
class NetworkSyscalls;
class EpollInstance;
class EventFd;
class TimerFd;
class SignalFd;
class InotifyInstance;
class FanotifyInstance;
class PosixMessageQueue;

/** Abstraction of a file descriptor, which defines an open file
 * and related flags.
 */
class EXPORTED_PUBLIC FileDescriptor {
 public:
  class PositionGuard;
  class TransferPositionGuard;

  /**
   * Shared state for one open file description. A lease keeps its file or
   * socket target alive independently of any numeric descriptor.
   */
  class OpenFileDescription {
   public:
    ~OpenFileDescription();

    AdvisoryOwner& advisoryOwner() {
      return m_AdvisoryOwner;
    }

    uint64_t identity() const {
      return m_AdvisoryOwner.identity();
    }

    File* getFile() const;
    FilesystemPathRef openingPath() const;
    ReadyMask queryFileReady(bool reading, bool writing) const;
    ReadinessGenerations fileReadinessGenerations() const;
    SharedPointer<ConsoleIoState> terminalEpoch(bool waitForReopen = true) const;
    SharedPointer<NetworkSyscalls> getNetworkImpl() const;
    SharedPointer<EventFd> getEventFdImpl() const;
    SharedPointer<TimerFd> getTimerFdImpl() const;
    SharedPointer<SignalFd> getSignalFdImpl() const;
    SharedPointer<InotifyInstance> getInotifyImpl() const;
    SharedPointer<FanotifyInstance> getFanotifyImpl() const;
    SharedPointer<PosixMessageQueue> getMqueueImpl() const;
    size_t descriptorOwnerCount() const;

   private:
    friend class FileDescriptor;
    friend class PositionGuard;
    friend class TransferPositionGuard;

    OpenFileDescription(File* file, uint64_t initialOffset, int initialStatusFlags);
    OpenFileDescription(const FilesystemPathRef& path, uint64_t initialOffset,
                        int initialStatusFlags);

    void addDescriptorOwner();
    void removeDescriptorOwner();
    void ensureVfsLease();

    AdvisoryOwner m_AdvisoryOwner;
    mutable Mutex lock;
    // Exactly one arm is set. Path-based descriptions derive their File from path.
    FilesystemPathRef path;
    File* anonymousFile;
    SharedPointer<NetworkSyscalls> networkImpl;
    SharedPointer<EventFd> eventFdImpl;
    SharedPointer<TimerFd> timerFdImpl;
    SharedPointer<SignalFd> signalFdImpl;
    SharedPointer<InotifyInstance> inotifyImpl;
    SharedPointer<FanotifyInstance> fanotifyImpl;
    SharedPointer<PosixMessageQueue> mqueueImpl;
    SharedPointer<ConsoleIoState> consoleEpoch;
    uint64_t offset;
    int statusFlags;
    size_t descriptorOwners;
    bool vfsLease;
  };

  using OpenFileDescriptionLease = SharedPointer<OpenFileDescription>;

  /** A serialized view of the offset shared by duplicated descriptors. */
  class PositionGuard {
   public:
    uint64_t offset() const;
    int statusFlags() const;
    File* file() const;
    bool isNoopSeekEndpoint() const;
    void setOffset(uint64_t offset);
    void advanceOffset(uint64_t amount);

   private:
    friend class FileDescriptor;

    explicit PositionGuard(const SharedPointer<OpenFileDescription>& description);

    SharedPointer<OpenFileDescription> m_Description;
    LockGuard<Mutex> m_Guard;
  };

  class TransferPositionGuard {
   public:
    enum class Endpoint { Input, Output };
    TransferPositionGuard(const OpenFileDescriptionLease& input,
                          const OpenFileDescriptionLease& output, bool lockInput, bool lockOutput);
    ~TransferPositionGuard();
    TransferPositionGuard(const TransferPositionGuard&) = delete;
    TransferPositionGuard& operator=(const TransferPositionGuard&) = delete;

    int statusFlags(Endpoint endpoint) const;
    uint64_t offset(Endpoint endpoint) const;
    void commitOffset(Endpoint endpoint, uint64_t finalOffset);
    bool sameDescription() const;

   private:
    OpenFileDescription& lockedDescription(Endpoint endpoint) const;

#if THREADS && !defined(STANDALONE_MUTEXES)
    TerminationDeferral m_TerminationDeferral;
#endif
    OpenFileDescriptionLease m_Input;
    OpenFileDescriptionLease m_Output;
    OpenFileDescription* m_First;
    OpenFileDescription* m_Second;
    int m_InputFlags;
    int m_OutputFlags;
  };

  /// Default constructor
  FileDescriptor();

  /// Parameterised constructor
  FileDescriptor(File* newFile, uint64_t newOffset = 0, size_t newFd = 0xFFFFFFFF, int fdFlags = 0,
                 int flFlags = 0, LockedFile* lf = 0);

  FileDescriptor(const FilesystemPathRef& path, uint64_t newOffset = 0, size_t newFd = 0xFFFFFFFF,
                 int fdFlags = 0, int flFlags = 0, LockedFile* lf = 0);

  /// Copy constructor
  FileDescriptor(FileDescriptor& desc);

  /// Pointer copy constructor
  FileDescriptor(FileDescriptor* desc);

  /// Descriptors own registrations and references which cannot be replaced.
  FileDescriptor& operator=(FileDescriptor& desc) = delete;

  /// Destructor - decreases file reference count
  virtual ~FileDescriptor();

  /// Set flags, distributing any associated changes as needed.
  void setFlags(int newFlags);

  /// Helper to add a single flag to the descriptor flags.
  void addFlag(int newFlag);

  /// Get current descriptor flags.
  int getFlags() const;

  /// Set status flags, distributing any associated changes as needed.
  void setStatusFlags(int newFlags);

  /// Helper to add a single flag to the status flags.
  void addStatusFlag(int newFlag);

  /// Helper to remove a single flag from the status flags.
  void removeStatusFlag(int flag);

  /// Get current status flags.
  int getStatusFlags() const;

  /** Retain and identify the open file description behind this descriptor. */
  OpenFileDescriptionLease acquireOpenFileDescription() const;

  /** Associate a socket implementation with this open file description. */
  void setNetworkImpl(const SharedPointer<NetworkSyscalls>& implementation);

  /** Whether this descriptor retained socket ownership during clone admission. */
  bool networkPublished() const;

  /** Associate an eventfd counter with this open file description. */
  void setEventFdImpl(const SharedPointer<EventFd>& implementation);

  /** Retain the eventfd counter behind this descriptor, if any. */
  SharedPointer<EventFd> getEventFdImpl() const;

  /** Whether this descriptor still owns a published eventfd alias. */
  bool eventFdPublished() const;

  void setTimerFdImpl(const SharedPointer<TimerFd>& implementation);
  SharedPointer<TimerFd> getTimerFdImpl() const;
  bool timerFdPublished() const;

  void setSignalFdImpl(const SharedPointer<SignalFd>& implementation);
  SharedPointer<SignalFd> getSignalFdImpl() const;
  bool signalFdPublished() const;

  /** Associate an inotify queue with this open file description. */
  void setInotifyImpl(const SharedPointer<InotifyInstance>& implementation);

  /** Retain the inotify queue behind this descriptor, if any. */
  SharedPointer<InotifyInstance> getInotifyImpl() const;
  void setFanotifyImpl(const SharedPointer<FanotifyInstance>& implementation);
  SharedPointer<FanotifyInstance> getFanotifyImpl() const;

  void setMqueueImpl(const SharedPointer<PosixMessageQueue>& implementation);
  SharedPointer<PosixMessageQueue> getMqueueImpl() const;

  /** Notify anonymous targets that this descriptor left its descriptor table. */
  void unpublish();

  /** Lock and access the offset shared by this open-file description. */
  PositionGuard lockPosition() const;

  /** Read the current shared offset. */
  uint64_t getOffset() const;

  /** Replace the current shared offset. */
  void setOffset(uint64_t offset);

  /** Read while applying this open-file description's offset policy. */
  uint64_t read(uint64_t size, uintptr_t buffer, bool canBlock = true);

  /** Write while applying this open-file description's offset policy. */
  uint64_t write(uint64_t size, uintptr_t buffer, bool canBlock = true);

  class TerminalOperation {
   public:
    TerminalOperation();
    ~TerminalOperation();

   private:
    friend class FileDescriptor;
    TerminationDeferral lifetime;
    SharedPointer<ConsoleIoState> state;
    OperationBarrier::Lease operation;
  };

  bool acquireTerminalOperation(TerminalOperation& operation) const;
  bool terminalHungUp() const;
  bool terminalAvailable() const;
  SharedPointer<ConsoleIoState> terminalEpoch(bool waitForReopen = true) const;
  uint64_t readFile(uint64_t location, uint64_t size, uintptr_t buffer, bool canBlock);
  uint64_t writeFile(uint64_t location, uint64_t size, uintptr_t buffer, bool canBlock);

  File* getFile() const;
  FilesystemPathRef openingPath() const;

  /// Descriptor number
  size_t fd;

  /// Locked file, non-zero if there is an advisory lock on the file
  LockedFile* lockedFile;

  /// Network syscall implementation for this descriptor (if it's a socket).
  SharedPointer<NetworkSyscalls> networkImpl;

  /// Epoll implementation for this descriptor (if it is an epoll object).
  SharedPointer<EpollInstance> epollImpl;

  /// IO event for reporting changes to files
  IoEvent* ioevent;

 public:  /// \todo swap this to private and fix everything that breaks
  /// File descriptor flags (fcntl)
  int fdflags;

 private:
  friend class PosixSubsystem;

  /** State and serialization shared by aliases of one open file. */
  OpenFileDescriptionLease m_OpenFile;

  /** Whether this object still owns one published socket alias. */
  bool m_NetworkPublished;
  /** Eventfd table ownership is released before in-flight syscall pins drain. */
  bool m_EventFdPublished;
  bool m_TimerFdPublished = false;
  bool m_SignalFdPublished = false;
};

#endif
