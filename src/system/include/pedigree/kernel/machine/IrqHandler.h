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

#ifndef KERNEL_MACHINE_IRQHANDLER_H
#define KERNEL_MACHINE_IRQHANDLER_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/processor/state_forward.h"

/** @addtogroup kernelmachine
 * @{ */

/** Result of an ordinary thread-context IRQ callback. */
enum class IrqDisposition
{
    NotHandled,
    Handled,
};

/** Common identity for handlers stored by an IRQ registry. */
class EXPORTED_PUBLIC IrqHandlerBase
{
  protected:
    IrqHandlerBase();

    /** Handlers are owned by their registering device, not the registry. */
    virtual ~IrqHandlerBase();
};

/**
 * Normal IRQ callback interface.
 *
 * Callbacks run in an ordinary kernel thread after the hard interrupt path has
 * recorded the physical-line event. They may use thread-context APIs and are
 * deliberately not given the interrupted processor state.
 */
class EXPORTED_PUBLIC IrqHandler : public IrqHandlerBase
{
  public:
    IrqHandler();

    /** Handles a pending IRQ in thread context. */
    virtual IrqDisposition irq(irq_id_t number) = 0;

  protected:
    /** Virtual destructor */
    virtual ~IrqHandler();
};

/**
 * Explicit hard-IRQ callback interface.
 *
 * Directly deriving from this class opts the whole callback into hard IRQ
 * context, where it must not block, allocate, invoke arbitrary callbacks, or
 * retain InterruptState. SplitIrqHandler is only for devices which require a
 * mandatory hard top half; it is not the normal threaded-delivery API.
 */
class EXPORTED_PUBLIC HardIrqHandler : public IrqHandlerBase
{
  public:
    HardIrqHandler();

    /** Handles an IRQ synchronously in hard interrupt context. */
    virtual bool irq(irq_id_t number, InterruptState &state) = 0;

  protected:
    /** Virtual destructor */
    virtual ~HardIrqHandler();
};

/** @} */

#endif
