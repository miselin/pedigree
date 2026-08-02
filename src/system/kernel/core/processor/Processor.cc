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

#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/new"
#include "system/kernel/core/processor/DeviceHardIrqContext.h"

size_t ProcessorBase::m_Initialised = 0;

Vector<ProcessorInformation *> ProcessorBase::m_ProcessorInformation;
ProcessorInformation ProcessorBase::m_SafeBspProcessorInformation(0);

size_t ProcessorBase::m_nProcessors = 1;

namespace
{
const char *deviceHardIrqOperationName(DeviceHardIrqOperation operation)
{
    switch (operation)
    {
        case DeviceHardIrqOperation::Schedule:
            return "schedule";
        case DeviceHardIrqOperation::SemaphoreAcquire:
            return "semaphore acquire";
        case DeviceHardIrqOperation::SemaphoreRelease:
            return "semaphore release";
        case DeviceHardIrqOperation::WaitQueueAccess:
            return "wait-queue access";
        case DeviceHardIrqOperation::HeapAllocate:
            return "heap allocation";
        case DeviceHardIrqOperation::HeapFree:
            return "heap free";
    }
    return "unknown operation";
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
ProcessorBase::DeviceHardIrqOperationHook g_DeviceHardIrqOperationHook =
    nullptr;
size_t g_DeviceHardIrqOperationDenials = 0;
#endif
}  // namespace

size_t ProcessorBase::isInitialised()
{
    return m_Initialised;
}

bool ProcessorBase::inDeviceHardIrq()
{
    return information().m_DeviceHardIrqDepth != 0;
}

bool ProcessorBase::guardDeviceHardIrqOperation(
    DeviceHardIrqOperation operation)
{
    if (!inDeviceHardIrq())
    {
        return true;
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    DeviceHardIrqOperationHook hook = __atomic_load_n(
        &g_DeviceHardIrqOperationHook, __ATOMIC_ACQUIRE);
    if (hook && hook(operation))
    {
        __atomic_add_fetch(
            &g_DeviceHardIrqOperationDenials, static_cast<size_t>(1),
            __ATOMIC_RELAXED);
        return false;
    }
#endif

    FATAL_NOLOCK(
        "Device hard-IRQ callback attempted forbidden "
        << deviceHardIrqOperationName(operation) << ".");
    return false;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
size_t ProcessorBase::deviceHardIrqDepthForTest()
{
    return information().m_DeviceHardIrqDepth;
}

void ProcessorBase::setDeviceHardIrqOperationHookForTest(
    DeviceHardIrqOperationHook hook)
{
    __atomic_store_n(
        &g_DeviceHardIrqOperationHook, hook, __ATOMIC_RELEASE);
}

size_t ProcessorBase::deviceHardIrqOperationDenialsForTest()
{
    return __atomic_load_n(
        &g_DeviceHardIrqOperationDenials, __ATOMIC_RELAXED);
}
#endif

DeviceHardIrqContext::DeviceHardIrqContext(
    size_t &previousDepth, bool &restorationArmed)
    : m_Information(Processor::information()),
      m_RestorationArmed(restorationArmed),
      m_PreviousDepth(m_Information.m_DeviceHardIrqDepth)
{
    assert(!m_RestorationArmed);
    previousDepth = m_PreviousDepth;
    m_RestorationArmed = true;
    ++m_Information.m_DeviceHardIrqDepth;
}

DeviceHardIrqContext::~DeviceHardIrqContext()
{
    assert(&Processor::information() == &m_Information);
    assert(m_RestorationArmed);
    restoreDepth(m_PreviousDepth);
    m_RestorationArmed = false;
}

void DeviceHardIrqContext::restoreDepth(size_t previousDepth)
{
    Processor::information().m_DeviceHardIrqDepth = previousDepth;
}

SuspendDeviceHardIrqContext::SuspendDeviceHardIrqContext()
    : m_Information(Processor::information())
{
    assert(m_Information.m_DeviceHardIrqDepth == 1);
    m_Information.m_DeviceHardIrqDepth = 0;
}

SuspendDeviceHardIrqContext::~SuspendDeviceHardIrqContext()
{
    assert(&Processor::information() == &m_Information);
    assert(m_Information.m_DeviceHardIrqDepth == 0);
    m_Information.m_DeviceHardIrqDepth = 1;
}

EnsureInterrupts::EnsureInterrupts(bool desired)
{
    EMIT_IF(!PEDIGREE_BENCHMARK)
    {
        m_bPrevious = ProcessorBase::getInterrupts();
        ProcessorBase::setInterrupts(desired);
    }
}

EnsureInterrupts::~EnsureInterrupts()
{
    EMIT_IF(!PEDIGREE_BENCHMARK)
    {
        ProcessorBase::setInterrupts(m_bPrevious);
    }
}
