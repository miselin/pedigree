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

#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include "modules/system/vfs/VFS.h"

#include "subsys/posix/net-syscalls.h"
#include "subsys/posix/UnixFilesystem.h"
#include "subsys/posix/PosixSubsystem.h"

#include <sys/un.h>

UnixFilesystem *g_pUnixFilesystem = 0;

int main(int argc, char **argv)
{
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
        fprintf(stderr, "FAIL: could not get a UNIX socket: %d [%s]\n", errno, strerror(errno));
        return 1;
    }

    int s2 = posix_socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s2 == -1)
    {
        fprintf(stderr, "FAIL: could not get a UNIX socket: %d [%s]\n", errno, strerror(errno));
        return 1;
    }

    // anonymous
    struct sockaddr_un sun;
    socklen_t socklen;
    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, "s1");
    socklen = 3 + sizeof(sa_family_t);

    int rc = posix_bind(s1, reinterpret_cast<const sockaddr *>(&sun), sizeof(sun));
    if (rc != 0)
    {
        fprintf(stderr, "FAIL: could not bind UNIX socket to 's1': %d [%s]\n", errno, strerror(errno));
        return 1;
    }

    return 0;
}

bool PosixSubsystem::checkAddress(uintptr_t addr, size_t extent, size_t flags)
{
    return true;
}
