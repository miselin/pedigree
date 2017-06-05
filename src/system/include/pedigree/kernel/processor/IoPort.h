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

#ifndef KERNEL_PROCESSOR_IOPORT_H
#define KERNEL_PROCESSOR_IOPORT_H

#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/types.h"

/** @addtogroup kernelprocessor
 * @{ */

#if !defined(KERNEL_PROCESSOR_NO_PORT_IO)

/** IoPort provides access to a range of hardware I/O port
 *\brief I/O port range */
class IoPort : public IoBase
{
  public:
    /** The default constructor does nothing */
    IoPort(const char *name);
    /** The destructor frees the allocated ressources */
    virtual ~IoPort();

    //
    // IoBase Interface
    //
    virtual size_t size() const;
    virtual uint8_t read8(size_t offset = 0);
    virtual uint16_t read16(size_t offset = 0);
    virtual uint32_t read32(size_t offset = 0);
#if defined(BITS_64)
    virtual uint64_t read64(size_t offset = 0);
#endif
    virtual void write8(uint8_t value, size_t offset = 0);
    virtual void write16(uint16_t value, size_t offset = 0);
    virtual void write32(uint32_t value, size_t offset = 0);
#if defined(BITS_64)
    virtual void write64(uint64_t value, size_t offset = 0);
#endif
    virtual operator bool() const;

    /** Get the base I/O port */
    io_port_t base() const;
    /** Get the name of the I/O port range
     *\return pointer to the name of the I/O port range */
    const char *name() const;
    /** Free an I/O port range */
    void free();
    /** Allocate an I/O port range
     *\param[in] ioPort the base I/O port
     *\param[in] size the number of successive I/O ports - 1
     *\return true, if successfull, false otherwise */
    bool allocate(io_port_t ioPort, size_t size);

  private:
    /** The copy-constructor
     *\note NOT implemented */
    IoPort(const IoPort &);
    /** The assignment operator
     *\note NOT implemented */
    IoPort &operator=(const IoPort &);

    /** The base I/O port */
    io_port_t m_IoPort;
    /** The number of successive I/O ports - 1 */
    size_t m_Size;
    /** User-visible name of this I/O port range */
    const char *m_Name;
};

#endif

/** @} */

#endif
