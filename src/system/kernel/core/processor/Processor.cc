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
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/new"
#include "system/kernel/core/processor/DeviceHardIrqContext.h"

size_t ProcessorBase::m_Initialised = 0;

Vector<ProcessorInformation *> ProcessorBase::m_ProcessorInformation;
ProcessorInformation ProcessorBase::m_SafeBspProcessorInformation(0);

size_t ProcessorBase::m_nProcessors = 1;

size_t ProcessorBase::isInitialised()
{
    return m_Initialised;
}

bool ProcessorBase::inDeviceHardIrq()
{
    return information().m_DeviceHardIrqDepth != 0;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
size_t ProcessorBase::deviceHardIrqDepthForTest()
{
    return information().m_DeviceHardIrqDepth;
}
#endif

DeviceHardIrqContext::DeviceHardIrqContext()
    : m_Information(Processor::information())
{
    ++m_Information.m_DeviceHardIrqDepth;
}

DeviceHardIrqContext::~DeviceHardIrqContext()
{
    assert(&Processor::information() == &m_Information);
    assert(m_Information.m_DeviceHardIrqDepth);
    --m_Information.m_DeviceHardIrqDepth;
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
