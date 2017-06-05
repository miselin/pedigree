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

#ifndef KERNEL_PROCESSOR_X86_COMMON_PROCESSORINFORMATION_H
#define KERNEL_PROCESSOR_X86_COMMON_PROCESSORINFORMATION_H

#define _PROCESSOR_INFORMATION_ONLY_WANT_PROCESSORID

#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/types.h"

class VirtualAddressSpace;
class PerProcessorScheduler;
class Thread;

/** @addtogroup kernelprocessorx86common
 * @{ */

/** Common x86 processor information structure */
class X86CommonProcessorInformation
{
    friend class Processor;
    friend class Multiprocessor;

  public:
#if defined(X86)
    typedef class X86TaskStateSegment TaskStateSegment;
#else
    typedef class X64TaskStateSegment TaskStateSegment;
#endif

    /** Get the current processor's VirtualAddressSpace
     *\return reference to the current processor's VirtualAddressSpace */
    VirtualAddressSpace &getVirtualAddressSpace() const;
    /** Set the current processor's VirtualAddressSpace
     *\param[in] virtualAddressSpace reference to the new VirtualAddressSpace */
    void setVirtualAddressSpace(VirtualAddressSpace &virtualAddressSpace);

    /** Set the processor's TSS selector
     *\param[in] TssSelector the new TSS selector */
    void setTssSelector(uint16_t TssSelector);
    /** Set the processor's TSS
     *\param[in] Tss pointer to the new TSS */
    void setTss(void *Tss);
    /** Get the processor's TSS selector
     *\return the TSS selector of the processor */
    uint16_t getTssSelector() const;
    /** Get the processor's TSS
     *\return the Tss of the processor */
    void *getTss() const;
    /** Gets the processor's TLS base segment */
    uint16_t getTlsSelector();
    /** Sets the processor's TLS base segment */
    void setTlsSelector(uint16_t tls);

    uintptr_t getKernelStack() const;
    void setKernelStack(uintptr_t stack);
    Thread *getCurrentThread() const;
    void setCurrentThread(Thread *pThread);

    PerProcessorScheduler &getScheduler();

  protected:
    /** Construct a X86CommonProcessor object
     *\param[in] processorId Identifier of the processor */
    X86CommonProcessorInformation(ProcessorId processorId, uint8_t apicId = 0);
    /** The destructor does nothing */
    virtual ~X86CommonProcessorInformation();

    void setIds(ProcessorId processorId, uint8_t apicId = 0);

  private:
    /** Default constructor
     *\note NOT implemented */
    X86CommonProcessorInformation();
    /** Copy-constructor
     *\note NOT implemented */
    X86CommonProcessorInformation(const X86CommonProcessorInformation &);
    /** Assignment operator
     *\note NOT implemented */
    X86CommonProcessorInformation &
    operator=(const X86CommonProcessorInformation &);

    /** Identifier of that processor */
    ProcessorId m_ProcessorId;
    /** The Task-State-Segment selector of that Processor */
    uint16_t m_TssSelector;
    /** Pointer to this processor's Task-State-Segment */
    TaskStateSegment *m_Tss;
    /** The current VirtualAddressSpace */
    VirtualAddressSpace *m_VirtualAddressSpace;
    /** Local APIC Id */
    uint8_t m_LocalApicId;
    /** The current thread */
    Thread *m_pCurrentThread;
    /** The processor's scheduler. */
    PerProcessorScheduler *m_Scheduler;
    /** The processor's TLS segment */
    uint16_t m_TlsSelector;
};

/** @} */

#endif
