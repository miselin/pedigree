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

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include <thread>

#include "modules/system/vfs/VFS.h"

#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/UnixFilesystem.h"
#include "modules/subsys/posix/net-syscalls.h"
#include "modules/subsys/posix/poll-syscalls.h"

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/StaticCord.h"

#include <sys/un.h>

UnixFilesystem *g_pUnixFilesystem = 0;

class StreamingStderrLogger : public Log::LogCallback
{
  public:
    void callback(const LogCord &cord)
    {
        for (size_t i = 0; i < cord.length(); ++i)
        {
            fprintf(stderr, "%c", cord[i]);
        }
    }
};

int main(int argc, char **argv)
{
    StreamingStderrLogger logger;
    Log::instance().installCallback(&logger, true);

    char buf[128];

    g_pUnixFilesystem = new UnixFilesystem();

    VFS::instance().addAlias(
        g_pUnixFilesystem, g_pUnixFilesystem->getVolumeLabel());

    printf("=> Datagram tests...\n");

    // do any cleanup we need to do
    unlink("s1");
    unlink("s2");
    unlink("s3");
    unlink("s4");

    int s1 = posix_socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s1 == -1)
    {
        fprintf(
            stderr, "FAIL: could not get a UNIX socket: %d [%s]\n", errno,
            strerror(errno));
        return 1;
    }

    int s2 = posix_socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s2 == -1)
    {
        fprintf(
            stderr, "FAIL: could not get a UNIX socket: %d [%s]\n", errno,
            strerror(errno));
        return 1;
    }

    printf("  --> unnamed -> named [via connect]\n");

    struct sockaddr_un sun_misc;
    socklen_t socklen_misc = 0;

    struct sockaddr_un sun1;
    socklen_t socklen;
    sun1.sun_family = AF_UNIX;
    strcpy(sun1.sun_path, "unix»/s1");
    socklen = strlen(sun1.sun_path) + sizeof(sa_family_t);

    int rc = posix_bind(s1, reinterpret_cast<const sockaddr *>(&sun1), socklen);
    if (rc != 0)
    {
        fprintf(
            stderr, "FAIL: could not bind UNIX socket to 's1': %d [%s]\n",
            errno, strerror(errno));
        return 1;
    }

    assert(
        posix_connect(s2, reinterpret_cast<sockaddr *>(&sun1), socklen) == 0);

    const char *msg = "hello";

    assert(posix_send(s2, msg, 6, 0) == 6);
    assert(
        posix_recvfrom(
            s1, buf, 128, 0, reinterpret_cast<sockaddr *>(&sun_misc),
            &socklen_misc) == 6);
    assert(!memcmp(buf, "hello", 6));
    memset(buf, 0, 128);

    printf(" socklen_misc=%zd sizeof=%zd\n", socklen_misc, sizeof(sa_family_t));

    // make sure recvfrom() gives an unnamed socket
    assert(sun_misc.sun_family == AF_UNIX);
    /// \todo this breaks with lwip vs Linux
    assert(socklen_misc == sizeof(sa_family_t));

    printf("  --> unnamed -> named [via sendto]\n");

    s2 = posix_socket(AF_UNIX, SOCK_DGRAM, 0);

    assert(
        posix_sendto(
            s2, msg, 6, 0, reinterpret_cast<sockaddr *>(&sun1), socklen) == 6);
    assert(posix_recv(s1, buf, 128, 0) == 6);
    assert(!memcmp(buf, "hello", 6));
    memset(buf, 0, 128);

    printf("  --> named <-> named\n");

    s2 = posix_socket(AF_UNIX, SOCK_DGRAM, 0);

    struct sockaddr_un sun2;
    sun2.sun_family = AF_UNIX;
    strcpy(sun2.sun_path, "unix»/s2");
    socklen = strlen(sun2.sun_path) + sizeof(sa_family_t);

    rc = posix_bind(s2, reinterpret_cast<const sockaddr *>(&sun2), socklen);
    if (rc != 0)
    {
        fprintf(
            stderr, "FAIL: could not bind UNIX socket to 's1': %d [%s]\n",
            errno, strerror(errno));
        return 1;
    }

    assert(
        posix_sendto(
            s1, msg, 6, 0, reinterpret_cast<sockaddr *>(&sun2), socklen) == 6);
    assert(
        posix_sendto(
            s2, msg, 6, 0, reinterpret_cast<sockaddr *>(&sun1), socklen) == 6);
    assert(posix_recv(s1, buf, 128, 0) == 6);
    assert(!memcmp(buf, "hello", 6));
    memset(buf, 0, 128);
    assert(
        posix_recvfrom(
            s2, buf, 128, 0, reinterpret_cast<sockaddr *>(&sun_misc),
            &socklen_misc) == 6);
    assert(!memcmp(buf, "hello", 6));
    memset(buf, 0, 128);

    // make sure recvfrom() gives an unnamed socket
    assert(sun_misc.sun_family == AF_UNIX);
    assert(socklen_misc == socklen);
    assert(!strcmp(sun_misc.sun_path, sun1.sun_path));

    // clean up existing bound unix sockets
    VFS::instance().remove(String("unix»/s1"));
    VFS::instance().remove(String("unix»/s2"));

    printf("=> Streaming tests...\n");
    printf("  --> client <-> server\n");

    s1 = posix_socket(AF_UNIX, SOCK_STREAM, 0);
    if (s1 < 0)
    {
        fprintf(
            stderr, "FAIL: could not get a streaming UNIX socket: %d [%s]\n",
            errno, strerror(errno));
        return 1;
    }

    s2 = posix_socket(AF_UNIX, SOCK_STREAM, 0);
    if (s2 < 0)
    {
        fprintf(
            stderr, "FAIL: could not get a streaming UNIX socket: %d [%s]\n",
            errno, strerror(errno));
        return 1;
    }

    rc = posix_bind(s1, reinterpret_cast<const sockaddr *>(&sun1), socklen);
    if (rc != 0)
    {
        fprintf(
            stderr, "FAIL: could not bind UNIX socket: %d [%s]\n", errno,
            strerror(errno));
        return 1;
    }

    rc = posix_listen(s1, 0);
    if (rc != 0)
    {
        fprintf(
            stderr, "FAIL: could not listen on UNIX socket: %d [%s]\n", errno,
            strerror(errno));
        return 1;
    }

    rc = posix_connect(s2, reinterpret_cast<const sockaddr *>(&sun1), socklen);
    if (rc != 0)
    {
        fprintf(
            stderr, "FAIL: could not connect to UNIX socket: %d [%s]\n", errno,
            strerror(errno));
        return 1;
    }

    memset(&sun_misc, 0, sizeof(sun_misc));

    struct pollfd fds[1];
    fds[0].fd = s1;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    rc = posix_poll(fds, 1, 0);
    if (rc <= 0)
    {
        fprintf(
            stderr,
            "WARNING: poll did not indicate readable on UNIX socket for "
            "accept(): %d [%s]\n",
            errno, strerror(errno));
    }

    int fd2 = posix_accept(
        s1, reinterpret_cast<sockaddr *>(&sun_misc), &socklen_misc);
    if (fd2 < 0)
    {
        fprintf(
            stderr, "FAIL: could not accept() on UNIX socket: %d [%s]\n", errno,
            strerror(errno));
        return 1;
    }

    fds[0].fd = fd2;
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    // we never bound the client, should be an unnamed sockaddr_un
    assert(sun_misc.sun_family == AF_UNIX);
    assert(socklen_misc == sizeof(sa_family_t));

    rc = posix_poll(fds, 1, 0);
    if (rc > 0)
    {
        fprintf(
            stderr,
            "WARNING: poll incorrectly indicated readable on UNIX socket "
            "before send(): %d [%s]\n",
            errno, strerror(errno));
    }

    // should now have a pipe between s2 and fd2
    assert(posix_send(s2, msg, 6, 0) == 6);

    fds[0].events = POLLIN;
    fds[0].revents = 0;
    rc = posix_poll(fds, 1, 0);
    if (rc <= 0)
    {
        fprintf(
            stderr,
            "WARNING: poll did not indicate readable on UNIX socket for "
            "recv(): %d [%s]\n",
            errno, strerror(errno));
    }

    assert(posix_recv(fd2, buf, 128, 0) == 6);
    assert(!memcmp(buf, "hello", 6));
    memset(buf, 0, 128);

    assert(posix_send(fd2, msg, 6, 0) == 6);
    assert(posix_recv(s2, buf, 128, 0) == 6);
    assert(!memcmp(buf, "hello", 6));
    memset(buf, 0, 128);

    // final test is to have two threads connect to each other

    fprintf(stderr, "All OK\n");

    Log::instance().removeCallback(&logger);
    return 0;
}

bool PosixSubsystem::checkAddress(uintptr_t addr, size_t extent, size_t flags)
{
    return true;
}
