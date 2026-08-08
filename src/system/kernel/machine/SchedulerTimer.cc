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

#include "pedigree/kernel/machine/SchedulerTimer.h"
#include "pedigree/kernel/machine/SchedulerTimerDispatchCleanup.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

SchedulerTimer::SchedulerTimer() = default;
SchedulerTimer::~SchedulerTimer() = default;

SchedulerTimerDispatchCleanup::SchedulerTimerDispatchCleanup(
    SchedulerTimerHandlerSlot::DispatchGuard &dispatch)
    : m_Dispatch(dispatch),
      m_Thread(Processor::information().getCurrentThread()), m_Cleanup(),
      m_Active(false)
{
    if (!m_Thread)
    {
        return;
    }

    // Publication precedes callback execution, so an abandoned frame cannot
    // strand the counted admission even at Thread state level zero.
    m_Thread->armAtomicStateCleanup(m_Cleanup, abandon, this);
    m_Active = true;
}

SchedulerTimerDispatchCleanup::~SchedulerTimerDispatchCleanup()
{
    if (!m_Active)
    {
        return;
    }

    // The raw dispatch guard releases immediately after this scope. Disarm
    // first so ordinary return has exactly one release path.
    m_Thread->disarmAtomicStateCleanup(m_Cleanup);
    m_Active = false;
}

void SchedulerTimerDispatchCleanup::abandon(void *context)
{
    SchedulerTimerDispatchCleanup *cleanup =
        reinterpret_cast<SchedulerTimerDispatchCleanup *>(context);
    if (cleanup)
    {
        cleanup->release();
    }
}

void SchedulerTimerDispatchCleanup::release()
{
    if (!m_Active)
    {
        return;
    }

    // Clear the scope before releasing the slot. The guard's destructor may
    // still execute in hosted model tests after the state has been abandoned.
    m_Active = false;
    m_Dispatch.release();
}

bool SchedulerTimer::canRemoveHandlerInCurrentContext()
{
    Thread *current = Processor::information().getCurrentThread();
    if (!current ||
        Processor::executionContext() != ExecutionContext::WaitableThread)
    {
        return false;
    }
#if HOSTED
    if (Processor::inHostedSignalFrame() || current->getHostedSignalDepth())
    {
        return false;
    }
#endif
    return true;
}
