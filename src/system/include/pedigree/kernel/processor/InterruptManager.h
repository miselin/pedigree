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

#ifndef KERNEL_PROCESSOR_INTERRUPTMANAGER_H
#define KERNEL_PROCESSOR_INTERRUPTMANAGER_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

class InterruptHandler;

/** @addtogroup kernelprocessor
 * @{ */

/** The interrupt manager allows interrupt handler registrations and handles
 *interrupts. \brief Handles interrupts and interrupt registrations from kernel
 *components */
class InterruptManager
{
  public:
    /** Get the interrupt manager instance
     *\return instance of the interrupt manager */
    EXPORTED_PUBLIC static InterruptManager &instance();
    /** Register an interrupt handler
     *\param[in] nInterruptNumber the interrupt's number
     *\param[in] pHandler the interrupt handler
     *\return true, if successfully registered, false otherwise */
    virtual bool registerInterruptHandler(
        size_t nInterruptNumber, InterruptHandler *pHandler) = 0;

#if DEBUGGER
    /** Register an interrupt handler (for the kernel debugger)
     *\param[in] nInterruptNumber the interrupt's number
     *\param[in] pHandler the interrupt handler
     *\return true, if successfully registered, false otherwise */
    virtual bool registerInterruptHandlerDebugger(
        size_t nInterruptNumber, InterruptHandler *pHandler) = 0;
    /** Get the interrupt number of the breakpoint exception
     *\return the interrupt number of the breakpoint exception */
    virtual size_t getBreakpointInterruptNumber() PURE = 0;
    /** Get the interrupt number of the debug exception
     *\return the interrupt number of the debug exception */
    virtual size_t getDebugInterruptNumber() PURE = 0;
#endif

  protected:
    /** The constructor */
    InterruptManager();
    /** The destructor */
    virtual ~InterruptManager();

  private:
    /** The copy-constructor
     *\note Not implemented (singleton) */
    InterruptManager(const InterruptManager &);
    /** The copy-constructor
     *\note Not implemented (singleton) */
    InterruptManager &operator=(const InterruptManager &);
};

/** @} */

#endif
