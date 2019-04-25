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

#ifndef KERNEL_PROCESSOR_ARM_COMMON_PROCESSORINFORMATION_H
#define KERNEL_PROCESSOR_ARM_COMMON_PROCESSORINFORMATION_H

#define _PROCESSOR_INFORMATION_ONLY_WANT_PROCESSORID

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/types.h"

class VirtualAddressSpace;
class PerProcessorScheduler;
class Thread;

/** @addtogroup kernelprocessorarmv7
 * @{ */

/** ARMv7 processor information structure */
class EXPORTED_PUBLIC ArmCommonProcessorInformation
{
    friend class ProcessorBase;
  public:
    VirtualAddressSpace &getVirtualAddressSpace() const;

    void setVirtualAddressSpace(VirtualAddressSpace &virtualAddressSpace);

    uintptr_t getKernelStack() const;
    void setKernelStack(uintptr_t stack);

    Thread *getCurrentThread() const;
    void setCurrentThread(Thread *pThread);

    PerProcessorScheduler &getScheduler();

  protected:
    /** Construct a ArmCommonProcessor object
     *\param[in] processorId Identifier of the processor */
    ArmCommonProcessorInformation(ProcessorId processorId, uint8_t apicId = 0);

    virtual ~ArmCommonProcessorInformation();

  private:
    /** Default constructor
     *\note NOT implemented */
    ArmCommonProcessorInformation();
    /** Copy-constructor
     *\note NOT implemented */
    ArmCommonProcessorInformation(const ArmCommonProcessorInformation &);
    /** Assignment operator
     *\note NOT implemented */
    ArmCommonProcessorInformation &
    operator=(const ArmCommonProcessorInformation &);

    /** Identifier of that processor */
    ProcessorId m_ProcessorId;
    /** The current VirtualAddressSpace */
    VirtualAddressSpace *m_VirtualAddressSpace;
#if THREADS
    /** The current thread */
    Thread *m_pCurrentThread;
    /** The processor's scheduler. */
    PerProcessorScheduler *m_Scheduler;
#endif
};

/** @} */

#endif
