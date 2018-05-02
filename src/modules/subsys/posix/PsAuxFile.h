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

#ifndef _POSIX_PSAUXFILE_H
#define _POSIX_PSAUXFILE_H

#include "modules/system/vfs/File.h"
#include "pedigree/kernel/utilities/Buffer.h"

class PsAuxFile : public File
{
  public:
    PsAuxFile(String str, size_t inode, Filesystem *pParentFS, File *pParentNode)
        : File(str, 0, 0, 0, inode, pParentFS, 0, pParentNode), m_Lock(false), m_Buffer(1024)
    {
        setPermissionsOnly(
            FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
        setUidOnly(0);
        setGidOnly(0);
    }
    ~PsAuxFile()
    {
    }

    bool initialise();

    uint64_t readBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    uint64_t writeBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    virtual int select(bool bWriting = false, int timeout = 0);

   private:
    Mutex m_Lock;
    Buffer<uint8_t> m_Buffer;

    static void subscriber(void *param, const void *buffer, size_t len);

    void handleIncoming(const void *buffer, size_t len);

    virtual bool isBytewise() const
    {
        return true;
    }
};

#endif
