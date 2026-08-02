/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/process/InterruptTimeAccounting.h"
#include "pedigree/kernel/process/DeferredTimeAccounting.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

InterruptTimeAccounting::InterruptTimeAccounting(bool fromUserspace)
    : m_pThread(Processor::information().getCurrentThread()),
      m_bFromUserspace(fromUserspace)
{
    if (!m_pThread || !m_pThread->getParent())
    {
        m_pThread = nullptr;
        return;
    }

    m_pThread->transitionTime(
        KernelTimeTransition::interrupted(m_bFromUserspace),
        KernelTimeTransition::handler());
}

InterruptTimeAccounting::~InterruptTimeAccounting()
{
    if (!m_pThread)
    {
        return;
    }

    // A userspace-origin interrupt still has an ordinary kernel return tail.
    // Its final architecture boundary owns the Kernel -> User transition so
    // callbacks and subsystem work are charged to the kernel.
    if (!m_bFromUserspace)
    {
        m_pThread->transitionTime(
            KernelTimeTransition::handler(),
            KernelTimeTransition::resumed(false));
    }
}

void InterruptTimeAccounting::finishUserReturn(Thread *thread)
{
    if (
        thread && thread->currentTimeAccountingMode() == CpuTimeMode::Kernel)
    {
        thread->transitionTimeAtInterruptReturn(
            CpuTimeMode::Kernel, CpuTimeMode::User);
    }
}
