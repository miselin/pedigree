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

#ifndef _UNIX_FILESYSTEM_H
#define _UNIX_FILESYSTEM_H

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/RingBuffer.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/Filesystem.h"
#include <sys/socket.h>

class Mutex;
class FileDescriptor;
class UnixSocket;

#if defined(PEDIGREE_EXTERNAL_SOURCE)
using UnixStreamSerializationGate = Mutex;
#else
using UnixStreamSerializationGate = Semaphore;
#endif

/** Descriptor ownership attached to one in-flight socket record. */
class SocketRights {
 public:
  enum : size_t { MaximumDescriptors = 253, MaximumInFlight = 16384 };

  ~SocketRights();

  /** Reserve the global in-flight budget for one SCM_RIGHTS record. */
  static bool create(size_t descriptorCount, SharedPointer<SocketRights>& rights);

  void append(FileDescriptor* descriptor);
  size_t count() const;
  FileDescriptor* descriptor(size_t index) const;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  static size_t inFlightForTest();
#endif

 private:
  explicit SocketRights(size_t reservation);

  SocketRights(const SocketRights&) = delete;
  SocketRights& operator=(const SocketRights&) = delete;

  Vector<FileDescriptor*> m_Descriptors;
  size_t m_Reservation;

  static Mutex m_InFlightLock;
  static size_t m_InFlight;
};

#define MAX_UNIX_DGRAM_BACKLOG 65536
#define MAX_UNIX_STREAM_QUEUE 65536

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
using UnixStreamControlLockHook = void (*)();
void setUnixStreamControlLockHookForTest(UnixStreamControlLockHook hook);
#endif

/**
 * Shared storage for a connected streaming socket pair.
 *
 * Keeping the byte streams independent of either endpoint means a concurrent
 * close cannot leave the surviving endpoint dereferencing a freed peer.
 */
class UnixSocketConnection {
  friend class UnixSocket;

 public:
  UnixSocketConnection();

 private:
  /** One ordered byte/control direction in a connected stream socket pair. */
  class Stream {
   public:
    Stream();
    ~Stream();

    size_t write(const uint8_t* buffer, size_t count, bool block,
                 const SharedPointer<SocketRights>& rights = SharedPointer<SocketRights>(),
                 bool* interrupted = nullptr);
    size_t writeVectors(const struct iovec* vectors, size_t vectorCount, bool block,
                        const SharedPointer<SocketRights>& rights, bool* interrupted = nullptr);
    size_t read(uint8_t* buffer, size_t count, bool block,
                SharedPointer<SocketRights>* rights = nullptr, bool* interrupted = nullptr);
    size_t readVectors(struct iovec* vectors, size_t vectorCount, bool block,
                       SharedPointer<SocketRights>* rights, bool* interrupted = nullptr);

    bool canWrite(bool block);
    bool canRead(bool block);
    uint64_t readableGeneration() const;
    uint64_t writableGeneration() const;
    void disableWrites();
    void disableReads();
    void monitor(Semaphore* waiter);
    void monitor(Thread* thread, Event* event);
    void cullMonitorTargets(Semaphore* waiter);
    void cullMonitorTargets(Event* event);

    Buffer<uint8_t, true>& buffer() {
      return m_Bytes;
    }

   private:
    class ControlGuard {
     public:
      explicit ControlGuard(Stream& stream);
      ~ControlGuard();

     private:
      Stream& m_Stream;
      LockGuard<Mutex> m_Guard;
    };

    struct Control {
      Control(uint64_t offset, const SharedPointer<SocketRights>& newRights)
          : byteOffset(offset), rights(newRights) {}

      uint64_t byteOffset;
      SharedPointer<SocketRights> rights;
    };

    void discardControls();
    void discardControlsIfRequested();

    Buffer<uint8_t, true> m_Bytes;
    UnixStreamSerializationGate m_SendLock;
    UnixStreamSerializationGate m_ReceiveLock;
    Mutex m_ControlLock;
    Atomic<bool> m_DiscardControlsRequested;
    List<Control*> m_Controls;
    uint64_t m_BytesWritten;
    uint64_t m_BytesRead;
  };

  Stream m_FirstStream;
  Stream m_SecondStream;
  bool m_Active;
  bool m_Failed;
  bool m_Closed[2];
  struct ucred m_Creds[2];
};

/**
 * UnixFilesystem: UNIX sockets.
 *
 * This filesystem is mounted with the "unix" 'volume' label, and provides
 * the filesystem abstraction for UNIX sockets (at least, non-anonymous ones).
 */
class UnixFilesystem : public Filesystem {
 public:
  SyncStatus sync() override {
    return SyncStatus::Success;
  }

  UnixFilesystem();
  virtual ~UnixFilesystem();

  virtual bool initialise(Disk* pDisk) {
    return false;
  }

  virtual File* getRoot() const {
    return m_pRoot;
  }

  virtual const String& getVolumeLabel() const {
    return m_VolumeLabel;
  }

  // Serialises pathname lookup, binding, unlink, and descriptor teardown for
  // the in-memory socket namespace.
  static Mutex& namespaceLock();

  virtual void truncate(File* pFile) {}

  virtual void fileAttributeChanged(File* pFile) {}

  virtual void cacheDirectoryContents(File* pFile) {
    if (pFile->isDirectory()) {
      Directory* pDir = Directory::fromFile(pFile);
      pDir->cacheDirectoryContents();
    }
  }

  virtual void extend(File* pFile, size_t size) {}

 protected:
  virtual bool createFile(File* parent, const String& filename, uint32_t mask);
  virtual bool createDirectory(File* parent, const String& filename, uint32_t mask);
  virtual bool createSymlink(File* parent, const String& filename, const String& value) {
    return false;
  }
  virtual bool removeNode(File* parent, const String& filename, File* file);
  virtual bool renameNode(Directory*, const String&, File*, Directory*, const String&, File*) {
    return true;
  }

 private:
  File* m_pRoot;

  static String m_VolumeLabel;
  static Mutex m_NamespaceLock;

  virtual bool isBytewise() const {
    return true;
  }
};

/**
 * A UNIX socket.
 */
class UnixSocket : public File {
 public:
  enum SocketType { Streaming, Datagram };

  enum SocketState {
    Listening,   // listening for connections
    Connecting,  // waiting for bind to be acked
    Inactive,    // unbound
    Active,      // bound, ready for data transfer
    Closed       // unbound but was once bound
  };

  UnixSocket(const String& name, Filesystem* pFs, File* pParent, UnixSocket* other = nullptr,
             SocketType type = Datagram);
  virtual ~UnixSocket();

  virtual uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                bool bCanBlock = true);
  virtual uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock = true);

  uint64_t recvfrom(uint64_t size, uintptr_t buffer, bool bCanBlock, String& from);

  /** Queue one indivisible datagram and its optional descriptor ownership. */
  bool sendDatagram(uint64_t size, uintptr_t buffer, bool bCanBlock, uintptr_t source,
                    const SharedPointer<SocketRights>& rights, int* error = nullptr);

  /** Remove one datagram, preserving its full length and ancillary ownership. */
  bool receiveDatagram(uint64_t size, uintptr_t buffer, bool bCanBlock, String& from,
                       SharedPointer<SocketRights>& rights, uint64_t& bytesRead,
                       uint64_t& datagramLength);

  /** Write one ordered stream record with optional descriptor ownership. */
  uint64_t sendStream(uint64_t size, uintptr_t buffer, bool bCanBlock,
                      const SharedPointer<SocketRights>& rights, bool* interrupted = nullptr);
  uint64_t sendStream(const struct iovec* vectors, size_t vectorCount, bool bCanBlock,
                      const SharedPointer<SocketRights>& rights, bool* interrupted = nullptr);

  /** Read stream bytes and optionally detach the first crossed control record. */
  uint64_t receiveStream(uint64_t size, uintptr_t buffer, bool bCanBlock,
                         SharedPointer<SocketRights>* rights, bool* interrupted = nullptr);
  uint64_t receiveStream(struct iovec* vectors, size_t vectorCount, bool bCanBlock,
                         SharedPointer<SocketRights>* rights, bool* interrupted = nullptr);

  virtual int select(bool bWriting = false, int timeout = 0);

  virtual bool isSocket() const {
    return true;
  }

  virtual bool isSeekable() const {
    return false;
  }

  // Bind this socket to another socket.
  // The other socket should not already be bound.
  bool bind(UnixSocket* other, bool block = false);

  // Break the bound socket.
  void unbind();

  // Acknowledges binding from another socket
  void acknowledgeBind();

  // Add a new socket for a client/server connection (for accept())
  bool addSocket(UnixSocket* socket);

  // Get the next socket in the listening queue (for non-datagram sockets).
  UnixSocket* getSocket(bool block = false);

  // Add a semaphore for the requested readiness directions.
  void addWaiter(Semaphore* waiter, bool read, bool write);

  // Remove a waiter semaphore.
  void removeWaiter(Semaphore* waiter);

  // Add an event to fire when the socket data changes.
  void addWaiter(Thread* thread, Event* event);

  // Remove a socket data change event.
  void removeWaiter(Event* event);

  // Get this socket's type
  SocketType getType() const {
    return m_Type;
  }

  // Get this socket's state
  SocketState getState() const;

  // Whether this endpoint completed a connection, including a peer that has
  // since closed.
  bool wasConnected() const;

  /** Rising-edge sequences for the endpoint's reusable I/O predicates. */
  ReadinessGenerations readinessGenerations() override;

  // Mark a queued connection as failed and wake all poll/read/write waiters.
  void failConnection();

  // Mark this socket a listening socket
  bool markListening();

  // Get our credentials.
  struct ucred getCredentials() const {
    return m_Creds;
  }

  // Get the credentials of the other side.
  struct ucred getPeerCredentials() const;

 private:
  typedef Buffer<uint8_t, true> UnixSocketStream;

  void setCreds();
  SocketState getStateLocked() const;
  UnixSocketConnection::Stream* incomingStream(
      const SharedPointer<UnixSocketConnection>& connection) const;
  UnixSocketConnection::Stream* outgoingStream(
      const SharedPointer<UnixSocketConnection>& connection) const;

  virtual bool isBytewise() const {
    return true;
  }

  struct buf {
    char* pBuffer;
    uint64_t len;
    char* remotePath;  // Path of the socket that dumped data here, if any.
    size_t remotePathLen;
    SharedPointer<SocketRights> rights;
  };

  static void destroyDatagram(struct buf* datagram);

  SocketType m_Type;
  SocketState m_State;

  // For datagram sockets.

  // Note: "servers" own the actual UNIX socket address, while clients get a
  // virtual address to track their existence (or are bound to a specific
  // name themselves).
  typedef RingBuffer<struct buf*> DatagramBuffer;
  DatagramBuffer m_Datagrams;

  // For stream sockets.

  // Listener readiness queue. Connected stream data lives in m_Connection.
  UnixSocketStream m_Stream;

  SharedPointer<UnixSocketConnection> m_Connection;
  bool m_ConnectionSide;

  // List of sockets pending accept() on this socket.
  List<UnixSocket*> m_PendingSockets;

  // Credentials associated at the time of bind()
  struct ucred m_Creds;

  // Serialises endpoint state and connection ownership. Buffer operations
  // use their own locks and are never performed while this is held.
  static Mutex m_ConnectionLock;
};

/**
 * Basic Directory subclass for UNIX socket support.
 */
class UnixDirectory : public Directory {
 public:
  UnixDirectory(const String& name, Filesystem* pFs, File* pParent);
  virtual ~UnixDirectory();

  bool addEntry(const String& filename, File* pFile);
  bool removeEntry(const String& filename, File* pFile);
  bool removeFromParent(UnixDirectory* parent, const String& filename);

  virtual void cacheDirectoryContents();

 private:
  Mutex m_Lock;
};

#endif
