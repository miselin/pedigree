/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

namespace
{
void assertCurrentThread(Thread *thread)
{
    if (
        thread &&
        Processor::information().getCurrentThread() != thread)
    {
        FATAL(
            "TerminationDeferral moved or released on a different Thread.");
    }
}
}  // namespace

TerminationDeferral::TerminationDeferral(bool active)
    : m_pThread(
          active ? Processor::information().getCurrentThread() : nullptr),
      m_Record()
{
    if (m_pThread)
    {
        m_pThread->registerDeferredScope(m_Record, true, false);
    }
}

TerminationDeferral::TerminationDeferral(TerminationDeferral &&other)
    : m_pThread(other.m_pThread), m_Record()
{
    assertCurrentThread(m_pThread);
    if (m_pThread)
    {
        m_pThread->moveDeferredScope(other.m_Record, m_Record);
    }
    other.m_pThread = nullptr;
}

TerminationDeferral::~TerminationDeferral()
{
    if (m_pThread)
    {
        assertCurrentThread(m_pThread);
        m_pThread->unregisterDeferredScope(m_Record);
    }
}

TerminationDeferral &TerminationDeferral::operator=(
    TerminationDeferral &&other)
{
    if (this != &other)
    {
        if (m_pThread)
        {
            assertCurrentThread(m_pThread);
            m_pThread->unregisterDeferredScope(m_Record);
        }
        assertCurrentThread(other.m_pThread);
        m_pThread = other.m_pThread;
        if (m_pThread)
        {
            m_pThread->moveDeferredScope(other.m_Record, m_Record);
        }
        other.m_pThread = nullptr;
    }
    return *this;
}
