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

#include "UnixFilesystem.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/syscallError.h"

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/logging.h"
#include "modules/system/vfs/VFS.h"

String UnixFilesystem::m_VolumeLabel("unix");
Mutex UnixFilesystem::m_NamespaceLock;
Mutex UnixSocket::m_ConnectionLock;
Mutex SocketRights::m_InFlightLock;
size_t SocketRights::m_InFlight = 0;

SocketRights::SocketRights(size_t reservation)
    : m_Descriptors(reservation), m_Reservation(reservation) {}

SocketRights::~SocketRights() {
  for (auto descriptor : m_Descriptors) {
    delete descriptor;
  }
  m_Descriptors.clear(true);

  LockGuard<Mutex> guard(m_InFlightLock);
  assert(m_InFlight >= m_Reservation);
  m_InFlight -= m_Reservation;
}

bool SocketRights::create(size_t descriptorCount, SharedPointer<SocketRights>& rights) {
  rights.reset();
  if (!descriptorCount || descriptorCount > MaximumDescriptors) {
    return false;
  }

  {
    LockGuard<Mutex> guard(m_InFlightLock);
    if (descriptorCount > MaximumInFlight - m_InFlight) {
      return false;
    }
    m_InFlight += descriptorCount;
  }

  rights.reset(new SocketRights(descriptorCount));
  return true;
}

void SocketRights::append(FileDescriptor* descriptor) {
  assert(descriptor);
  assert(m_Descriptors.count() < m_Reservation);
  m_Descriptors.pushBack(descriptor);
}

size_t SocketRights::count() const {
  return m_Descriptors.count();
}

FileDescriptor* SocketRights::descriptor(size_t index) const {
  return m_Descriptors[index];
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
size_t SocketRights::inFlightForTest() {
  LockGuard<Mutex> guard(m_InFlightLock);
  return m_InFlight;
}
#endif

UnixSocketConnection::Stream::Stream()
    : m_Bytes(MAX_UNIX_STREAM_QUEUE),
      m_SendLock(),
      m_ReceiveLock(),
      m_ControlLock(),
      m_Controls(),
      m_BytesWritten(0),
      m_BytesRead(0) {}

UnixSocketConnection::Stream::~Stream() {
  LockGuard<Mutex> guard(m_ControlLock);
  discardControls();
}

size_t UnixSocketConnection::Stream::write(const uint8_t* buffer, size_t count, bool block,
                                           const SharedPointer<SocketRights>& rights) {
  struct iovec vector = {const_cast<uint8_t*>(buffer), count};
  return writeVectors(&vector, 1, block, rights);
}

size_t UnixSocketConnection::Stream::writeVectors(const struct iovec* vectors, size_t vectorCount,
                                                  bool block,
                                                  const SharedPointer<SocketRights>& rights) {
  size_t firstVector = 0;
  while (firstVector < vectorCount && !vectors[firstVector].iov_len) {
    ++firstVector;
  }
  if (firstVector == vectorCount) {
    return 0;
  }

  Control* pendingControl = rights ? new Control(0, rights) : nullptr;
  LockGuard<Mutex> sendGuard(m_SendLock);
  size_t written = 0;
  size_t firstOffset = 0;

  if (pendingControl) {
    if (!m_Bytes.canWrite(block)) {
      delete pendingControl;
      return 0;
    }

    LockGuard<Mutex> controlGuard(m_ControlLock);
    const uint8_t* first = reinterpret_cast<const uint8_t*>(vectors[firstVector].iov_base);
    if (m_Bytes.write(first, 1, block) != 1) {
      delete pendingControl;
      return 0;
    }

    pendingControl->byteOffset = m_BytesWritten;
    m_Controls.pushBack(pendingControl);
    ++m_BytesWritten;
    ++written;
    firstOffset = 1;
  }

  for (size_t i = firstVector; i < vectorCount; ++i) {
    const uint8_t* buffer = reinterpret_cast<const uint8_t*>(vectors[i].iov_base);
    const size_t count = vectors[i].iov_len;
    const size_t offset = i == firstVector ? firstOffset : 0;
    if (offset >= count) {
      continue;
    }

    const size_t tail = m_Bytes.write(buffer + offset, count - offset, block);
    if (tail) {
      LockGuard<Mutex> controlGuard(m_ControlLock);
      m_BytesWritten += tail;
      written += tail;
    }
    if (tail < count - offset) {
      break;
    }
  }

  return written;
}

size_t UnixSocketConnection::Stream::read(uint8_t* buffer, size_t count, bool block,
                                          SharedPointer<SocketRights>* rights) {
  struct iovec vector = {buffer, count};
  return readVectors(&vector, 1, block, rights);
}

size_t UnixSocketConnection::Stream::readVectors(struct iovec* vectors, size_t vectorCount,
                                                 bool block, SharedPointer<SocketRights>* rights) {
  if (rights) {
    rights->reset();
  }

  LockGuard<Mutex> receiveGuard(m_ReceiveLock);
  size_t totalRead = 0;
  bool canBlock = block;
  for (size_t i = 0; i < vectorCount; ++i) {
    uint8_t* buffer = reinterpret_cast<uint8_t*>(vectors[i].iov_base);
    size_t count = vectors[i].iov_len;
    if (!count) {
      continue;
    }

    if (!m_Bytes.canRead(canBlock)) {
      break;
    }

    LockGuard<Mutex> controlGuard(m_ControlLock);
    while (m_Controls.count()) {
      Control* stale = *m_Controls.begin();
      if (stale->byteOffset >= m_BytesRead) {
        break;
      }
      delete m_Controls.popFront();
    }

    size_t amount = count;
    if (rights && m_Controls.count()) {
      Control* next = *m_Controls.begin();
      const uint64_t distance = next->byteOffset - m_BytesRead;
      if (distance < amount) {
        amount = static_cast<size_t>(distance + 1);
      }
    }

    const size_t bytesRead = m_Bytes.read(buffer, amount, canBlock);
    m_BytesRead += bytesRead;
    bool consumedControl = false;
    while (bytesRead && m_Controls.count()) {
      Control* crossed = *m_Controls.begin();
      if (crossed->byteOffset >= m_BytesRead) {
        break;
      }
      crossed = m_Controls.popFront();
      if (rights) {
        *rights = crossed->rights;
        consumedControl = true;
      }
      delete crossed;
      if (rights) {
        break;
      }
    }

    totalRead += bytesRead;
    canBlock = false;
    if (consumedControl || bytesRead < amount) {
      break;
    }
  }

  return totalRead;
}

bool UnixSocketConnection::Stream::canWrite(bool block) {
  return m_Bytes.canWrite(block);
}

bool UnixSocketConnection::Stream::canRead(bool block) {
  return m_Bytes.canRead(block);
}

uint64_t UnixSocketConnection::Stream::readableGeneration() const {
  return m_Bytes.readableGeneration();
}

uint64_t UnixSocketConnection::Stream::writableGeneration() const {
  return m_Bytes.writableGeneration();
}

void UnixSocketConnection::Stream::disableWrites() {
  m_Bytes.disableWrites();
}

void UnixSocketConnection::Stream::disableReads() {
  // Closing the receiver must reject new sends before queued descriptor
  // ownership is drained; otherwise a late marker could outlive both ends.
  disableWrites();
  m_Bytes.disableReads();

  LockGuard<Mutex> sendGuard(m_SendLock);
  LockGuard<Mutex> receiveGuard(m_ReceiveLock);
  LockGuard<Mutex> controlGuard(m_ControlLock);
  discardControls();
}

void UnixSocketConnection::Stream::monitor(Semaphore* waiter) {
  m_Bytes.monitor(waiter);
}

void UnixSocketConnection::Stream::monitor(Thread* thread, Event* event) {
  m_Bytes.monitor(thread, event);
}

void UnixSocketConnection::Stream::cullMonitorTargets(Semaphore* waiter) {
  m_Bytes.cullMonitorTargets(waiter);
}

void UnixSocketConnection::Stream::cullMonitorTargets(Event* event) {
  m_Bytes.cullMonitorTargets(event);
}

void UnixSocketConnection::Stream::discardControls() {
  while (m_Controls.count()) {
    delete m_Controls.popFront();
  }
}

UnixSocketConnection::UnixSocketConnection()
    : m_FirstStream(),
      m_SecondStream(),
      m_Active(false),
      m_Failed(false),
      m_Closed{false, false},
      m_Creds() {
  for (size_t i = 0; i < 2; ++i) {
    m_Creds[i].uid = -1;
    m_Creds[i].gid = -1;
    m_Creds[i].pid = -1;
  }
}

UnixSocket::UnixSocket(const String& name, Filesystem* pFs, File* pParent, UnixSocket* other,
                       SocketType type)
    : File(name, 0, 0, 0, 0, pFs, 0, pParent),
      m_Type(type),
      m_State(Inactive),
      m_Datagrams(MAX_UNIX_DGRAM_BACKLOG),
      m_Stream(MAX_UNIX_STREAM_QUEUE),
      m_Connection(),
      m_ConnectionSide(false),
      m_PendingSockets(),
      m_Creds() {
  (void)other;

  if (m_Type == Datagram) {
    // Datagram sockets are always active, they don't bind to each other.
    m_State = Active;
  }

  m_Creds.uid = -1;
  m_Creds.gid = -1;
  m_Creds.pid = -1;
}

UnixSocket::~UnixSocket() {
  unbind();
}

int UnixSocket::select(bool bWriting, int timeout) {
  if (m_Type == Streaming) {
    SharedPointer<UnixSocketConnection> connection;
    SocketState state;
    {
      LockGuard<Mutex> guard(m_ConnectionLock);
      state = getStateLocked();
      connection = m_Connection;
    }

    if (state == Listening) {
      return !bWriting && m_Stream.canRead(timeout == 1);
    }

    if (state == Closed) {
      return !bWriting;
    }

    if (state != Active || !connection) {
      return false;
    }

    if (bWriting) {
      if (outgoingStream(connection)->canWrite(timeout == 1)) {
        return true;
      }
    } else {
      if (incomingStream(connection)->canRead(timeout == 1)) {
        return true;
      }
    }

    return false;
  } else {
    {
      LockGuard<Mutex> guard(m_ConnectionLock);
      if (m_State == Closed) {
        return !bWriting;
      }
    }

    if (timeout) {
      return m_Datagrams.waitFor(bWriting ? RingBufferWait::Writing : RingBufferWait::Reading);
    } else if (bWriting) {
      return m_Datagrams.canWrite();
    } else {
      return m_Datagrams.dataReady();
    }
  }
}

uint64_t UnixSocket::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                  bool bCanBlock) {
  String remote;
  return recvfrom(size, buffer, bCanBlock, remote);
}

uint64_t UnixSocket::recvfrom(uint64_t size, uintptr_t buffer, bool bCanBlock, String& from) {
  if (m_Type == Streaming) {
    from = String();
    return receiveStream(size, buffer, bCanBlock, nullptr);
  }

  SharedPointer<SocketRights> rights;
  uint64_t bytesRead = 0;
  uint64_t datagramLength = 0;
  receiveDatagram(size, buffer, bCanBlock, from, rights, bytesRead, datagramLength);
  return bytesRead;
}

uint64_t UnixSocket::receiveStream(uint64_t size, uintptr_t buffer, bool bCanBlock,
                                   SharedPointer<SocketRights>* rights) {
  struct iovec vector = {reinterpret_cast<void*>(buffer), size};
  return receiveStream(&vector, 1, bCanBlock, rights);
}

uint64_t UnixSocket::receiveStream(struct iovec* vectors, size_t vectorCount, bool bCanBlock,
                                   SharedPointer<SocketRights>* rights) {
  if (rights) {
    rights->reset();
  }

  SharedPointer<UnixSocketConnection> connection;
  SocketState state;
  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    state = getStateLocked();
    connection = m_Connection;
  }

  if (m_Type != Streaming || !connection || (state != Active && state != Closed)) {
    return 0;
  }

  return incomingStream(connection)
      ->readVectors(vectors, vectorCount, state == Active && bCanBlock, rights);
}

bool UnixSocket::receiveDatagram(uint64_t size, uintptr_t buffer, bool bCanBlock, String& from,
                                 SharedPointer<SocketRights>& rights, uint64_t& bytesRead,
                                 uint64_t& datagramLength) {
  rights.reset();
  bytesRead = 0;
  datagramLength = 0;

  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    if (m_State == Closed || m_Type != Datagram) {
      return false;
    }
  }

  if (bCanBlock) {
    if (!select(false, 1)) {
      return false;
    }
  } else if (!select(false, 0)) {
    return false;
  }

  struct buf* datagram = nullptr;
  DatagramBuffer::Error error = DatagramBuffer::NoError;
  if (!m_Datagrams.read(datagram, error)) {
    return false;
  }

  datagramLength = datagram->len;
  bytesRead = size < datagramLength ? size : datagramLength;
  if (bytesRead) {
    MemoryCopy(reinterpret_cast<void*>(buffer), datagram->pBuffer, bytesRead);
  }
  if (datagram->remotePath) {
    from.assign(datagram->remotePath, datagram->remotePathLen);
  }
  rights = datagram->rights;
  destroyDatagram(datagram);
  return true;
}

uint64_t UnixSocket::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                   bool bCanBlock) {
  if (m_Type == Streaming) {
    SharedPointer<SocketRights> rights;
    return sendStream(size, buffer, bCanBlock, rights);
  }

  SharedPointer<SocketRights> rights;
  return sendDatagram(size, buffer, bCanBlock, location, rights) ? size : 0;
}

uint64_t UnixSocket::sendStream(uint64_t size, uintptr_t buffer, bool bCanBlock,
                                const SharedPointer<SocketRights>& rights) {
  struct iovec vector = {reinterpret_cast<void*>(buffer), size};
  return sendStream(&vector, 1, bCanBlock, rights);
}

uint64_t UnixSocket::sendStream(const struct iovec* vectors, size_t vectorCount, bool bCanBlock,
                                const SharedPointer<SocketRights>& rights) {
  SharedPointer<UnixSocketConnection> connection;
  SocketState state;
  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    state = getStateLocked();
    connection = m_Connection;
  }

  if (m_Type != Streaming || !connection || state != Active) {
    N_NOTICE("UnixSocket::write => closed or not connected");
    return 0;
  }

  return outgoingStream(connection)->writeVectors(vectors, vectorCount, bCanBlock, rights);
}

bool UnixSocket::sendDatagram(uint64_t size, uintptr_t buffer, bool bCanBlock, uintptr_t source,
                              const SharedPointer<SocketRights>& rights) {
  if (m_Type != Datagram) {
    return false;
  }

  if (bCanBlock) {
    if (!select(true, 1)) {
      return false;
    }
  } else if (!select(true, 0)) {
    return false;
  }

  struct buf* b = new struct buf;
  b->pBuffer = new char[size];
  if (size) {
    MemoryCopy(b->pBuffer, reinterpret_cast<void*>(buffer), size);
  }
  b->len = size;
  b->remotePath = 0;
  b->remotePathLen = 0;
  b->rights = rights;
  if (source) {
    b->remotePath = new char[255];
    StringCopyN(b->remotePath, reinterpret_cast<char*>(source), 255);
    b->remotePathLen = StringLength(b->remotePath);
  }
  const DatagramBuffer::Error error = m_Datagrams.write(b);
  if (error != DatagramBuffer::NoError) {
    destroyDatagram(b);
    return false;
  }

  dataChanged();
  return true;
}

void UnixSocket::destroyDatagram(struct buf* datagram) {
  if (!datagram) {
    return;
  }

  delete[] datagram->remotePath;
  delete[] datagram->pBuffer;
  delete datagram;
}

bool UnixSocket::bind(UnixSocket* other, bool block) {
  (void)block;

  if (!other || m_Type != Streaming || other->m_Type != Streaming) {
    return false;
  }

  SharedPointer<UnixSocketConnection> connection(new UnixSocketConnection());
  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    if (m_State != Inactive || other->m_State != Inactive || m_Connection || other->m_Connection) {
      N_NOTICE("UnixSocket::bind endpoints are not inactive");
      return false;
    }

    m_Connection = connection;
    m_ConnectionSide = false;
    other->m_Connection = connection;
    other->m_ConnectionSide = true;
    m_State = Connecting;
    other->m_State = Connecting;

    setCreds();
    connection->m_Creds[0] = m_Creds;
  }

  return true;
}

void UnixSocket::unbind() {
  SharedPointer<UnixSocketConnection> connection;
  bool side = false;
  List<UnixSocket*> pending;
  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    connection = m_Connection;
    if (connection) {
      side = m_ConnectionSide;
      connection->m_Closed[side] = true;
    }

    m_State = Closed;
    while (m_PendingSockets.count()) {
      pending.pushBack(m_PendingSockets.popFront());
    }
  }

  N_NOTICE("UnixSocket::unbind");

  if (m_Type == Datagram) {
    m_Datagrams.close();
    struct buf* datagram = nullptr;
    while (m_Datagrams.takeAfterClose(datagram)) {
      destroyDatagram(datagram);
    }
  }

  if (connection) {
    UnixSocketConnection::Stream* incoming =
        side ? &connection->m_SecondStream : &connection->m_FirstStream;
    UnixSocketConnection::Stream* outgoing =
        side ? &connection->m_FirstStream : &connection->m_SecondStream;
    incoming->disableReads();
    outgoing->disableWrites();
    incoming->buffer().notifyMonitors();
    outgoing->buffer().notifyMonitors();
  }

  m_Stream.disableWrites();
  m_Stream.disableReads();
  m_Stream.notifyMonitors();

  while (pending.count()) {
    UnixSocket* socket = pending.popFront();
    socket->failConnection();
    delete socket;
  }
}

void UnixSocket::acknowledgeBind() {
  SharedPointer<UnixSocketConnection> connection;
  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    connection = m_Connection;
    if (!connection || connection->m_Failed || connection->m_Closed[0] || connection->m_Closed[1] ||
        connection->m_Active) {
      return;
    }

    N_NOTICE("acking bind");

    connection->m_Active = true;
    m_State = Active;

    setCreds();
    connection->m_Creds[m_ConnectionSide ? 1 : 0] = m_Creds;
  }

  connection->m_FirstStream.buffer().notifyMonitors();
  connection->m_SecondStream.buffer().notifyMonitors();
}

bool UnixSocket::addSocket(UnixSocket* socket) {
  SharedPointer<UnixSocketConnection> connection;
  LockGuard<Mutex> guard(m_ConnectionLock);
  if (m_State != Listening || !socket || !socket->m_Connection || socket->m_Connection->m_Failed ||
      socket->m_Connection->m_Closed[0] || socket->m_Connection->m_Closed[1]) {
    return false;
  }

  connection = socket->m_Connection;
  socket->m_Creds = m_Creds;
  connection->m_Creds[socket->m_ConnectionSide ? 1 : 0] = m_Creds;
  connection->m_Active = true;
  socket->m_State = Active;
  m_PendingSockets.pushBack(socket);

  N_NOTICE("adding listening socket");

  // No data moving on listen sockets so we use the stream buffer as a
  // signaling primitive. Keep queue ownership and its signal atomic with
  // listener teardown so a failed enqueue remains caller-owned.
  uint8_t c = 0;
  if (m_Stream.write(&c, 1, false) == 1) {
    connection->m_FirstStream.buffer().notifyMonitors();
    connection->m_SecondStream.buffer().notifyMonitors();
    return true;
  }

  for (List<UnixSocket*>::Iterator it = m_PendingSockets.begin(); it != m_PendingSockets.end();
       ++it) {
    if (*it == socket) {
      m_PendingSockets.erase(it);
      break;
    }
  }
  return false;
}

UnixSocket* UnixSocket::getSocket(bool block) {
  uint8_t c = 0;
  if (m_Stream.read(&c, 1, block) != 1) {
    return nullptr;
  }

  N_NOTICE("got a socket");

  LockGuard<Mutex> guard(m_ConnectionLock);

  if (m_State != Listening || !m_PendingSockets.count()) {
    return nullptr;
  }

  N_NOTICE("popping socket");
  return m_PendingSockets.popFront();
}

void UnixSocket::addWaiter(Semaphore* waiter, bool read, bool write) {
  if (m_Type == Datagram) {
    m_Datagrams.monitor(waiter);
    return;
  }

  SharedPointer<UnixSocketConnection> connection;
  bool side = false;
  bool closed = false;
  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    closed = getStateLocked() == Closed;
    connection = m_Connection;
    side = m_ConnectionSide;
  }

  if (closed) {
    // Closing readiness is persistent. Do not strand a waiter behind
    // unbind's one-shot monitor notification.
    waiter->release();
    return;
  }

  const bool monitorRead = read || (!read && !write);
  if (!connection) {
    if (monitorRead || write) {
      m_Stream.monitor(waiter);
    }
    {
      LockGuard<Mutex> guard(m_ConnectionLock);
      closed = getStateLocked() == Closed;
    }
    if (closed) {
      m_Stream.notifyMonitors();
    }
    return;
  }

  UnixSocketConnection::Stream* incoming =
      side ? &connection->m_SecondStream : &connection->m_FirstStream;
  UnixSocketConnection::Stream* outgoing =
      side ? &connection->m_FirstStream : &connection->m_SecondStream;
  if (monitorRead) {
    incoming->monitor(waiter);
  }
  if (write && (!monitorRead || outgoing != incoming)) {
    outgoing->monitor(waiter);
  }

  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    closed = getStateLocked() == Closed;
  }
  if (closed) {
    // Repair close-before-enrollment without nesting buffer operations
    // under m_ConnectionLock. A concurrent notifier clears these targets.
    if (monitorRead) {
      incoming->buffer().notifyMonitors();
    }
    if (write && (!monitorRead || outgoing != incoming)) {
      outgoing->buffer().notifyMonitors();
    }
  }
}

void UnixSocket::removeWaiter(Semaphore* waiter) {
  if (m_Type == Datagram) {
    m_Datagrams.cullMonitorTargets(waiter);
    return;
  }

  SharedPointer<UnixSocketConnection> connection;
  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    connection = m_Connection;
    if (!connection) {
      m_Stream.cullMonitorTargets(waiter);
      return;
    }
  }

  connection->m_FirstStream.cullMonitorTargets(waiter);
  connection->m_SecondStream.cullMonitorTargets(waiter);
}

void UnixSocket::addWaiter(Thread* thread, Event* event) {
  if (m_Type == Datagram) {
    m_Datagrams.monitor(thread, event);
    return;
  }

  SharedPointer<UnixSocketConnection> connection;
  bool closed = false;
  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    closed = getStateLocked() == Closed;
    connection = m_Connection;
  }

  if (closed) {
    // Closing readiness is persistent; send without retaining the
    // connection lock across event delivery.
#if !defined(PEDIGREE_EXTERNAL_SOURCE)
    thread->sendEvent(event);
#endif
    return;
  }

  if (!connection) {
    m_Stream.monitor(thread, event);
    {
      LockGuard<Mutex> guard(m_ConnectionLock);
      closed = getStateLocked() == Closed;
    }
    if (closed) {
      m_Stream.notifyMonitors();
    }
    return;
  }

  UnixSocketConnection::Stream* first = &connection->m_FirstStream;
  UnixSocketConnection::Stream* second = &connection->m_SecondStream;
  first->monitor(thread, event);
  if (second != first) {
    second->monitor(thread, event);
  }

  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    closed = getStateLocked() == Closed;
  }
  if (closed) {
    // Repair close-before-enrollment; notifyMonitors is idempotent with a
    // concurrent unbind notifier because it consumes registered targets.
    first->buffer().notifyMonitors();
    if (second != first) {
      second->buffer().notifyMonitors();
    }
  }
}

void UnixSocket::removeWaiter(Event* event) {
  if (m_Type == Datagram) {
    m_Datagrams.cullMonitorTargets(event);
    return;
  }

  SharedPointer<UnixSocketConnection> connection;
  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    connection = m_Connection;
    if (!connection) {
      m_Stream.cullMonitorTargets(event);
      return;
    }
  }

  connection->m_FirstStream.cullMonitorTargets(event);
  connection->m_SecondStream.cullMonitorTargets(event);
}

bool UnixSocket::markListening() {
  LockGuard<Mutex> guard(m_ConnectionLock);

  if (m_Type != Streaming) {
    // can't listen() on a non-streaming socket
    return false;
  }

  if (m_State != Inactive) {
    // can't listen on a bound socket
    return false;
  }

  setCreds();
  m_State = Listening;
  return true;
}

UnixSocket::SocketState UnixSocket::getState() const {
  LockGuard<Mutex> guard(m_ConnectionLock);
  return getStateLocked();
}

UnixSocket::SocketState UnixSocket::getStateLocked() const {
  if (m_Type != Streaming || !m_Connection) {
    return m_State;
  }

  if (m_Connection->m_Failed || m_Connection->m_Closed[m_ConnectionSide ? 1 : 0] ||
      m_Connection->m_Closed[m_ConnectionSide ? 0 : 1]) {
    return Closed;
  }

  return m_Connection->m_Active ? Active : Connecting;
}

bool UnixSocket::wasConnected() const {
  LockGuard<Mutex> guard(m_ConnectionLock);
  return m_Connection && m_Connection->m_Active && !m_Connection->m_Failed;
}

ReadinessGenerations UnixSocket::readinessGenerations() {
  ReadinessGenerations generations;
  if (m_Type == Datagram) {
    generations.read = m_Datagrams.readableGeneration();
    return generations;
  }

  SharedPointer<UnixSocketConnection> connection;
  SocketState state;
  bool side = false;
  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    state = getStateLocked();
    connection = m_Connection;
    side = m_ConnectionSide;
  }

  if (state == Listening || !connection) {
    generations.read = m_Stream.readableGeneration();
    generations.write = m_Stream.writableGeneration();
    return generations;
  }

  UnixSocketConnection::Stream* incoming =
      side ? &connection->m_SecondStream : &connection->m_FirstStream;
  UnixSocketConnection::Stream* outgoing =
      side ? &connection->m_FirstStream : &connection->m_SecondStream;
  generations.read = incoming->readableGeneration();
  generations.write = outgoing->writableGeneration();
  return generations;
}

void UnixSocket::failConnection() {
  SharedPointer<UnixSocketConnection> connection;
  {
    LockGuard<Mutex> guard(m_ConnectionLock);
    connection = m_Connection;
    if (!connection) {
      m_State = Closed;
      return;
    }

    connection->m_Failed = true;
    connection->m_Closed[0] = true;
    connection->m_Closed[1] = true;
    m_State = Closed;
  }

  connection->m_FirstStream.disableWrites();
  connection->m_FirstStream.disableReads();
  connection->m_SecondStream.disableWrites();
  connection->m_SecondStream.disableReads();
  connection->m_FirstStream.buffer().notifyMonitors();
  connection->m_SecondStream.buffer().notifyMonitors();
}

struct ucred UnixSocket::getPeerCredentials() const {
  LockGuard<Mutex> guard(m_ConnectionLock);
  if (!m_Connection) {
    struct ucred empty;
    empty.uid = -1;
    empty.gid = -1;
    empty.pid = -1;
    return empty;
  }

  return m_Connection->m_Creds[m_ConnectionSide ? 0 : 1];
}

UnixSocketConnection::Stream* UnixSocket::incomingStream(
    const SharedPointer<UnixSocketConnection>& connection) const {
  return m_ConnectionSide ? &connection->m_SecondStream : &connection->m_FirstStream;
}

UnixSocketConnection::Stream* UnixSocket::outgoingStream(
    const SharedPointer<UnixSocketConnection>& connection) const {
  return m_ConnectionSide ? &connection->m_FirstStream : &connection->m_SecondStream;
}

void UnixSocket::setCreds() {
#if THREADS
  Process* pCurrentProcess = Processor::information().getCurrentThread()->getParent();
  m_Creds.uid = pCurrentProcess->getUserId();
  m_Creds.gid = pCurrentProcess->getGroupId();
  m_Creds.pid = pCurrentProcess->getId();
#endif
}

UnixDirectory::UnixDirectory(const String& name, Filesystem* pFs, File* pParent)
    : Directory(name, 0, 0, 0, 0, pFs, 0, pParent), m_Lock() {
  cacheDirectoryContents();
}

UnixDirectory::~UnixDirectory() {}

bool UnixDirectory::addEntry(const String& filename, File* pFile) {
  return addDirectoryEntry(filename, pFile);
}

bool UnixDirectory::removeEntry(const String& filename, File* pFile) {
  LockGuard<Mutex> guard(m_Lock);
  return removeDirectoryEntry(filename.view(), pFile);
}

bool UnixDirectory::removeFromParent(UnixDirectory* parent, const String& filename) {
  if (parent == this) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  LockGuard<Mutex> namespaceGuard(namespaceMutationLock());
  LockGuard<Mutex> guard(m_Lock);
  bool empty = false;
  if (isEmpty(empty) != ReadStatus::Complete) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  if (!empty) {
    SYSCALL_ERROR(NotEmpty);
    return false;
  }
  if (!parent->removeEntry(filename, this))
    return false;
  markDetached();
  return true;
}

void UnixDirectory::cacheDirectoryContents() {
  markCachePopulated();
}

UnixFilesystem::UnixFilesystem() : Filesystem(), m_pRoot(0) {
  UnixDirectory* pRoot = new UnixDirectory(String(""), this, 0);

  m_pRoot = pRoot;
  VFS::instance().trackFile(m_pRoot);

  // allow owner/group rwx but others only r-x on the filesystem root
  m_pRoot->setPermissions(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR |
                          FILE_OX);
}

UnixFilesystem::~UnixFilesystem() {
  Directory::fromFile(m_pRoot)->emptyCache();
  if (!VFS::instance().untrackFile(m_pRoot)) {
    ERROR("UnixFilesystem::~UnixFilesystem: root didn't get destroyed");
  }
}

Mutex& UnixFilesystem::namespaceLock() {
  return m_NamespaceLock;
}

bool UnixFilesystem::createFile(File* parent, const String& filename, uint32_t mask) {
  UnixDirectory* pParent = static_cast<UnixDirectory*>(Directory::fromFile(parent));

  UnixSocket* pSocket = new UnixSocket(filename, this, parent);
  if (!pParent->addEntry(filename, pSocket)) {
    delete pSocket;
    return false;
  }

  // give owner/group full permission to the socket by default
  pSocket->setPermissions(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR |
                          FILE_OX);

  return true;
}

bool UnixFilesystem::createDirectory(File* parent, const String& filename, uint32_t mask) {
  UnixDirectory* pParent = static_cast<UnixDirectory*>(Directory::fromFile(parent));

  UnixDirectory* pChild = new UnixDirectory(filename, this, parent);
  if (!pParent->addEntry(filename, pChild)) {
    delete pChild;
    return false;
  }

  // give owner/group full permission to the directory by default
  pChild->setPermissions(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR |
                         FILE_OX);

  return true;
}

bool UnixFilesystem::removeNode(File* parent, const String& filename, File* file) {
  UnixDirectory* pParent = static_cast<UnixDirectory*>(Directory::fromFile(parent));
  if (file->isDirectory()) {
    return static_cast<UnixDirectory*>(file)->removeFromParent(pParent, filename);
  }
  return pParent->removeEntry(filename, file);
}
