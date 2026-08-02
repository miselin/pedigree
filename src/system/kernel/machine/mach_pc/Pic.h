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

#ifndef KERNEL_MACHINE_X86_COMMON_PIC_H
#define KERNEL_MACHINE_X86_COMMON_PIC_H

#include "PicIrqState.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqDiagnosticSnapshotStore.h"
#include "pedigree/kernel/machine/IrqHandlerRegistry.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/processor/InterruptHandler.h"
#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/new"

class Device;
class HardIrqHandler;
class IrqHandler;
class IrqHandlerBase;

/** @addtogroup kernelmachinex86common
 * @{ */

/** The x86/x64 programmable interrupt controller as IrqManager */
class Pic : public IrqManager, private InterruptHandler
{
  public:
    /** Get the Pic class instance
     *\return the Pic class instance */
    inline static Pic &instance()
    {
        return m_Instance;
    }

    //
    // IrqManager interface
    //
    virtual irq_id_t registerIsaIrqHandler(
        uint8_t irq, IrqHandler *handler, const IrqPolicy &policy);
    virtual irq_id_t registerPciIrqHandler(
        IrqHandler *handler, Device *pDevice, const IrqPolicy &policy);
    virtual irq_id_t registerHardIsaIrqHandler(
        uint8_t irq, HardIrqHandler *handler, const IrqPolicy &policy);
    virtual irq_id_t registerHardPciIrqHandler(
        HardIrqHandler *handler, Device *pDevice,
        const IrqPolicy &policy);
    virtual bool unregisterHandler(irq_id_t Id, IrqHandlerBase *handler);
    virtual size_t
    snapshotIrqLines(IrqLineDiagnosticSnapshot *out, size_t capacity) const;

    /** Initialises the PIC hardware and registers the interrupts with the
     *  InterruptManager.
     *\return true, if successfull, false otherwise */
    bool initialise() INITIALISATION_ONLY;

    /** Starts manager-owned bottom-half workers after scheduler startup. */
    bool initialiseThreaded();

    /** Masks all lines, cancels queued batches, then joins every worker. */
    bool shutdownThreaded();

    /** Called every millisecond, typically handles IRQ mitigation. */
    virtual void tick();

    /** Controls specific elements of a given IRQ */
    virtual bool control(uint8_t irq, ControlCode code, size_t argument);

  private:
    /** The default constructor */
    Pic() INITIALISATION_ONLY;
    /** The destructor */
    inline virtual ~Pic()
    {
    }
    /** The copy-constructor
     *\note NOT implemented */
    Pic(const Pic &);
    /** The assignment operator
     *\note NOT implemented */
    Pic &operator=(const Pic &);

    //
    // InterruptHandler interface
    //
    virtual void interrupt(size_t interruptNumber, InterruptState &state);

    static void dispatchThreadedLine(void *context, uint8_t irq, size_t cookie);
    size_t advanceThreadedCookieLocked(uint8_t irq);
    void publishDiagnosticLineLocked(uint8_t irq);
    void publishAllDiagnosticLinesLocked();

    void eoiLocked(uint8_t irq);
    void applyMaskLocked();
    void setEnabledLocked(uint8_t irq, bool enable);
    void enable(uint8_t irq, bool enable);
    void enableAll(bool enable);

    /** Handle a potentially-spurious IRQ while the PIC lock is held. */
    bool spuriousLocked(size_t irq);

    /** The slave PIC I/O Port range */
    IoPort m_SlavePort;
    /** The master PIC I/O Port range */
    IoPort m_MasterPort;

    /** IRQ handlers and their callback lifetime state. */
    IrqHandlerRegistry m_Handlers;
    /** Trigger mode, registration ownership and the complete 16-bit mask. */
    PicIrqState m_IrqState;
    /** Stable one-worker-per-physical-line bottom-half dispatcher. */
    ThreadedIrqDispatcher m_ThreadedDispatcher;
    /** Invalidates queued work when a physical line changes ownership. */
    size_t m_ThreadedCookies[PicIrqState::LineCount];
    /** PIC dispatch generation associated with each queued cookie. */
    size_t m_ThreadedDispatchGenerations[PicIrqState::LineCount];
    /** Atomic diagnostics for work rejected after dispatcher closure. */
    size_t m_ThreadedPublicationFailures[PicIrqState::LineCount];
    /** Delivery is stable while a line has registered handlers. */
    IrqDelivery m_LineDeliveries[PicIrqState::LineCount];
    /** Per-line immutable diagnostic publications. */
    IrqDiagnosticSnapshotStore<PicIrqState::LineCount> m_Diagnostics;
    /** Unregister operations which have not completed line accounting. */
    size_t m_UnregisterReservations[PicIrqState::LineCount];
    /** Closes registration and re-enable paths before worker shutdown. */
    bool m_ShuttingDown;
    /** IRQ counts for given handlers */
    size_t m_IrqCount[16];
    /** Architecturally spurious or disabled in-flight occurrences. */
    size_t m_SpuriousIrqCount[16];
    /** Occurrences for which no callback accepted ownership. */
    size_t m_UnhandledIrqCount[16];
    /** Mitigated IRQs */
    bool m_MitigatedIrqs[16];
    /** Mitigation thresholds */
    size_t m_MitigationThreshold[16];
    /** Main lock for all modifications */
    Spinlock m_Lock;

    /** The Pic instance */
    static Pic m_Instance;
};

/** @} */

#endif
