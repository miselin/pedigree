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

#include "DiskImage.h"
#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/utility.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern BootstrapStruct_t *g_pBootstrapInfo;

#define USE_FILE_IO 0

DiskImage::DiskImage(const char *path)
    : Disk(), m_pFileName(path), m_nSize(0), m_pFile(0), m_FileNo(-1),
      m_pBuffer(0)
{
}

DiskImage::~DiskImage()
{
    if (m_pBuffer)
    {
#if USE_FILE_IO
        fflush(m_pFile);
        delete[](char *) m_pBuffer;
#elif HAS_ADDRESS_SANITIZER
        for (auto it : m_BufferMap)
        {
            void *buf = it.second;
            msync(buf, 4096, MS_SYNC);
            munmap(buf, 4096);
        }
#else
        msync(m_pBuffer, m_nSize, MS_SYNC);
        munmap(m_pBuffer, m_nSize);
#endif
        m_pBuffer = 0;
    }

    if (m_pFile)
    {
        fflush(m_pFile);
        fclose(m_pFile);
    }

    if (m_FileNo >= 0)
    {
        close(m_FileNo);
    }
}

bool DiskImage::initialise()
{
    if (m_pFile)
        return false;

    m_pFile = fopen(m_pFileName, "rb+");
    if (!m_pFile)
        return false;

    m_FileNo = fileno(m_pFile);
    struct stat st;
    int r = fstat(m_FileNo, &st);
    if (r < 0)
    {
        fclose(m_pFile);
        m_pFile = 0;
        return false;
    }

    m_nSize = st.st_size;

#if USE_FILE_IO
    m_pBuffer = (void *) new char[m_nSize];
#elif HAS_ADDRESS_SANITIZER
    m_BufferMap.clear();
#else
    m_pBuffer =
        mmap(0, m_nSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_FileNo, 0);
    if (m_pBuffer == MAP_FAILED)
    {
        fclose(m_pFile);
        m_pFile = 0;
        return false;
    }

    posix_madvise(m_pBuffer, m_nSize, POSIX_MADV_SEQUENTIAL);
#endif

    return true;
}

uintptr_t DiskImage::read(uint64_t location)
{
    if ((location > m_nSize) || !m_pFile)
    {
        fprintf(
            stderr, "DiskImage::read: read past EOF (%lu vs %lu)\n", location,
            m_nSize);
        return ~0;
    }

    uint64_t off = location & 0xFFF;
#if USE_FILE_IO
    location &= ~0xFFF;
    fseek(m_pFile, location, SEEK_SET);
    ssize_t x = fread(adjust_pointer(m_pBuffer, location), 4096, 1, m_pFile);
    if (!x)
        return ~0;
    return reinterpret_cast<uintptr_t>(m_pBuffer) + location + off;
#elif HAS_ADDRESS_SANITIZER
    location &= ~0xFFF;
    auto it = m_BufferMap.find(location);
    if (it != m_BufferMap.end())
    {
        return reinterpret_cast<uintptr_t>((*it).second) + off;
    }

    void *p =
        mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m_FileNo, location);
    if (p == MAP_FAILED)
    {
        fprintf(stderr, "DiskImage::read: mmap failed (%s)\n", strerror(errno));
        return ~0;
    }

    m_BufferMap.insert({location, p});
    return reinterpret_cast<uintptr_t>(p) + off;
#else
    return reinterpret_cast<uintptr_t>(m_pBuffer) + location;
#endif
}

void DiskImage::write(uint64_t location)
{
    if ((location > m_nSize) || !m_pFile)
    {
        return;
    }

#if USE_FILE_IO
    location &= ~0xFFF;
    fseek(m_pFile, location, SEEK_SET);
    fwrite(adjust_pointer(m_pBuffer, location), 4096, 1, m_pFile);
#elif HAS_ADDRESS_SANITIZER
    location &= ~0xFFF;
    auto it = m_BufferMap.find(location);
    if (it != m_BufferMap.end())
    {
        msync((*it).second, 4096, MS_ASYNC);
    }
#else
    msync(adjust_pointer(m_pBuffer, location), getBlockSize(), MS_ASYNC);
#endif
}

size_t DiskImage::getSize() const
{
    return m_nSize;
}

void DiskImage::pin(uint64_t location)
{
}

void DiskImage::unpin(uint64_t location)
{
}
