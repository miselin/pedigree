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

#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/Log.h"

StringView::StringView() : m_String(nullptr), m_Length(0)
{
    m_Hash = 0;
}

StringView::StringView(const char *s) : m_String(s), m_Length(StringLength(s)), m_Hash(0)
{
    m_Hash = jenkinsHash(m_String, m_Length);
}

StringView::StringView(const char *s, size_t length) : m_String(s), m_Length(length), m_Hash(0)
{
    m_Hash = jenkinsHash(m_String, m_Length);
}

StringView::StringView(const char *s, size_t length, uint32_t hash) : m_String(s), m_Length(length), m_Hash(hash)
{
}

StringView::StringView(const StringView &other) : m_String(other.m_String), m_Length(other.m_Length), m_Hash(other.m_Hash)
{
}

StringView::~StringView() = default;

StringView &StringView::operator= (const StringView &s)
{
    m_String = s.m_String;
    m_Length = s.m_Length;
    m_Hash = s.m_Hash;
    return *this;
}

bool StringView::operator== (const char *s) const
{
    if (!m_Length)
    {
        if (!*s)
        {
            // empty strings match
            return true;
        }
        return false;
    }

    // This is a little slow, but necessary as we aren't guaranteed to own a
    // null-terminated string - so StringCompareN doesn't work for things like
    // StringView("hello") == "hello world".
    size_t otherLength = StringLength(s);
    if (otherLength != m_Length)
    {
        return false;
    }

    return !StringCompareN(s, m_String, m_Length);
}

bool StringView::operator== (const String &s) const
{
    return *this == static_cast<const char *>(s);
}

bool StringView::operator== (const StringView &s) const
{
    if (m_Length != s.m_Length)
    {
        return false;
    }
    else if (!(m_Length && s.m_Length))
    {
        return false;
    }
    else if (m_Hash != s.m_Hash)
    {
        return false;
    }

    return !StringCompareN(m_String, s.m_String, m_Length);
}

size_t StringView::length() const
{
    return m_Length;
}

StringView StringView::substring(size_t start, size_t end) const
{
    if (start == 0 && end == m_Length)
    {
        return StringView(m_String, m_Length, m_Hash);
    }

    if (end > m_Length)
    {
        end = m_Length;
    }

    if ((start > m_Length) || (start >= end))
    {
        return StringView();
    }

    return StringView(m_String + start, end - start);
}

String StringView::toString() const
{
    return String(m_String, m_Length);
}

char StringView::operator[] (size_t index) const
{
    if (index >= m_Length)
    {
        ERROR("operator[] - index " << index << " exceeds length " << m_Length);
    }
    assert(index < m_Length);
    return m_String[index];
}

size_t StringView::nextCharacter(size_t c) const
{
    return ::nextCharacter(m_String, c);
}

size_t StringView::prevCharacter(size_t c) const
{
    return ::prevCharacter(m_String, c);
}

uint32_t StringView::hash() const
{
    return m_Hash;
}

const char *StringView::str() const
{
    return m_String;
}
