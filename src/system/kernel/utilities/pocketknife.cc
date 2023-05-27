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

#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/pocketknife.h"

class Process;

namespace pocketknife
{
void runConcurrently(int (*func)(void *), void *param)
{
#if THREADS
    Process *parent = Processor::information().getCurrentThread()->getParent();
    Thread *pThread = new Thread(parent, func, param);
    pThread->detach();
#else
    func(param);
#endif
}

void *runConcurrentlyAttached(int (*func)(void *), void *param)
{
#if THREADS
    Process *parent = Processor::information().getCurrentThread()->getParent();
    Thread *pThread = new Thread(parent, func, param);
    return pThread;
#else
    int result = func(param);
    return reinterpret_cast<void *>(result);
#endif
}

int attachTo(void *handle)
{
#if THREADS
    Thread *pThread = reinterpret_cast<Thread *>(handle);
    return pThread->join();
#else
    return reinterpret_cast<int>(handle);
#endif
}

VirtualAddressSpaceSwitch::VirtualAddressSpaceSwitch() : va(nullptr)
{
    EMIT_IF(KERNEL_NEEDS_ADDRESS_SPACE_SWITCH)
    {
        VirtualAddressSpace &va = VirtualAddressSpace::getKernelAddressSpace();
        VirtualAddressSpace &currva =
            Processor::information().getVirtualAddressSpace();
        if (Processor::m_Initialised == 2)
            Processor::switchAddressSpace(va);

        this->va = &currva;
    }
}

VirtualAddressSpaceSwitch::~VirtualAddressSpaceSwitch()
{
    restore();
}

void VirtualAddressSpaceSwitch::restore()
{
    if (!va)
    {
        return;
    }

    EMIT_IF(KERNEL_NEEDS_ADDRESS_SPACE_SWITCH)
    {
        if (Processor::m_Initialised == 2)
            Processor::switchAddressSpace(*reinterpret_cast<VirtualAddressSpace *>(va));

        va = nullptr;
    }
}
}  // namespace pocketknife
