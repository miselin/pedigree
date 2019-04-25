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

#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/processor/IoPortManager.h"

IoPort::IoPort(const char *name) : m_IoPort(0), m_Size(0), m_Name(name)
{
}

IoPort::~IoPort()
{
    free();
}

bool IoPort::allocate(io_port_t ioPort, size_t size)
{
    // Free any allocated I/O ports
    if (m_Size != 0)
        free();

    if (IoPortManager::instance().allocate(this, ioPort, size) == true)
    {
        m_IoPort = ioPort;
        m_Size = size;
        return true;
    }
    return false;
}

void IoPort::free()
{
    if (m_Size != 0)
    {
        IoPortManager::instance().free(this);

        m_IoPort = 0;
        m_Size = 0;
    }
}

size_t IoPort::size() const
{
    return m_Size;
}

io_port_t IoPort::base() const
{
    return m_IoPort;
}

IoPort::operator bool() const
{
    return (m_Size != 0);
}

const char *IoPort::name() const
{
    return m_Name;
}

#if KERNEL_PROCESSOR_NO_PORT_IO
/// \todo all these should actually panic or something

uint8_t IoPort::read8(size_t offset)
{
    return 0;
}
uint16_t IoPort::read16(size_t offset)
{
    return 0;
}
uint32_t IoPort::read32(size_t offset)
{
    return 0;
}
#if BITS_64
uint64_t IoPort::read64(size_t offset)
{
    return 0;
}
#endif
void IoPort::write8(uint8_t value, size_t offset)
{
}
void IoPort::write16(uint16_t value, size_t offset)
{
}
void IoPort::write32(uint32_t value, size_t offset)
{
}
#if BITS_64
void IoPort::write64(uint64_t value, size_t offset)
{
}
#endif
#endif
