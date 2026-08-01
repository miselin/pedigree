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
#include "modules/subsys/posix/logging.h"
#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

String UnixFilesystem::m_VolumeLabel("unix");
Mutex UnixFilesystem::m_NamespaceLock;
Mutex UnixSocket::m_ConnectionLock;

UnixSocketConnection::UnixSocketConnection()
    : m_FirstStream(MAX_UNIX_STREAM_QUEUE),
      m_SecondStream(MAX_UNIX_STREAM_QUEUE), m_Active(false), m_Failed(false),
      m_Closed{false, false}, m_Creds()
{
    for (size_t i = 0; i < 2; ++i)
    {
        m_Creds[i].uid = -1;
        m_Creds[i].gid = -1;
        m_Creds[i].pid = -1;
    }
}

UnixSocket::UnixSocket(
    const String &name, Filesystem *pFs, File *pParent, UnixSocket *other,
    SocketType type)
    : File(name, 0, 0, 0, 0, pFs, 0, pParent), m_Type(type), m_State(Inactive),
      m_Datagrams(MAX_UNIX_DGRAM_BACKLOG), m_Stream(MAX_UNIX_STREAM_QUEUE),
      m_Connection(), m_ConnectionSide(false), m_PendingSockets(),
      m_Creds()
{
    (void) other;

    if (m_Type == Datagram)
    {
        // Datagram sockets are always active, they don't bind to each other.
        m_State = Active;
    }

    m_Creds.uid = -1;
    m_Creds.gid = -1;
    m_Creds.pid = -1;
}

UnixSocket::~UnixSocket()
{
    unbind();
}

int UnixSocket::select(bool bWriting, int timeout)
{
    if (m_Type == Streaming)
    {
        SharedPointer<UnixSocketConnection> connection;
        SocketState state;
        {
            LockGuard<Mutex> guard(m_ConnectionLock);
            state = getStateLocked();
            connection = m_Connection;
        }

        if (state == Listening)
        {
            return !bWriting && m_Stream.canRead(timeout == 1);
        }

        if (state == Closed)
        {
            return !bWriting;
        }

        if (state != Active || !connection)
        {
            return false;
        }

        if (bWriting)
        {
            if (outgoingStream(connection)->canWrite(timeout == 1))
            {
                return true;
            }
        }
        else
        {
            if (incomingStream(connection)->canRead(timeout == 1))
            {
                return true;
            }
        }

        return false;
    }
    else
    {
        {
            LockGuard<Mutex> guard(m_ConnectionLock);
            if (m_State == Closed)
            {
                return !bWriting;
            }
        }

        if (timeout)
        {
            return m_Datagrams.waitFor(
                bWriting ? RingBufferWait::Writing : RingBufferWait::Reading);
        }
        else if (bWriting)
        {
            return m_Datagrams.canWrite();
        }
        else
        {
            return m_Datagrams.dataReady();
        }
    }
}

uint64_t UnixSocket::readBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    String remote;
    return recvfrom(size, buffer, bCanBlock, remote);
}

uint64_t UnixSocket::recvfrom(
    uint64_t size, uintptr_t buffer, bool bCanBlock, String &from)
{
    if (m_Type == Streaming)
    {
        SharedPointer<UnixSocketConnection> connection;
        SocketState state;
        {
            LockGuard<Mutex> guard(m_ConnectionLock);
            state = getStateLocked();
            connection = m_Connection;
        }

        if (!connection || (state != Active && state != Closed))
        {
            return 0;
        }

        from = String();
        return incomingStream(connection)->read(
            reinterpret_cast<uint8_t *>(buffer), size,
            state == Active && bCanBlock);
    }

    {
        LockGuard<Mutex> guard(m_ConnectionLock);
        if (m_State == Closed)
        {
            return 0;
        }
    }

    if (bCanBlock)
    {
        if (!select(false, 1))
        {
            return 0;  // Interrupted
        }
    }
    else if (!select(false, 0))
    {
        // No data available.
        return 0;
    }

    struct buf *b = nullptr;
    DatagramBuffer::Error error = DatagramBuffer::NoError;
    if (!m_Datagrams.read(b, error))
    {
        // TODO: set an error
        return 0;
    }
    if (size > b->len)
        size = b->len;
    MemoryCopy(reinterpret_cast<void *>(buffer), b->pBuffer, size);
    if (b->remotePath)
    {
        from.assign(b->remotePath, b->remotePathLen);
        delete[] b->remotePath;
    }
    delete[] b->pBuffer;
    delete b;

    return size;
}

uint64_t UnixSocket::writeBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    if (m_Type == Streaming)
    {
        SharedPointer<UnixSocketConnection> connection;
        SocketState state;
        {
            LockGuard<Mutex> guard(m_ConnectionLock);
            state = getStateLocked();
            connection = m_Connection;
        }

        if (!connection || state != Active)
        {
            N_NOTICE("UnixSocket::write => closed or not connected");
            return 0;
        }

        return outgoingStream(connection)->write(
            reinterpret_cast<uint8_t *>(buffer), size, bCanBlock);
    }

    if (bCanBlock)
    {
        if (!select(true, 1))
        {
            return 0;  // Interrupted
        }
    }
    else if (!select(true, 0))
    {
        // No data available.
        return 0;
    }

    struct buf *b = new struct buf;
    b->pBuffer = new char[size];
    MemoryCopy(b->pBuffer, reinterpret_cast<void *>(buffer), size);
    b->len = size;
    b->remotePath = 0;
    if (location)
    {
        b->remotePath = new char[255];
        StringCopyN(b->remotePath, reinterpret_cast<char *>(location), 255);
        b->remotePathLen = StringLength(b->remotePath);
    }
    const DatagramBuffer::Error error = m_Datagrams.write(b);
    if (error != DatagramBuffer::NoError)
    {
        delete[] b->remotePath;
        delete[] b->pBuffer;
        delete b;
        return 0;
    }

    dataChanged();

    return size;
}

bool UnixSocket::bind(UnixSocket *other, bool block)
{
    (void) block;

    if (!other || m_Type != Streaming || other->m_Type != Streaming)
    {
        return false;
    }

    SharedPointer<UnixSocketConnection> connection(
        new UnixSocketConnection());
    {
        LockGuard<Mutex> guard(m_ConnectionLock);
        if (
            m_State != Inactive || other->m_State != Inactive ||
            m_Connection || other->m_Connection)
        {
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

void UnixSocket::unbind()
{
    SharedPointer<UnixSocketConnection> connection;
    bool side = false;
    List<UnixSocket *> pending;
    {
        LockGuard<Mutex> guard(m_ConnectionLock);
        connection = m_Connection;
        if (connection)
        {
            side = m_ConnectionSide;
            connection->m_Closed[side] = true;
        }

        m_State = Closed;
        while (m_PendingSockets.count())
        {
            pending.pushBack(m_PendingSockets.popFront());
        }
    }

    N_NOTICE("UnixSocket::unbind");

    if (m_Type == Datagram)
    {
        m_Datagrams.close();
    }

    if (connection)
    {
        UnixSocketConnection::Stream *incoming =
            side ? &connection->m_SecondStream : &connection->m_FirstStream;
        UnixSocketConnection::Stream *outgoing =
            side ? &connection->m_FirstStream : &connection->m_SecondStream;
        incoming->disableReads();
        outgoing->disableWrites();
        incoming->notifyMonitors();
        outgoing->notifyMonitors();
    }

    m_Stream.disableWrites();
    m_Stream.disableReads();
    m_Stream.notifyMonitors();

    while (pending.count())
    {
        UnixSocket *socket = pending.popFront();
        socket->failConnection();
        delete socket;
    }
}

void UnixSocket::acknowledgeBind()
{
    SharedPointer<UnixSocketConnection> connection;
    {
        LockGuard<Mutex> guard(m_ConnectionLock);
        connection = m_Connection;
        if (
            !connection || connection->m_Failed ||
            connection->m_Closed[0] || connection->m_Closed[1] ||
            connection->m_Active)
        {
            return;
        }

        N_NOTICE("acking bind");

        connection->m_Active = true;
        m_State = Active;

        setCreds();
        connection->m_Creds[m_ConnectionSide ? 1 : 0] = m_Creds;
    }

    connection->m_FirstStream.notifyMonitors();
    connection->m_SecondStream.notifyMonitors();
}

bool UnixSocket::addSocket(UnixSocket *socket)
{
    SharedPointer<UnixSocketConnection> connection;
    LockGuard<Mutex> guard(m_ConnectionLock);
    if (
        m_State != Listening || !socket || !socket->m_Connection ||
        socket->m_Connection->m_Failed ||
        socket->m_Connection->m_Closed[0] ||
        socket->m_Connection->m_Closed[1])
    {
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
    if (m_Stream.write(&c, 1, false) == 1)
    {
        connection->m_FirstStream.notifyMonitors();
        connection->m_SecondStream.notifyMonitors();
        return true;
    }

    for (
        List<UnixSocket *>::Iterator it = m_PendingSockets.begin();
        it != m_PendingSockets.end(); ++it)
    {
        if (*it == socket)
        {
            m_PendingSockets.erase(it);
            break;
        }
    }
    return false;
}

UnixSocket *UnixSocket::getSocket(bool block)
{
    uint8_t c = 0;
    if (m_Stream.read(&c, 1, block) != 1)
    {
        return nullptr;
    }

    N_NOTICE("got a socket");

    LockGuard<Mutex> guard(m_ConnectionLock);

    if (m_State != Listening || !m_PendingSockets.count())
    {
        return nullptr;
    }

    N_NOTICE("popping socket");
    return m_PendingSockets.popFront();
}

void UnixSocket::addWaiter(Semaphore *waiter, bool read, bool write)
{
    if (m_Type == Datagram)
    {
        m_Datagrams.monitor(waiter);
        return;
    }

    SharedPointer<UnixSocketConnection> connection;
    bool side = false;
    {
        LockGuard<Mutex> guard(m_ConnectionLock);
        connection = m_Connection;
        if (!connection)
        {
            m_Stream.monitor(waiter);
            return;
        }
        side = m_ConnectionSide;
    }

    UnixSocketConnection::Stream *incoming =
        side ? &connection->m_SecondStream : &connection->m_FirstStream;
    UnixSocketConnection::Stream *outgoing =
        side ? &connection->m_FirstStream : &connection->m_SecondStream;

    if (read || (!read && !write))
    {
        incoming->monitor(waiter);
    }
    if (write)
    {
        outgoing->monitor(waiter);
    }
}

void UnixSocket::removeWaiter(Semaphore *waiter)
{
    if (m_Type == Datagram)
    {
        m_Datagrams.cullMonitorTargets(waiter);
        return;
    }

    SharedPointer<UnixSocketConnection> connection;
    {
        LockGuard<Mutex> guard(m_ConnectionLock);
        connection = m_Connection;
        if (!connection)
        {
            m_Stream.cullMonitorTargets(waiter);
            return;
        }
    }

    connection->m_FirstStream.cullMonitorTargets(waiter);
    connection->m_SecondStream.cullMonitorTargets(waiter);
}

void UnixSocket::addWaiter(Thread *thread, Event *event)
{
    if (m_Type == Datagram)
    {
        m_Datagrams.monitor(thread, event);
        return;
    }

    SharedPointer<UnixSocketConnection> connection;
    {
        LockGuard<Mutex> guard(m_ConnectionLock);
        connection = m_Connection;
        if (!connection)
        {
            m_Stream.monitor(thread, event);
            return;
        }
    }

    connection->m_FirstStream.monitor(thread, event);
    connection->m_SecondStream.monitor(thread, event);
}

void UnixSocket::removeWaiter(Event *event)
{
    if (m_Type == Datagram)
    {
        m_Datagrams.cullMonitorTargets(event);
        return;
    }

    SharedPointer<UnixSocketConnection> connection;
    {
        LockGuard<Mutex> guard(m_ConnectionLock);
        connection = m_Connection;
        if (!connection)
        {
            m_Stream.cullMonitorTargets(event);
            return;
        }
    }

    connection->m_FirstStream.cullMonitorTargets(event);
    connection->m_SecondStream.cullMonitorTargets(event);
}

bool UnixSocket::markListening()
{
    LockGuard<Mutex> guard(m_ConnectionLock);

    if (m_Type != Streaming)
    {
        // can't listen() on a non-streaming socket
        return false;
    }

    if (m_State != Inactive)
    {
        // can't listen on a bound socket
        return false;
    }

    setCreds();
    m_State = Listening;
    return true;
}

UnixSocket::SocketState UnixSocket::getState() const
{
    LockGuard<Mutex> guard(m_ConnectionLock);
    return getStateLocked();
}

UnixSocket::SocketState UnixSocket::getStateLocked() const
{
    if (m_Type != Streaming || !m_Connection)
    {
        return m_State;
    }

    if (
        m_Connection->m_Failed ||
        m_Connection->m_Closed[m_ConnectionSide ? 1 : 0] ||
        m_Connection->m_Closed[m_ConnectionSide ? 0 : 1])
    {
        return Closed;
    }

    return m_Connection->m_Active ? Active : Connecting;
}

bool UnixSocket::wasConnected() const
{
    LockGuard<Mutex> guard(m_ConnectionLock);
    return m_Connection && m_Connection->m_Active && !m_Connection->m_Failed;
}

void UnixSocket::failConnection()
{
    SharedPointer<UnixSocketConnection> connection;
    {
        LockGuard<Mutex> guard(m_ConnectionLock);
        connection = m_Connection;
        if (!connection)
        {
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
    connection->m_FirstStream.notifyMonitors();
    connection->m_SecondStream.notifyMonitors();
}

struct ucred UnixSocket::getPeerCredentials() const
{
    LockGuard<Mutex> guard(m_ConnectionLock);
    if (!m_Connection)
    {
        struct ucred empty;
        empty.uid = -1;
        empty.gid = -1;
        empty.pid = -1;
        return empty;
    }

    return m_Connection->m_Creds[m_ConnectionSide ? 0 : 1];
}

UnixSocketConnection::Stream *UnixSocket::incomingStream(
    const SharedPointer<UnixSocketConnection> &connection) const
{
    return m_ConnectionSide ? &connection->m_SecondStream
                            : &connection->m_FirstStream;
}

UnixSocketConnection::Stream *UnixSocket::outgoingStream(
    const SharedPointer<UnixSocketConnection> &connection) const
{
    return m_ConnectionSide ? &connection->m_FirstStream
                            : &connection->m_SecondStream;
}

void UnixSocket::setCreds()
{
#if THREADS
    Process *pCurrentProcess =
        Processor::information().getCurrentThread()->getParent();
    m_Creds.uid = pCurrentProcess->getUserId();
    m_Creds.gid = pCurrentProcess->getGroupId();
    m_Creds.pid = pCurrentProcess->getId();
#endif
}

UnixDirectory::UnixDirectory(const String &name, Filesystem *pFs, File *pParent)
    : Directory(name, 0, 0, 0, 0, pFs, 0, pParent), m_Lock()
{
    cacheDirectoryContents();
}

UnixDirectory::~UnixDirectory()
{
}

bool UnixDirectory::addEntry(const String &filename, File *pFile)
{
    LockGuard<Mutex> guard(m_Lock);
    addDirectoryEntry(filename, pFile);
    return true;
}

bool UnixDirectory::removeEntry(File *pFile)
{
    LockGuard<Mutex> guard(m_Lock);
    remove(pFile->getName().view());
    return true;
}

void UnixDirectory::cacheDirectoryContents()
{
    markCachePopulated();
}

UnixFilesystem::UnixFilesystem() : Filesystem(), m_pRoot(0)
{
    UnixDirectory *pRoot = new UnixDirectory(String(""), this, 0);
    pRoot->addEntry(String("."), pRoot);
    pRoot->addEntry(String(".."), pRoot);

    m_pRoot = pRoot;
    VFS::instance().trackFile(m_pRoot);

    // allow owner/group rwx but others only r-x on the filesystem root
    m_pRoot->setPermissions(
        FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR |
        FILE_OX);
}

UnixFilesystem::~UnixFilesystem()
{
    Directory::fromFile(m_pRoot)->emptyCache();
    if (!VFS::instance().untrackFile(m_pRoot))
    {
        ERROR("UnixFilesystem::~UnixFilesystem: root didn't get destroyed");
    }
}

Mutex &UnixFilesystem::namespaceLock()
{
    return m_NamespaceLock;
}

bool UnixFilesystem::createFile(
    File *parent, const String &filename, uint32_t mask)
{
    UnixDirectory *pParent =
        static_cast<UnixDirectory *>(Directory::fromFile(parent));

    UnixSocket *pSocket = new UnixSocket(filename, this, parent);
    if (!pParent->addEntry(filename, pSocket))
    {
        delete pSocket;
        return false;
    }

    // give owner/group full permission to the socket by default
    pSocket->setPermissions(
        FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR |
        FILE_OX);

    return true;
}

bool UnixFilesystem::createDirectory(
    File *parent, const String &filename, uint32_t mask)
{
    UnixDirectory *pParent =
        static_cast<UnixDirectory *>(Directory::fromFile(parent));

    UnixDirectory *pChild = new UnixDirectory(filename, this, parent);
    if (!pParent->addEntry(filename, pChild))
    {
        delete pChild;
        return false;
    }

    pChild->addEntry(String("."), pChild);
    pChild->addEntry(String(".."), pParent);

    // give owner/group full permission to the directory by default
    pChild->setPermissions(
        FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR |
        FILE_OX);

    return true;
}

bool UnixFilesystem::remove(File *parent, File *file)
{
    UnixDirectory *pParent =
        static_cast<UnixDirectory *>(Directory::fromFile(parent));
    return pParent->removeEntry(file);
}
