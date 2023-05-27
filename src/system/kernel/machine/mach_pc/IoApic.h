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

#ifndef KERNEL_MACHINE_X86_COMMON_IO_APIC_H
#define KERNEL_MACHINE_X86_COMMON_IO_APIC_H

#if APIC

#include "pedigree/kernel/processor/MemoryMappedIo.h"

/** @addtogroup kernelmachinex86common
 * @{ */

/** The x86/x64 I/O APIC */
class IoApic
{
  public:
    IoApic();
    virtual ~IoApic();

  private:
    IoApic(const IoApic &) = delete;
    IoApic &operator=(const IoApic &) = delete;

    /** The I/O APIC memory-mapped I/O space */
    MemoryMappedIo m_IoSpace;
};

/** @} */

#endif

#endif
