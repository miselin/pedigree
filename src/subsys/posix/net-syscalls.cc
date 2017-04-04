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

#include <syscallError.h>
#include <processor/types.h>
#include <processor/Processor.h>
#include <process/Process.h>
#include <utilities/Tree.h>
#include <vfs/File.h>
#include <vfs/VFS.h>
#include <machine/Network.h>
#include <network-stack/NetManager.h>
#include <network-stack/NetworkStack.h>
#include <network-stack/RoutingTable.h>
#include <network-stack/Tcp.h>
#include <network-stack/UdpManager.h>

#include <Subsystem.h>
#include <PosixSubsystem.h>
#include <UnixFilesystem.h>

#include "file-syscalls.h"
#include "net-syscalls.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>

int posix_socket(int domain, int type, int protocol)
{
    N_NOTICE("socket(" << domain << ", " << type << ", " << protocol << ")");

    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    size_t fd = pSubsystem->getFd();

    File* file = 0;
    bool valid = true;
    if (domain == AF_INET)
    {
        if ((type == SOCK_DGRAM && protocol == 0) || (type == SOCK_DGRAM && protocol == IPPROTO_UDP))
            file = NetManager::instance().newEndpoint(NETMAN_TYPE_UDP, protocol);
        else if ((type == SOCK_STREAM && protocol == 0) || (type == SOCK_STREAM && protocol == IPPROTO_TCP))
            file = NetManager::instance().newEndpoint(NETMAN_TYPE_TCP, protocol);
        else if (type == SOCK_RAW)
        {
            file = NetManager::instance().newEndpoint(NETMAN_TYPE_RAW, protocol);
        }
        else
        {
            SYSCALL_ERROR(InvalidArgument);
            valid = false;
        }
    }
    /*
    else if (domain == PF_SOCKET)
    {
        // raw wire-level sockets
        file = NetManager::instance().newEndpoint(NETMAN_TYPE_RAW, 0xff);
    }
    */
    else if (domain == AF_UNIX)
    {
        file = 0;
    }
    else
    {
        WARNING("domain = " << domain << " - not known!");
        SYSCALL_ERROR(InvalidArgument);
        valid = false;
    }

    if (!valid)
    {
        N_NOTICE(" -> invalid socket parameters");
        return -1;
    }

    FileDescriptor *f = new FileDescriptor;
    f->file = file;
    f->offset = 0;
    f->fd = fd;
    f->so_domain = domain;
    f->so_type = type;
    pSubsystem->addFileDescriptor(fd, f);

    N_NOTICE("  -> " << Dec << fd << Hex);
    return static_cast<int> (fd);
}

int posix_connect(int sock, struct sockaddr* address, socklen_t addrlen)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address), addrlen, PosixSubsystem::SafeRead))
    {
        N_NOTICE("connect -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE("posix_connect(" << sock << ", " << reinterpret_cast<uintptr_t>(address) << ", " << addrlen << ")");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(sock);
    if (!f || !f->file || (f->so_domain == AF_UNIX))
    {
        if(f && ((!f->file) || (f->file == f->so_local)) && (f->so_domain == AF_UNIX))
        {
            if(address->sa_family != AF_UNIX)
            {
                // EAFNOSUPPORT
                N_NOTICE(" -> address family unsupported");
                return -1;
            }

            // Valid state. But no socket, so do the magic here.
            struct sockaddr_un *un = reinterpret_cast<struct sockaddr_un *>(address);
            String pathname;
            normalisePath(pathname, un->sun_path);

            N_NOTICE(" -> unix connect: '" << pathname << "'");

            f->file = VFS::instance().find(pathname);
            if(!f->file)
            {
                SYSCALL_ERROR(DoesNotExist);
                N_NOTICE(" -> unix socket '" << pathname << "' doesn't exist");
                return -1;
            }

            if (!f->file->isSocket())
            {
                /// \todo wrong error
                SYSCALL_ERROR(DoesNotExist);
                N_NOTICE(" -> target '" << pathname << "' is not a unix socket");
                return -1;
            }

            f->so_remotePath = pathname;

            return 0;
        }

        SYSCALL_ERROR(BadFileDescriptor);
        N_NOTICE(" -> bad fd");
        return -1;
    }
    Socket *s = static_cast<Socket *>(f->file);
    if(f->so_domain != address->sa_family)
    {
        // EAFNOSUPPORT
        N_NOTICE(" -> address family unsupported");
        return -1;
    }

    Endpoint* p = s->getEndpoint();
    if(p->getType() == Endpoint::ConnectionBased)
    {
        ConnectionBasedEndpoint *ce = static_cast<ConnectionBasedEndpoint *>(p);
        int endpointState = ce->state();
        if (endpointState < Tcp::CLOSED)
        {
            if (endpointState < Tcp::ESTABLISHED)
            {
                // EALREADY - connection attempt in progress
                SYSCALL_ERROR(Already);
                N_NOTICE(" -> connection attempt in progress already");
                return -1;
            }
            else
            {
                // EISCONN - already connected
                SYSCALL_ERROR(IsConnected);
                N_NOTICE(" -> already connected");
                return -1;
            }
        }
    }

    bool blocking = !((f->flflags & O_NONBLOCK) == O_NONBLOCK);

    Endpoint::RemoteEndpoint remoteHost;

    /// \bug Assuming _in here...
    struct sockaddr_in* address_in = reinterpret_cast<struct sockaddr_in*>(address);
    IpAddress dest(address_in->sin_addr.s_addr);
    Network *iface = RoutingTable::instance().DetermineRoute(&dest);
    if(!iface || !iface->isConnected())
    {
        // Can't use this interface...
        N_NOTICE(" -> interface not connected");
        return false;
    }
    StationInfo iface_info = iface->getStationInfo();

    /// \todo Other protocols
    bool success = false;
    if (s->getProtocol() == NETMAN_TYPE_TCP)
    {
        ConnectionBasedEndpoint *ce = static_cast<ConnectionBasedEndpoint *>(p);

        struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(address);
        remoteHost.remotePort = BIG_TO_HOST16(sin->sin_port);
        remoteHost.ip.setIp(sin->sin_addr.s_addr);

        success = ce->connect(remoteHost, blocking);

        if (!blocking)
        {
            SYSCALL_ERROR(InProgress);
            N_NOTICE(" -> connection now in progress");
            return -1;
        }
    }
    else if (s->getProtocol() == NETMAN_TYPE_UDP)
    {
        // connect on a UDP socket sets a remote address and port for send/recv
        // to send to multiple addresses and receive from multiple clients
        // sendto and recvfrom must be used

        ConnectionlessEndpoint *ce = static_cast<ConnectionlessEndpoint *>(p);

        struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(address);
        remoteHost.remotePort = BIG_TO_HOST16(sin->sin_port);
        remoteHost.ip.setIp(sin->sin_addr.s_addr);

        // If no bind has been done, allocate a port and bind.
        ce->setLocalIp(iface_info.ipv4);
        if(!ce->getLocalPort())
            ce->setLocalPort(0);
        ce->setRemotePort(remoteHost.remotePort);
        ce->setRemoteIp(remoteHost.ip);
        success = true;
    }
    else if (s->getProtocol() == NETMAN_TYPE_RAW)
    {
        success = true;
    }

    N_NOTICE(" -> success=" << success);
    return success ? 0 : -1;
}

ssize_t posix_send(int sock, const void* buff, size_t bufflen, int flags)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen, PosixSubsystem::SafeRead))
    {
        N_NOTICE("send -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE("posix_send");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(sock);
    if (!f || !f->file)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    else if(f->so_domain == AF_UNIX)
    {
        return f->file->write(reinterpret_cast<uintptr_t>(static_cast<const char *>(f->so_localPath)), bufflen, reinterpret_cast<uintptr_t>(buff), (f->flflags & O_NONBLOCK) == 0);
    }

    Socket *s = static_cast<Socket *>(f->file);

    Endpoint* p = s->getEndpoint();

    bool success = false;
    if (s->getProtocol() == NETMAN_TYPE_TCP)
    {
        ConnectionBasedEndpoint *ce = static_cast<ConnectionBasedEndpoint *>(p);
        return ce->send(bufflen, reinterpret_cast<uintptr_t>(buff));
    }
    else if (s->getProtocol() == NETMAN_TYPE_UDP)
    {
        ConnectionlessEndpoint *ce = static_cast<ConnectionlessEndpoint *>(p);

        // special handling - need to check for a remote host
        IpAddress remoteIp = p->getRemoteIp();
        if (remoteIp.getIp() != 0)
        {
            Endpoint::RemoteEndpoint remoteHost;
            remoteHost.remotePort = p->getRemotePort();
            remoteHost.ip = remoteIp;
            int num = ce->send(bufflen, reinterpret_cast<uintptr_t>(buff), remoteHost, false);

            success = num >= 0;
        }
    }

    return success ? bufflen : -1;
}

ssize_t posix_sendto(int sock, void *buff, size_t bufflen, int flags, struct sockaddr *address, socklen_t addrlen)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen, PosixSubsystem::SafeRead))
    {
        N_NOTICE("sendto -> invalid address for transmission buffer");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE("posix_sendto(" << sock << ", " << buff << ", " << bufflen << ", " << flags << ", " << address << ", " << addrlen << ")");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(sock);
    if (!f || !f->file)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    if(address && (f->so_domain != address->sa_family))
    {
        // EAFNOSUPPORT
        return -1;
    }

    if(f->so_domain == AF_UNIX)
    {
        File *pFile = 0;
        if (address)
        {
            if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address), sizeof(struct sockaddr_un), PosixSubsystem::SafeRead))
            {
                N_NOTICE("sendto -> invalid address for AF_UNIX struct sockaddr_un");
                SYSCALL_ERROR(InvalidArgument);
                return -1;
            }

            const struct sockaddr_un *un = reinterpret_cast<const struct sockaddr_un *>(address);
            String pathname;
            normalisePath(pathname, un->sun_path);

            pFile = VFS::instance().find(pathname);
            if(!pFile)
            {
                SYSCALL_ERROR(DoesNotExist);
                N_NOTICE(" -> sendto path '" << pathname << "' does not exist");
                return -1;
            }
        }
        else
        {
            // Assume we're connect()'d, write to socket.
            pFile = f->file;
        }

        /// \todo sanity check pFile?
        String s(reinterpret_cast<const char *>(buff), bufflen);
        N_NOTICE(" -> send '" << s << "'");
        ssize_t result = pFile->write(reinterpret_cast<uintptr_t>(static_cast<const char *>(f->so_localPath)), bufflen, reinterpret_cast<uintptr_t>(buff), (f->flflags & O_NONBLOCK) == 0);
        N_NOTICE(" -> " << result);
    }

    Socket *s = static_cast<Socket *>(f->file);
    if (!s)
    {
        return -1;
    }

    Endpoint* p = s->getEndpoint();
    if (!p)
    {
        return -1;
    }

    if (s->getProtocol() == NETMAN_TYPE_TCP)
    {
        /// \todo I need to write a sendto for TcpManager and TcpEndpoint
        ///       which probably means UDP gets a free sendto as well.
        ///       Until then, this is NOT valid according to the standards.
        ERROR("TCP sendto called, but not implemented properly!");
        ConnectionBasedEndpoint *ce = static_cast<ConnectionBasedEndpoint *>(p);
        return ce->send(bufflen, reinterpret_cast<uintptr_t>(buff));
    }
    else if (s->getProtocol() == NETMAN_TYPE_UDP || s->getProtocol() == NETMAN_TYPE_RAW)
    {
        ConnectionlessEndpoint *ce = static_cast<ConnectionlessEndpoint *>(p);

        Endpoint::RemoteEndpoint remoteHost;
        bool remoteOk = false;
        if (address)
        {
            const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(address);
            remoteHost.remotePort = BIG_TO_HOST16(sin->sin_port);
            remoteHost.ip.setIp(sin->sin_addr.s_addr);
            remoteOk = true;
        }
        else
        {
            // special handling - need to check for a remote host
            IpAddress remoteIp = p->getRemoteIp();
            if (remoteIp.getIp() != 0)
            {
                remoteHost.remotePort = p->getRemotePort();
                remoteHost.ip = remoteIp;
                remoteOk = true;
            }
        }

        if (remoteOk)
        {
            return ce->send(bufflen, reinterpret_cast<uintptr_t>(buff), remoteHost, false);
        }
    }

    return -1;
}

ssize_t posix_recv(int sock, void* buff, size_t bufflen, int flags)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen, PosixSubsystem::SafeWrite))
    {
        N_NOTICE("recv -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE("posix_recv");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(sock);
    if (!f || !f->file)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    else if(f->so_domain == AF_UNIX)
    {
        return f->so_local->read(0, bufflen, reinterpret_cast<uintptr_t>(buff), (f->flflags & O_NONBLOCK) == 0);
    }
    Socket *s = static_cast<Socket *>(f->file);

    Endpoint* p = s->getEndpoint();

    bool blocking = !((f->flflags & O_NONBLOCK) == O_NONBLOCK);

    int ret = -1;
    if (s->getProtocol() == NETMAN_TYPE_TCP)
    {
        ConnectionBasedEndpoint *ce = static_cast<ConnectionBasedEndpoint *>(p);
        ret = ce->recv(reinterpret_cast<uintptr_t>(buff), bufflen, blocking, flags & MSG_PEEK);
    }
    else if (s->getProtocol() == NETMAN_TYPE_UDP)
    {
        /// \todo Actually, we only should read this data if it's from the IP specified
        ///       during connect - otherwise we fail (UDP should use sendto/recvfrom)
        ///       However, to do that we need to tell recv not to remove from the queue
        ///       and instead peek at the message (in other words, we need flags)
        ConnectionlessEndpoint *ce = static_cast<ConnectionlessEndpoint *>(p);
        Endpoint::RemoteEndpoint remoteHost;
        ret = ce->recv(reinterpret_cast<uintptr_t>(buff), bufflen, blocking, &remoteHost);
    }
    return ret;
}

ssize_t posix_recvfrom(int sock, void *buff, size_t bufflen, int flags, struct sockaddr *address, socklen_t *addrlen)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buff), bufflen, PosixSubsystem::SafeWrite) &&
        ((!address) || PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(addrlen), sizeof(socklen_t), PosixSubsystem::SafeWrite))))
    {
        N_NOTICE("recvfrom -> invalid address for receive buffer or addrlen parameter");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE("posix_recvfrom(" << sock << ", " << buff << ", " << bufflen << ", " << flags << ", " << address << ", " << addrlen);

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(sock);
    if (!f || !f->file)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    if(f->so_domain == AF_UNIX)
    {
        File *pFile = f->so_local;
        struct sockaddr_un un_temp;
        struct sockaddr_un *un = 0;
        if (address)
        {
            un = reinterpret_cast<struct sockaddr_un *>(address);
        }
        else
        {
            un = &un_temp;
        }

        // this will load sun_path into sun_path automatically
        un->sun_family = AF_UNIX;
        ssize_t r = pFile->read(reinterpret_cast<uintptr_t>(un->sun_path), bufflen, reinterpret_cast<uintptr_t>(buff), (f->flflags & O_NONBLOCK) == 0);

        if((r > 0) && address && addrlen)
        {
            *addrlen = sizeof(struct sockaddr_un);
        }

        N_NOTICE(" -> " << r);
        return r;
    }

    Socket *s = static_cast<Socket *>(f->file);

    Endpoint* p = s->getEndpoint();

    bool blocking = !((f->flflags & O_NONBLOCK) == O_NONBLOCK);

    int ret = -1;
    if (s->getProtocol() == NETMAN_TYPE_TCP)
    {
        ConnectionBasedEndpoint *ce = static_cast<ConnectionBasedEndpoint *>(p);

        ret = ce->recv(reinterpret_cast<uintptr_t>(buff), bufflen, blocking, flags & MSG_PEEK);

        if (address)
        {
            struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(address);
            sin->sin_port = HOST_TO_BIG16(p->getRemotePort());
            sin->sin_addr.s_addr = p->getRemoteIp().getIp();
            if (addrlen)
            {
                *addrlen = sizeof(struct sockaddr_in);
            }
        }
    }
    else if (s->getProtocol() == NETMAN_TYPE_UDP || s->getProtocol() == NETMAN_TYPE_RAW)
    {
        ConnectionlessEndpoint *ce = static_cast<ConnectionlessEndpoint *>(p);

        Endpoint::RemoteEndpoint remoteHost;
        ret = ce->recv(reinterpret_cast<uintptr_t>(buff), bufflen, blocking, &remoteHost);

        if (address)
        {
            struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(address);
            sin->sin_port = HOST_TO_BIG16(remoteHost.remotePort);
            sin->sin_addr.s_addr = remoteHost.ip.getIp();
            sin->sin_family = AF_INET;
            if (addrlen)
            {
                *addrlen = sizeof(struct sockaddr_in);
            }
        }
    }

    N_NOTICE("  -> " << Dec << ret << Hex);
    return ret;
}

int posix_bind(int sock, const struct sockaddr *address, socklen_t addrlen)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address), addrlen, PosixSubsystem::SafeRead))
    {
        N_NOTICE("bind -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE("posix_bind");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(sock);
    if (!f || !f->file)
    {
        if(f && (!f->file) && (f->so_domain == AF_UNIX))
        {
            if(address->sa_family != AF_UNIX)
            {
                // EAFNOSUPPORT
                return -1;
            }

            // Valid state. But no socket, so do the magic here.
            const struct sockaddr_un *un = reinterpret_cast<const struct sockaddr_un *>(address);

            String adjusted_pathname;
            normalisePath(adjusted_pathname, un->sun_path);

            N_NOTICE(" -> unix bind: '" << adjusted_pathname << "'");

            File *cwd = Processor::information().getCurrentThread()->getParent()->getCwd();
            if (adjusted_pathname.endswith('/'))
            {
                // uh, that's a directory
                SYSCALL_ERROR(IsADirectory);
                return -1;
            }

            File *parentDirectory = cwd;

            const char *pDirname = DirectoryName(static_cast<const char *>(adjusted_pathname));
            const char *pBasename = BaseName(static_cast<const char *>(adjusted_pathname));

            String basename(pBasename);
            delete [] pBasename;

            if (pDirname)
            {
                // Reorder rfind result to be from beginning of string.
                String dirname(pDirname);
                delete [] pDirname;

                N_NOTICE(" -> dirname=" << dirname);

                parentDirectory = VFS::instance().find(dirname);
                if (!parentDirectory)
                {
                    N_NOTICE(" -> parent directory '" << dirname << "' doesn't exist");
                    SYSCALL_ERROR(DoesNotExist);
                    return -1;
                }
            }

            if (!parentDirectory->isDirectory())
            {
                SYSCALL_ERROR(NotADirectory);
                return -1;
            }

            Directory *pDir = Directory::fromFile(parentDirectory);

            UnixSocket *socket = new UnixSocket(basename, parentDirectory->getFilesystem(), parentDirectory);
            if (!pDir->addEphemeralFile(socket))
            {
                /// \todo errno?
                delete socket;
                return false;
            }

            N_NOTICE(" -> basename=" << basename);

            // HERE: set UnixSocket mode (dgram, stream) for correct operation

            // bind() then connect().
            f->so_local = f->file = socket;
            f->so_localPath = adjusted_pathname;

            return 0;
        }

        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    Socket *s = static_cast<Socket *>(f->file);

    Endpoint* p = s->getEndpoint();
    if (p)
    {
        int ret = -1;
        if (s->getProtocol() == NETMAN_TYPE_TCP || s->getProtocol() == NETMAN_TYPE_UDP)
        {
            const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(address);

            p->setLocalPort(BIG_TO_HOST16(sin->sin_port));

            ret = 0;
        }

        return ret;
    }
    else
        return -1;
}

int posix_listen(int sock, int backlog)
{
    N_NOTICE("posix_listen");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(sock);
    if(!(f && f->file))
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    if(f->so_type != SOCK_STREAM)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    Socket *s = static_cast<Socket *>(f->file);

    Endpoint* p = s->getEndpoint();
    if(p->getType() != Endpoint::ConnectionBased)
    {
        SYSCALL_ERROR(OperationNotSupported);
        return -1;
    }

    ConnectionBasedEndpoint *ce = static_cast<ConnectionBasedEndpoint *>(p);

    if(s->getProtocol() == NETMAN_TYPE_TCP)
    {
        if((ce->state() != Tcp::CLOSED) && (ce->state() != Tcp::UNKNOWN))
        {
            ERROR("State was " << ce->state() << ".");
            SYSCALL_ERROR(InvalidArgument);
            return -1;
        }
    }

    ce->listen();

    return 0;
}

int posix_accept(int sock, struct sockaddr* address, socklen_t* addrlen)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address), sizeof(struct sockaddr_storage), PosixSubsystem::SafeWrite) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(addrlen), sizeof(socklen_t), PosixSubsystem::SafeWrite)))
    {
        N_NOTICE("accept -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE("posix_accept");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(sock);
    Socket *s1 = static_cast<Socket *>(f->file);
    if (!s1)
        return -1;

    Socket *s = static_cast<Socket *>(NetManager::instance().accept(s1));
    if (!s)
        return -1;

    // add into the descriptor table
    size_t fd = pSubsystem->getFd();

    Endpoint* e = s->getEndpoint(); // NetManager::instance().getEndpoint(f);

    if (address && addrlen)
    {
        if (s->getProtocol() == NETMAN_TYPE_TCP || s->getProtocol() == NETMAN_TYPE_UDP)
        {
            struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(address);
            sin->sin_port = HOST_TO_BIG16(e->getRemotePort());
            sin->sin_addr.s_addr = e->getRemoteIp().getIp();

            *addrlen = sizeof(struct sockaddr_in);
        }
    }

    FileDescriptor *desc = new FileDescriptor(s, 0, fd, 0, O_RDWR);
    if (desc)
    {
        NOTICE("posix_accept: new socket is fd " << Dec << fd << Hex << ".");
        pSubsystem->addFileDescriptor(fd, desc);
    }

    return static_cast<int> (fd);
}

int posix_shutdown(int socket, int how)
{
    N_NOTICE("posix_shutdown");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(socket);
    Socket *s = static_cast<Socket *>(f->file);
    if (!s)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    Endpoint *e = s->getEndpoint();
    Endpoint::ShutdownType howType;
    if(how == SHUT_RD)
        howType = Endpoint::ShutReceiving;
    else if(how == SHUT_WR)
        howType = Endpoint::ShutSending;
    else if(how == SHUT_RDWR)
        howType = Endpoint::ShutBoth;
    else
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    if(e->shutdown(howType))
        return 0;
    else
        return -1;
}

int posix_getpeername(int socket, struct sockaddr *address, socklen_t *address_len)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address), sizeof(struct sockaddr_storage), PosixSubsystem::SafeWrite) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address_len), sizeof(socklen_t), PosixSubsystem::SafeWrite)))
    {
        N_NOTICE("getpeername -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE("posix_getpeername");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(socket);
    if (!f || !f->file)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    Socket *s = static_cast<Socket *>(f->file);

    Endpoint* p = s->getEndpoint();
    if (s->getProtocol() == NETMAN_TYPE_TCP)
    {
        /// \todo this may not be accurate.
        /// \todo IPv6?
        struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(address);
        sin->sin_family = AF_INET;
        sin->sin_port = HOST_TO_BIG16(p->getRemotePort());
        sin->sin_addr.s_addr = p->getRemoteIp().getIp();
        *address_len = sizeof(struct sockaddr_in);
    }
    else
    {
        /// \todo NotConnected more correct?
        SYSCALL_ERROR(OperationNotSupported);
        return -1;
    }

    return 0;
}

int posix_getsockname(int socket, struct sockaddr *address, socklen_t *address_len)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address), sizeof(struct sockaddr_storage), PosixSubsystem::SafeWrite) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(address_len), sizeof(socklen_t), PosixSubsystem::SafeWrite)))
    {
        N_NOTICE("getsockname -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE("posix_getsockname");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(socket);
    if (!f || !f->file)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    Socket *s = static_cast<Socket *>(f->file);

    Endpoint* p = s->getEndpoint();
    /// \todo this may not be accurate.
    /// \todo IPv6?
    struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(address);
    sin->sin_family = AF_INET;
    sin->sin_port = HOST_TO_BIG16(p->getLocalPort());
    sin->sin_addr.s_addr = p->getLocalIp().getIp();
    *address_len = sizeof(struct sockaddr_in);

    return 0;
}

int posix_getsockopt(int sock, int level, int optname, void* optvalue, socklen_t *optlen)
{
    N_NOTICE("getsockopt");

    // Check optlen first, then use it to check optvalue.
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(optlen), sizeof(socklen_t), PosixSubsystem::SafeRead) &&
         PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(optlen), sizeof(socklen_t), PosixSubsystem::SafeWrite)))
    {
        N_NOTICE("getsockopt -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(optvalue), *optlen, PosixSubsystem::SafeWrite)))
    {
        N_NOTICE("getsockopt -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Valid socket?
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(sock);
    if (!f || !f->file)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        N_NOTICE(" -> bad file descriptor");
        return -1;
    }

    Socket *s = static_cast<Socket *>(f->file);

    if (level != SOL_SOCKET)
    {
        SYSCALL_ERROR(InvalidArgument);
        N_NOTICE(" -> invalid argument, level is not SOL_SOCKET");
        return -1;
    }

    switch (optname)
    {
        case SO_ERROR:
        {
            Endpoint* p = s->getEndpoint();
            int err = static_cast<int>(p->getError());
            N_NOTICE(" -> getting error [" << err << "]");
            /// \todo handle optlen < sizeof(socklen_t)!
            *reinterpret_cast<uint32_t *>(optvalue) = err;
            *optlen = sizeof(socklen_t);
            p->resetError();
            break;
        }

        default:
            N_NOTICE(" -> unknown optname " << optname);

            // Combination of level/optname not supported otherwise.
            SYSCALL_ERROR(ProtocolNotAvailable);
            return -1;
    }

    N_NOTICE(" -> 0");
    return 0;
}

int posix_sethostname(const char *name, size_t len)
{
    N_NOTICE("sethostname");

    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(name), len, PosixSubsystem::SafeRead)))
    {
        N_NOTICE(" -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE("sethostname(" << String(name, len) << ")");

    /// \todo integrate this

    return 0;
}

