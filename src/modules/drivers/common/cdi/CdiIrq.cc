/*
 * Copyright (c) 2007 Kevin Wolf
 *
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/projects/COPYING.WTFPL for more details.
 */

#include <stddef.h>

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/types.h"

struct cdi_device;

class CdiIrqHandler : public IrqHandler
{
  public:
    IrqDisposition irq(irq_id_t number) override;
};

static CdiIrqHandler cdi_irq_handler;

/** Anzahl der verfuegbaren IRQs */
#define IRQ_COUNT 0x10

struct CdiIrqSlot
{
    CdiIrqSlot()
        : handler(nullptr), device(nullptr), registration(0), counter(0)
    {
    }

    void (*handler)(struct cdi_device *);
    struct cdi_device *device;
    irq_id_t registration;
    Semaphore counter;
};

static Mutex irqSlotLock;
static CdiIrqSlot driverIrqs[IRQ_COUNT];

namespace
{
size_t resetIrqCounter(Semaphore &counter)
{
    return counter.drainAvailable();
}

int waitForIrqCounter(Semaphore &counter, uint32_t timeout)
{
    if (timeout == 0)
    {
        return counter.tryAcquire() ? 0 : -3;
    }

    Semaphore::SemaphoreError error = Semaphore::NoError;
    const bool acquired = counter.acquireWithError(
        1, timeout / 1000, (timeout % 1000) * 1000, error);
    return acquired ? 0 : -3;
}
}  // namespace

/**
 * Interner IRQ-Handler, der den IRQ-Handler des Treibers aufruft
 */
IrqDisposition CdiIrqHandler::irq(irq_id_t irq)
{
    if (irq >= IRQ_COUNT)
    {
        return IrqDisposition::NotHandled;
    }

    void (*handler)(struct cdi_device *) = nullptr;
    struct cdi_device *device = nullptr;
    {
        LockGuard<Mutex> lock(irqSlotLock);
        handler = driverIrqs[irq].handler;
        device = driverIrqs[irq].device;
    }

    if (!handler)
        return IrqDisposition::NotHandled;

    driverIrqs[irq].counter.release();
    handler(device);
    return IrqDisposition::Handled;
}

extern "C" {
/**
 * Registiert einen neuen IRQ-Handler.
 *
 * @param irq Nummer des zu reservierenden IRQ
 * @param handler Handlerfunktion
 * @param device Geraet, das dem Handler als Parameter uebergeben werden soll
 */
EXPORTED_PUBLIC void cdi_register_irq(
    uint8_t irq, void (*handler)(struct cdi_device *),
    struct cdi_device *device)
{
    if (irq >= IRQ_COUNT)
    {
        // FIXME: Eigentlich sollte diese Funktion etwas weniger optimistisch
        // sein, und einen Rueckgabewert haben.
        return;
    }
    if (!handler)
        return;

    {
        LockGuard<Mutex> lock(irqSlotLock);
        // Der Interrupt wurde schon mal registriert
        if (driverIrqs[irq].handler)
        {
            NOTICE("cdi: Versuch IRQ " << irq << " mehrfach zu registrieren");
            return;
        }

        const size_t discarded = resetIrqCounter(driverIrqs[irq].counter);
        (void) discarded;
        driverIrqs[irq].handler = handler;
        driverIrqs[irq].device = device;
    }

    const irq_id_t registration =
        Machine::instance().getIrqManager()->registerIsaIrqHandler(
            irq, static_cast<IrqHandler *>(&cdi_irq_handler),
            IrqPolicy::levelThreaded());
    if (!registration)
    {
        LockGuard<Mutex> lock(irqSlotLock);
        driverIrqs[irq].handler = nullptr;
        driverIrqs[irq].device = nullptr;
        ERROR("cdi: IRQ " << irq << " konnte nicht registriert werden");
        return;
    }

    LockGuard<Mutex> lock(irqSlotLock);
    driverIrqs[irq].registration = registration;
}

/**
 * Setzt den IRQ-Zaehler fuer cdi_wait_irq zurueck.
 *
 * @param irq Nummer des IRQ
 *
 * @return 0 bei Erfolg, -1 im Fehlerfall
 */
EXPORTED_PUBLIC int cdi_reset_wait_irq(uint8_t irq)
{
    if (irq >= IRQ_COUNT)
    {
        return -1;
    }

    LockGuard<Mutex> lock(irqSlotLock);
    if (!driverIrqs[irq].handler)
    {
        return -1;
    }

    const size_t discarded = resetIrqCounter(driverIrqs[irq].counter);
    (void) discarded;
    return 0;
}

/**
 * Wartet bis der IRQ aufgerufen wurde. Der interne Zaehler muss zuerst mit
 * cdi_reset_wait_irq zurueckgesetzt werden, damit auch die IRQs abgefangen
 * werden koennen, die kurz vor dem Aufruf von dieser Funktion aufgerufen
 * werden.
 *
 * @param irq       Nummer des IRQ auf den gewartet werden soll
 * @param timeout   Anzahl der Millisekunden, die maximal gewartet werden sollen
 *
 * @return 0 wenn der irq aufgerufen wurde, -1 wenn eine ungueltige IRQ-Nummer
 * angegeben wurde, -2 wenn eine nicht registrierte IRQ-Nummer angegeben wurde,
 * und -3 im Falle eines Timeouts.
 */
EXPORTED_PUBLIC int cdi_wait_irq(uint8_t irq, uint32_t timeout)
{
    if (irq >= IRQ_COUNT)
    {
        return -1;
    }

    {
        LockGuard<Mutex> lock(irqSlotLock);
        if (!driverIrqs[irq].handler)
            return -2;
    }

    return waitForIrqCounter(driverIrqs[irq].counter, timeout);
}
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace
{
Atomic<size_t> g_HostedCdiIrqCalls(0);

void hostedCdiIrqHandler(struct cdi_device *)
{
    g_HostedCdiIrqCalls += 1;
}
}  // namespace

bool runHostedCdiIrqRegressions()
{
    constexpr uint8_t TestIrq = IRQ_COUNT - 1;
    {
        LockGuard<Mutex> lock(irqSlotLock);
        g_HostedCdiIrqCalls = 0;
        const size_t discarded = resetIrqCounter(driverIrqs[TestIrq].counter);
        (void) discarded;
        driverIrqs[TestIrq].handler = hostedCdiIrqHandler;
        driverIrqs[TestIrq].device = nullptr;
    }

    const IrqDisposition disposition = cdi_irq_handler.irq(TestIrq);
    const bool reset = cdi_reset_wait_irq(TestIrq) == 0;
    const bool emptyAfterReset = !driverIrqs[TestIrq].counter.tryAcquire();
    const bool zeroTimeout = cdi_wait_irq(TestIrq, 0) == -3;
    driverIrqs[TestIrq].counter.release();
    const bool zeroSuccess = cdi_wait_irq(TestIrq, 0) == 0;
    driverIrqs[TestIrq].counter.release();
    const bool finiteSuccess = cdi_wait_irq(TestIrq, 1) == 0;
    const bool finiteTimeout = cdi_wait_irq(TestIrq, 1) == -3;
    const bool invalidReset = cdi_reset_wait_irq(IRQ_COUNT) == -1;
    const bool invalidWait = cdi_wait_irq(IRQ_COUNT, 0) == -1;

    {
        LockGuard<Mutex> lock(irqSlotLock);
        driverIrqs[TestIrq].handler = nullptr;
        driverIrqs[TestIrq].device = nullptr;
        const size_t discarded = resetIrqCounter(driverIrqs[TestIrq].counter);
        (void) discarded;
    }

    const bool passed = disposition == IrqDisposition::Handled &&
                        g_HostedCdiIrqCalls == 1 && reset && emptyAfterReset &&
                        zeroTimeout && zeroSuccess && finiteSuccess &&
                        finiteTimeout && invalidReset && invalidWait;
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS cdi-irq-wait-contract");
    }
    else
    {
        ERROR("HOSTED-WAIT-TEST: FAIL cdi-irq-wait-contract");
    }
    return passed;
}
#endif
