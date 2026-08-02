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

#include "pedigree/kernel/processor/hosted/ProcessorInformation.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "VirtualAddressSpace.h"

namespace __pedigree_hosted
{
};
using namespace __pedigree_hosted;

#include <errno.h>
#include <signal.h>

#ifndef SS_AUTODISARM
#define SS_AUTODISARM (1U << 31)
#endif

extern void *safe_stack_top;

HostedProcessorInformation::HostedProcessorInformation(
    ProcessorId processorId, uint8_t apicId)
    : m_ProcessorId(processorId),
      m_VirtualAddressSpace(&VirtualAddressSpace::getKernelAddressSpace()),
      m_pCurrentThread(0), m_Scheduler(0),
      m_KernelStack(0), m_HostedExecutionThreadId(0),
      m_DeviceHardIrqDepth(0),
      m_HostedSignalFrameDepth(0)
{
}

HostedProcessorInformation::~HostedProcessorInformation()
{
}

VirtualAddressSpace &HostedProcessorInformation::getVirtualAddressSpace() const
{
    if (m_VirtualAddressSpace)
        return *m_VirtualAddressSpace;
    else
        return VirtualAddressSpace::getKernelAddressSpace();
}

void HostedProcessorInformation::setVirtualAddressSpace(
    VirtualAddressSpace &virtualAddressSpace)
{
    m_VirtualAddressSpace = &virtualAddressSpace;
}

Thread *HostedProcessorInformation::getCurrentThread() const
{
    return m_pCurrentThread;
}

void HostedProcessorInformation::setCurrentThread(Thread *pThread)
{
    m_pCurrentThread = pThread;
}

PerProcessorScheduler &HostedProcessorInformation::getScheduler()
{
    if (m_Scheduler == nullptr)
    {
        m_Scheduler = new PerProcessorScheduler();
    }
    return *m_Scheduler;
}

/**
 * So, the sigaltstack implementation implements EPERM for sigaltstack by
 * checking the userspace stack pointer. While this is usually OK, as it will
 * protect most bad uses of sigaltstack, we need to outsmart this to make
 * sigaltstack work more like the TSS-based stack pointers seen in x86.
 *
 * Use a dedicated scratch stack so changing one thread's signal stack cannot
 * overwrite a saved frame belonging to the thread being switched in.
 */
static int trickSigaltstack(stack_t *p)
{
    return callOnStack(
        reinterpret_cast<uintptr_t>(&safe_stack_top),
        reinterpret_cast<uintptr_t>(sigaltstack),
        reinterpret_cast<uintptr_t>(p));
}

static void installHostedSignalStack(stack_t &stack)
{
    int result = sigaltstack(&stack, nullptr);
    if (result < 0 && errno == EPERM)
    {
        result = trickSigaltstack(&stack);
    }

    if (result >= 0)
    {
        return;
    }

    if (errno == EINVAL && !(stack.ss_flags & SS_DISABLE))
    {
        panic(
            "Hosted requires Linux SS_AUTODISARM support for safe "
            "scheduler preemption");
    }

    FATAL(
        "Hosted failed to install a scheduler signal stack: errno="
        << Dec << errno);
}

void HostedProcessorInformation::setKernelStack(uintptr_t stack)
{
    if (stack)
    {
        void *new_sp = reinterpret_cast<void *>(stack - KERNEL_STACK_SIZE);
        stack_t s;
        if (sigaltstack(nullptr, &s) < 0)
        {
            FATAL("Hosted failed to inspect the scheduler signal stack");
        }
        if (
            s.ss_sp != new_sp || s.ss_size != KERNEL_STACK_SIZE ||
            (s.ss_flags & (SS_DISABLE | SS_AUTODISARM)) != SS_AUTODISARM)
        {
            ByteSet(&s, 0, sizeof(s));
            s.ss_sp = new_sp;
            s.ss_size = KERNEL_STACK_SIZE;
            s.ss_flags = SS_AUTODISARM;
            installHostedSignalStack(s);
        }
    }
    else
    {
        stack_t s;
        if (sigaltstack(nullptr, &s) < 0)
        {
            FATAL("Hosted failed to inspect the scheduler signal stack");
        }
        if (!(s.ss_flags & SS_DISABLE))
        {
            ByteSet(&s, 0, sizeof(s));
            s.ss_flags = SS_DISABLE;
            installHostedSignalStack(s);
        }
    }

    m_KernelStack = stack;
}

uintptr_t HostedProcessorInformation::getKernelStack() const
{
    return m_KernelStack;
}
