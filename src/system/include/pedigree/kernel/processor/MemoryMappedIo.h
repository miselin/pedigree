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

#ifndef KERNEL_PROCESSOR_MEMORYMAPPEDIO_H
#define KERNEL_PROCESSOR_MEMORYMAPPEDIO_H

#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/types.h"

/** @addtogroup kernelprocessor
 * @{ */

/** The MemoryMappedIo handles special MemoryRegions for I/O to hardware devices
 *\brief Memory mapped I/O range */
class MemoryMappedIo : public IoBase, public MemoryRegion
{
  public:
    MemoryMappedIo(
        const char *pName, uintptr_t offset = 0, uintptr_t padding = 1);
    /** The destructor frees the allocated ressources */
    virtual ~MemoryMappedIo();

    //
    // IoBase Interface
    //
    virtual size_t size() const;
    virtual uint8_t read8(size_t offset = 0);
    virtual uint16_t read16(size_t offset = 0);
    virtual uint32_t read32(size_t offset = 0);
    virtual uint64_t read64(size_t offset = 0);
    virtual void write8(uint8_t value, size_t offset = 0);
    virtual void write16(uint16_t value, size_t offset = 0);
    virtual void write32(uint32_t value, size_t offset = 0);
    virtual void write64(uint64_t value, size_t offset = 0);
    virtual operator bool() const;

    //
    // MemoryRegion Interface
    //

  private:
    /** The copy-constructor
     *\note NOT implemented */
    MemoryMappedIo(const MemoryMappedIo &);
    /** The assignment operator
     *\note NOT implemented */
    MemoryMappedIo &operator=(const MemoryMappedIo &);

    /** MemoryRegion only supports allocation on a page boundary.
        This variable adds an offset onto each access to make up for this
        (if required) */
    uintptr_t m_Offset;

    /** It is possible that registers may not follow one another directly in
       memory, instead being padded to some boundary. */
    size_t m_Padding;
};

/** @} */

#endif
