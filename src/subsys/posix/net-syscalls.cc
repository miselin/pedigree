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

#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS  // don't need them here

#include "modules/system/vfs/File.h"
#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Tree.h"

#include "modules/system/lwip/include/lwip/api.h"
#include "modules/system/lwip/include/lwip/ip_addr.h"
#include "modules/system/lwip/include/lwip/tcp.h"

#include "pedigree/kernel/Subsystem.h"
#include <PosixSubsystem.h>
#include <FileDescriptor.h>
#include <UnixFilesystem.h>

#include "file-syscalls.h"
#include "net-syscalls.h"

#include <fcntl.h>

#ifndef UTILITY_LINUX
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#endif

#include <sys/un.h>
#include <netinet/in.h>

static Tree<struct netconn *, struct netconnMetadata *> g_NetconnMetadata;

extern UnixFilesystem *g_pUnixFilesystem;

netconnMetadata::netconnMetadata() : recv(0), send(0), error(false), lock(false), semaphores(), offset(0), pb(nullptr), buf(nullptr)
{
}

static void netconnCallback(struct netconn *conn, enum netconn_evt evt, u16_t len)
{
    struct netconnMetadata *meta = getNetconnMetadata(conn);

    meta->lock.acquire();

    switch(evt)
    {
        case NETCONN_EVT_RCVPLUS:
            NOTICE("RCV+");
            ++(meta->recv);
            break;
        case NETCONN_EVT_RCVMINUS:
            NOTICE("RCV-");
            if (meta->recv)
            {
                --(meta->recv);
            }
            break;
        case NETCONN_EVT_SENDPLUS:
            NOTICE("SND+");
            meta->send = 1;
            break;
        case NETCONN_EVT_SENDMINUS:
            NOTICE("SND-");
            meta->send = 0;
            break;
        case NETCONN_EVT_ERROR:
            NOTICE("ERR");
            meta->error = true;  /// \todo figure out how to bubble errors
            break;
        default:
            NOTICE("Unknown error.");
    }

    /// \todo need a way to do this with lwip when threads are off
#ifdef THREADS
    for (auto &it : meta->semaphores)
    {
        it->release();
    }
#endif

    meta->lock.release();
}

static void lwipToSyscallError(err_t err)
{
    if (err != ERR_OK)
    {
        N_NOTICE(" -> lwip strerror gives '" << lwip_strerr(err) << "'");
    }
    // Based on lwIP's err_to_errno_table.
    switch(err)
    {
        case ERR_OK:
            break;
        case ERR_MEM:
            SYSCALL_ERROR(OutOfMemory);
            break;
        case ERR_BUF:
            SYSCALL_ERROR(NoMoreBuffers);
            break;
        case ERR_TIMEOUT:
            SYSCALL_ERROR(TimedOut);
            break;
        case ERR_RTE:
            SYSCALL_ERROR(HostUnreachable);
            break;
        case ERR_INPROGRESS:
            SYSCALL_ERROR(InProgress);
            break;
        case ERR_VAL:
            SYSCALL_ERROR(InvalidArgument);
            break;
        case ERR_WOULDBLOCK:
            SYSCALL_ERROR(NoMoreProcesses);
            break;
        case ERR_USE:
            // address in use
            SYSCALL_ERROR(InvalidArgument);
            break;
        case ERR_ALREADY:
            SYSCALL_ERROR(Already);
            break;
        case ERR_ISCONN:
            SYSCALL_ERROR(IsConnected);
            break;
        case ERR_CONN:
            SYSCALL_ERROR(NotConnected);
            break;
        case ERR_IF:
            // no error
            break;
        case ERR_ABRT:
            SYSCALL_ERROR(ConnectionAborted);
            break;
        case ERR_RST:
            SYSCALL_ERROR(ConnectionReset);
            break;
        case ERR_CLSD:
            SYSCALL_ERROR(NotConnected);
            break;
        case ERR_ARG:
            SYSCALL_ERROR(IoError);
            break;
    }
}

#ifdef UTILITY_LINUX
#include <vector>

std::vector<FileDescriptor *> g_Descriptors;

static FileDescriptor *getDescriptor(int socket)
{
    if (socket >= g_Descriptors.size())
    {
        return nullptr;
    }

    return g_Descriptors[socket];
}

static void addDescriptor(int socket, FileDescriptor *f)
{
    FileDescriptor *old = getDescriptor(socket);
    if (old)
    {
        delete old;
    }

    if (socket > g_Descriptors.capacity())
    {
        g_Descriptors.reserve(socket + 1);
    }

    g_Descriptors.insert(g_Descriptors.begin() + socket, f);
}

static size_t getAvailableDescriptor()
{
    return g_Descriptors.size();
}
#else
/// \todo move these into a common area, this code is duplicated EVERYWHERE
static PosixSubsystem *getSubsystem()
{
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for one or both of the processes!");
        return nullptr;
    }

    return pSubsystem;
}

static FileDescriptor *getDescriptor(int socket)
{
    PosixSubsystem *pSubsystem = getSubsystem();
    return pSubsystem->getFileDescriptor(socket);
}

static void addDescriptor(int socket, FileDescriptor *f)
{
    PosixSubsystem *pSubsystem = getSubsystem();
    pSubsystem->addFileDescriptor(socket, f);
}

static size_t getAvailableDescriptor()
{
    PosixSubsystem *pSubsystem = getSubsystem();
    return pSubsystem->getFd();
}
#endif

/// Pass is_create = true to indicate that the operation is permitted to
// operate if the socket does not yet have valid members (i.e. before a bind).
static bool isSaneSocket(FileDescriptor *f, bool is_create = false)
{
    if (!f)
    {
        N_NOTICE(" -> isSaneSocket: descriptor is null");
        SYSCALL_ERROR(BadFileDescriptor);
        return false;
    }

    if (is_create)
    {
        return true;
    }

    if (!(f->file || f->socket))
    {
        N_NOTICE(" -> isSaneSocket: both file and socket members are null");
        SYSCALL_ERROR(BadFileDescriptor);
        return false;
    }

    if (f->file && f->socket)
    {
        N_NOTICE(" -> isSaneSocket: both file and socket members are non-null");
        SYSCALL_ERROR(BadFileDescriptor);
        return false;
    }

    return true;
}

static ip_addr_t sockaddrToIpaddr(const struct sockaddr *saddr, uint16_t &port)
{
    ip_addr_t result;
    ByteSet(&result, 0, sizeof(result));

    if (saddr->sa_family == AF_INET)
    {
        const struct sockaddr_in *sin = reinterpret_cast<const struct sockaddr_in *>(saddr);
        result.u_addr.ip4.addr = sin->sin_addr.s_addr;
        result.type = IPADDR_TYPE_V4;

        port = BIG_TO_HOST16(sin->sin_port);
    }
    else
    {
        ERROR("sockaddrToIpaddr: only AF_INET is supported at the moment.");
    }

    return result;
}

/// Bind an unnamed socket to the given file descriptor if one doesn't exist
/// yet, so things like sendto() can work immediately after socket().
static void autobindUnnamedUnixSocket(FileDescriptor *f)
{
    if (f->so_domain != AF_UNIX)
    {
        return;
    }

    // Need to create a local socket if we don't already have one
    // (i.e. not bound yet).
    if (!f->so_local)
    {
        // Create a new unnamed socket.
        f->so_local = new UnixSocket(String(), g_pUnixFilesystem, nullptr);
        f->so_localPath = String();  // unnamed.
    }
}

struct netconnMetadata *getNetconnMetadata(struct netconn *conn)
{
    struct netconnMetadata *result = g_NetconnMetadata.lookup(conn);
    if (!result)
    {
        result = new struct netconnMetadata;
        g_NetconnMetadata.insert(conn, result);
    }

    return result;
}

NetworkSyscalls::NetworkSyscalls(int domain, int type, int protocol) : m_Domain(domain), m_Type(type), m_Protocol(protocol), m_Fd(nullptr) {}

NetworkSyscalls::~NetworkSyscalls() = default;

bool NetworkSyscalls::create()
{
    return true;
}

int NetworkSyscalls::shutdown(int socket, int how)
{
    return 0;
}


LwipSocketSyscalls::LwipSocketSyscalls(int domain, int type, int protocol) : NetworkSyscalls(domain, type, protocol), m_Socket(nullptr) {}

LwipSocketSyscalls::~LwipSocketSyscalls()
{
    if (m_Socket)
    {
        netconn_delete(new_conn);
        m_Socket = nullptr;
    }
}

bool LwipSocketSyscalls::create()
{
    netconn_type connType = NETCONN_INVALID;

    if (domain == AF_INET)
    {
        switch (protocol)
        {
            case IPPROTO_TCP:
                connType = NETCONN_TCP;
                break;
            case IPPROTO_UDP:
                connType = NETCONN_UDP;
                break;
        }
    }
    else if (domain == AF_INET6)
    {
        switch (protocol)
        {
            case IPPROTO_TCP:
                connType = NETCONN_TCP_IPV6;
                break;
            case IPPROTO_UDP:
                connType = NETCONN_UDP_IPV6;
                break;
        }
    }
    else if (domain == AF_PACKET)
    {
        connType = NETCONN_RAW;
    }
    else
    {
        WARNING("LwipSocketSyscalls: domain " << domain << " is not known!");
        SYSCALL_ERROR(InvalidArgument);
        return false;
    }

    if (connType == NETCONN_INVALID)
    {
        N_NOTICE("LwipSocketSyscalls: invalid socket creation parameters");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    m_Socket = netconn_new_with_callback(connType, netconnCallback);
    if (!m_Socket)
    {
        lwipToSyscallError(err);
        return false;
    }

    return true;
}

int LwipSocketSyscalls::connect(struct sockaddr *address, socklen_t addrlen)
{
    /// \todo need to figure out if we've already done a bind()
    ip_addr_t ipaddr;
    ByteSet(&ipaddr, 0, sizeof(ipaddr));
    err_t err = netconn_bind(m_socket, &ipaddr, 0);  // bind to any address
    if (err != ERR_OK)
    {
        N_NOTICE(" -> lwip error when binding before connect");
        lwipToSyscallError(err);
        return -1;
    }

    uint16_t port = 0;
    ipaddr = sockaddrToIpaddr(address, port);

    // set blocking status if needed
    bool blocking = !((getFileDescriptor()->flflags & O_NONBLOCK) == O_NONBLOCK);
    netconn_set_nonblocking(m_Socket, blocking ? 0 : 1);

    N_NOTICE("using socket " << m_Socket << "!");
    N_NOTICE(" -> connecting to remote " << ipaddr_ntoa(&ipaddr) << " on port " << Dec << port);

    err = netconn_connect(m_Socket, &ipaddr, port);
    if (err != ERR_OK)
    {
        N_NOTICE(" -> lwip error");
        lwipToSyscallError(err);
        return -1;
    }

    return 0;
}

ssize_t LwipSocketSyscalls::sendto(void *buffer, size_t bufferlen, int flags, struct sockaddr *address, socklen_t addrlen)
{
    //
}

ssize_t LwipSocketSyscalls::recvfrom(void *buffer, size_t bufferlen, int flags, struct sockaddr *address, socklen_t *addrlen)
{
    //
}

int LwipSocketSyscalls::listen(int backlog)
{
    //
}

int LwipSocketSyscalls::bind(const struct sockaddr *address, socklen_t addrlen)
{
    //
}

int LwipSocketSyscalls::accept(struct sockaddr *address, socklen_t *addrlen)
{
    //
}

int LwipSocketSyscalls::shutdown(int socket, int how)
{
    //
}

int LwipSocketSyscalls::posix_getpeername(struct sockaddr *address, socklen_t *address_len)
{
    //
}

int LwipSocketSyscalls::posix_getsockname(struct sockaddr *address, socklen_t *address_len)
{
    //
}

int LwipSocketSyscalls::posix_setsockopt(int level, int optname, const void *optvalue, socklen_t optlen)
{
    //
}

int LwipSocketSyscalls::posix_getsockopt(int level, int optname, void *optvalue, socklen_t *optlen)
{
    //
}































int posix_socket(int domain, int type, int protocol)
{
    N_NOTICE("socket(" << domain << ", " << type << ", " << protocol << ")");

    size_t fd = getAvailableDescriptor();

    netconn_type connType = NETCONN_INVALID;

    File *file = nullptr;
    struct netconn *conn = nullptr;
    bool valid = true;

    NetworkSyscalls *syscalls;

    if (domain == AF_UNIX)
    {
        syscalls = new UnixSocketSyscalls(domain, type, protocol);
    }
    else
    {
        syscalls = new LwipSocketSyscalls(domain, type, protocol);
    }

    if (!syscalls->create())
    {
        return -1;
    }

    FileDescriptor *f = new FileDescriptor;
    f->networkImpl = syscalls;
    f->fd = fd;
    addDescriptor(fd, f);
    syscalls->associate(f);

    N_NOTICE("  -> " << Dec << fd << Hex);
    return static_cast<int>(fd);
}

int posix_connect(int sock, struct sockaddr *address, socklen_t addrlen)
{
    N_NOTICE("connect");

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(address), addrlen,
            PosixSubsystem::SafeRead))
    {
        N_NOTICE("connect -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE(
        "connect(" << sock << ", " << reinterpret_cast<uintptr_t>(address)
                         << ", " << addrlen << ")");

    FileDescriptor *f = getDescriptor(sock);
    if (!isSaneSocket(f, true))
    {
        return -1;
    }

    if (f->so_domain == AF_UNIX)
    {
        if (address->sa_family != AF_UNIX)
        {
            // EAFNOSUPPORT
            N_NOTICE(" -> address family unsupported");
            return -1;
        }

        autobindUnnamedUnixSocket(f);

        // Valid state. Need to find the target socket.
        struct sockaddr_un *un =
            reinterpret_cast<struct sockaddr_un *>(address);
        String pathname;
        normalisePath(pathname, un->sun_path);

        N_NOTICE(" -> unix connect: '" << pathname << "'");

        f->file = VFS::instance().find(pathname);
        if (!f->file)
        {
            SYSCALL_ERROR(DoesNotExist);
            N_NOTICE(" -> unix socket '" << pathname << "' doesn't exist");
            return -1;
        }

        if (!f->file->isSocket())
        {
            /// \todo wrong error
            SYSCALL_ERROR(DoesNotExist);
            N_NOTICE(
                " -> target '" << pathname << "' is not a unix socket");
            return -1;
        }

        if (f->so_type == SOCK_STREAM)
        {
            // Add a new unnamed socket for this connection on the server side.
            UnixSocket *remote = new UnixSocket(String(), g_pUnixFilesystem, nullptr);
            UnixSocket *server = static_cast<UnixSocket *>(f->file);
            server->addSocket(remote);
            f->so_local->bind(remote);
        }

        f->so_remotePath = pathname;

        return 0;
    }
    else if (f->so_domain != address->sa_family)
    {
        // EAFNOSUPPORT
        N_NOTICE(" -> incorrect address family passed to connect()");
        return -1;
    }

    struct netconn *conn = f->socket;

    /// \todo need to figure out if we've already done a bind()
    ip_addr_t ipaddr;
    ByteSet(&ipaddr, 0, sizeof(ipaddr));
    err_t err = netconn_bind(conn, &ipaddr, 0);  // bind to any address
    if (err != ERR_OK)
    {
        N_NOTICE(" -> lwip error when binding before connect");
        lwipToSyscallError(err);
        return -1;
    }

    uint16_t port = 0;
    ipaddr = sockaddrToIpaddr(address, port);

    // set blocking status if needed
    bool blocking = !((f->flflags & O_NONBLOCK) == O_NONBLOCK);
    netconn_set_nonblocking(conn, blocking ? 0 : 1);

    N_NOTICE("using socket " << conn << "!");
    N_NOTICE(" -> connecting to remote " << ipaddr_ntoa(&ipaddr) << " on port " << Dec << port);

    err = netconn_connect(conn, &ipaddr, port);
    if (err != ERR_OK)
    {
        N_NOTICE(" -> lwip error");
        lwipToSyscallError(err);
        return -1;
    }

    return 0;
}

ssize_t posix_send(int sock, const void *buff, size_t bufflen, int flags)
{
    N_NOTICE("send");

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(buff), bufflen,
            PosixSubsystem::SafeRead))
    {
        N_NOTICE("send -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE(
        "send(" << sock << ", " << buff << ", " << bufflen << ", "
                      << flags << ")");

    FileDescriptor *f = getDescriptor(sock);
    if (!isSaneSocket(f))
    {
        return -1;
    }

    if (f->so_domain == AF_UNIX)
    {
        return f->file->write(
            reinterpret_cast<uintptr_t>(
                static_cast<const char *>(f->so_localPath)),
            bufflen, reinterpret_cast<uintptr_t>(buff),
            (f->flflags & O_NONBLOCK) == 0);
    }
    else if (f->socket)
    {
        struct netconnMetadata *meta = getNetconnMetadata(f->socket);

        // Handle non-blocking sockets that would block to send.
        bool blocking = !((f->flflags & O_NONBLOCK) == O_NONBLOCK);
        if (!blocking && !meta->send)
        {
            // Can't safely send.
            SYSCALL_ERROR(NoMoreProcesses);
            return -1;
        }

        /// \todo flags
        size_t bytesWritten = 0;
        err_t err = netconn_write_partly(f->socket, buff, bufflen, 0, &bytesWritten);
        if (err != ERR_OK)
        {
            N_NOTICE(" -> lwip failed in netconn_write");
            lwipToSyscallError(err);
            return -1;
        }

        return bytesWritten;
    }
    else
    {
        // invalid somehow
        return -1;
    }
}

ssize_t posix_sendto(
    int sock, void *buff, size_t bufflen, int flags, struct sockaddr *address,
    socklen_t addrlen)
{
    N_NOTICE("sendto");

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(buff), bufflen,
            PosixSubsystem::SafeRead))
    {
        N_NOTICE("sendto -> invalid address for transmission buffer");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE(
        "sendto(" << sock << ", " << buff << ", " << bufflen << ", "
                        << flags << ", " << address << ", " << addrlen << ")");

    FileDescriptor *f = getDescriptor(sock);

    autobindUnnamedUnixSocket(f);

    if (!isSaneSocket(f))
    {
        return -1;
    }

    if (address && (f->so_domain != address->sa_family))
    {
        // EAFNOSUPPORT
        return -1;
    }

    if (f->so_domain == AF_UNIX)
    {
        File *pFile = 0;
        if (address)
        {
            if (!PosixSubsystem::checkAddress(
                    reinterpret_cast<uintptr_t>(address),
                    sizeof(struct sockaddr_un), PosixSubsystem::SafeRead))
            {
                N_NOTICE(
                    "sendto -> invalid address for AF_UNIX struct sockaddr_un");
                SYSCALL_ERROR(InvalidArgument);
                return -1;
            }

            const struct sockaddr_un *un =
                reinterpret_cast<const struct sockaddr_un *>(address);
            String pathname;
            normalisePath(pathname, un->sun_path);

            pFile = VFS::instance().find(pathname);
            if (!pFile)
            {
                SYSCALL_ERROR(DoesNotExist);
                N_NOTICE(" -> sendto path '" << pathname << "' does not exist");
                return -1;
            }
        }
        else
        {
            // Assume we're connect()'d, write to socket.
            /// \todo handle no connect() call yet - error out
            pFile = f->file;
        }

        /// \todo sanity check pFile?
        String s(reinterpret_cast<const char *>(buff), bufflen);
        N_NOTICE(" -> send '" << s << "'");
        ssize_t result = pFile->write(
            reinterpret_cast<uintptr_t>(
                static_cast<const char *>(f->so_localPath)),
            bufflen, reinterpret_cast<uintptr_t>(buff),
            (f->flflags & O_NONBLOCK) == 0);
        N_NOTICE(" -> " << result);

        return result;
    }

    uint16_t port = 0;
    ip_addr_t ipaddr = sockaddrToIpaddr(address, port);

    /// \todo netconn_sendto

    N_NOTICE(" -> sendto() not implemented with lwIP");

    SYSCALL_ERROR(Unimplemented);
    return -1;
}

ssize_t posix_recv(int sock, void *buff, size_t bufflen, int flags)
{
    N_NOTICE("recv");

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(buff), bufflen,
            PosixSubsystem::SafeWrite))
    {
        N_NOTICE("recv -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE(
        "recv(" << sock << ", " << buff << ", " << bufflen << ", "
                      << flags << ")");

    FileDescriptor *f = getDescriptor(sock);
    if (!isSaneSocket(f))
    {
        return -1;
    }

    if (f->so_domain == AF_UNIX)
    {
        return f->so_local->read(
            0, bufflen, reinterpret_cast<uintptr_t>(buff),
            (f->flflags & O_NONBLOCK) == 0);
    }

    err_t err = ERR_OK;

    struct netconnMetadata *meta = getNetconnMetadata(f->socket);

    // Handle non-blocking sockets having nothing pending on the socket.
    bool blocking = !((f->flflags & O_NONBLOCK) == O_NONBLOCK);
    if (!blocking && !meta->recv && !meta->pb)
    {
        // No pending data to receive.
        SYSCALL_ERROR(NoMoreProcesses);
        return -1;
    }

    if (meta->pb == nullptr)
    {
        struct pbuf *pb = nullptr;
        struct netbuf *buf = nullptr;

        // No partial data present from a previous read. Read new data from
        // the socket.
        if (NETCONNTYPE_GROUP(netconn_type(f->socket)) == NETCONN_TCP)
        {
            err = netconn_recv_tcp_pbuf(f->socket, &pb);
        }
        else
        {
            err = netconn_recv(f->socket, &buf);
        }

        if (err != ERR_OK)
        {
            N_NOTICE(" -> lwIP error");
            lwipToSyscallError(err);
            return -1;
        }

        if (pb == nullptr && buf != nullptr)
        {
            pb = buf->p;
        }

        meta->pb = pb;
        meta->buf = buf;
    }

    // now we read some things.
    size_t len = meta->offset + bufflen;
    if (len > meta->pb->tot_len)
    {
        len = meta->pb->tot_len - meta->offset;
    }

    pbuf_copy_partial(meta->pb, buff, len, meta->offset);

    // partial read?
    if ((meta->offset + len) < meta->pb->tot_len)
    {
        meta->offset += len;
    }
    else
    {
        if (meta->buf == nullptr)
        {
            pbuf_free(meta->pb);
        }
        else
        {
            // will indirectly clean up meta->pb as it's a member of the netbuf
            netbuf_free(meta->buf);
        }

        meta->pb = nullptr;
        meta->buf = nullptr;
        meta->offset = 0;
    }

    N_NOTICE(" -> " << len);
    return len;
}

ssize_t posix_recvfrom(
    int sock, void *buff, size_t bufflen, int flags, struct sockaddr *address,
    socklen_t *addrlen)
{
    N_NOTICE("recvfrom");

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(buff), bufflen,
              PosixSubsystem::SafeWrite) &&
          ((!address) || PosixSubsystem::checkAddress(
                             reinterpret_cast<uintptr_t>(addrlen),
                             sizeof(socklen_t), PosixSubsystem::SafeWrite))))
    {
        N_NOTICE("recvfrom -> invalid address for receive buffer or addrlen "
                 "parameter");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE(
        "recvfrom(" << sock << ", " << buff << ", " << bufflen << ", "
                          << flags << ", " << address << ", " << addrlen);

    FileDescriptor *f = getDescriptor(sock);
    if (!isSaneSocket(f))
    {
        return -1;
    }

    if (f->so_domain == AF_UNIX)
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
        /// \todo what happens if sun_path is empty?
        un->sun_family = AF_UNIX;
        ssize_t r = pFile->read(
            reinterpret_cast<uintptr_t>(un->sun_path), bufflen,
            reinterpret_cast<uintptr_t>(buff), (f->flflags & O_NONBLOCK) == 0);

        if ((r > 0) && address && addrlen)
        {
            *addrlen = sizeof(struct sockaddr_un);
        }

        N_NOTICE(" -> " << r);
        return r;
    }

    SYSCALL_ERROR(Unimplemented);
    return -1;
}

int posix_bind(int sock, const struct sockaddr *address, socklen_t addrlen)
{
    N_NOTICE("bind");

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(address), addrlen,
            PosixSubsystem::SafeRead))
    {
        N_NOTICE("bind -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE(
        "bind(" << sock << ", " << address << ", " << addrlen << ")");

    FileDescriptor *f = getDescriptor(sock);
    if (!isSaneSocket(f, true))
    {
        return -1;
    }

    if (address->sa_family != f->so_domain)
    {
        // EAFNOSUPPORT
        return -1;
    }

    if (f->so_domain == AF_UNIX)
    {
        /// \todo unbind existing socket if one exists.

        // Valid state. But no socket, so do the magic here.
        const struct sockaddr_un *un =
            reinterpret_cast<const struct sockaddr_un *>(address);

        if (SUN_LEN(un) == sizeof(sa_family_t))
        {
            // Bind an unnamed address.
            autobindUnnamedUnixSocket(f);
            return 0;
        }

        String adjusted_pathname;
        normalisePath(adjusted_pathname, un->sun_path);

        N_NOTICE(" -> unix bind: '" << adjusted_pathname << "'");

        File *cwd = VFS::instance().find(String("."));
        if (adjusted_pathname.endswith('/'))
        {
            // uh, that's a directory
            SYSCALL_ERROR(IsADirectory);
            return -1;
        }

        File *parentDirectory = cwd;

        const char *pDirname =
            DirectoryName(static_cast<const char *>(adjusted_pathname));
        const char *pBasename =
            BaseName(static_cast<const char *>(adjusted_pathname));

        String basename(pBasename);
        delete[] pBasename;

        if (pDirname)
        {
            // Reorder rfind result to be from beginning of string.
            String dirname(pDirname);
            delete[] pDirname;

            N_NOTICE(" -> dirname=" << dirname);

            parentDirectory = VFS::instance().find(dirname);
            if (!parentDirectory)
            {
                N_NOTICE(
                    " -> parent directory '" << dirname
                                             << "' doesn't exist");
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

        UnixSocket *socket = new UnixSocket(
            basename, parentDirectory->getFilesystem(), parentDirectory);
        if (!pDir->addEphemeralFile(socket))
        {
            /// \todo errno?
            delete socket;
            return false;
        }

        N_NOTICE(" -> basename=" << basename);

        // bind() then connect().
        f->so_local = socket;
        f->so_localPath = adjusted_pathname;

        return 0;
    }

    uint16_t port = 0;
    ip_addr_t ipaddr = sockaddrToIpaddr(address, port);

    err_t err = netconn_bind(f->socket, &ipaddr, port);
    if (err != ERR_OK)
    {
        N_NOTICE(" -> lwIP error");
        lwipToSyscallError(err);
        return -1;
    }

    return 0;
}

int posix_listen(int sock, int backlog)
{
    N_NOTICE("listen(" << sock << ", " << backlog << ")");

    FileDescriptor *f = getDescriptor(sock);
    if (!isSaneSocket(f))
    {
        return -1;
    }

    if (f->so_type != SOCK_STREAM)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    if (f->so_domain == AF_UNIX)
    {
        /// \todo set a flag on the UnixSocket so read/write fail.
        return 0;
    }

    err_t err = netconn_listen_with_backlog(f->socket, backlog);
    if (err != ERR_OK)
    {
        N_NOTICE(" -> lwIP error");
        lwipToSyscallError(err);
        return -1;
    }

    return 0;
}

int posix_accept(int sock, struct sockaddr *address, socklen_t *addrlen)
{
    N_NOTICE("accept");

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(address),
              sizeof(struct sockaddr_storage), PosixSubsystem::SafeWrite) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(addrlen), sizeof(socklen_t),
              PosixSubsystem::SafeWrite)))
    {
        N_NOTICE("accept -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE(
        "accept(" << sock << ", " << address << ", " << addrlen << ")");

    FileDescriptor *f = getDescriptor(sock);
    if (!isSaneSocket(f))
    {
        return -1;
    }

    struct netconn *new_conn = nullptr;
    File *file = nullptr;
    UnixSocket *so_local = nullptr;

    if (f->so_domain == AF_UNIX)
    {
        if (f->so_type != SOCK_STREAM)
        {
            SYSCALL_ERROR(OperationNotSupported);
            return -1;
        }

        /// \todo don't block if we aren't allowed to
        UnixSocket *remote = f->so_local->getSocket(true);
        if (remote)
        {
            if (remote->getParent())
            {
                // Named.
                String name = remote->getFullPath();

                struct sockaddr_un *sun =
                    reinterpret_cast<struct sockaddr_un *>(address);
                StringCopy(sun->sun_path, name);
                *addrlen = sizeof(sa_family_t) + name.length();
            }
            else
            {
                *addrlen = sizeof(sa_family_t);
            }

            so_local = remote;
            file = remote->getOther();
        }
    }
    else
    {
        /// \todo check family

        err_t err = netconn_accept(f->socket, &new_conn);
        if (err != ERR_OK)
        {
            N_NOTICE(" -> lwIP error");
            lwipToSyscallError(err);
            return -1;
        }

        // get the new peer
        ip_addr_t peer;
        uint16_t port;
        err = netconn_peer(new_conn, &peer, &port);
        if (err != ERR_OK)
        {
            netconn_delete(new_conn);
            lwipToSyscallError(err);
            return -1;
        }

        /// \todo handle other families
        struct sockaddr_in *sin =
            reinterpret_cast<struct sockaddr_in *>(address);
        sin->sin_family = AF_INET;
        sin->sin_port = HOST_TO_BIG16(port);
        sin->sin_addr.s_addr = peer.u_addr.ip4.addr;
        *addrlen = sizeof(sockaddr_in);
    }

    size_t fd = getAvailableDescriptor();

    FileDescriptor *desc = new FileDescriptor;
    desc->socket = new_conn;
    desc->file = file;
    desc->offset = 0;
    desc->fd = fd;
    desc->so_domain = f->so_domain;
    desc->so_type = f->so_type;
    desc->so_local = so_local;
    addDescriptor(fd, desc);

    return static_cast<int>(fd);
}

int posix_shutdown(int socket, int how)
{
    N_NOTICE("shutdown(" << socket << ", " << how << ")");

    FileDescriptor *f = getDescriptor(socket);
    if (!isSaneSocket(f))
    {
        return -1;
    }

    if (f->so_domain == AF_UNIX)
    {
        /// \todo make this a thing
        return 0;
    }

    /// \todo unix sockets don't do this

    int rx = 0;
    int tx = 0;
    if (how == SHUT_RDWR)
    {
        rx = tx = 1;
    }
    else if (how == SHUT_RD)
    {
        rx = 1;
    }
    else
    {
        tx = 1;
    }

    err_t err = netconn_shutdown(f->socket, rx, tx);
    if (err != ERR_OK)
    {
        lwipToSyscallError(err);
        return -1;
    }

    return 0;
}

int posix_getpeername(
    int socket, struct sockaddr *address, socklen_t *address_len)
{
    N_NOTICE("getpeername");

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(address),
              sizeof(struct sockaddr_storage), PosixSubsystem::SafeWrite) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(address_len), sizeof(socklen_t),
              PosixSubsystem::SafeWrite)))
    {
        N_NOTICE("getpeername -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE(
        "getpeername(" << socket << ", " << address << ", " << address_len
                             << ")");

    FileDescriptor *f = getDescriptor(socket);
    if (!isSaneSocket(f))
    {
        return -1;
    }

    if (f->so_domain == AF_UNIX)
    {
        UnixSocket *socket = static_cast<UnixSocket *>(f->file);

        /// \todo peer name needs to be connected socket not the unnamed accept() socket?
        if (f->so_type == SOCK_STREAM)
        {
            socket = f->so_local->getOther();  // get peer
        }

        String name = socket->getFullPath();

        struct sockaddr_un *sun =
            reinterpret_cast<struct sockaddr_un *>(address);
        StringCopy(sun->sun_path, name);
        *address_len = sizeof(sa_family_t) + name.length();
    }

    ip_addr_t peer;
    uint16_t port;
    err_t err = netconn_peer(f->socket, &peer, &port);
    if (err != ERR_OK)
    {
        lwipToSyscallError(err);
        return -1;
    }

    /// \todo handle other families
    struct sockaddr_in *sin =
        reinterpret_cast<struct sockaddr_in *>(address);
    sin->sin_family = AF_INET;
    sin->sin_port = HOST_TO_BIG16(port);
    sin->sin_addr.s_addr = peer.u_addr.ip4.addr;
    *address_len = sizeof(sockaddr_in);

    return 0;
}

int posix_getsockname(
    int socket, struct sockaddr *address, socklen_t *address_len)
{
    N_NOTICE("getsockname");

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(address),
              sizeof(struct sockaddr_storage), PosixSubsystem::SafeWrite) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(address_len), sizeof(socklen_t),
              PosixSubsystem::SafeWrite)))
    {
        N_NOTICE("getsockname -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE(
        "getsockname(" << socket << ", " << address << ", " << address_len
                             << ")");

    FileDescriptor *f = getDescriptor(socket);
    if (!isSaneSocket(f))
    {
        return -1;
    }

    if (f->so_domain == AF_UNIX)
    {
        UnixSocket *socket = static_cast<UnixSocket *>(f->file);

        /// \todo socket name needs to be server socket not the unnamed accept()ed socket?
        if (f->so_type == SOCK_STREAM)
        {
            socket = f->so_local;  // get self
        }

        String name = socket->getFullPath();

        struct sockaddr_un *sun =
            reinterpret_cast<struct sockaddr_un *>(address);
        StringCopy(sun->sun_path, name);
        *address_len = sizeof(sa_family_t) + name.length();
    }

    ip_addr_t self;
    uint16_t port;
    err_t err = netconn_addr(f->socket, &self, &port);
    if (err != ERR_OK)
    {
        lwipToSyscallError(err);
        return -1;
    }

    /// \todo handle other families
    struct sockaddr_in *sin =
        reinterpret_cast<struct sockaddr_in *>(address);
    sin->sin_family = AF_INET;
    sin->sin_port = HOST_TO_BIG16(port);
    sin->sin_addr.s_addr = self.u_addr.ip4.addr;
    *address_len = sizeof(sockaddr_in);

    return 0;
}

int posix_setsockopt(
    int sock, int level, int optname, const void *optvalue, socklen_t optlen)
{
    N_NOTICE("setsockopt(" << sock << ", " << level << ", " << optname << ", " << optvalue << ", " << optlen << ")");

    if (!(PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(optvalue), optlen,
            PosixSubsystem::SafeWrite)))
    {
        N_NOTICE("getsockopt -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    FileDescriptor *f = getDescriptor(sock);
    if (!isSaneSocket(f))
    {
        return -1;
    }

    if (level == IPPROTO_TCP)
    {
#ifdef TCP_NODELAY
        if (optname == TCP_NODELAY)
        {
            N_NOTICE(" -> TCP_NODELAY");
            const uint32_t *val = reinterpret_cast<const uint32_t *>(optvalue);
            N_NOTICE("  --> val=" << *val);

            // TCP_NODELAY controls Nagle's algorithm usage
            if (*val)
            {
                tcp_nagle_disable(f->socket->pcb.tcp);
            }
            else
            {
                tcp_nagle_enable(f->socket->pcb.tcp);
            }
        }
#endif
    }

    /// \todo this will have actually useful things to do

    // SO_ERROR etc
    /// \todo implement with lwIP functionality
    return -1;
}

int posix_getsockopt(
    int sock, int level, int optname, void *optvalue, socklen_t *optlen)
{
    N_NOTICE("getsockopt");

    // Check optlen first, then use it to check optvalue.
    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(optlen), sizeof(socklen_t),
              PosixSubsystem::SafeRead) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(optlen), sizeof(socklen_t),
              PosixSubsystem::SafeWrite)))
    {
        N_NOTICE("getsockopt -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }
    if (!(PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(optvalue), *optlen,
            PosixSubsystem::SafeWrite)))
    {
        N_NOTICE("getsockopt -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    FileDescriptor *f = getDescriptor(sock);
    if (!isSaneSocket(f))
    {
        return -1;
    }

    // SO_ERROR etc
    /// \todo implement with lwIP functionality
    return -1;
}

int posix_sethostname(const char *name, size_t len)
{
    N_NOTICE("sethostname");

    if (!(PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(name), len, PosixSubsystem::SafeRead)))
    {
        N_NOTICE(" -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    N_NOTICE("sethostname(" << String(name, len) << ")");

    /// \todo integrate this

    return 0;
}
