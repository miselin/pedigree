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

/** Abstraction of a file descriptor, which defines an open file
 * and related flags.
 */
class EXPORTED_PUBLIC FileDescriptor {
 private:
  struct OpenFilePosition;

 public:
  /** A serialized view of the offset shared by duplicated descriptors. */
  class PositionGuard {
   public:
    uint64_t offset() const;
    void setOffset(uint64_t offset);
    void advanceOffset(uint64_t amount);

   private:
    friend class FileDescriptor;

    explicit PositionGuard(const SharedPointer<OpenFilePosition>& position);

    SharedPointer<OpenFilePosition> m_Position;
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

  /// Get current status flags.
  int getStatusFlags() const;

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
  SharedPointer<class NetworkSyscalls> networkImpl;

  /// IO event for reporting changes to files
  IoEvent* ioevent;

 public:  /// \todo swap this to private and fix everything that breaks
  /// File descriptor flags (fcntl)
  int fdflags;

  /// File status flags (fcntl)
  int flflags;

 private:
  struct OpenFilePosition {
    explicit OpenFilePosition(uint64_t initialOffset) : lock(), offset(initialOffset) {}

    Mutex lock;
    uint64_t offset;
  };

  /** Offset and serialization shared by aliases of one open file. */
  SharedPointer<OpenFilePosition> m_Position;

  /** Whether this descriptor retained an established VFS File owner. */
  bool m_bVfsLease;
};

#endif
