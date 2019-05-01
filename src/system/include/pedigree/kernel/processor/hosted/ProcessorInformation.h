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

#ifndef KERNEL_PROCESSOR_HOSTED_PROCESSORINFORMATION_H
#define KERNEL_PROCESSOR_HOSTED_PROCESSORINFORMATION_H

#include "pedigree/kernel/processor/types.h"

class Thread;
class VirtualAddressSpace;
class PerProcessorScheduler;

/** @addtogroup kernelprocessorhosted
 * @{ */

/** Common hosted processor information structure */
class HostedProcessorInformation
{
    friend class ProcessorBase;
    friend class Multiprocessor;

  public:
    /** Get the current processor's VirtualAddressSpace
     *\return reference to the current processor's VirtualAddressSpace */
    VirtualAddressSpace &getVirtualAddressSpace() const;
    /** Set the current processor's VirtualAddressSpace
     *\param[in] virtualAddressSpace reference to the new VirtualAddressSpace */
    void setVirtualAddressSpace(VirtualAddressSpace &virtualAddressSpace);

    uintptr_t getKernelStack() const;
    void setKernelStack(uintptr_t stack);
    Thread *getCurrentThread() const;
    void setCurrentThread(Thread *pThread);
    PerProcessorScheduler &getScheduler();

  protected:
    /** Construct a HostedProcessorInformation object
     *\param[in] processorId Identifier of the processor */
    HostedProcessorInformation(ProcessorId processorId, uint8_t apicId = 0);
    /** The destructor does nothing */
    virtual ~HostedProcessorInformation();

  private:
    /** Default constructor
     *\note NOT implemented */
    HostedProcessorInformation();
    /** Copy-constructor
     *\note NOT implemented */
    HostedProcessorInformation(const HostedProcessorInformation &);
    /** Assignment operator
     *\note NOT implemented */
    HostedProcessorInformation &operator=(const HostedProcessorInformation &);

    /** Identifier of that processor */
    ProcessorId m_ProcessorId;
    /** The current VirtualAddressSpace */
    VirtualAddressSpace *m_VirtualAddressSpace;
    /** The current thread */
    Thread *m_pCurrentThread;
    /** The processor's scheduler. */
    PerProcessorScheduler *m_Scheduler;
    /** Kernel stack. */
    uintptr_t m_KernelStack;
};

/** @} */

#endif
