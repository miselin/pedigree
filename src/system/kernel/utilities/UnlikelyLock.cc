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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/utilities/UnlikelyLock.h"

UnlikelyLock::UnlikelyLock()
    : m_Lock(false), m_Condition(), m_nReaders(0), m_bActiveWriter(false)
{
}

UnlikelyLock::~UnlikelyLock()
{
}

bool UnlikelyLock::enter()
{
    LockGuard<Mutex> guard(m_Lock);
    while (m_bActiveWriter)
    {
        m_Condition.wait(m_Lock);
    }

    ++m_nReaders;
    return true;
}

void UnlikelyLock::leave()
{
    LockGuard<Mutex> guard(m_Lock);
    if (!--m_nReaders)
    {
        m_Condition.signal();
    }
}

bool UnlikelyLock::acquire()
{
    LockGuard<Mutex> guard(m_Lock);
    while (m_bActiveWriter || m_nReaders)
    {
        m_Condition.wait(m_Lock);
    }

    m_bActiveWriter = true;
    return true;
}

void UnlikelyLock::release()
{
    LockGuard<Mutex> guard(m_Lock);
    m_bActiveWriter = false;
    m_Condition.broadcast();
}
