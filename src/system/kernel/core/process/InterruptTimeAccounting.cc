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

    m_pThread->transitionTime(
        KernelTimeTransition::handler(),
        KernelTimeTransition::resumed(m_bFromUserspace));
}
