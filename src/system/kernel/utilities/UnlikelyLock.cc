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

#include "pedigree/kernel/utilities/UnlikelyLock.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"

UnlikelyLock::UnlikelyLock()
    : m_Lock(), m_Condition(), m_nReaders(0), m_nWaitingWriters(0),
      m_bActiveWriter(false)
{
}

UnlikelyLock::~UnlikelyLock()
{
    LockGuard<Mutex> guard(m_Lock);
    if (m_nReaders || m_nWaitingWriters || m_bActiveWriter)
    {
        FATAL("Destroying an owned or contended UnlikelyLock.");
    }
}

void UnlikelyLock::enter()
{
    LockGuard<Mutex> guard(m_Lock);
    while (m_bActiveWriter || m_nWaitingWriters)
    {
        m_Condition.waitForCompletion(m_Lock);
    }

    ++m_nReaders;
}

void UnlikelyLock::leave()
{
    LockGuard<Mutex> guard(m_Lock);
    if (!m_nReaders)
    {
        FATAL("UnlikelyLock::leave without a matching enter.");
    }
    if (!--m_nReaders)
    {
        // A signal can select a reader, which must remain gated while a
        // writer is pending. Wake the writer set as well.
        m_Condition.broadcast();
    }
}

void UnlikelyLock::acquire()
{
    LockGuard<Mutex> guard(m_Lock);
    ++m_nWaitingWriters;
    while (m_bActiveWriter || m_nReaders)
    {
        m_Condition.waitForCompletion(m_Lock);
    }

    --m_nWaitingWriters;
    m_bActiveWriter = true;
}

void UnlikelyLock::release()
{
    LockGuard<Mutex> guard(m_Lock);
    if (!m_bActiveWriter)
    {
        FATAL("UnlikelyLock::release without writer ownership.");
    }
    m_bActiveWriter = false;
    m_Condition.broadcast();
}
