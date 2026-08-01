/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/process/Completion.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Thread.h"

Completion::Completion()
    : m_Waiters(), m_Completed(false), m_WaitClaimed(false)
{
}

Completion::~Completion() = default;

bool Completion::wait(
    WaitQueue::AbandonCallback onAbandon, void *abandonContext)
{
    bool claimed = false;
    while (true)
    {
        auto guard = m_Waiters.acquire();
        if (!claimed)
        {
            if (m_WaitClaimed)
            {
                FATAL("Completion waited more than once");
            }
            m_WaitClaimed = true;
            claimed = true;
        }

        if (m_Completed)
        {
            return true;
        }

        WaitQueue::WakeReason reason = guard.wait(
            WaitQueue::Channel(), Thread::SemWait,
            reinterpret_cast<uintptr_t>(__builtin_return_address(0)),
            onAbandon, abandonContext);
        if (
            reason == WaitQueue::WakeReason::Unwinding ||
            reason == WaitQueue::WakeReason::Terminating)
        {
            return false;
        }
    }
}

bool Completion::complete()
{
    auto guard = m_Waiters.acquire();
    if (m_Completed)
    {
        return false;
    }

    m_Completed = true;
    guard.wakeOne();
    return true;
}

bool Completion::isComplete()
{
    auto guard = m_Waiters.acquire();
    return m_Completed;
}
