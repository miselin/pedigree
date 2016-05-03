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

#include <machine/Device.h>

#include <DevFs.h>

static DevFsDirectory *g_TreeDirectory = 0;

class DeviceFile : public File
{
public:
    DeviceFile(String str, size_t inode, Filesystem *pParentFS, File *pParentNode);
    ~DeviceFile();

    bool initialise(String contents)
    {
        m_Contents = contents;
        return true;
    }

    virtual uint64_t read(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock = true)
    {
        if (location > m_Contents.length())
        {
            return 0;
        }
        else if ((location + size) > m_Contents.length())
        {
            size = m_Contents.length() - location;
        }

        const char *data = m_Contents;
        MemoryCopy(reinterpret_cast<void *>(buffer), data + location, size);
        return size;
    }
    virtual uint64_t write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock = true)
    {
        return 0;
    }

private:
    String m_Contents;
};
