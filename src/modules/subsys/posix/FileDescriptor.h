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
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/String.h"

class File;
class LockedFile;
class UnixSocket;
class IoEvent;
class NetworkSyscalls;
class EpollInstance;
class EventFd;

/** Abstraction of a file descriptor, which defines an open file
 * and related flags.
 */
class EXPORTED_PUBLIC FileDescriptor {
 public:
  class PositionGuard;

  /**
   * Shared state for one open file description. A lease keeps its file or
   * socket target alive independently of any numeric descriptor.
   */
  class OpenFileDescription {
   public:
    ~OpenFileDescription();

    File* getFile() const;
    SharedPointer<NetworkSyscalls> getNetworkImpl() const;
    SharedPointer<EventFd> getEventFdImpl() const;
    size_t descriptorOwnerCount() const;

   private:
    friend class FileDescriptor;
    friend class PositionGuard;

    OpenFileDescription(File* file, uint64_t initialOffset, int initialStatusFlags);

    void addDescriptorOwner();
    void removeDescriptorOwner();
    void ensureVfsLease();

    mutable Mutex lock;
    File* file;
    SharedPointer<NetworkSyscalls> networkImpl;
    SharedPointer<EventFd> eventFdImpl;
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
    void setOffset(uint64_t offset);
    void advanceOffset(uint64_t amount);

   private:
    friend class FileDescriptor;

    explicit PositionGuard(const SharedPointer<OpenFileDescription>& description);

    SharedPointer<OpenFileDescription> m_Description;
    LockGuard<Mutex> m_Guard;
  };

  /// Default constructor
  FileDescriptor();

  /// Parameterised constructor
  FileDescriptor(File* newFile, uint64_t newOffset = 0, size_t newFd = 0xFFFFFFFF, int fdFlags = 0,
                 int flFlags = 0, LockedFile* lf = 0);

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

  /** Associate an eventfd counter with this open file description. */
  void setEventFdImpl(const SharedPointer<EventFd>& implementation);

  /** Retain the eventfd counter behind this descriptor, if any. */
  SharedPointer<EventFd> getEventFdImpl() const;

  /** Whether this descriptor still owns a published eventfd alias. */
  bool eventFdPublished() const;

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

  /// Our open file pointer
  File* file;

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
  /** State and serialization shared by aliases of one open file. */
  OpenFileDescriptionLease m_OpenFile;

  /** Eventfd table ownership is released before in-flight syscall pins drain. */
  bool m_EventFdPublished;
};

#endif
