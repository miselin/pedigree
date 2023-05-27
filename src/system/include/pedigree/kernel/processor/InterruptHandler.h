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

#ifndef KERNEL_PROCESSOR_INTERRUPTHANDLER_H
#define KERNEL_PROCESSOR_INTERRUPTHANDLER_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

/** @addtogroup kernelprocessor
 * @{ */

/** Abstract base class for all interrupt-handlers. All interrupt-handlers must
 *  be derived from this class
 *\brief Abstract base class for interrupt-handlers */
class EXPORTED_PUBLIC InterruptHandler
{
  public:
    /** Called when the handler is registered with the interrupt manager and the
     *interrupt occurred \param[in] nInterruptNumber the interrupt number
     *\param[in] state reference to the state before the interrupt */
    virtual void interrupt(size_t nInterruptNumber, InterruptState &state) = 0;

  protected:
    /** Virtual destructor */
    virtual ~InterruptHandler();
};

/** @} */

#endif
