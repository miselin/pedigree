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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/types.h"

struct cdi_device;

class CdiIrqHandler : public IrqHandler
{
  public:
    bool irq(irq_id_t number, InterruptState &state) override;
};

static CdiIrqHandler cdi_irq_handler;

/** Anzahl der verfuegbaren IRQs */
#define IRQ_COUNT 0x10

/** Array mit allen IRQ-Handlern; Als index wird die Nummer benutzt */
static void (*driver_irq_handler[IRQ_COUNT])(struct cdi_device *) = {nullptr};
/** Array mit den passenden Geraeten zu den registrierten IRQs */
static struct cdi_device *driver_irq_device[IRQ_COUNT] = {nullptr};
/**
 * Array, das die jeweilige Anzahl an aufgerufenen Interrupts seit dem
 * cdi_reset_wait_irq speichert.
 */
static Spinlock irqCountLock;
static Semaphore *driver_irq_count[IRQ_COUNT] = {nullptr};

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
}

/**
 * Interner IRQ-Handler, der den IRQ-Handler des Treibers aufruft
 */
bool CdiIrqHandler::irq(irq_id_t irq, InterruptState &state)
{
    if (irq >= IRQ_COUNT)
    {
        return false;
    }

    {
        LockGuard<Spinlock> lock(irqCountLock);
        if (driver_irq_count[irq])
        {
            driver_irq_count[irq]->release();
        }
    }

    if (driver_irq_handler[irq])
    {
        driver_irq_handler[irq](driver_irq_device[irq]);
    }

    return true;
}

extern "C"
{
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

    // Der Interrupt wurde schon mal registriert
    if (driver_irq_handler[irq])
    {
        NOTICE("cdi: Versuch IRQ " << irq << " mehrfach zu registrieren");
        return;
    }

    if (driver_irq_count[irq])
    {
        delete driver_irq_count[irq];
    }
    driver_irq_count[irq] = new Semaphore(0);

    driver_irq_handler[irq] = handler;
    driver_irq_device[irq] = device;

    Machine::instance().getIrqManager()->registerIsaIrqHandler(
        irq, static_cast<IrqHandler *>(&cdi_irq_handler));
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

    LockGuard<Spinlock> lock(irqCountLock);
    Semaphore *counter = driver_irq_count[irq];
    if (!counter)
    {
        return -1;
    }

    const size_t discarded = resetIrqCounter(*counter);
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

    if (!driver_irq_handler[irq])
    {
        return -2;
    }

    Semaphore *semaphore;
    {
        LockGuard<Spinlock> lock(irqCountLock);
        semaphore = driver_irq_count[irq];
        if (!semaphore)
        {
            semaphore = new Semaphore(0);
            driver_irq_count[irq] = semaphore;
        }
    }

    return semaphore ? waitForIrqCounter(*semaphore, timeout) : -2;
}
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace
{
void hostedCdiIrqHandler(struct cdi_device *)
{
}
}

bool runHostedCdiIrqRegressions()
{
    constexpr uint8_t TestIrq = IRQ_COUNT - 1;
    Semaphore counter(3);
    {
        LockGuard<Spinlock> lock(irqCountLock);
        driver_irq_handler[TestIrq] = hostedCdiIrqHandler;
        driver_irq_device[TestIrq] = nullptr;
        driver_irq_count[TestIrq] = &counter;
    }

    const bool reset = cdi_reset_wait_irq(TestIrq) == 0;
    const bool emptyAfterReset = !counter.tryAcquire();
    const bool zeroTimeout = cdi_wait_irq(TestIrq, 0) == -3;
    counter.release();
    const bool zeroSuccess = cdi_wait_irq(TestIrq, 0) == 0;
    counter.release();
    const bool finiteSuccess = cdi_wait_irq(TestIrq, 1) == 0;
    const bool finiteTimeout = cdi_wait_irq(TestIrq, 1) == -3;
    const bool invalidReset = cdi_reset_wait_irq(IRQ_COUNT) == -1;
    const bool invalidWait = cdi_wait_irq(IRQ_COUNT, 0) == -1;

    {
        LockGuard<Spinlock> lock(irqCountLock);
        driver_irq_handler[TestIrq] = nullptr;
        driver_irq_device[TestIrq] = nullptr;
        driver_irq_count[TestIrq] = nullptr;
    }

    const bool passed =
        reset && emptyAfterReset && zeroTimeout && zeroSuccess &&
        finiteSuccess && finiteTimeout && invalidReset && invalidWait;
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
