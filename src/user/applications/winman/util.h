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

#ifndef _WINMAN_UTIL_H
#define _WINMAN_UTIL_H

#ifndef TARGET_LINUX
#include <native/ipc/Ipc.h>
#endif

/**
 * \brief Abstracts a buffer shared between multiple processes.
 *
 * Pedigree uses shared IPC messages for this purpose, while Linux uses
 * actual Linux shmem regions and passes file descriptors around.
 */
class SharedBuffer
{
  public:
    SharedBuffer(size_t size);
    SharedBuffer(size_t size, void *handle);

    virtual ~SharedBuffer();

    /** Retrieve the memory address of the buffer. */
    void *getBuffer();

    /** Retrieve a handle that can used create a matching SharedBuffer */
    void *getHandle();

#ifdef TARGET_LINUX
    char m_ShmName[8];
    int m_ShmFd;

    void *m_pBuffer;
    size_t m_Size;

    static size_t m_NextId;
#else
    PedigreeIpc::SharedIpcMessage *m_pFramebuffer;
#endif
};

#endif
