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

#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/Filesystem.h"

#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/RingBuffer.h"

#include <sys/socket.h>

class Mutex;

#define MAX_UNIX_DGRAM_BACKLOG 65536
#define MAX_UNIX_STREAM_QUEUE 65536

/**
 * UnixFilesystem: UNIX sockets.
 *
 * This filesystem is mounted with the "unix" 'volume' label, and provides
 * the filesystem abstraction for UNIX sockets (at least, non-anonymous ones).
 */
class UnixFilesystem : public Filesystem
{
  public:
    UnixFilesystem();
    virtual ~UnixFilesystem();

    virtual bool initialise(Disk *pDisk)
    {
        return false;
    }

    virtual File *getRoot() const
    {
        return m_pRoot;
    }

    virtual String getVolumeLabel() const
    {
        return m_VolumeLabel;
    }

    virtual void truncate(File *pFile)
    {
    }

    virtual void fileAttributeChanged(File *pFile)
    {
    }

    virtual void cacheDirectoryContents(File *pFile)
    {
        if (pFile->isDirectory())
        {
            Directory *pDir = Directory::fromFile(pFile);
            pDir->cacheDirectoryContents();
        }
    }

    virtual void extend(File *pFile, size_t size)
    {
    }

  protected:
    virtual bool
    createFile(File *parent, const String &filename, uint32_t mask);
    virtual bool
    createDirectory(File *parent, const String &filename, uint32_t mask);
    virtual bool
    createSymlink(File *parent, const String &filename, const String &value)
    {
        return false;
    }
    virtual bool remove(File *parent, File *file);

  private:
    File *m_pRoot;

    static String m_VolumeLabel;

    virtual bool isBytewise() const
    {
        return true;
    }
};

/**
 * A UNIX socket.
 */
class UnixSocket : public File
{
  public:
    enum SocketType
    {
        Streaming,
        Datagram
    };

    enum SocketState
    {
        Listening,   // listening for connections
        Connecting,  // waiting for bind to be acked
        Inactive,    // unbound
        Active,      // bound, ready for data transfer
        Closed       // unbound but was once bound
    };

    UnixSocket(
        String name, Filesystem *pFs, File *pParent,
        UnixSocket *other = nullptr, SocketType type = Datagram);
    virtual ~UnixSocket();

    virtual uint64_t readBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t writeBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    uint64_t
    recvfrom(uint64_t size, uintptr_t buffer, bool bCanBlock, String &from);

    virtual int select(bool bWriting = false, int timeout = 0);

    virtual bool isSocket() const
    {
        return true;
    }

    UnixSocket *getOther() const
    {
        return m_pOther;
    }

    // Bind this socket to another socket.
    // The other socket should not already be bound.
    bool bind(UnixSocket *other, bool block = false);

    // Break the bound socket.
    void unbind();

    // Acknowledges binding from another socket
    void acknowledgeBind();

    // Add a new socket for a client/server connection (for accept())
    void addSocket(UnixSocket *socket);

    // Get the next socket in the listening queue (for non-datagram sockets).
    UnixSocket *getSocket(bool block = false);

    // Add a semaphore to be notified when the socket data changes.
    void addWaiter(Semaphore *waiter);

    // Remove a waiter semaphore.
    void removeWaiter(Semaphore *waiter);

    // Add an event to fire when the socket data changes.
    void addWaiter(Thread *thread, Event *event);

    // Remove a socket data change event.
    void removeWaiter(Event *event);

    // Get this socket's type
    SocketType getType() const
    {
        return m_Type;
    }

    // Get this socket's state
    SocketState getState() const
    {
        return m_State;
    }

    // Mark this socket a listening socket
    bool markListening();

    // Get our credentials.
    struct ucred getCredentials() const
    {
        return m_Creds;
    }

    // Get the credentials of the other side.
    struct ucred getPeerCredentials() const
    {
        return m_pOther->getCredentials();
    }

  private:
    typedef Buffer<uint8_t, true> UnixSocketStream;

    void setCreds();

    virtual bool isBytewise() const
    {
        return true;
    }

    struct buf
    {
        char *pBuffer;
        uint64_t len;
        char *remotePath;  // Path of the socket that dumped data here, if any.
    };

    SocketType m_Type;
    SocketState m_State;

    // For datagram sockets.

    // Note: "servers" own the actual UNIX socket address, while clients get a
    // virtual address to track their existence (or are bound to a specific
    // name themselves).
    RingBuffer<struct buf *> m_Datagrams;

    // For stream sockets.

    // Other side of the connection (for stream sockets).
    UnixSocket *m_pOther;

    // Data stream.
    UnixSocketStream m_Stream;

    // List of sockets pending accept() on this socket.
    List<UnixSocket *> m_PendingSockets;

    // Mutual exclusion for this socket.
    Mutex m_Mutex;

    // Ack waiter lock
#if THREADS
    Semaphore m_AckWaiter;
#endif

    // Credentials associated at the time of bind()
    struct ucred m_Creds;
};

/**
 * Basic Directory subclass for UNIX socket support.
 */
class UnixDirectory : public Directory
{
  public:
    UnixDirectory(String name, Filesystem *pFs, File *pParent);
    virtual ~UnixDirectory();

    bool addEntry(String filename, File *pFile);
    bool removeEntry(File *pFile);

    virtual void cacheDirectoryContents();

  private:
    Mutex m_Lock;
};

#endif
