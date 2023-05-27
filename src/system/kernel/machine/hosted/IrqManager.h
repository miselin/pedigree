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

#ifndef KERNEL_MACHINE_HOSTED_IRQMANAGER_H
#define KERNEL_MACHINE_HOSTED_IRQMANAGER_H

#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/InterruptHandler.h"
#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/utilities/List.h"

/** @addtogroup kernelmachinehosted
 * @{ */

class HostedIrqManager : public IrqManager, private InterruptHandler
{
  public:
    /** Get the HostedIrqManager class instance
     *\return the HostedIrqManager class instance */
    inline static HostedIrqManager &instance()
    {
        return m_Instance;
    }

    //
    // IrqManager interface
    //
    virtual irq_id_t
    registerIsaIrqHandler(uint8_t irq, IrqHandler *handler, bool bEdge = false);
    virtual irq_id_t
    registerPciIrqHandler(IrqHandler *handler, class Device *pDevice);
    virtual void acknowledgeIrq(irq_id_t Id);
    virtual void unregisterHandler(irq_id_t Id, IrqHandler *handler);

    /** Initialises the PIC hardware and registers the interrupts with the
     *  InterruptManager.
     *\return true, if successfull, false otherwise */
    bool initialise() INITIALISATION_ONLY;

    /** Called every millisecond, typically handles IRQ mitigation. */
    virtual void tick();

    /** Controls specific elements of a given IRQ */
    virtual bool control(uint8_t irq, ControlCode code, size_t argument);

    virtual void enable(uint8_t irq, bool enable)
    {
    }

  private:
    /** The default constructor */
    HostedIrqManager() INITIALISATION_ONLY;
    /** The destructor */
    inline virtual ~HostedIrqManager()
    {
    }
    /** The copy-constructor
     *\note NOT implemented */
    HostedIrqManager(const HostedIrqManager &);
    /** The assignment operator
     *\note NOT implemented */
    HostedIrqManager &operator=(const HostedIrqManager &);

    //
    // InterruptHandler interface
    //
    virtual void interrupt(size_t interruptNumber, InterruptState &state);

    /** The IRQ handler */
    List<IrqHandler *> m_Handler[2];

    /** Main lock for all modifications */
    Spinlock m_Lock;

    /** The HostedIrqManager instance */
    static HostedIrqManager m_Instance;
};

/** @} */

#endif
