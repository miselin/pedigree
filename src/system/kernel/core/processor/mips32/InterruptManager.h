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

#ifndef KERNEL_PROCESSOR_MIPS32_INTERRUPTMANAGER_H
#define KERNEL_PROCESSOR_MIPS32_INTERRUPTMANAGER_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/types.h"

/** @addtogroup kernelprocessorMIPS32
 * @{ */

/** The interrupt handler on mips32 processors */
class MIPS32InterruptManager : public ::InterruptManager,
                               public ::SyscallManager
{
  public:
    /** Get the MIPS32InterruptManager class instance
     *\return instance of the MIPS32InterruptManager class */
    inline static MIPS32InterruptManager &instance()
    {
        return m_Instance;
    }

    // InterruptManager Interface
    virtual bool
    registerInterruptHandler(size_t interruptNumber, InterruptHandler *handler);
    // Specific to MIPS, all external interrupts are vectored differently. IRQs
    // are vectored in a different manner too.
    virtual bool registerExternalInterruptHandler(
        size_t interruptNumber, InterruptHandler *handler);

#if DEBUGGER
    virtual bool registerInterruptHandlerDebugger(
        size_t interruptNumber, InterruptHandler *handler);
    virtual size_t getBreakpointInterruptNumber() PURE;
    virtual size_t getDebugInterruptNumber() PURE;
#endif

    // SyscallManager Interface
    virtual bool
    registerSyscallHandler(Service_t Service, SyscallHandler *handler);

    /** Initialises this processors IDTR
     *\note This should only be called from initialiseProcessor()
     *\todo and some smp/acpi function */
    static void initialiseProcessor();

  private:
    /** Called when an interrupt was triggered
     *\param[in] interruptState reference to the usermode/kernel state before
     *the interrupt */
    static void interrupt(InterruptState &interruptState);
    /** The constructor */
    MIPS32InterruptManager();
    /** Copy constructor
     *\note NOT implemented */
    MIPS32InterruptManager(const MIPS32InterruptManager &);
    /** Assignment operator
     *\note NOT implemented */
    MIPS32InterruptManager &operator=(const MIPS32InterruptManager &);
    /** The destructor */
    virtual ~MIPS32InterruptManager();

    InterruptHandler *m_Handler[64];
    InterruptHandler *m_ExternalHandler[8];
#if DEBUGGER
    /** The debugger interrupt handlers */
    InterruptHandler *m_DbgHandler[64];
#endif
    /** The syscall handlers */
    SyscallHandler *m_SyscallHandler[SyscallManager::serviceEnd];

    /** The instance of the interrupt manager  */
    static MIPS32InterruptManager m_Instance;
};

/** @} */

#endif
