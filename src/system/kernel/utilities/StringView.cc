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
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

StringView::StringView() : m_String(nullptr), m_Length(0), m_HashingEnabled(HASH_STRINGVIEWS_BY_DEFAULT)
{
    m_Hash = 0;
}

StringView::StringView(const char *s)
    : m_String(s), m_Length(StringLength(s)), m_Hash(0), m_HashingEnabled(HASH_STRINGVIEWS_BY_DEFAULT)
{
    setHashingEnable(m_HashingEnabled);
}

StringView::StringView(const char *s, size_t length)
    : m_String(s), m_Length(length), m_Hash(0), m_HashingEnabled(HASH_STRINGVIEWS_BY_DEFAULT)
{
    setHashingEnable(m_HashingEnabled);
}

StringView::StringView(const char *s, size_t length, uint32_t hash, bool hashingEnabled)
    : m_String(s), m_Length(length), m_Hash(hash), m_HashingEnabled(hashingEnabled)
{
}

StringView::StringView(const StringView &other)
    : m_String(other.m_String), m_Length(other.m_Length), m_Hash(other.m_Hash),
      m_HashingEnabled(other.m_HashingEnabled)
{
}

StringView::StringView(const String &other) : StringView(other.view())
{
}

StringView::~StringView() = default;

StringView &StringView::operator=(const StringView &s)
{
    m_String = s.m_String;
    m_Length = s.m_Length;
    m_Hash = s.m_Hash;
    m_HashingEnabled = s.m_HashingEnabled;
    return *this;
}

bool StringView::operator==(const char *s) const
{
    return compare(s, StringLength(s));
}

bool StringView::operator==(const String &s) const
{
    if (m_Length != s.length())
    {
        return false;
    }
    else if (!compareHash(s))
    {
        return false;
    }

    return compare(static_cast<const char *>(s), s.length());
}

bool StringView::operator==(const StringView &s) const
{
    if (m_Length != s.m_Length)
    {
        return false;
    }
    else if (!(m_Length && s.m_Length))
    {
        return false;
    }
    else if (!compareHash(s))
    {
        return false;
    }

    return !StringMatchN(m_String, s.m_String, m_Length);
}

bool StringView::compare(const char *s, size_t length) const
{
    if (UNLIKELY(!m_Length))
    {
        if (!*s)
        {
            // empty strings match
            return true;
        }
        return false;
    }

    if (length != m_Length)
    {
        return false;
    }

    return !StringMatchN(s, m_String, m_Length);
}

size_t StringView::length() const
{
    return m_Length;
}

StringView StringView::substring(size_t start, size_t end, bool hashed) const
{
    if (start == 0 && end == m_Length)
    {
        return StringView(m_String, m_Length, m_Hash, m_HashingEnabled);
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

char StringView::operator[](size_t index) const
{
#if ADDITIONAL_CHECKS
    if (UNLIKELY(index >= m_Length))
    {
        ERROR("operator[] - index " << index << " exceeds length " << m_Length);
        assert(index < m_Length);
    }
#endif
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
    if (m_Hash)
    {
        return m_Hash;
    }
    else
    {
        return computeHash();
    }
}

uint32_t StringView::hash()
{
    // Just-in-time hashing (something clearly needs it).
    if (!m_HashingEnabled)
    {
        setHashingEnable(true);
    }
    return m_Hash;
}

const char *StringView::str() const
{
    return m_String;
}

void StringView::setHashingEnable(bool enabled)
{
    m_HashingEnabled = enabled;

    if (!enabled)
    {
        m_Hash = 0;
    }
    else if (!m_Hash)
    {
        m_Hash = computeHash();
    }
}

bool StringView::compareHash(const StringView &other) const
{
    if (!(m_HashingEnabled && other.m_HashingEnabled))
    {
        return true;
    }
    else
    {
        return hash() == other.hash();
    }
}

bool StringView::compareHash(const String &other) const
{
    if (!m_HashingEnabled)
    {
        return true;
    }
    else
    {
        return hash() == other.hash();
    }
}

uint32_t StringView::computeHash() const
{
    if (!m_Length)
    {
        return 0;
    }
    else
    {
        return spookyHash(m_String, m_Length);
    }
}

bool StringView::defaultHashingEnabled() const
{
    return HASH_STRINGVIEWS_BY_DEFAULT;
}

HashedStringView::HashedStringView(const char *s) : StringView(s)
{
    setHashingEnable(true);
}

HashedStringView::HashedStringView(const char *s, size_t length) : StringView(s, length)
{
    setHashingEnable(true);
}

HashedStringView::HashedStringView(const StringView &other) : StringView(other)
{
    setHashingEnable(true);
}

HashedStringView::HashedStringView(const HashedStringView &other) : StringView(other)
{
    setHashingEnable(true);
}

HashedStringView::HashedStringView(const String &other) : HashedStringView(other.view())
{
    setHashingEnable(true);
}

bool HashedStringView::defaultHashingEnabled() const
{
    return true;
}
