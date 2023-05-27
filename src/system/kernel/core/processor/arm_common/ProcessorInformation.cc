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

#include "pedigree/kernel/processor/arm_common/ProcessorInformation.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/new"

VirtualAddressSpace &ArmCommonProcessorInformation::getVirtualAddressSpace() const
{
    if (m_VirtualAddressSpace)
    {
        return *m_VirtualAddressSpace;
    }
    else
    {
        return VirtualAddressSpace::getKernelAddressSpace();
    }
}

void ArmCommonProcessorInformation::setVirtualAddressSpace(VirtualAddressSpace &virtualAddressSpace)
{
    m_VirtualAddressSpace = &virtualAddressSpace;
}

Thread *ArmCommonProcessorInformation::getCurrentThread() const
{
    return m_pCurrentThread;
}

void ArmCommonProcessorInformation::setCurrentThread(Thread *pThread)
{
    m_pCurrentThread = pThread;
}

PerProcessorScheduler &ArmCommonProcessorInformation::getScheduler()
{
    // Allocate the scheduler lazily.
    if (m_Scheduler == nullptr)
    {
        m_Scheduler = new PerProcessorScheduler();
    }
    return *m_Scheduler;
}

ArmCommonProcessorInformation::ArmCommonProcessorInformation(
    ProcessorId processorId, uint8_t apicId)
    : m_ProcessorId(processorId),
      m_VirtualAddressSpace(&VirtualAddressSpace::getKernelAddressSpace()),
      m_pCurrentThread(0), m_Scheduler(nullptr)
{
}

ArmCommonProcessorInformation::~ArmCommonProcessorInformation()
{
}

uintptr_t ArmCommonProcessorInformation::getKernelStack() const
{
    return 0;
    /*
        uintptr_t ret = 0;

        // Switch to IRQ mode
        uint32_t cpsr = 0;
        asm volatile("mrs %0, cpsr" : "=r" (cpsr));
        uint32_t oldMode = cpsr & 0x3F;
        if(oldMode != 0x12)
        {
            cpsr &= ~0x3F;
            cpsr |= 0x12;
            asm volatile("msr cpsr_c, %0" : : "r" (cpsr));
        }

        // Load new stack and all that
        asm volatile("mov %0, sp" : "=r" (ret));

        // Switch back to the previous mode
        if(oldMode != 0x12)
        {
            cpsr &= ~0x3F;
            cpsr |= oldMode;
            asm volatile("msr cpsr_c, %0" : : "r" (cpsr));
        }

        return ret;
    */
}

void ArmCommonProcessorInformation::setKernelStack(uintptr_t stack)
{
    /*
        // Handle IRQ save location
        stack -= 0x10;

        // Switch to IRQ mode
        uint32_t cpsr = 0;
        asm volatile("mrs %0, cpsr" : "=r" (cpsr));
        uint32_t oldMode = cpsr & 0x3F;
        if(oldMode == 0x12)
        {
            // Can't switch a stack we're using!
            return;
        }
        cpsr &= ~0x3F;
        cpsr |= 0x12;
        asm volatile("msr cpsr_c, %0" : : "r" (cpsr));

        // Load new stack and all that
        asm volatile("mov sp, %0; mov r13, %1" : : "r" (stack), "r" (stack +
       0x10) : "sp", "r13");

        // Switch back to the previous mode
        cpsr &= ~0x3F;
        cpsr |= oldMode;
        asm volatile("msr cpsr_c, %0" : : "r" (cpsr));
    */
}
