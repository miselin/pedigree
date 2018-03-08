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
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/processor/Processor.h"
#include "subsys/posix/logging.h"

UnixSocket::UnixSocket(String name, Filesystem *pFs, File *pParent, UnixSocket *other, SocketType type)
    : File(name, 0, 0, 0, 0, pFs, 0, pParent), m_Type(type), m_State(Inactive),
      m_Datagrams(MAX_UNIX_DGRAM_BACKLOG), m_pOther(other), m_Stream(MAX_UNIX_STREAM_QUEUE),
      m_PendingSockets(), m_Mutex(false)
#ifdef THREADS
      , m_AckWaiter(0)
#endif
      , m_Creds()
{
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
    // unbind from the other side of our connection if needed
    if (m_Type == Streaming)
    {
        if (m_pOther)
        {
            /// \todo update read/write to handle the other socket going away correctly
            assert(m_pOther->m_pOther == this);
            m_pOther->m_pOther = nullptr;
            m_pOther->m_State = Inactive;
        }
    }

    // remove name on disk that points to us
    if (getName().length() > 0)
    {
        Directory *parent = Directory::fromFile(getParent());
        parent->remove(getName());
    }
}

int UnixSocket::select(bool bWriting, int timeout)
{
    if (m_Type == Streaming)
    {
        if (m_State == Inactive || m_State == Connecting)
        {
            return false;
        }

        if (bWriting)
        {
            if (m_pOther->m_Stream.canWrite(timeout == 1))
            {
                return true;
            }
        }
        else
        {
            if (m_Stream.canRead(timeout == 1))
            {
                return true;
            }
        }

        return false;
    }
    else
    {
        if (timeout)
        {
            return m_Datagrams.waitFor(bWriting ? RingBufferWait::Writing : RingBufferWait::Reading);
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

uint64_t UnixSocket::read(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    String remote;
    return recvfrom(size, buffer, bCanBlock, remote);
}

uint64_t UnixSocket::recvfrom(uint64_t size, uintptr_t buffer, bool bCanBlock, String &from)
{
    if (m_State != Active)
    {
        // attempt a read if at all possible to clear out remainder of socket
        // but non-blocking so we return 0 on true EOF
        N_NOTICE("UnixSocket::read => EOF (reading remainder of stream first)");
        return m_Stream.read(reinterpret_cast<uint8_t *>(buffer), size, false);
    }

    if (m_pOther)
    {
        from = String();
        return m_Stream.read(reinterpret_cast<uint8_t *>(buffer), size, bCanBlock);
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

    struct buf *b = m_Datagrams.read();
    if (size > b->len)
        size = b->len;
    MemoryCopy(reinterpret_cast<void *>(buffer), b->pBuffer, size);
    if (b->remotePath)
    {
        from = b->remotePath;
        delete [] b->remotePath;
    }
    delete [] b->pBuffer;
    delete b;

    return size;
}

uint64_t UnixSocket::write(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    if (m_State != Active)
    {
        // other side has gone away, EOF
        N_NOTICE("UnixSocket::write => EOF");
        return 0;
    }

    if (m_pOther)
    {
        return m_pOther->m_Stream.write(reinterpret_cast<uint8_t *>(buffer), size, bCanBlock);
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
    }
    m_Datagrams.write(b);

    dataChanged();

    return size;
}

bool UnixSocket::bind(UnixSocket *other, bool block)
{
    if (other->m_pOther)
    {
        ERROR("UnixSocket: trying to bind a socket that's already bound");
        return false;
    }

    if (m_State != Inactive)
    {
        return false;
    }

    m_pOther = other;
    other->m_pOther = this;

    m_State = Connecting;
    m_pOther->m_State = Connecting;

#ifdef THREADS
    Process *pCurrentProcess = Processor::information().getCurrentThread()->getParent();
    m_Creds.uid = pCurrentProcess->getUserId();
    m_Creds.gid = pCurrentProcess->getGroupId();
    m_Creds.pid = pCurrentProcess->getId();

    if (block)
    {
        m_AckWaiter.acquire();

        if (m_State != Active)
        {
            return false;
        }
    }
#endif

    return true;
}

void UnixSocket::unbind()
{
    if (!m_pOther)
    {
        return;
    }

    N_NOTICE("UnixSocket::unbind");

    m_State = Closed;
    m_pOther->m_State = Closed;

#ifdef THREADS
    m_AckWaiter.release();
    m_pOther->m_AckWaiter.release();
#endif

    if (m_Type == Streaming)
    {
        N_NOTICE("streaming notify eof");

        // notify anything waiting on this socket that we're shutting down
        m_Stream.notifyMonitors();
        m_pOther->m_Stream.notifyMonitors();
    }
}

void UnixSocket::acknowledgeBind()
{
    if (m_State != Connecting)
    {
        return;
    }

    m_State = Active;
    m_pOther->m_State = Active;

#ifdef THREADS
    Process *pCurrentProcess = Processor::information().getCurrentThread()->getParent();
    m_Creds.uid = pCurrentProcess->getUserId();
    m_Creds.gid = pCurrentProcess->getGroupId();
    m_Creds.pid = pCurrentProcess->getId();

    m_AckWaiter.release();
    m_pOther->m_AckWaiter.release();
#endif
}

void UnixSocket::addSocket(UnixSocket *socket)
{
    LockGuard<Mutex> guard(m_Mutex);

    if (m_State != Listening)
    {
        // not listening
        return;
    }

    m_PendingSockets.pushBack(socket);

    // No data moving on listen sockets so we use the stream buffer as a signaling primitive.
    uint8_t c = 0;
    m_Stream.write(&c, 1);
}

UnixSocket *UnixSocket::getSocket(bool block)
{
    LockGuard<Mutex> guard(m_Mutex);

    if (m_State != Listening)
    {
        // not listening
        return nullptr;
    }

    uint8_t c = 0;
    if (m_Stream.read(&c, 1, block) != 1)
    {
        return nullptr;
    }

    UnixSocket *result = m_PendingSockets.popFront();
    result->acknowledgeBind();
    return result;
}

void UnixSocket::addWaiter(Semaphore *waiter)
{
    m_Stream.monitor(waiter);
    if (m_pOther)
    {
        m_pOther->m_Stream.monitor(waiter);
    }
}

void UnixSocket::removeWaiter(Semaphore *waiter)
{
    m_Stream.cullMonitorTargets(waiter);
    if (m_pOther)
    {
        m_pOther->m_Stream.cullMonitorTargets(waiter);
    }
}

void UnixSocket::addWaiter(Thread *thread, Event *event)
{
    m_Stream.monitor(thread, event);
    if (m_pOther)
    {
        m_pOther->m_Stream.monitor(thread, event);
    }
}

void UnixSocket::removeWaiter(Event *event)
{
    m_Stream.cullMonitorTargets(event);
    if (m_pOther)
    {
        m_pOther->m_Stream.cullMonitorTargets(event);
    }
}

bool UnixSocket::markListening()
{
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

    m_State = Listening;
    return true;
}

UnixDirectory::UnixDirectory(String name, Filesystem *pFs, File *pParent)
    : Directory(name, 0, 0, 0, 0, pFs, 0, pParent), m_Lock(false)
{
    cacheDirectoryContents();
}

UnixDirectory::~UnixDirectory()
{
}

bool UnixDirectory::addEntry(String filename, File *pFile)
{
    LockGuard<Mutex> guard(m_Lock);
    addDirectoryEntry(filename, pFile);
    return true;
}

bool UnixDirectory::removeEntry(File *pFile)
{
    String filename = pFile->getName();

    LockGuard<Mutex> guard(m_Lock);
    remove(filename);
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

    // allow owner/group rwx but others only r-x on the filesystem root
    m_pRoot->setPermissions(
        FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR |
        FILE_OX);
}

UnixFilesystem::~UnixFilesystem()
{
    delete m_pRoot;
}

bool UnixFilesystem::createFile(File *parent, String filename, uint32_t mask)
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
    File *parent, String filename, uint32_t mask)
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
