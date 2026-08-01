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

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqHandlerRegistry.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/processor/InterruptHandler.h"

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
    virtual irq_id_t registerHardIsaIrqHandler(
        uint8_t irq, HardIrqHandler *handler, bool bEdge = false);
    virtual irq_id_t
    registerHardPciIrqHandler(HardIrqHandler *handler, class Device *pDevice);
    virtual bool unregisterHandler(irq_id_t Id, IrqHandlerBase *handler);

    /** Initialises the PIC hardware and registers the interrupts with the
     *  InterruptManager.
     *\return true, if successfull, false otherwise */
    bool initialise() INITIALISATION_ONLY;

    /** Starts manager-owned bottom-half workers after scheduler startup. */
    bool initialiseThreaded();

    /** Stops and joins every manager-owned bottom-half worker. */
    bool shutdownThreaded();

    /** Called every millisecond, typically handles IRQ mitigation. */
    virtual void tick();

    /** Controls specific elements of a given IRQ */
    virtual bool control(uint8_t irq, ControlCode code, size_t argument);

    virtual void enable(uint8_t irq, bool enable)
    {
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    using HandlerPinHook = IrqHandlerRegistry::HandlerPinHook;
    using HandlerPrePinHook = IrqHandlerRegistry::HandlerPrePinHook;
    using HandlerHazardHook = IrqHandlerRegistry::HandlerHazardHook;
    using HandlerHazardStage = IrqHandlerRegistry::HandlerHazardStage;
    using MutationLockHook = IrqHandlerRegistry::MutationLockHook;

    /** Installs a deterministic observer after a handler has been pinned. */
    static EXPORTED_PUBLIC void setHandlerPinHook(HandlerPinHook hook);

    /** Installs a deterministic observer before a tentative pin commits. */
    static EXPORTED_PUBLIC void setHandlerPrePinHook(HandlerPrePinHook hook);

    /** Installs an observer around active-hazard publication. */
    static EXPORTED_PUBLIC void setHandlerHazardHook(HandlerHazardHook hook);

    /** Dispatches one handler through the production registry path. */
    static EXPORTED_PUBLIC bool dispatchHandlerForTest(
        uint8_t irq, HardIrqHandler *handler, InterruptState &state,
        bool &handled);

    /** Runs a deterministic test seam while the writer lock is held. */
    static EXPORTED_PUBLIC void
    withRegistryMutationLockForTest(MutationLockHook hook);

    /** Counts active hazards for deterministic abandoned-stack tests. */
    static EXPORTED_PUBLIC size_t
    activeDispatchCountForTest(IrqHandlerBase *handler);

    /** Counts claimed hazards, including claims not yet committed to a slot. */
    static EXPORTED_PUBLIC size_t claimedDispatchCountForTest();

    /** Reports whether any nonempty line slot still publishes a handler. */
    static EXPORTED_PUBLIC bool
    containsHandlerForTest(uint8_t irq, IrqHandlerBase *handler);

    /** Abandons the current test Thread through the kernel scheduler. */
    static EXPORTED_PUBLIC void abandonCurrentThreadForTest();
#endif

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

    static void dispatchThreadedLine(void *context, uint8_t irq, size_t cookie);

    /** IRQ handlers and their callback lifetime state. */
    IrqHandlerRegistry m_Handlers;

    /** Stable one-worker-per-signal threaded IRQ dispatcher. */
    ThreadedIrqDispatcher m_ThreadedDispatcher;
    size_t m_ThreadedCookies[3];
    size_t m_ThreadedPublicationFailures[3];

    /** The HostedIrqManager instance */
    static HostedIrqManager m_Instance;
};

/** @} */

#endif
