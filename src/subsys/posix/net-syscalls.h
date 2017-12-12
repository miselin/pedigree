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

#ifndef NET_SYSCALLS_H
#define NET_SYSCALLS_H

#include "logging.h"

#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Tree.h"

#include "modules/system/lwip/include/lwip/api.h"

#include <sys/socket.h>
#include <sys/types.h>

struct sockaddr;

class Semaphore;
class FileDescriptor;
class UnixSocket;

struct netconnMetadata
{
    netconnMetadata();

    ssize_t recv;
    ssize_t send;
    bool error;

    Mutex lock;
    List<Semaphore *> semaphores;

    size_t offset;
    struct pbuf *pb;
    struct netbuf *buf;
};

class NetworkSyscalls
{
    public:
        NetworkSyscalls(int domain, int type, int protocol);
        virtual ~NetworkSyscalls();

        /// Implementation-specific final socket creation logic,
        /// implementations must set a SYSCALL_ERROR on failure.
        virtual bool create();
        virtual int connect(const struct sockaddr *address, socklen_t addrlen) = 0;

        virtual ssize_t sendto(const void *buffer, size_t bufferlen, int flags, const struct sockaddr *address, socklen_t addrlen) = 0;
        virtual ssize_t recvfrom(void *buffer, size_t bufferlen, int flags, struct sockaddr *address, socklen_t *addrlen) = 0;

        virtual int listen(int backlog) = 0;
        virtual int bind(const struct sockaddr *address, socklen_t addrlen) = 0;
        virtual int accept(struct sockaddr *address, socklen_t *addrlen) = 0;

        virtual int shutdown(int how);

        virtual int getpeername(struct sockaddr *address, socklen_t *address_len) = 0;
        virtual int getsockname(struct sockaddr *address, socklen_t *address_len) = 0;

        virtual int setsockopt(int level, int optname, const void *optvalue, socklen_t optlen) = 0;
        virtual int getsockopt(int level, int optname, void *optvalue, socklen_t *optlen) = 0;

        virtual bool canPoll() const;
        virtual bool poll(bool &read, bool &write, bool &error, Semaphore *waiter);
        virtual void unPoll(Semaphore *waiter);

        void associate(FileDescriptor *fd);

        int getDomain() const
        {
            return m_Domain;
        }

        int getType() const
        {
            return m_Type;
        }

        int getProtocol() const
        {
            return m_Protocol;
        }

        FileDescriptor *getFileDescriptor() const
        {
            return m_Fd;
        }

        bool isBlocking() const;

    protected:
        int m_Domain;
        int m_Type;
        int m_Protocol;

        FileDescriptor *m_Fd;
};

class LwipSocketSyscalls : public NetworkSyscalls
{
    public:
        LwipSocketSyscalls(int domain, int type, int protocol);
        virtual ~LwipSocketSyscalls();

        /// Implementation-specific final socket creation logic.
        virtual bool create();
        virtual int connect(const struct sockaddr *address, socklen_t addrlen);

        virtual ssize_t sendto(const void *buffer, size_t bufferlen, int flags, const struct sockaddr *address, socklen_t addrlen);
        virtual ssize_t recvfrom(void *buffer, size_t bufferlen, int flags, struct sockaddr *address, socklen_t *addrlen);

        virtual int listen(int backlog);
        virtual int bind(const struct sockaddr *address, socklen_t addrlen);
        virtual int accept(struct sockaddr *address, socklen_t *addrlen);

        virtual int shutdown(int how);

        virtual int getpeername(struct sockaddr *address, socklen_t *address_len);
        virtual int getsockname(struct sockaddr *address, socklen_t *address_len);

        virtual int setsockopt(int level, int optname, const void *optvalue, socklen_t optlen);
        virtual int getsockopt(int level, int optname, void *optvalue, socklen_t *optlen);

        virtual bool canPoll() const;
        virtual bool poll(bool &read, bool &write, bool &error, Semaphore *waiter);
        virtual void unPoll(Semaphore *waiter);

    private:
        static Tree<struct netconn *, LwipSocketSyscalls *> m_SyscallObjects;

        static void netconnCallback(struct netconn *conn, enum netconn_evt evt, uint16_t len);
        static void lwipToSyscallError(err_t err);

        struct netconn *m_Socket;

        struct LwipMetadata {
            LwipMetadata();

            ssize_t recv;
            ssize_t send;
            bool error;

            Mutex lock;
            List<Semaphore *> semaphores;

            size_t offset;
            struct pbuf *pb;
            struct netbuf *buf;
        } m_Metadata;
};

class UnixSocketSyscalls : public NetworkSyscalls
{
    public:
        UnixSocketSyscalls(int domain, int type, int protocol);
        virtual ~UnixSocketSyscalls();

        /// Implementation-specific final socket creation logic.
        virtual bool create();
        virtual int connect(const struct sockaddr *address, socklen_t addrlen);

        virtual ssize_t sendto(const void *buffer, size_t bufferlen, int flags, const struct sockaddr *address, socklen_t addrlen);
        virtual ssize_t recvfrom(void *buffer, size_t bufferlen, int flags, struct sockaddr *address, socklen_t *addrlen);

        virtual int listen(int backlog);
        virtual int bind(const struct sockaddr *address, socklen_t addrlen);
        virtual int accept(struct sockaddr *address, socklen_t *addrlen);

        virtual int shutdown(int how);

        virtual int getpeername(struct sockaddr *address, socklen_t *address_len);
        virtual int getsockname(struct sockaddr *address, socklen_t *address_len);

        virtual int setsockopt(int level, int optname, const void *optvalue, socklen_t optlen);
        virtual int getsockopt(int level, int optname, void *optvalue, socklen_t *optlen);

    private:
        UnixSocket *m_Socket;
        UnixSocket *m_Remote;  // other side of the unix socket

        String m_LocalPath;
        String m_RemotePath;
};

/// Get metadata for a given lwIP connection.
struct netconnMetadata *getNetconnMetadata(struct netconn *conn);

int posix_socket(int domain, int type, int protocol);
int posix_connect(int sock, const struct sockaddr *address, socklen_t addrlen);

ssize_t posix_send(int sock, const void *buff, size_t bufflen, int flags);
ssize_t posix_sendto(
    int sock, const void *buff, size_t bufflen, int flags, struct sockaddr *address,
    socklen_t addrlen);
ssize_t posix_recv(int sock, void *buff, size_t bufflen, int flags);
ssize_t posix_recvfrom(
    int sock, void *buff, size_t bufflen, int flags, struct sockaddr *address,
    socklen_t *addrlen);

int posix_listen(int sock, int backlog);
int posix_bind(int sock, const struct sockaddr *address, socklen_t addrlen);
int posix_accept(int sock, struct sockaddr *address, socklen_t *addrlen);

int posix_shutdown(int socket, int how);

int posix_getpeername(
    int socket, struct sockaddr *address, socklen_t *address_len);
int posix_getsockname(
    int socket, struct sockaddr *address, socklen_t *address_len);

int posix_setsockopt(
    int sock, int level, int optname, const void *optvalue, socklen_t optlen);
int posix_getsockopt(
    int sock, int level, int optname, void *optvalue, socklen_t *optlen);

int posix_sethostname(const char *name, size_t len);

#endif
