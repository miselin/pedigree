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

#ifndef KERNEL_CORE_PROCESSOR_PAGEFAULTHANDLER_H_
#define KERNEL_CORE_PROCESSOR_PAGEFAULTHANDLER_H_

#include <processor/InterruptManager.h>
#include <utilities/List.h>

/** @addtogroup kernelprocessor
 * @{ */

class MemoryTrapHandler
{
public:
    virtual ~MemoryTrapHandler() {}

    /** Trap event handler.
        \param address The address of the trap.
        \param bIsWrite True if the trap was caused by a write, false if by a read.
        \return True if the trap was handled successfully (and the handler can
                return), or false if another handler needs to be tried. */
    virtual bool trap(uintptr_t address, bool bIsWrite) = 0;
};

/** The x86 Page Fault Exception handler. */
class PageFaultHandler : private InterruptHandler
{
public:
    /** Get the PageFaultHandler instance
     *  \return the PageFaultHandler instance.  */
    inline static PageFaultHandler& instance()  {return m_Instance;}

    /** Register the PageFaultHandler with the InterruptManager.
     * \return true if sucessful, false otherwise.  */
    bool initialise();

    /** Registers a trap handler. */
    void registerHandler(MemoryTrapHandler *pHandler);

    //
    // InterruptHandler interface.
    //
    virtual void interrupt(size_t interruptNumber, InterruptState &state);
private:
    /** The default constructor.  */
    PageFaultHandler() INITIALISATION_ONLY;

    /**The copy constructor.
     * Note not implemented.  */
    PageFaultHandler(const PageFaultHandler&);

    List<MemoryTrapHandler *> m_Handlers;

    /** The PageFaultHandler instance */
    static PageFaultHandler m_Instance;
};

/** @} */

#endif /* KERNEL_CORE_PROCESSOR_X86_PAGEFAULTHANDLER_H_ */
