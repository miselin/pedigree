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

#include "subsys/posix/PsAuxFile.h"
#include "modules/drivers/x86/ps2mouse/Ps2Mouse.h"

bool PsAuxFile::initialise()
{
    // g_Ps2Mouse is a weak extern, so if nothing defines it it'll be null
    // This could happen if the ps2mouse driver fails to load.
    if (!g_Ps2Mouse)
    {
        return false;
    }

    g_Ps2Mouse->subscribe(subscriber, this);
    return true;
}

uint64_t PsAuxFile::read(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    return m_Buffer.read(reinterpret_cast<uint8_t *>(buffer), size, bCanBlock);
}

uint64_t PsAuxFile::write(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    g_Ps2Mouse->write(reinterpret_cast<const char *>(buffer), size);
    return size;
}

int PsAuxFile::select(bool bWriting, int timeout)
{
    if (bWriting)
    {
        return m_Buffer.canWrite(timeout == 1) ? 1 : 0;
    }
    else
    {
        return m_Buffer.canRead(timeout == 1) ? 1 : 0;
    }
}

void PsAuxFile::subscriber(void *param, const void *buffer, size_t len)
{
    reinterpret_cast<PsAuxFile *>(param)->handleIncoming(buffer, len);
}

void PsAuxFile::handleIncoming(const void *buffer, size_t len)
{
    if (m_Buffer.write(reinterpret_cast<const uint8_t *>(buffer), len, false))
    {
        dataChanged();
    }
}
